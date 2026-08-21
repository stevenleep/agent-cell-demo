#ifndef AGENT_CELL_H
#define AGENT_CELL_H

#include <stddef.h>
#include <stdint.h>

#define CELL_MAGIC UINT32_C(0x43454c4c)
#define CELL_VERSION UINT16_C(1)
#define CELL_MAX_ARGV_BYTES (64U * 1024U)
#define CELL_MAX_STDIN_BYTES (1024U * 1024U)

enum cell_opcode {
    CELL_OP_EXEC = 1
};

struct cell_mailbox_header {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint64_t request_id;
    uint32_t payload_len;
    int32_t status;
};

struct cell_request {
    char **argv;
    size_t argc;
    const uint8_t *stdin_data;
    size_t stdin_len;
    uint32_t memory_mib;
    uint32_t timeout_ms;
};

struct cell_reply {
    uint64_t request_id;
    int32_t exit_code;
    const char *state;
    const char *note;
};

int cell_request_validate(const struct cell_request *request, char *error,
                          size_t error_size);
void cell_header_encode(uint8_t out[24], uint64_t request_id,
                        uint32_t payload_len);
int cell_header_decode(const uint8_t in[24], uint64_t *request_id,
                       uint32_t *payload_len);
int cell_dry_run(const struct cell_request *request, struct cell_reply *reply,
                 char *error, size_t error_size);
int cell_kvm_smoke(char *guest_output, size_t output_size, char *error,
                   size_t error_size);
int cell_kvm_boot_linux(const char *kernel_path, const char *initrd_path,
                        uint32_t memory_mib, uint32_t timeout_seconds,
                        char *error, size_t error_size);

#endif
