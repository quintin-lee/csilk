/**
 * @file raft_rpc.c
 * @brief Raft RPC wire framing and payload serialization.
 *
 * Implements the on-wire header (csilk_raft_hdr_t) and RPC message codecs
 * for RequestVote, AppendEntries, and InstallSnapshot.
 * @copyright MIT License
 */

#include <limits.h>
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

    csilk_raft_hdr_t hdr;
    hdr.magic = CSILK_RAFT_MAGIC;
    hdr.msg_type = (uint8_t)msg_type;
    hdr.payload_len = payload_len;
    hdr.term = term;
    memcpy(out_buf, &hdr, sizeof(hdr));

    return (int)sizeof(csilk_raft_hdr_t);
}

int
csilk_raft_rpc_decode_hdr(const uint8_t* buf, size_t len, csilk_raft_hdr_t* out_hdr)
{
    if (!buf || len < sizeof(csilk_raft_hdr_t) || !out_hdr) {
        return -1;
    }

    csilk_raft_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != CSILK_RAFT_MAGIC) {
        return -1;
    }

    *out_hdr = hdr;
    return 0;
}

int
_raft_rpc_encode_rv_req(const csilk_raft_rv_req_t* req, uint8_t* out_buf, size_t out_cap)
{
    if (!req || !out_buf || out_cap < sizeof(csilk_raft_rv_req_t)) {
        return -1;
    }
    memcpy(out_buf, req, sizeof(*req));
    return (int)sizeof(*req);
}

int
_raft_rpc_decode_rv_req(const uint8_t* buf, size_t len, csilk_raft_rv_req_t* out_req)
{
    if (!buf || !out_req || len < sizeof(csilk_raft_rv_req_t)) {
        return -1;
    }
    memcpy(out_req, buf, sizeof(*out_req));
    out_req->candidate_str[sizeof(out_req->candidate_str) - 1] = '\0';
    return 0;
}

int
_raft_rpc_encode_rv_resp(const csilk_raft_rv_resp_t* resp, uint8_t* out_buf, size_t out_cap)
{
    if (!resp || !out_buf || out_cap < sizeof(csilk_raft_rv_resp_t)) {
        return -1;
    }
    memcpy(out_buf, resp, sizeof(*resp));
    return (int)sizeof(*resp);
}

int
_raft_rpc_decode_rv_resp(const uint8_t* buf, size_t len, csilk_raft_rv_resp_t* out_resp)
{
    if (!buf || !out_resp || len < sizeof(csilk_raft_rv_resp_t)) {
        return -1;
    }
    memcpy(out_resp, buf, sizeof(*out_resp));
    return 0;
}

int
_raft_rpc_encode_ae_req(const csilk_raft_ae_req_t*    req,
                        const csilk_raft_log_entry_t* entries,
                        size_t                        entry_count,
                        uint8_t*                      out_buf,
                        size_t                        out_cap)
{
    if (!req || !out_buf || out_cap < sizeof(csilk_raft_ae_req_t)) {
        return -1;
    }
    if (entry_count > 0 && !entries) {
        return -1;
    }

    csilk_raft_ae_req_t wire_req = *req;
    wire_req.entry_count = (uint32_t)entry_count;
    memcpy(out_buf, &wire_req, sizeof(wire_req));
    size_t pos = sizeof(wire_req);

    for (size_t i = 0; i < entry_count; i++) {
        uint64_t idx = entries[i].index;
        uint64_t term = entries[i].term;
        if (entries[i].len > UINT32_MAX) {
            return -1;
        }
        uint32_t data_len = (uint32_t)entries[i].len;
        size_t   ent_hdr_sz = sizeof(idx) + sizeof(term) + sizeof(data_len);
        if (pos + ent_hdr_sz + data_len > out_cap) {
            return -1;
        }
        if (data_len > 0 && !entries[i].data) {
            return -1;
        }

        memcpy(out_buf + pos, &idx, sizeof(idx));
        pos += sizeof(idx);
        memcpy(out_buf + pos, &term, sizeof(term));
        pos += sizeof(term);
        memcpy(out_buf + pos, &data_len, sizeof(data_len));
        pos += sizeof(data_len);
        if (data_len > 0) {
            memcpy(out_buf + pos, entries[i].data, data_len);
            pos += data_len;
        }
    }

    if (pos > (size_t)INT_MAX) {
        return -1;
    }
    return (int)pos;
}

