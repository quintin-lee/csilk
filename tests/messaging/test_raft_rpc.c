#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "messaging/raft_internal.h"

static void
test_raft_rpc_framing(void)
{
    uint8_t buf[128];
    int res = csilk_raft_rpc_encode_hdr(CSILK_RAFT_MSG_REQUEST_VOTE_REQ, 42, 100, buf, sizeof(buf));
    assert(res == (int)sizeof(csilk_raft_hdr_t));

    csilk_raft_hdr_t decoded;
    res = csilk_raft_rpc_decode_hdr(buf, sizeof(buf), &decoded);
    assert(res == 0);
    assert(decoded.magic == CSILK_RAFT_MAGIC);
    assert(decoded.msg_type == CSILK_RAFT_MSG_REQUEST_VOTE_REQ);
    assert(decoded.term == 42);
    assert(decoded.payload_len == 100);

    buf[0] ^= 0xFF;
    assert(csilk_raft_rpc_decode_hdr(buf, sizeof(buf), &decoded) == -1);
    printf("test_raft_rpc_framing passed\n");
}

static void
test_raft_rpc_payload_codecs(void)
{
    uint8_t buf[4096];

    /* RequestVote req/resp */
    csilk_raft_rv_req_t rv_in = {
        .term = 7, .candidate_id = 3, .last_log_index = 12, .last_log_term = 6};
    snprintf(rv_in.candidate_str, sizeof(rv_in.candidate_str), "node-3");
    int n = _raft_rpc_encode_rv_req(&rv_in, buf, sizeof(buf));
    assert(n > 0);
    csilk_raft_rv_req_t rv_out;
    assert(_raft_rpc_decode_rv_req(buf, (size_t)n, &rv_out) == 0);
    assert(rv_out.term == 7 && rv_out.candidate_id == 3 && rv_out.last_log_index == 12);
    assert(strcmp(rv_out.candidate_str, "node-3") == 0);

    csilk_raft_rv_resp_t rvr_in = {.term = 7, .voter_id = 2, .vote_granted = 1};
    n = _raft_rpc_encode_rv_resp(&rvr_in, buf, sizeof(buf));
    assert(n > 0);
    csilk_raft_rv_resp_t rvr_out;
    assert(_raft_rpc_decode_rv_resp(buf, (size_t)n, &rvr_out) == 0);
    assert(rvr_out.term == 7 && rvr_out.voter_id == 2 && rvr_out.vote_granted == 1);

    /* AppendEntries req/resp with entries */
    csilk_raft_log_entry_t ents[2];
    memset(ents, 0, sizeof(ents));
    ents[0].index = 10;
    ents[0].term = 5;
    ents[0].data = (uint8_t*)"cmd-one";
    ents[0].len = 7;
    ents[1].index = 11;
    ents[1].term = 6;
    ents[1].data = (uint8_t*)"cmd-two-payload";
    ents[1].len = 15;

    csilk_raft_ae_req_t ae_in = {
        .term = 6, .leader_id = 1, .prev_log_index = 9, .prev_log_term = 5, .leader_commit = 8};
    n = _raft_rpc_encode_ae_req(&ae_in, ents, 2, buf, sizeof(buf));
    assert(n > 0);

    csilk_raft_ae_req_t     ae_out;
    csilk_raft_log_entry_t* dec_ents = NULL;
    size_t                  dec_cnt = 0;
    assert(_raft_rpc_decode_ae_req(buf, (size_t)n, &ae_out, &dec_ents, &dec_cnt) == 0);
    assert(dec_cnt == 2);
    assert(ae_out.prev_log_index == 9 && ae_out.leader_commit == 8);
    assert(dec_ents[0].index == 10 && dec_ents[0].len == 7 &&
           memcmp(dec_ents[0].data, "cmd-one", 7) == 0);
    assert(dec_ents[1].index == 11 && dec_ents[1].len == 15 &&
           memcmp(dec_ents[1].data, "cmd-two-payload", 15) == 0);
    for (size_t i = 0; i < dec_cnt; i++) {
        free(dec_ents[i].data);
    }
    free(dec_ents);

    /* Truncated AppendEntries must fail cleanly without leaks */
    assert(_raft_rpc_decode_ae_req(buf, (size_t)n - 3U, &ae_out, &dec_ents, &dec_cnt) == -1);

    /* InstallSnapshot req/resp */
    const uint8_t       snap_chunk[] = {1, 2, 3, 4, 5, 6};
    csilk_raft_is_req_t is_in = {.term = 8,
                                 .leader_id = 1,
                                 .last_included_index = 50,
                                 .last_included_term = 7,
                                 .total_len = 100,
                                 .offset = 0,
                                 .chunk_len = sizeof(snap_chunk),
                                 .done = 0};
    n = _raft_rpc_encode_is_req(&is_in, snap_chunk, buf, sizeof(buf));
    assert(n > 0);
    csilk_raft_is_req_t is_out;
    const uint8_t*      chunk_ptr = NULL;
    assert(_raft_rpc_decode_is_req(buf, (size_t)n, &is_out, &chunk_ptr) == 0);
    assert(is_out.last_included_index == 50 && is_out.chunk_len == sizeof(snap_chunk));
    assert(chunk_ptr != NULL && memcmp(chunk_ptr, snap_chunk, sizeof(snap_chunk)) == 0);

    printf("test_raft_rpc_payload_codecs passed\n");
}

int
main(void)
{
    test_raft_rpc_framing();
    test_raft_rpc_payload_codecs();
    printf("All test_raft_rpc tests passed successfully!\n");
    return 0;
}
