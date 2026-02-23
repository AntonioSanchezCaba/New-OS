/*
 * net/socket.c - BSD socket API implementation
 *
 * Provides socket(), bind(), connect(), listen(), accept(), send(), recv(),
 * sendto(), recvfrom() implemented on top of the existing TCP/UDP stack.
 *
 * Socket descriptors are separate from file descriptors in Phase 1.
 * Phase 2 will unify them through the VFS (sockfs).
 */
#include <net/socket.h>
#include <net/tcp.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel.h>
#include <memory.h>
#include <string.h>
#include <process.h>
#include <scheduler.h>
#include <drivers/timer.h>

/* ── Global socket table ─────────────────────────────────────────────── */

static sock_t sock_table[NET_SOCK_MAX];

void socket_init(void)
{
    memset(sock_table, 0, sizeof(sock_table));
    kinfo("Socket: BSD socket layer initialized (%d slots)", NET_SOCK_MAX);
}

/* ── Internal helpers ────────────────────────────────────────────────── */

static sock_t* sock_get(int sd)
{
    if (sd < 0 || sd >= NET_SOCK_MAX) return NULL;
    if (!sock_table[sd].used) return NULL;
    return &sock_table[sd];
}

static int sock_alloc(void)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        if (!sock_table[i].used) return i;
    }
    return -EMFILE;
}

static uint16_t sock_ephemeral_port(void)
{
    static uint16_t next_port = 49152;
    uint16_t p = next_port++;
    if (next_port == 0 || next_port > 65535) next_port = 49152;
    return p;
}

/* Ring buffer helpers for socket RX */
static size_t rxbuf_available(const sock_t* s)
{
    if (s->rx_tail >= s->rx_head)
        return s->rx_tail - s->rx_head;
    return SOCK_RXBUF_SIZE - s->rx_head + s->rx_tail;
}

static void rxbuf_push(sock_t* s, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s->rxbuf[s->rx_tail] = data[i];
        s->rx_tail = (s->rx_tail + 1) % SOCK_RXBUF_SIZE;
        if (s->rx_tail == s->rx_head) {
            /* Overflow: advance head to drop oldest byte */
            s->rx_head = (s->rx_head + 1) % SOCK_RXBUF_SIZE;
        }
    }
}

static size_t rxbuf_pop(sock_t* s, uint8_t* buf, size_t len)
{
    size_t avail = rxbuf_available(s);
    size_t n     = MIN(len, avail);
    for (size_t i = 0; i < n; i++) {
        buf[i]  = s->rxbuf[s->rx_head];
        s->rx_head = (s->rx_head + 1) % SOCK_RXBUF_SIZE;
    }
    return n;
}

/* ── socket() ────────────────────────────────────────────────────────── */

int sock_create(int domain, int type, int protocol)
{
    if (domain != AF_INET && domain != AF_UNIX) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return -EINVAL;

    int sd = sock_alloc();
    if (sd < 0) return -EMFILE;

    sock_t* s     = &sock_table[sd];
    memset(s, 0, sizeof(*s));
    s->used       = true;
    s->domain     = domain;
    s->type       = type;
    s->protocol   = protocol;
    s->state      = SOCK_STATE_CREATED;
    s->tcp_sock   = -1;
    s->owner      = current_process ? current_process->pid : 0;

    /* For TCP sockets allocate an underlying TCP socket */
    if (type == SOCK_STREAM && domain == AF_INET) {
        s->tcp_sock = tcp_connect(0, 0); /* Will be properly set on connect */
        /* Just allocate the slot for now */
    }

    kdebug("Socket: created sd=%d domain=%d type=%d", sd, domain, type);
    return sd;
}

/* ── bind() ──────────────────────────────────────────────────────────── */

int sock_bind(int sd, const sockaddr_in_t* addr)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (!addr || addr->sin_family != AF_INET) return -EINVAL;
    if (s->state != SOCK_STATE_CREATED) return -EINVAL;

    s->local = *addr;
    s->state = SOCK_STATE_BOUND;
    kdebug("Socket: sd=%d bound to port %u",
           sd, ntohs(addr->sin_port));
    return 0;
}

