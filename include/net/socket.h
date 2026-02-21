/*
 * include/net/socket.h - BSD-compatible socket API
 *
 * Provides a kernel-side socket table that bridges the BSD socket interface
 * (socket, bind, connect, listen, accept, send, recv) to the internal
 * TCP/UDP/ICMP stacks.
 *
 * Exposed to userland via syscalls:
 *   SYS_SOCKET, SYS_BIND, SYS_CONNECT, SYS_LISTEN, SYS_ACCEPT,
 *   SYS_SEND, SYS_RECV, SYS_SENDTO, SYS_RECVFROM, SYS_SETSOCKOPT,
 *   SYS_GETSOCKOPT, SYS_SHUTDOWN, SYS_GETPEERNAME, SYS_GETSOCKNAME
 *
 * Each socket is represented by a kernel-side sock_t, allocated from a
 * fixed-size pool (NET_SOCK_MAX = 256).  Sockets are referenced by an
 * integer "socket descriptor" (not the same as a file descriptor yet;
 * a future VFS integration will unify them).
 */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <types.h>
#include <net/net.h>
#include <net/tcp.h>

/* ── Address families ────────────────────────────────────────────────── */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    10

/* ── Socket types ────────────────────────────────────────────────────── */
#define SOCK_STREAM  1   /* TCP */
#define SOCK_DGRAM   2   /* UDP */
#define SOCK_RAW     3   /* Raw IP */

/* ── Protocol numbers ────────────────────────────────────────────────── */
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

/* ── Socket options ──────────────────────────────────────────────────── */
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_KEEPALIVE   9
#define SO_RCVBUF      8
#define SO_SNDBUF      7
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ERROR       4

/* ── Shutdown flags ──────────────────────────────────────────────────── */
#define SHUT_RD    0
#define SHUT_WR    1
#define SHUT_RDWR  2

/* ── send/recv flags ─────────────────────────────────────────────────── */
#define MSG_DONTWAIT 0x40
#define MSG_PEEK     0x02
#define MSG_WAITALL  0x100

/* ── sockaddr structures ─────────────────────────────────────────────── */
typedef struct {
    uint16_t sa_family;
    uint8_t  sa_data[14];
} sockaddr_t;

typedef struct {
    uint16_t sin_family;   /* AF_INET */
    uint16_t sin_port;     /* Port in network byte order */
    uint32_t sin_addr;     /* IPv4 address in network byte order */
    uint8_t  sin_zero[8];
} PACKED sockaddr_in_t;

/* ── Internal socket state ───────────────────────────────────────────── */
typedef enum {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_CREATED,
    SOCK_STATE_BOUND,
    SOCK_STATE_LISTENING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_CLOSING,
    SOCK_STATE_CLOSED,
} sock_state_t;

#define NET_SOCK_MAX      256
#define SOCK_BACKLOG_MAX  8
#define SOCK_RXBUF_SIZE   8192

typedef struct sock {
    bool        used;
    int         domain;       /* AF_INET etc. */
    int         type;         /* SOCK_STREAM etc. */
    int         protocol;
    sock_state_t state;

    /* Local and remote addresses */
    sockaddr_in_t local;
    sockaddr_in_t remote;

    /* For TCP: index into tcp_socket_t table */
    int           tcp_sock;

    /* Receive ring buffer */
    uint8_t  rxbuf[SOCK_RXBUF_SIZE];
    uint32_t rx_head, rx_tail;

    /* Options */
    int      rcvtimeo_ms;   /* 0 = blocking */
    int      sndtimeo_ms;
    bool     reuseaddr;

    /* Pending accept queue (for SOCK_STREAM listening sockets) */
    int      backlog[SOCK_BACKLOG_MAX];  /* Socket indices waiting accept */
    int      backlog_head, backlog_tail;
    int      backlog_size;

    /* Error code (SO_ERROR) */
    int      err;

    /* Owner PID */
    pid_t    owner;
} sock_t;

/* ── Socket subsystem API ────────────────────────────────────────────── */
void socket_init(void);

/* Kernel-facing API */
int  sock_create(int domain, int type, int protocol);
int  sock_bind(int sd, const sockaddr_in_t* addr);
int  sock_connect(int sd, const sockaddr_in_t* addr);
int  sock_listen(int sd, int backlog);
int  sock_accept(int sd, sockaddr_in_t* peer);
ssize_t sock_send(int sd, const void* buf, size_t len, int flags);
ssize_t sock_recv(int sd, void* buf, size_t len, int flags);
ssize_t sock_sendto(int sd, const void* buf, size_t len, int flags,
                     const sockaddr_in_t* dst);
ssize_t sock_recvfrom(int sd, void* buf, size_t len, int flags,
                       sockaddr_in_t* src);
int  sock_setsockopt(int sd, int level, int optname,
                      const void* optval, size_t optlen);
int  sock_getsockopt(int sd, int level, int optname,
                      void* optval, size_t* optlen);
int  sock_shutdown(int sd, int how);
int  sock_close(int sd);

/* Called from UDP/TCP receive path when a packet arrives */
void sock_deliver_udp(ip4_addr_t src_ip, uint16_t src_port,
                       uint16_t dst_port, const void* data, size_t len);
void sock_notify_accept(uint16_t port, ip4_addr_t remote_ip,
                         uint16_t remote_port, int tcp_sock_idx);

/* ── Syscall entry points ────────────────────────────────────────────── */
int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto,
                    uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_bind(uint64_t sd, uint64_t addr, uint64_t addrlen,
                  uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_connect(uint64_t sd, uint64_t addr, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_listen(uint64_t sd, uint64_t backlog,
                    uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_accept(uint64_t sd, uint64_t addr, uint64_t addrlen_ptr,
                    uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_send(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                  uint64_t a5, uint64_t a6);
int64_t sys_recv(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                  uint64_t a5, uint64_t a6);
int64_t sys_sendto(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                    uint64_t addr, uint64_t addrlen);
int64_t sys_recvfrom(uint64_t sd, uint64_t buf, uint64_t len, uint64_t flags,
                      uint64_t addr, uint64_t addrlen_ptr);

#endif /* NET_SOCKET_H */
