#define _GNU_SOURCE
#include "cell.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_RAM_BASE UINT64_C(0x40000000)
#define GUEST_RAM_SIZE 4096U
#define OUTPUT_MMIO UINT64_C(0x1000)

static int set_one_reg(int vcpu, uint64_t id, uint64_t *value) {
    struct kvm_one_reg reg = {.id = id, .addr = (uint64_t)(uintptr_t)value};
    return ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}

static size_t build_guest(uint32_t *code, size_t capacity) {
    static const char message[] = "cell:kvm:ok\n";
    size_t cursor = 0;
    if (capacity < 2U + (sizeof message - 1U) * 2U) return 0;
    code[cursor++] = UINT32_C(0xd2820001); /* movz x1, #0x1000 */
    for (size_t i = 0; i < sizeof message - 1U; ++i) {
        code[cursor++] = UINT32_C(0x52800000) |
                         ((uint32_t)(uint8_t)message[i] << 5); /* movz w0, #c */
        code[cursor++] = UINT32_C(0x39000020); /* strb w0, [x1] */
    }
    code[cursor++] = UINT32_C(0x14000000); /* b . */
    return cursor;
}

int cell_kvm_smoke(char *guest_output, size_t output_size, char *error,
                   size_t error_size) {
    int result = -1, kvm = -1, vm = -1, vcpu = -1;
    uint8_t *memory = MAP_FAILED;
    struct kvm_run *run = MAP_FAILED;
    size_t run_size = 0, output_len = 0;
    static const char expected[] = "cell:kvm:ok\n";

    if (guest_output == NULL || output_size < sizeof expected ||
        error == NULL || error_size == 0) return -1;
    guest_output[0] = '\0';
    error[0] = '\0';

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) { snprintf(error, error_size, "open /dev/kvm: %s", strerror(errno)); goto done; }
    if (ioctl(kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION) {
        snprintf(error, error_size, "unsupported KVM API version"); goto done;
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) { snprintf(error, error_size, "KVM_CREATE_VM: %s", strerror(errno)); goto done; }
    memory = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { snprintf(error, error_size, "map guest RAM: %s", strerror(errno)); goto done; }
    if (build_guest((uint32_t *)memory, GUEST_RAM_SIZE / sizeof(uint32_t)) == 0) {
        snprintf(error, error_size, "guest program is too large"); goto done;
    }
    struct kvm_userspace_memory_region region = {
        .slot = 0, .guest_phys_addr = GUEST_RAM_BASE,
        .memory_size = GUEST_RAM_SIZE,
        .userspace_addr = (uint64_t)(uintptr_t)memory
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        snprintf(error, error_size, "register guest RAM: %s", strerror(errno)); goto done;
    }
    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) { snprintf(error, error_size, "KVM_CREATE_VCPU: %s", strerror(errno)); goto done; }
    struct kvm_vcpu_init init;
    memset(&init, 0, sizeof init);
    if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0 ||
        ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
        snprintf(error, error_size, "initialize arm64 vCPU: %s", strerror(errno)); goto done;
    }
    uint64_t pc = GUEST_RAM_BASE;
    uint64_t pstate = UINT64_C(0x3c5); /* EL1h with D/A/I/F masked */
    if (set_one_reg(vcpu, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                    KVM_REG_ARM_CORE_REG(regs.pc), &pc) < 0 ||
        set_one_reg(vcpu, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                    KVM_REG_ARM_CORE_REG(regs.pstate), &pstate) < 0) {
        snprintf(error, error_size, "set arm64 registers: %s", strerror(errno)); goto done;
    }
    int mapped_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mapped_size < (int)sizeof(struct kvm_run)) {
        snprintf(error, error_size, "invalid vCPU mmap size"); goto done;
    }
    run_size = (size_t)mapped_size;
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) { snprintf(error, error_size, "map kvm_run: %s", strerror(errno)); goto done; }

    for (;;) {
        if (ioctl(vcpu, KVM_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            snprintf(error, error_size, "KVM_RUN: %s", strerror(errno)); goto done;
        }
        if (run->exit_reason != KVM_EXIT_MMIO || !run->mmio.is_write ||
            run->mmio.phys_addr != OUTPUT_MMIO || run->mmio.len != 1) {
            snprintf(error, error_size, "unexpected KVM exit reason=%u", run->exit_reason);
            goto done;
        }
        char byte = (char)run->mmio.data[0];
        if (output_len + 1U >= output_size || byte != expected[output_len]) {
            snprintf(error, error_size, "invalid guest output at byte %zu", output_len);
            goto done;
        }
        guest_output[output_len++] = byte;
        guest_output[output_len] = '\0';
        if (output_len == sizeof expected - 1U) { result = 0; break; }
    }

done:
    if (run != MAP_FAILED) (void)munmap(run, run_size);
    if (vcpu >= 0) (void)close(vcpu);
    if (memory != MAP_FAILED) (void)munmap(memory, GUEST_RAM_SIZE);
    if (vm >= 0) (void)close(vm);
    if (kvm >= 0) (void)close(kvm);
    return result;
}
