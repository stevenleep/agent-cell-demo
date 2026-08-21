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

#define BOOT_PARAMS_ADDR UINT64_C(0x7000)
#define CMDLINE_ADDR UINT64_C(0x20000)
#define GDT_ADDR UINT64_C(0x500)
#define KERNEL_ADDR UINT64_C(0x100000)
#define BOOT_PARAMS_SIZE 4096U
#define CMDLINE_MAX 2048U
#define MIN_GUEST_MIB 32U
#define UART_BASE 0x3f8U

static volatile sig_atomic_t timed_out;

static void timeout_handler(int signal_number) {
    (void)signal_number;
    timed_out = 1;
}

static void fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

static void wr64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

static int read_file(const char *path, uint8_t **data, size_t *size,
                     char *error, size_t error_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fail(error, error_size, "open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        fail(error, error_size, "stat %s: %s", path, strerror(errno)); close(fd); return -1;
    }
    if ((uintmax_t)st.st_size > SIZE_MAX) {
        fail(error, error_size, "%s is too large", path); close(fd); return -1;
    }
    *size = (size_t)st.st_size;
    *data = malloc(*size);
    if (*data == NULL) { fail(error, error_size, "allocate %zu bytes", *size); close(fd); return -1; }
    size_t done = 0;
    while (done < *size) {
        ssize_t count = read(fd, *data + done, *size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { fail(error, error_size, "read %s: %s", path, count < 0 ? strerror(errno) : "short read"); free(*data); *data = NULL; close(fd); return -1; }
        done += (size_t)count;
    }
    close(fd);
    return 0;
}

static void set_segment(struct kvm_segment *seg, uint16_t selector, uint8_t type) {
    memset(seg, 0, sizeof *seg);
    seg->base = 0; seg->limit = UINT32_MAX; seg->selector = selector;
    seg->type = type; seg->present = 1; seg->s = 1; seg->g = 1; seg->db = 1;
}

static void write_gdt(uint8_t *memory) {
    /* null, flat 32-bit code, flat 32-bit data */
    static const uint64_t gdt[] = {0, UINT64_C(0x00cf9b000000ffff),
                                   UINT64_C(0x00cf93000000ffff)};
    memcpy(memory + GDT_ADDR, gdt, sizeof gdt);
}

static void add_e820(uint8_t *params, unsigned index, uint64_t addr,
                     uint64_t size, uint32_t type) {
    uint8_t *entry = params + 0x2d0U + index * 20U;
    wr64(entry, addr); wr64(entry + 8, size); wr32(entry + 16, type);
}

static int install_cpuid(int kvm_fd, int vcpu_fd, char *error, size_t error_size) {
    const uint32_t count = 256;
    size_t bytes = sizeof(struct kvm_cpuid2) + count * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = calloc(1, bytes);
    if (cpuid == NULL) { fail(error, error_size, "allocate CPUID table"); return -1; }
    cpuid->nent = count;
    int rc = ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid);
    if (rc == 0) rc = ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid);
    if (rc < 0) fail(error, error_size, "configure KVM CPUID: %s", strerror(errno));
    free(cpuid);
    return rc < 0 ? -1 : 0;
}