/* ── connect() ───────────────────────────────────────────────────────── */

int sock_connect(int sd, const sockaddr_in_t* addr)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (!addr || addr->sin_family != AF_INET) return -EINVAL;
    if (s->type != SOCK_STREAM) return -EPROTOTYPE;

    s->remote = *addr;
    if (s->local.sin_port == 0) {
        s->local.sin_port = htons(sock_ephemeral_port());
    }

    /* Delegate to TCP stack */
    ip4_addr_t dest_ip   = addr->sin_addr;
    uint16_t   dest_port = ntohs(addr->sin_port);

    int tcp_sd = tcp_connect(dest_ip, dest_port);
    if (tcp_sd < 0) return -ECONNREFUSED;

    s->tcp_sock = tcp_sd;
    s->state    = SOCK_STATE_CONNECTED;
    kdebug("Socket: sd=%d connected to %u.%u.%u.%u:%u",
           sd,
           (dest_ip      ) & 0xFF, (dest_ip >>  8) & 0xFF,
           (dest_ip >> 16) & 0xFF, (dest_ip >> 24) & 0xFF,
           dest_port);
    return 0;
}

/* ── listen() ────────────────────────────────────────────────────────── */

int sock_listen(int sd, int backlog)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->state != SOCK_STATE_BOUND && s->state != SOCK_STATE_CREATED)
        return -EINVAL;

    if (backlog > SOCK_BACKLOG_MAX) backlog = SOCK_BACKLOG_MAX;
    s->backlog_size = backlog;
    s->state        = SOCK_STATE_LISTENING;
    kdebug("Socket: sd=%d listening (backlog=%d)", sd, backlog);
    return 0;
}

/* ── accept() ────────────────────────────────────────────────────────── */

int sock_accept(int sd, sockaddr_in_t* peer)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_LISTENING) return -EINVAL;

    /* Block until a connection is in the backlog queue */
    uint64_t deadline = timer_get_ticks() + 5000; /* 5-second timeout */
    while (s->backlog_head == s->backlog_tail) {
        if (timer_get_ticks() > deadline) return -ETIMEDOUT;
        scheduler_yield();
    }

    int new_sd = s->backlog[s->backlog_head];
    s->backlog_head = (s->backlog_head + 1) % SOCK_BACKLOG_MAX;

    sock_t* ns = sock_get(new_sd);
    if (!ns) return -EIO;

    if (peer) *peer = ns->remote;

    kdebug("Socket: accept sd=%d → new_sd=%d", sd, new_sd);
    return new_sd;
}

/* ── send() ──────────────────────────────────────────────────────────── */

ssize_t sock_send(int sd, const void* buf, size_t len, int flags)
{
    (void)flags;
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_CONNECTED) return -ENOTCONN;
    if (!buf || len == 0) return 0;

    if (s->type == SOCK_STREAM && s->tcp_sock >= 0) {
        return tcp_send(s->tcp_sock, buf, len);
    }

    return -ENOTSUP;
}

/* ── recv() ──────────────────────────────────────────────────────────── */

ssize_t sock_recv(int sd, void* buf, size_t len, int flags)
{
    (void)flags;
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_CONNECTED) return -ENOTCONN;
    if (!buf || len == 0) return 0;

    if (s->type == SOCK_STREAM && s->tcp_sock >= 0) {
        /* First drain our local RX buffer */
        if (rxbuf_available(s) > 0) {
            return (ssize_t)rxbuf_pop(s, (uint8_t*)buf, len);
        }
        /* Then delegate to TCP */
        return tcp_recv(s->tcp_sock, buf, len);
    }

    /* UDP: drain from local RX ring */
    if (rxbuf_available(s) == 0) {
        if (flags & MSG_DONTWAIT) return -EAGAIN;
        /* Blocking wait */
        uint64_t deadline = timer_get_ticks() +
            (s->rcvtimeo_ms ? (uint64_t)s->rcvtimeo_ms / 10 : 5000);
        while (rxbuf_available(s) == 0) {
            if (timer_get_ticks() > deadline) return -ETIMEDOUT;
            scheduler_yield();
        }
    }

    return (ssize_t)rxbuf_pop(s, (uint8_t*)buf, len);
}

