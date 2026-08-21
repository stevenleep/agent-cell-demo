#include "cell.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(struct cell_mailbox_header) == 24,
               "mailbox header ABI must be 24 bytes");

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

int cell_request_validate(const struct cell_request *r, char *error,
                          size_t error_size) {
    if (r == NULL || r->argv == NULL || r->argc == 0) {
        snprintf(error, error_size, "argv must not be empty");
        return -1;
    }
    if (r->memory_mib < 16 || r->memory_mib > 512) {
        snprintf(error, error_size, "memory must be in 16..512 MiB");
        return -1;
    }
    if (r->timeout_ms == 0 || r->timeout_ms > 300000) {
        snprintf(error, error_size, "timeout must be in 1..300000 ms");
        return -1;
    }
    if (r->stdin_len > CELL_MAX_STDIN_BYTES) {
        snprintf(error, error_size, "stdin is too large");
        return -1;
    }
    size_t total = 0;
    for (size_t i = 0; i < r->argc; ++i) {
        if (r->argv[i] == NULL) {
            snprintf(error, error_size, "argv contains null pointer");
            return -1;
        }
        size_t len = strlen(r->argv[i]);
        if (len >= CELL_MAX_ARGV_BYTES || total > CELL_MAX_ARGV_BYTES - len - 1) {
            snprintf(error, error_size, "argv is too large");
            return -1;
        }
        total += len + 1;
    }
    return 0;
}
void cell_header_encode(uint8_t out[24], uint64_t id, uint32_t len) {
    put_u32(out, CELL_MAGIC);
    put_u16(out + 4, CELL_VERSION);
    put_u16(out + 6, CELL_OP_EXEC);
    put_u64(out + 8, id);
    put_u32(out + 16, len);
    put_u32(out + 20, 0);
}

int cell_header_decode(const uint8_t in[24], uint64_t *id, uint32_t *len) {
    if (get_u32(in) != CELL_MAGIC || get_u16(in + 4) != CELL_VERSION ||
        get_u16(in + 6) != CELL_OP_EXEC || get_u32(in + 20) != 0)
        return -1;
    *id = get_u64(in + 8);
    *len = get_u32(in + 16);
    return 0;
}
