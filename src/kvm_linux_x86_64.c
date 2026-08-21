#define _GNU_SOURCE
#include "cell.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_MEMORY_SIZE 4096U
#define DEBUG_PORT 0xe9U

static void set_error(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

/* Real-mode guest: OUT each byte to port 0xe9, then HLT. */
static size_t build_guest(uint8_t *memory, size_t capacity) {
    static const char message[] = "cell:kvm:ok\n";
    size_t cursor = 0;
    for (size_t i = 0; i < sizeof message - 1; ++i) {
        if (capacity - cursor < 4) return 0;
        memory[cursor++] = 0xb0; /* mov al, imm8 */
        memory[cursor++] = (uint8_t)message[i];
        memory[cursor++] = 0xe6; /* out imm8, al */
        memory[cursor++] = (uint8_t)DEBUG_PORT;
    }
    if (cursor == capacity) return 0;
    memory[cursor++] = 0xf4; /* hlt */
    return cursor;
}

int cell_kvm_smoke(char *guest_output, size_t output_size, char *error,
                   size_t error_size) {
    int result = -1, kvm_fd = -1, vm_fd = -1, vcpu_fd = -1;
    uint8_t *memory = MAP_FAILED;
    struct kvm_run *run = MAP_FAILED;
    size_t run_size = 0, output_len = 0;

    if (guest_output == NULL || output_size == 0 || error == NULL ||
        error_size == 0) return -1;
    guest_output[0] = '\0';
    error[0] = '\0';

    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0) {
        set_error(error, error_size, "open /dev/kvm: %s", strerror(errno));
        goto done;
    }
    int api_version = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api_version != KVM_API_VERSION) {
        set_error(error, error_size, "KVM API version %d, expected %d",
                  api_version, KVM_API_VERSION);
        goto done;
    }
    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) {
        set_error(error, error_size, "KVM_CREATE_VM: %s", strerror(errno));
        goto done;
    }
    memory = mmap(NULL, GUEST_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        set_error(error, error_size, "map guest memory: %s", strerror(errno));
        goto done;
    }
    (void)madvise(memory, GUEST_MEMORY_SIZE, MADV_DONTDUMP);
    if (build_guest(memory, GUEST_MEMORY_SIZE) == 0) {
        set_error(error, error_size, "guest program does not fit in memory");
        goto done;
    }
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = GUEST_MEMORY_SIZE,
        .userspace_addr = (uint64_t)(uintptr_t)memory
    };
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        set_error(error, error_size, "KVM_SET_USER_MEMORY_REGION: %s",
                  strerror(errno));
        goto done;
    }
    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) {
        set_error(error, error_size, "KVM_CREATE_VCPU: %s", strerror(errno));
        goto done;
    }
    int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < (int)sizeof(struct kvm_run)) {
        set_error(error, error_size, "invalid KVM vCPU mmap size: %d", mmap_size);
        goto done;
    }
    run_size = (size_t)mmap_size;
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED) {
        set_error(error, error_size, "map kvm_run: %s", strerror(errno));
        goto done;
    }

    struct kvm_sregs sregs;
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        set_error(error, error_size, "KVM_GET_SREGS: %s", strerror(errno));
        goto done;
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        set_error(error, error_size, "KVM_SET_SREGS: %s", strerror(errno));
        goto done;
    }
    struct kvm_regs regs = {.rip = 0, .rflags = 2};
    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        set_error(error, error_size, "KVM_SET_REGS: %s", strerror(errno));
        goto done;
    }

    for (;;) {
        if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR) continue;
            set_error(error, error_size, "KVM_RUN: %s", strerror(errno));
            goto done;
        }
        if (run->exit_reason == KVM_EXIT_HLT) break;
        if (run->exit_reason != KVM_EXIT_IO ||
            run->io.direction != KVM_EXIT_IO_OUT ||
            run->io.port != DEBUG_PORT || run->io.size != 1) {
            set_error(error, error_size, "unexpected KVM exit reason=%u",
                      run->exit_reason);
            goto done;
        }
        size_t bytes = (size_t)run->io.count;
        if (run->io.data_offset > run_size || bytes > run_size - run->io.data_offset ||
            bytes > output_size - output_len - 1) {
            set_error(error, error_size, "invalid or oversized guest output");
            goto done;
        }
        memcpy(guest_output + output_len,
               (const uint8_t *)run + run->io.data_offset, bytes);
        output_len += bytes;
        guest_output[output_len] = '\0';
    }
    result = 0;

done:
    if (run != MAP_FAILED) (void)munmap(run, run_size);
    close_fd(&vcpu_fd);
    if (memory != MAP_FAILED) (void)munmap(memory, GUEST_MEMORY_SIZE);
    close_fd(&vm_fd);
    close_fd(&kvm_fd);
    return result;
}
