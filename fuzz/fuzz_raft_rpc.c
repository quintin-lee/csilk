#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "messaging/raft_internal.h"

static void
_fuzz_decode_payload(uint8_t msg_type, const uint8_t* payload, size_t payload_len)
{
    switch (msg_type) {
    case CSILK_RAFT_MSG_REQUEST_VOTE_REQ: {
        csilk_raft_rv_req_t req;
        _raft_rpc_decode_rv_req(payload, payload_len, &req);
        break;
    }
    case CSILK_RAFT_MSG_REQUEST_VOTE_RESP: {
        csilk_raft_rv_resp_t resp;
        _raft_rpc_decode_rv_resp(payload, payload_len, &resp);
        break;
    }
    case CSILK_RAFT_MSG_APPEND_ENTRIES_REQ: {
        csilk_raft_ae_req_t     req;
        csilk_raft_log_entry_t* entries = nullptr;
        size_t                  count = 0;
        if (_raft_rpc_decode_ae_req(payload, payload_len, &req, &entries, &count) == 0) {
            for (size_t i = 0; i < count; i++) {
                free(entries[i].data);
            }
            free(entries);
        }
        break;
    }
    case CSILK_RAFT_MSG_APPEND_ENTRIES_RESP: {
        csilk_raft_ae_resp_t resp;
        _raft_rpc_decode_ae_resp(payload, payload_len, &resp);
        break;
    }
    case CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ: {
        csilk_raft_is_req_t req;
        const uint8_t*      chunk = nullptr;
        _raft_rpc_decode_is_req(payload, payload_len, &req, &chunk);
        break;
    }
    case CSILK_RAFT_MSG_INSTALL_SNAPSHOT_RESP: {
        csilk_raft_is_resp_t resp;
        _raft_rpc_decode_is_resp(payload, payload_len, &resp);
        break;
    }
    default:
        break;
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) {
        return 0;
    }

    csilk_raft_hdr_t hdr;
    if (csilk_raft_rpc_decode_hdr(data, size, &hdr) == 0 && size >= sizeof(csilk_raft_hdr_t)) {
        _fuzz_decode_payload(
            hdr.msg_type, data + sizeof(csilk_raft_hdr_t), size - sizeof(csilk_raft_hdr_t));
    }

    /* Also directly exercise each payload decoder without needing the 4-byte magic */
    uint8_t tag = (uint8_t)((data[0] % 6) + 1);
    _fuzz_decode_payload(tag, data + 1, size - 1);

    return 0;
}
