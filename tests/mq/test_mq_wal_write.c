#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"

void
test_mq_wal_write()
{
    printf("Testing MQ WAL Write...\n");
    const char* wal_path = "test_publish.wal";
    unlink(wal_path);

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    csilk_mq_t*     mq = csilk_server_get_mq(server);

    int rc = csilk_mq_set_persistence(mq, wal_path);
    assert(rc == 0);

    const char* topic = "test.topic";
    const char* payload = "hello wal";
    size_t      payload_len = strlen(payload);

    rc = csilk_mq_publish(mq, topic, payload, payload_len);
    assert(rc == 0);

    /* Verify WAL file content */
    int fd = open(wal_path, O_RDONLY);
    assert(fd >= 0);

    uint32_t tlen = 0, plen = 0, checksum = 0;
    assert(read(fd, &tlen, 4) == 4);
    assert(tlen == (uint32_t)strlen(topic));

    char* tbuf = malloc(tlen + 1);
    assert(read(fd, tbuf, tlen) == (ssize_t)tlen);
    tbuf[tlen] = '\0';
    assert(strcmp(tbuf, topic) == 0);

    assert(read(fd, &plen, 4) == 4);
    assert(plen == (uint32_t)payload_len);

    char* pbuf = malloc(plen + 1);
    assert(read(fd, pbuf, plen) == (ssize_t)plen);
    pbuf[plen] = '\0';
    assert(strcmp(pbuf, payload) == 0);

    assert(read(fd, &checksum, 4) == 4);

    /* Verify checksum */
    uint32_t expected_checksum = 0;
    for (uint32_t i = 0; i < tlen; i++) {
        expected_checksum ^= (uint8_t)topic[i];
    }
    for (uint32_t i = 0; i < plen; i++) {
        expected_checksum ^= (uint8_t)payload[i];
    }
    assert(checksum == expected_checksum);

    close(fd);
    free(tbuf);
    free(pbuf);

    csilk_server_free(server);
    csilk_router_free(router);
    unlink(wal_path);
    printf("test_mq_wal_write: PASS\n");
}

int
main()
{
    test_mq_wal_write();
    return 0;
}
