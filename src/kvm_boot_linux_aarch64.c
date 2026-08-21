#define _GNU_SOURCE
#include "cell.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAM_BASE UINT64_C(0x40000000)
#define KERNEL_OFFSET UINT64_C(0x200000)
#define DTB_OFFSET UINT64_C(0x07000000)
#define GIC_DIST_ADDR UINT64_C(0x08000000)
#define GIC_REDIST_ADDR UINT64_C(0x080a0000)
#define UART_ADDR UINT64_C(0x09000000)
#define COMPLETION_ADDR UINT64_C(0x10000000)

static volatile sig_atomic_t boot_timed_out;

static void timeout_handler(int signal_number) {
    (void)signal_number;
    boot_timed_out = 1;
}

static void fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int read_file(const char *path, uint8_t **data, size_t *size,
                     char *error, size_t error_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fail(error, error_size, "open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        fail(error, error_size, "invalid file %s", path); close(fd); return -1;
    }
    *size = (size_t)st.st_size;
    *data = malloc(*size);
    if (*data == NULL) { fail(error, error_size, "allocate %zu bytes", *size); close(fd); return -1; }
    size_t done = 0;
    while (done < *size) {
        ssize_t count = read(fd, *data + done, *size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { fail(error, error_size, "read %s failed", path); free(*data); *data = NULL; close(fd); return -1; }
        done += (size_t)count;
    }
    close(fd);
    return 0;
}

static int set_one_reg(int vcpu, uint64_t id, uint64_t *value) {
    struct kvm_one_reg reg = {.id = id, .addr = (uint64_t)(uintptr_t)value};
    return ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}

static int set_device_addr(int device, uint64_t type, uint64_t *address) {
    struct kvm_device_attr attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = type,
        .addr = (uint64_t)(uintptr_t)address
    };
    return ioctl(device, KVM_SET_DEVICE_ATTR, &attr);
}

