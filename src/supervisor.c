#include "cell.h"

#include <limits.h>
#include <string.h>

int cell_dry_run(const struct cell_request *request, struct cell_reply *reply,
                 char *error, size_t error_size) {
    static uint64_t next_id = 1;
    if (cell_request_validate(request, error, error_size) != 0) return -1;

    size_t payload_len = request->stdin_len;
    for (size_t i = 0; i < request->argc; ++i)
        payload_len += strlen(request->argv[i]) + 1;
    if (payload_len > UINT32_MAX) return -1;

    uint8_t wire[24];
    uint64_t decoded_id;
    uint32_t decoded_len;
    cell_header_encode(wire, next_id++, (uint32_t)payload_len);
    if (cell_header_decode(wire, &decoded_id, &decoded_len) != 0 ||
        decoded_len != payload_len) return -1;

    reply->request_id = decoded_id;
    reply->exit_code = 0;
    reply->state = "protocol-ok";
    reply->note = "dry-run only; no command executed and no isolation provided";
    return 0;
}
