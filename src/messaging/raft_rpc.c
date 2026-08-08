#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

int
csilk_raft_rpc_encode_hdr(csilk_raft_msg_type_t msg_type,
                          uint64_t              term,
                          uint32_t              payload_len,
                          uint8_t*              out_buf,
                          size_t                out_cap)
{
    if (!out_buf || out_cap < sizeof(csilk_raft_hdr_t)) {
        return -1;
    }

    csilk_raft_hdr_t* hdr = (csilk_raft_hdr_t*)out_buf;
    hdr->magic = CSILK_RAFT_MAGIC;
    hdr->msg_type = (uint8_t)msg_type;
    hdr->payload_len = payload_len;
    hdr->term = term;

    return (int)sizeof(csilk_raft_hdr_t);
}

int
csilk_raft_rpc_decode_hdr(const uint8_t* buf, size_t len, csilk_raft_hdr_t* out_hdr)
{
    if (!buf || len < sizeof(csilk_raft_hdr_t) || !out_hdr) {
        return -1;
    }

    const csilk_raft_hdr_t* hdr = (const csilk_raft_hdr_t*)buf;
    if (hdr->magic != CSILK_RAFT_MAGIC) {
        return -1;
    }

    *out_hdr = *hdr;
    return 0;
}