/* ── sendto / recvfrom ───────────────────────────────────────────────── */

ssize_t sock_sendto(int sd, const void* buf, size_t len, int flags,
                     const sockaddr_in_t* dst)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->type == SOCK_STREAM) return sock_send(sd, buf, len, flags);

    /* UDP sendto */
    if (!dst) return -EDESTADDRREQ;
    uint16_t src_port = s->local.sin_port ? ntohs(s->local.sin_port)
                                           : sock_ephemeral_port();
    return udp_send(dst->sin_addr, src_port, ntohs(dst->sin_port), buf, len);
}

ssize_t sock_recvfrom(int sd, void* buf, size_t len, int flags,
                       sockaddr_in_t* src)
{
    ssize_t n = sock_recv(sd, buf, len, flags);
    /* Populate sender address (set by sock_deliver_udp on arrival) */
    if (n >= 0 && src) {
        sock_t* s = sock_get(sd);
        if (s) *src = s->remote;
    }
    return n;
}

/* ── setsockopt / getsockopt ─────────────────────────────────────────── */

int sock_setsockopt(int sd, int level, int optname,
                     const void* optval, size_t optlen)
{
    sock_t* s = sock_get(sd);
    if (!s || !optval) return -EINVAL;
    (void)level; (void)optlen;

    switch (optname) {
    case SO_REUSEADDR:  s->reuseaddr    = *(const int*)optval != 0; break;
    case SO_RCVTIMEO:   s->rcvtimeo_ms  = *(const int*)optval;      break;
    case SO_SNDTIMEO:   s->sndtimeo_ms  = *(const int*)optval;      break;
    default: return -ENOPROTOOPT;
    }
    return 0;
}

int sock_getsockopt(int sd, int level, int optname,
                     void* optval, size_t* optlen)
{
    sock_t* s = sock_get(sd);
    if (!s || !optval || !optlen) return -EINVAL;
    (void)level;

    switch (optname) {
    case SO_ERROR:
        *(int*)optval = s->err;
        *optlen = sizeof(int);
        s->err  = 0;
        break;
    default: return -ENOPROTOOPT;
    }
    return 0;
}

/* ── shutdown / close ────────────────────────────────────────────────── */

int sock_shutdown(int sd, int how)
{
    (void)how;
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->tcp_sock >= 0) tcp_close(s->tcp_sock);
    s->state = SOCK_STATE_CLOSING;
    return 0;
}

int sock_close(int sd)
{
    sock_t* s = sock_get(sd);
    if (!s) return -EBADF;
    if (s->tcp_sock >= 0) tcp_close(s->tcp_sock);
    memset(s, 0, sizeof(*s));
    return 0;
}

/* ── Delivery from network stack ─────────────────────────────────────── */

void sock_deliver_udp(ip4_addr_t src_ip, uint16_t src_port,
                       uint16_t dst_port, const void* data, size_t len)
{
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        sock_t* s = &sock_table[i];
        if (!s->used) continue;
        if (s->type != SOCK_DGRAM) continue;
        if (ntohs(s->local.sin_port) != dst_port) continue;

        rxbuf_push(s, (const uint8_t*)data, len);

        /* Save sender info into a simple "last sender" field */
        s->remote.sin_family  = AF_INET;
        s->remote.sin_addr    = src_ip;
        s->remote.sin_port    = htons(src_port);
        return;
    }
}

