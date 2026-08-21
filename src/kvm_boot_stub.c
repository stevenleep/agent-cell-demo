#include "cell.h"

#include <stdio.h>

int cell_kvm_boot_linux(const char *kernel_path, const char *initrd_path,
                        uint32_t memory_mib, uint32_t timeout_seconds,
                        char *error, size_t error_size) {
    (void)kernel_path; (void)initrd_path; (void)memory_mib; (void)timeout_seconds;
    snprintf(error, error_size,
             "Linux direct-boot backend requires Linux x86_64 with /dev/kvm");
    return -1;
}