int cell_kvm_boot_linux(const char *kernel_path, const char *dtb_path,
                        uint32_t memory_mib, uint32_t timeout_seconds,
                        char *error, size_t error_size) {
    int rc = -1, kvm = -1, vm = -1, vcpu = -1, gic = -1;
    uint8_t *kernel = NULL, *dtb = NULL, *memory = MAP_FAILED;
    size_t kernel_size = 0, dtb_size = 0, run_size = 0;
    struct kvm_run *run = MAP_FAILED;
    struct sigaction old_action;
    bool signal_installed = false;
    bool guest_ready = false;
    size_t marker_cursor = 0;
    static const char marker[] = "CELL_MVP_OK\n";

    if (kernel_path == NULL || dtb_path == NULL || memory_mib < 32 ||
        memory_mib > 512 || timeout_seconds == 0 || timeout_seconds > 300) {
        fail(error, error_size, "arm64 boot requires Image, DTB, memory and timeout"); return -1;
    }
    if (read_file(kernel_path, &kernel, &kernel_size, error, error_size) < 0 ||
        read_file(dtb_path, &dtb, &dtb_size, error, error_size) < 0) goto done;
    if (kernel_size < 64 || read_le32(kernel + 0x38) != UINT32_C(0x644d5241)) {
        fail(error, error_size, "kernel is not an uncompressed arm64 Linux Image"); goto done;
    }
    if (dtb_size < 40 || read_be32(dtb) != UINT32_C(0xd00dfeed)) {
        fail(error, error_size, "invalid flattened device tree"); goto done;
    }
    size_t memory_size = (size_t)memory_mib * 1024U * 1024U;
    if (KERNEL_OFFSET + kernel_size >= DTB_OFFSET || DTB_OFFSET + dtb_size > memory_size) {
        fail(error, error_size, "Image or DTB does not fit in guest RAM"); goto done;
    }

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) { fail(error, error_size, "open /dev/kvm: %s", strerror(errno)); goto done; }
    if (ioctl(kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION) {
        fail(error, error_size, "unsupported KVM API version"); goto done;
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) { fail(error, error_size, "KVM_CREATE_VM: %s", strerror(errno)); goto done; }
    memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) { fail(error, error_size, "map guest RAM: %s", strerror(errno)); goto done; }
    (void)madvise(memory, memory_size, MADV_DONTDUMP);
    memcpy(memory + KERNEL_OFFSET, kernel, kernel_size);
    memcpy(memory + DTB_OFFSET, dtb, dtb_size);
    struct kvm_userspace_memory_region region = {
        .slot = 0, .guest_phys_addr = RAM_BASE, .memory_size = memory_size,
        .userspace_addr = (uint64_t)(uintptr_t)memory
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        fail(error, error_size, "register guest RAM: %s", strerror(errno)); goto done;
    }

    struct kvm_create_device create_gic = {.type = KVM_DEV_TYPE_ARM_VGIC_V3};
    if (ioctl(vm, KVM_CREATE_DEVICE, &create_gic) < 0) {
        fail(error, error_size, "create GICv3: %s", strerror(errno)); goto done;
    }
    gic = (int)create_gic.fd;
    uint64_t dist = GIC_DIST_ADDR, redist = GIC_REDIST_ADDR;
    if (set_device_addr(gic, KVM_VGIC_V3_ADDR_TYPE_DIST, &dist) < 0 ||
        set_device_addr(gic, KVM_VGIC_V3_ADDR_TYPE_REDIST, &redist) < 0) {
        fail(error, error_size, "configure GICv3: %s", strerror(errno)); goto done;
    }

    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) { fail(error, error_size, "KVM_CREATE_VCPU: %s", strerror(errno)); goto done; }
    struct kvm_vcpu_init init;
    memset(&init, 0, sizeof init);
    if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
        fail(error, error_size, "get preferred arm64 target: %s", strerror(errno)); goto done;
    }
    init.features[0] |= UINT64_C(1) << KVM_ARM_VCPU_PSCI_0_2;
    if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
        fail(error, error_size, "initialize arm64 vCPU: %s", strerror(errno)); goto done;
    }
    struct kvm_device_attr finalize = {
        .group = KVM_DEV_ARM_VGIC_GRP_CTRL,
        .attr = KVM_DEV_ARM_VGIC_CTRL_INIT
    };
    if (ioctl(gic, KVM_SET_DEVICE_ATTR, &finalize) < 0) {
        fail(error, error_size, "finalize GICv3: %s", strerror(errno)); goto done;
    }

    uint64_t x0 = RAM_BASE + DTB_OFFSET;
    uint64_t pc = RAM_BASE + KERNEL_OFFSET;
    uint64_t pstate = UINT64_C(0x3c5);
    if (set_one_reg(vcpu, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                    KVM_REG_ARM_CORE_REG(regs.regs[0]), &x0) < 0 ||
        set_one_reg(vcpu, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                    KVM_REG_ARM_CORE_REG(regs.pc), &pc) < 0 ||
        set_one_reg(vcpu, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                    KVM_REG_ARM_CORE_REG(regs.pstate), &pstate) < 0) {
        fail(error, error_size, "set arm64 boot registers: %s", strerror(errno)); goto done;
    }
    int mapped_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mapped_size < (int)sizeof(struct kvm_run)) { fail(error, error_size, "invalid vCPU mmap size"); goto done; }
    run_size = (size_t)mapped_size;
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) { fail(error, error_size, "map kvm_run: %s", strerror(errno)); goto done; }

    struct sigaction action;
    memset(&action, 0, sizeof action); action.sa_handler = timeout_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, &old_action) < 0) { fail(error, error_size, "install timeout"); goto done; }
    signal_installed = true; boot_timed_out = 0; alarm(timeout_seconds);
    for (;;) {
        if (ioctl(vcpu, KVM_RUN, 0) < 0) {
            if (errno == EINTR && !boot_timed_out) continue;
            if (boot_timed_out) { fail(error, error_size, "guest timed out after %u seconds", timeout_seconds); goto done; }
            fail(error, error_size, "KVM_RUN: %s", strerror(errno)); goto done;
        }
        if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT) {
            if (!guest_ready) {
                fail(error, error_size, "guest reset before completion marker");
                goto done;
            }
            rc = 0;
            break;
        }
        if (run->exit_reason == KVM_EXIT_HLT) continue;
        if (run->exit_reason == KVM_EXIT_MMIO &&
            run->mmio.phys_addr >= UART_ADDR &&
            run->mmio.phys_addr < UART_ADDR + UINT64_C(0x1000)) {
            uint64_t offset = run->mmio.phys_addr - UART_ADDR;
            if (run->mmio.is_write && offset == 0 && run->mmio.len != 0) {
                (void)fputc((int)run->mmio.data[0], stdout);
                (void)fflush(stdout);
            } else if (!run->mmio.is_write) {
                memset(run->mmio.data, 0, sizeof run->mmio.data);
            }
            continue;
        }
        if (run->exit_reason == KVM_EXIT_MMIO && run->mmio.is_write &&
            run->mmio.phys_addr == COMPLETION_ADDR && run->mmio.len != 0) {
            for (uint32_t i = 0; i < run->mmio.len; ++i) {
                uint8_t byte = run->mmio.data[i];
                if (marker_cursor >= sizeof marker - 1U ||
                    byte != (uint8_t)marker[marker_cursor]) {
                    fail(error, error_size, "invalid guest completion marker");
                    goto done;
                }
                ++marker_cursor;
            }
            guest_ready = marker_cursor == sizeof marker - 1U;
            continue;
        }
        fail(error, error_size, "unexpected KVM exit reason=%u", run->exit_reason);
        goto done;
    }

done:
    alarm(0);
    if (signal_installed) (void)sigaction(SIGALRM, &old_action, NULL);
    if (run != MAP_FAILED) (void)munmap(run, run_size);
    if (vcpu >= 0) (void)close(vcpu);
    if (gic >= 0) (void)close(gic);
    if (memory != MAP_FAILED) (void)munmap(memory, (size_t)memory_mib * 1024U * 1024U);
    if (vm >= 0) (void)close(vm);
    if (kvm >= 0) (void)close(kvm);
    free(dtb); free(kernel);
    return rc;
}
