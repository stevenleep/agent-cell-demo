#include "cell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) return -1;
    *out = (uint32_t)value;
    return 0;
}

static int selftest(void) {
    uint8_t wire[24];
    uint64_t id = 0;
    uint32_t len = 0;
    cell_header_encode(wire, UINT64_C(42), UINT32_C(99));
    if (cell_header_decode(wire, &id, &len) != 0 || id != 42 || len != 99)
        return 1;
    wire[0] ^= 1;
    if (cell_header_decode(wire, &id, &len) == 0) return 1;

    char *argv[] = {"/bin/true"};
    struct cell_request request = {
        .argv = argv, .argc = 1, .memory_mib = 16, .timeout_ms = 1
    };
    char error[128];
    if (cell_request_validate(&request, error, sizeof error) != 0) return 1;
    request.argc = 0;
    if (cell_request_validate(&request, error, sizeof error) == 0) return 1;
    puts("selftest=ok");
    return 0;
}

static int run(int argc, char **argv) {
    uint32_t memory_mib = 32, timeout_ms = 1000;
    int i = 0;
    while (i < argc && strcmp(argv[i], "--") != 0) {
        uint32_t *target = NULL;
        if (strcmp(argv[i], "--memory-mib") == 0) target = &memory_mib;
        else if (strcmp(argv[i], "--timeout-ms") == 0) target = &timeout_ms;
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        if (++i >= argc || parse_u32(argv[i], target) != 0) {
            fputs("invalid option value\n", stderr); return 2;
        }
        ++i;
    }
    if (i >= argc || ++i >= argc) {
        fputs("missing -- PROGRAM\n", stderr); return 2;
    }
    struct cell_request request = {
        .argv = &argv[i], .argc = (size_t)(argc - i),
        .memory_mib = memory_mib, .timeout_ms = timeout_ms
    };
    struct cell_reply reply;
    char error[128];
    if (cell_dry_run(&request, &reply, error, sizeof error) != 0) {
        fprintf(stderr, "error: %s\n", error); return 1;
    }
    printf("request_id=%llu state=%s exit_code=%d\nnote: %s\n",
           (unsigned long long)reply.request_id, reply.state,
           reply.exit_code, reply.note);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) return selftest();
    if (argc == 2 && strcmp(argv[1], "kvm-smoke") == 0) {
        char output[128], error[256];
        if (cell_kvm_smoke(output, sizeof output, error, sizeof error) != 0) {
            fprintf(stderr, "kvm-smoke: %s\n", error);
            return 1;
        }
        printf("kvm-smoke=ok guest-output=%s", output);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "kvm-boot") == 0) {
        char error[256];
        const char *initrd = argc == 4 ? argv[3] : NULL;
        if (cell_kvm_boot_linux(argv[2], initrd, 128, 10,
                                error, sizeof error) != 0) {
            fprintf(stderr, "kvm-boot: %s\n", error);
            return 1;
        }
        puts("kvm-boot=ok");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0) return run(argc - 2, argv + 2);
    fprintf(stderr, "usage: %s selftest | kvm-smoke | kvm-boot BZIMAGE [INITRAMFS] | run [options] -- PROGRAM [ARG...]\n", argv[0]);
    return 2;
}
