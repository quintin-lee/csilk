/**
 * @file uring_tcp.c
 * @brief TCP connection operations and address utilities.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include "uring_internal.h"

/* ====================================================================
 * TCP handle init / open
 * ==================================================================== */

int
csilk_io_tcp_init(csilk_io_loop_t* loop, csilk_io_tcp_t* handle)
{
    if (!handle) {
        return -1;
    }
    uint8_t gen = handle->generation ? (uint8_t)(handle->generation + 1) : 1;
    memset(handle, 0, sizeof(*handle));
    handle->loop = loop;
    handle->fd = -1;
    handle->type = CSILK_IO_HANDLE_TCP;
    handle->generation = gen ? gen : 1;
    return 0;
}

int
csilk_io_tcp_open(csilk_io_tcp_t* handle, csilk_io_os_sock_t sock)
{
    if (!handle) {
        return -1;
    }
    handle->fd = sock;
    return 0;
}

/* ====================================================================
 * Bind / listen / accept
 * ==================================================================== */

int
csilk_io_tcp_bind(csilk_io_tcp_t* handle, const struct sockaddr* addr, unsigned int flags)
{
    (void)flags;
    if (!handle || !addr) {
        return -1;
    }
    if (handle->fd < 0) {
        int family = addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            return -1;
        }
        int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        handle->fd = fd;
    }
    socklen_t addrlen =
        (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    int rc = bind(handle->fd, addr, addrlen);
    if (rc < 0) {
        close(handle->fd);
        handle->fd = -1;
        return -1;
    }
    return 0;
}

int
csilk_io_listen(csilk_io_stream_t* stream, int backlog, csilk_io_connection_cb cb)
{
    if (!stream || stream->fd < 0) {
        return -1;
    }
    if (listen(stream->fd, backlog) < 0) {
        return -1;
    }
    stream->connection_cb = cb;
    stream->flags |= CSILK_IO_HANDLE_ACTIVE;
    stream->generation++;

    csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();
    stream->loop = loop;
    loop->active_handles++;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_poll_add(sqe, stream->fd, POLLIN);
    io_uring_sqe_set_data64(
        sqe, uring_encode_handle_data(URING_OP_POLL_LISTEN, (csilk_io_handle_t*)stream));
    io_uring_submit(&loop->ring);
    return 0;
}

int
csilk_io_accept(csilk_io_stream_t* server, csilk_io_stream_t* client)
{
    if (!server || server->fd < 0 || !client) {
        return -1;
    }
    int fd = accept4(server->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    client->fd = fd;
    client->loop = server->loop;
    return 0;
}

/* ====================================================================
 * TCP socket options
 * ==================================================================== */

int
csilk_io_tcp_nodelay(csilk_io_tcp_t* handle, int enable)
{
    if (!handle || handle->fd < 0) {
        return -1;
    }
    return setsockopt(handle->fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
}

int
csilk_io_tcp_keepalive(csilk_io_tcp_t* handle, int enable, unsigned int delay)
{
    if (!handle || handle->fd < 0) {
        return -1;
    }
    (void)delay;
    return setsockopt(handle->fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
}

int
csilk_io_tcp_getpeername(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen)
{
    if (!handle || handle->fd < 0 || !name || !namelen) {
        return -1;
    }
    socklen_t slen = (socklen_t)*namelen;
    int       r = getpeername(handle->fd, name, &slen);
    *namelen = (int)slen;
    return r;
}

int
csilk_io_tcp_getsockname(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen)
{
    if (!handle || handle->fd < 0 || !name || !namelen) {
        return -1;
    }
    socklen_t slen = (socklen_t)*namelen;
    int       r = getsockname(handle->fd, name, &slen);
    *namelen = (int)slen;
    return r;
}

/* ====================================================================
 * Address utilities
 * ==================================================================== */

int
csilk_io_ip4_addr(const char* ip, int port, struct sockaddr_in* addr)
{
    if (!ip || !addr) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
        return -1;
    }
    return 0;
}

int
csilk_io_ip4_name(const struct sockaddr_in* src, char* dst, size_t size)
{
    if (!src || !dst) {
        return -1;
    }
    return inet_ntop(AF_INET, &src->sin_addr, dst, (socklen_t)size) ? 0 : -1;
}

int
csilk_io_ip6_name(const struct sockaddr_in6* src, char* dst, size_t size)
{
    if (!src || !dst) {
        return -1;
    }
    return inet_ntop(AF_INET6, &src->sin6_addr, dst, (socklen_t)size) ? 0 : -1;
}

#endif