int cell_kvm_boot_linux(const char *kernel_path, const char *initrd_path,
                        uint32_t memory_mib, uint32_t timeout_seconds,
                        char *error, size_t error_size) {
    int rc = -1, kvm = -1, vm = -1, vcpu = -1;
    uint8_t *kernel = NULL, *initrd = NULL, *memory = MAP_FAILED;
    size_t kernel_size = 0, initrd_size = 0, run_size = 0;
    struct kvm_run *run = MAP_FAILED;
    struct sigaction old_action;
    bool signal_installed = false;
    bool guest_ready = false;
    size_t marker_cursor = 0;
    static const char marker[] = "CELL_MVP_OK\n";

    if (kernel_path == NULL || memory_mib < MIN_GUEST_MIB || memory_mib > 512 ||
        timeout_seconds == 0 || timeout_seconds > 300) {
        fail(error, error_size, "invalid kernel, memory, or timeout argument"); return -1;
    }
    if (read_file(kernel_path, &kernel, &kernel_size, error, error_size) < 0) goto done;
    if (kernel_size < BOOT_PARAMS_SIZE || rd32(kernel + 0x202) != UINT32_C(0x53726448) ||
        rd16(kernel + 0x206) < UINT16_C(0x0202)) {
        fail(error, error_size, "kernel is not a bzImage with boot protocol >= 2.02"); goto done;
    }
    size_t setup_size = ((size_t)(kernel[0x1f1] == 0 ? 4 : kernel[0x1f1]) + 1) * 512U;
    if (setup_size >= kernel_size) { fail(error, error_size, "invalid bzImage setup size"); goto done; }
    size_t payload_size = kernel_size - setup_size;
    if (initrd_path != NULL && read_file(initrd_path, &initrd, &initrd_size, error, error_size) < 0) goto done;

    size_t memory_size = (size_t)memory_mib * 1024U * 1024U;
    if (KERNEL_ADDR + payload_size >= memory_size) {
        fail(error, error_size, "kernel does not fit in %u MiB", memory_mib); goto done;
    }
    size_t initrd_addr = 0;
    if (initrd_size != 0) {
        initrd_addr = (memory_size - initrd_size) & ~(size_t)(4096U - 1U);
        if (initrd_addr <= KERNEL_ADDR + payload_size || initrd_addr > UINT32_MAX) {
            fail(error, error_size, "initramfs does not fit in guest memory"); goto done;
        }
    }

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) { fail(error, error_size, "open /dev/kvm: %s", strerror(errno)); goto done; }
    if (ioctl(kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION) {
        fail(error, error_size, "unsupported KVM API version"); goto done;
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) { fail(error, error_size, "KVM_CREATE_VM: %s", strerror(errno)); goto done; }
    if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) < 0) {
        fail(error, error_size, "KVM_CREATE_IRQCHIP: %s", strerror(errno)); goto done;
    }
    struct kvm_pit_config pit = {0};
    if (ioctl(vm, KVM_CREATE_PIT2, &pit) < 0) {
        fail(error, error_size, "KVM_CREATE_PIT2: %s", strerror(errno)); goto done;
    }
    memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) { fail(error, error_size, "map guest RAM: %s", strerror(errno)); goto done; }
    (void)madvise(memory, memory_size, MADV_DONTDUMP);
    struct kvm_userspace_memory_region region = {
        .slot = 0, .guest_phys_addr = 0, .memory_size = memory_size,
        .userspace_addr = (uint64_t)(uintptr_t)memory
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        fail(error, error_size, "register guest RAM: %s", strerror(errno)); goto done;
    }

    uint8_t *params = memory + BOOT_PARAMS_ADDR;
    memset(params, 0, BOOT_PARAMS_SIZE);
    memcpy(params, kernel, BOOT_PARAMS_SIZE);
    params[0x210] = 0xff;              /* unknown bootloader */
    params[0x211] |= 0x80;             /* CAN_USE_HEAP */
    wr16(params + 0x224, 0xfe00);
    wr32(params + 0x214, (uint32_t)KERNEL_ADDR);
    wr32(params + 0x228, (uint32_t)CMDLINE_ADDR);
    params[0x1e8] = 2;
    add_e820(params, 0, 0, UINT64_C(0x9fc00), 1);
    add_e820(params, 1, KERNEL_ADDR, memory_size - KERNEL_ADDR, 1);
    static const char cmdline[] = "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=1 reboot=k pci=off acpi=off rdinit=/init";
    if (sizeof cmdline > CMDLINE_MAX) { fail(error, error_size, "internal command line too long"); goto done; }
    memcpy(memory + CMDLINE_ADDR, cmdline, sizeof cmdline);
    memcpy(memory + KERNEL_ADDR, kernel + setup_size, payload_size);
    if (initrd_size != 0) {
        memcpy(memory + initrd_addr, initrd, initrd_size);
        wr32(params + 0x218, (uint32_t)initrd_addr);
        wr32(params + 0x21c, (uint32_t)initrd_size);
    }
    write_gdt(memory);

    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) { fail(error, error_size, "KVM_CREATE_VCPU: %s", strerror(errno)); goto done; }
    if (install_cpuid(kvm, vcpu, error, error_size) < 0) goto done;
    int mapped_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mapped_size < (int)sizeof(struct kvm_run)) { fail(error, error_size, "invalid vCPU mmap size"); goto done; }
    run_size = (size_t)mapped_size;
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) { fail(error, error_size, "map kvm_run: %s", strerror(errno)); goto done; }

    struct kvm_sregs sregs;
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) { fail(error, error_size, "KVM_GET_SREGS: %s", strerror(errno)); goto done; }
    set_segment(&sregs.cs, 8, 11); set_segment(&sregs.ds, 16, 3);
    sregs.es = sregs.ds; sregs.fs = sregs.ds; sregs.gs = sregs.ds; sregs.ss = sregs.ds;
    sregs.gdt.base = GDT_ADDR; sregs.gdt.limit = 3U * 8U - 1U;
    sregs.cr0 |= 1U;
    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) { fail(error, error_size, "KVM_SET_SREGS: %s", strerror(errno)); goto done; }
    struct kvm_regs regs = {.rip = KERNEL_ADDR, .rsi = BOOT_PARAMS_ADDR,
                            .rsp = UINT64_C(0x8000), .rflags = 2};
    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) { fail(error, error_size, "KVM_SET_REGS: %s", strerror(errno)); goto done; }

    struct sigaction action;
    memset(&action, 0, sizeof action); action.sa_handler = timeout_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, &old_action) < 0) { fail(error, error_size, "install timeout: %s", strerror(errno)); goto done; }
    signal_installed = true; timed_out = 0; alarm(timeout_seconds);
    for (;;) {
        if (ioctl(vcpu, KVM_RUN, 0) < 0) {
            if (errno == EINTR && !timed_out) continue;
            if (timed_out) { fail(error, error_size, "guest timed out after %u seconds", timeout_seconds); goto done; }
            fail(error, error_size, "KVM_RUN: %s", strerror(errno)); goto done;
        }
        if (run->exit_reason == KVM_EXIT_SHUTDOWN) {
            if (!guest_ready) { fail(error, error_size, "guest shut down before MVP marker"); goto done; }
            rc = 0; break;
        }
#ifdef KVM_EXIT_SYSTEM_EVENT
        if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT) {
            if (!guest_ready) { fail(error, error_size, "guest reset before MVP marker"); goto done; }
            rc = 0; break;
        }
