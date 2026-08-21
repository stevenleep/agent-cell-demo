#include "cell.h"

#include <stdio.h>

int cell_kvm_smoke(char *guest_output, size_t output_size, char *error,
                   size_t error_size) {
    (void)guest_output;
    (void)output_size;
    snprintf(error, error_size,
             "KVM smoke backend currently requires Linux x86_64 with /dev/kvm");
    return -1;
}