int
_raft_rpc_decode_ae_req(const uint8_t*           buf,
                        size_t                   len,
                        csilk_raft_ae_req_t*     out_req,
                        csilk_raft_log_entry_t** out_entries,
                        size_t*                  out_count)
{
    if (!buf || !out_req || !out_entries || !out_count || len < sizeof(csilk_raft_ae_req_t)) {
        return -1;
    }

    *out_entries = NULL;
    *out_count = 0;

    csilk_raft_ae_req_t req;
    memcpy(&req, buf, sizeof(req));
    size_t pos = sizeof(req);

    const size_t min_ent_sz = sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);
    if ((size_t)req.entry_count > (len - pos) / min_ent_sz) {
        return -1;
    }

    csilk_raft_log_entry_t* list = NULL;
    if (req.entry_count > 0) {
        list = calloc(req.entry_count, sizeof(csilk_raft_log_entry_t));
        if (!list) {
            return -1;
        }
    }

    for (uint32_t i = 0; i < req.entry_count; i++) {
        if (pos + min_ent_sz > len) {
            goto fail;
        }
        uint64_t idx = 0;
        uint64_t term = 0;
        uint32_t data_len = 0;
        memcpy(&idx, buf + pos, sizeof(idx));
        pos += sizeof(idx);
        memcpy(&term, buf + pos, sizeof(term));
        pos += sizeof(term);
        memcpy(&data_len, buf + pos, sizeof(data_len));
        pos += sizeof(data_len);

        if ((size_t)data_len > len - pos) {
            goto fail;
        }

        uint8_t* copy = NULL;
        if (data_len > 0) {
            copy = malloc(data_len);
            if (!copy) {
                goto fail;
            }
            memcpy(copy, buf + pos, data_len);
            pos += data_len;
        }

        list[i].index = idx;
        list[i].term = term;
        list[i].data = copy;
        list[i].len = data_len;
        list[i].wal_offset = 0;
    }

    *out_req = req;
    *out_entries = list;
    *out_count = req.entry_count;
    return 0;

fail:
    if (list) {
        for (uint32_t j = 0; j < req.entry_count; j++) {
            free(list[j].data);
        }
        free(list);
    }
    return -1;
}

int
_raft_rpc_encode_ae_resp(const csilk_raft_ae_resp_t* resp, uint8_t* out_buf, size_t out_cap)
{
    if (!resp || !out_buf || out_cap < sizeof(csilk_raft_ae_resp_t)) {
        return -1;
    }
    memcpy(out_buf, resp, sizeof(*resp));
    return (int)sizeof(*resp);
}

int
_raft_rpc_decode_ae_resp(const uint8_t* buf, size_t len, csilk_raft_ae_resp_t* out_resp)
{
    if (!buf || !out_resp || len < sizeof(csilk_raft_ae_resp_t)) {
        return -1;
    }
    memcpy(out_resp, buf, sizeof(*out_resp));
    return 0;
}

int
_raft_rpc_encode_is_req(const csilk_raft_is_req_t* req,
                        const uint8_t*             chunk,
                        uint8_t*                   out_buf,
                        size_t                     out_cap)
{
    if (!req || !out_buf || out_cap < sizeof(csilk_raft_is_req_t)) {
        return -1;
    }
    if (req->chunk_len > out_cap - sizeof(csilk_raft_is_req_t)) {
        return -1;
    }
    if (req->chunk_len > 0 && !chunk) {
        return -1;
    }

    memcpy(out_buf, req, sizeof(*req));
    if (req->chunk_len > 0) {
        memcpy(out_buf + sizeof(*req), chunk, req->chunk_len);
    }

    size_t total = sizeof(*req) + (size_t)req->chunk_len;
    if (total > (size_t)INT_MAX) {
        return -1;
    }
    return (int)total;
}

int
_raft_rpc_decode_is_req(const uint8_t*       buf,
                        size_t               len,
                        csilk_raft_is_req_t* out_req,
                        const uint8_t**      out_chunk)
{
    if (!buf || !out_req || len < sizeof(csilk_raft_is_req_t)) {
        return -1;
    }

    csilk_raft_is_req_t req;
    memcpy(&req, buf, sizeof(req));
    if ((size_t)req.chunk_len > len - sizeof(req)) {
        return -1;
    }

    *out_req = req;
    if (out_chunk) {
        *out_chunk = (req.chunk_len > 0) ? (buf + sizeof(req)) : NULL;
    }
    return 0;
}

int
_raft_rpc_encode_is_resp(const csilk_raft_is_resp_t* resp, uint8_t* out_buf, size_t out_cap)
{
    if (!resp || !out_buf || out_cap < sizeof(csilk_raft_is_resp_t)) {
        return -1;
    }
    memcpy(out_buf, resp, sizeof(*resp));
    return (int)sizeof(*resp);
}

int
_raft_rpc_decode_is_resp(const uint8_t* buf, size_t len, csilk_raft_is_resp_t* out_resp)
{
    if (!buf || !out_resp || len < sizeof(csilk_raft_is_resp_t)) {
        return -1;
    }
    memcpy(out_resp, buf, sizeof(*out_resp));
    return 0;
}
