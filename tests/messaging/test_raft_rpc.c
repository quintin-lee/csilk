#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "messaging/raft_internal.h"

int csilk_raft_rpc_encode_hdr(csilk_raft_msg_type_t msg_type,
                              uint64_t              term,
                              uint32_t              payload_len,
                              uint8_t*              out_buf,
                              size_t                out_cap);
int csilk_raft_rpc_decode_hdr(const uint8_t* buf, size_t len, csilk_raft_hdr_t* out_hdr);

static void
test_raft_rpc_framing(void)
{
    uint8_t buf[128];
    int res = csilk_raft_rpc_encode_hdr(CSILK_RAFT_MSG_REQUEST_VOTE_REQ, 42, 100, buf, sizeof(buf));
    assert(res == sizeof(csilk_raft_hdr_t));

    csilk_raft_hdr_t decoded;
    res = csilk_raft_rpc_decode_hdr(buf, sizeof(buf), &decoded);
    assert(res == 0);
    assert(decoded.magic == CSILK_RAFT_MAGIC);
    assert(decoded.msg_type == CSILK_RAFT_MSG_REQUEST_VOTE_REQ);
    assert(decoded.term == 42);
    assert(decoded.payload_len == 100);

    printf("test_raft_rpc_framing passed\n");
}

int
main(void)
{
    test_raft_rpc_framing();
    printf("All test_raft_rpc tests passed successfully!\n");
    return 0;
}
