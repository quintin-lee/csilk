/**
 * @file raft_rpc.c
 * @brief Raft RPC wire framing — header encode/decode.
 *
 * Implements the on-wire header (csilk_raft_hdr_t) serialization used by Raft
 * RPCs.  Headers carry a "RAFT" magic, message type, payload length, and term,
 * stored in a fixed packed struct layout.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raft_internal.h"

/**
 * @brief Encode a Raft RPC header into a buffer.
 *
 * Writes the magic, message type, payload length, and term into @p out_buf as
 * a csilk_raft_hdr_t.  The caller supplies the buffer and capacity; the
 * function validates that the buffer is large enough.
 *
 * @param[in]  msg_type    Raft message type (vote/append/snapshot).
 * @param[in]  term        Current term to embed.
 * @param[in]  payload_len Length of the RPC payload in bytes.
 * @param[out] out_buf     Destination buffer (must hold sizeof(header)).
 * @param[in]  out_cap     Capacity of @p out_buf in bytes.
 * @return sizeof(csilk_raft_hdr_t) on success, -1 on NULL/undersized buffer.
 */
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

/**
 * @brief Decode a Raft RPC header from a buffer.
 *
 * Validates the buffer length and the "RAFT" magic, then copies the header
 * fields into @p out_hdr.  Rejects truncated or magic-mismatched frames.
 *
 * @param[in]  buf     Source buffer holding the encoded header.
 * @param[in]  len     Length of @p buf in bytes.
 * @param[out] out_hdr Destination header struct to populate.
 * @return 0 on success, -1 on NULL/short buffer or magic mismatch.
 */
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