#endif
        /* HLT is also the kernel idle path; re-enter KVM and wait for an IRQ. */
        if (run->exit_reason == KVM_EXIT_HLT) continue;
        if (run->exit_reason != KVM_EXIT_IO) { fail(error, error_size, "unexpected KVM exit reason=%u", run->exit_reason); goto done; }
        uint64_t offset = run->io.data_offset;
        size_t bytes = (size_t)run->io.size * (size_t)run->io.count;
        if (offset > run_size || bytes > run_size - (size_t)offset) { fail(error, error_size, "invalid KVM I/O buffer"); goto done; }
        uint8_t *data = (uint8_t *)run + offset;
        if (run->io.direction == KVM_EXIT_IO_OUT &&
            (run->io.port == UART_BASE || run->io.port == 0xe9U)) {
            if (run->io.port == UART_BASE) {
                (void)fwrite(data, 1, bytes, stdout); (void)fflush(stdout);
            }
            for (size_t i = 0; i < bytes; ++i) {
                if (data[i] == (uint8_t)marker[marker_cursor]) {
                    if (++marker_cursor == sizeof marker - 1) {
                        guest_ready = true;
                        marker_cursor = 0;
                    }
                } else {
                    marker_cursor = data[i] == (uint8_t)marker[0] ? 1U : 0U;
                }
            }
        } else if (run->io.direction == KVM_EXIT_IO_OUT &&
                   (run->io.port == 0x64U || run->io.port == 0x604U) && guest_ready) {
            rc = 0;
            break;
        } else if (run->io.direction == KVM_EXIT_IO_IN) {
            memset(data, run->io.port == UART_BASE + 5 ? 0x60 : 0xff, bytes);
        }
    }

done:
    alarm(0);
    if (signal_installed) (void)sigaction(SIGALRM, &old_action, NULL);
    if (run != MAP_FAILED) munmap(run, run_size);
    if (vcpu >= 0) close(vcpu);
    if (memory != MAP_FAILED) munmap(memory, (size_t)memory_mib * 1024U * 1024U);
    if (vm >= 0) close(vm);
    if (kvm >= 0) close(kvm);
    free(initrd); free(kernel);
    return rc;
}