void sock_notify_accept(uint16_t port, ip4_addr_t remote_ip,
                         uint16_t remote_port, int tcp_sock_idx)
{
    /* Find the listening socket on @port */
    for (int i = 0; i < NET_SOCK_MAX; i++) {
        sock_t* s = &sock_table[i];
        if (!s->used) continue;
        if (s->state != SOCK_STATE_LISTENING) continue;
        if (ntohs(s->local.sin_port) != port) continue;

        /* Create a new connected socket */
        int new_sd = sock_alloc();
        if (new_sd < 0) return;

        sock_t* ns = &sock_table[new_sd];
        memset(ns, 0, sizeof(*ns));
        ns->used     = true;
        ns->domain   = AF_INET;
        ns->type     = SOCK_STREAM;
        ns->state    = SOCK_STATE_CONNECTED;
        ns->tcp_sock = tcp_sock_idx;
        ns->remote.sin_family  = AF_INET;
        ns->remote.sin_addr    = remote_ip;
        ns->remote.sin_port    = htons(remote_port);
        ns->local              = s->local;
        ns->owner              = s->owner;

        /* Push into listening socket's backlog */
        int next = (s->backlog_tail + 1) % SOCK_BACKLOG_MAX;
        if (next != s->backlog_head) {
            s->backlog[s->backlog_tail] = new_sd;
            s->backlog_tail = next;
        }
        return;
    }
}

/* ── Syscall entry points ────────────────────────────────────────────── */

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    return sock_create((int)domain, (int)type, (int)proto);
}

int64_t sys_bind(uint64_t sd, uint64_t addr, uint64_t addrlen,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addrlen; (void)a4; (void)a5; (void)a6;
    if (!addr) return -EFAULT;
    return sock_bind((int)sd, (const sockaddr_in_t*)addr);
}

int64_t sys_connect(uint64_t sd, uint64_t addr, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addrlen; (void)a4; (void)a5; (void)a6;
    if (!addr) return -EFAULT;
    return sock_connect((int)sd, (const sockaddr_in_t*)addr);
}

int64_t sys_listen(uint64_t sd, uint64_t backlog,
                    uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    return sock_listen((int)sd, (int)backlog);
}

int64_t sys_accept(uint64_t sd, uint64_t addr, uint64_t addrlen_ptr,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addrlen_ptr; (void)a4; (void)a5; (void)a6;
    return sock_accept((int)sd, (sockaddr_in_t*)addr);
}

int64_t sys_send(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                  uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    if (!buf) return -EFAULT;
    return sock_send((int)sd, (const void*)buf, (size_t)len, (int)flags);
}

int64_t sys_recv(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                  uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    if (!buf) return -EFAULT;
    return sock_recv((int)sd, (void*)buf, (size_t)len, (int)flags);
}

int64_t sys_sendto(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                    uint64_t addr, uint64_t addrlen)
{
    (void)addrlen;
    if (!buf) return -EFAULT;
    return sock_sendto((int)sd, (const void*)buf, (size_t)len, (int)flags,
                        (const sockaddr_in_t*)addr);
}

int64_t sys_recvfrom(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                      uint64_t addr, uint64_t addrlen_ptr)
{
    (void)addrlen_ptr;
    if (!buf) return -EFAULT;
    return sock_recvfrom((int)sd, (void*)buf, (size_t)len, (int)flags,
                          (sockaddr_in_t*)addr);
}

int64_t sys_setsockopt(uint64_t sd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen, uint64_t a6)
{
    (void)a6;
    if (!optval) return -EFAULT;
    return sock_setsockopt((int)sd, (int)level, (int)optname,
                            (const void*)optval, (size_t)optlen);
}

int64_t sys_getsockopt(uint64_t sd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen_ptr, uint64_t a6)
{
    (void)a6;
    if (!optval || !optlen_ptr) return -EFAULT;
    return sock_getsockopt((int)sd, (int)level, (int)optname,
                            (void*)optval, (size_t*)optlen_ptr);
}

int64_t sys_shutdown(uint64_t sd, uint64_t how, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    int rc = sock_shutdown((int)sd, (int)how);
    if (rc == 0) sock_close((int)sd);  /* fully release on shutdown */
    return rc;
}
