/*
 * include/net/udp.h — UDP (User Datagram Protocol) layer
 *
 * Provides stateless datagram send/receive on top of the IPv4 stack.
 * UDP packets are dispatched here by ip4_receive() when protocol == PROTO_UDP.
 *
 * Two usage modes:
 *   1. Callback-based: udp_listen(port, cb, ctx)   — register per-port handler
 *   2. Blocking recv:  udp_recv(port, buf, maxlen, timeout_ms)
 *
 * The socket layer (net/socket.c) uses mode 1 for SOCK_DGRAM sockets.
 * Applications can use mode 2 directly for simple request/response protocols.
 */
#ifndef NET_UDP_H
#define NET_UDP_H

#include <net/net.h>
#include <net/ip.h>

/* =========================================================
 * UDP header (RFC 768 — 8 bytes)
 * ========================================================= */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* Header + data */
    uint16_t checksum;  /* Optional (0 = disabled) */
} __attribute__((packed)) udp_hdr_t;

/* =========================================================
 * Maximum registered listeners
 * ========================================================= */
#define UDP_MAX_LISTENERS  16

/* =========================================================
 * Receive callback type
 * src_ip, src_port: sender identity
 * data, len:        payload (points into the receive buffer)
 * ctx:              opaque value from udp_listen()
 * ========================================================= */
typedef void (*udp_recv_cb)(ip4_addr_t src_ip, uint16_t src_port,
                             const void* data, uint16_t len, void* ctx);

/* =========================================================
 * API
 * ========================================================= */

/* Initialise the UDP dispatch table */
void udp_init(void);

/* Send a UDP datagram.
 * dst_ip    : destination IPv4 address (network byte order)
 * src_port  : local source port
 * dst_port  : destination port
 * data/len  : payload
 * Returns 0 on success, -1 on error. */
int udp_send(ip4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* data, size_t len);

/* Register a per-port receive callback.
 * Multiple listeners on the same port are NOT supported (last wins).
 * Pass cb=NULL to unregister.
 * Returns 0 on success, -1 if the listener table is full. */
int udp_listen(uint16_t port, udp_recv_cb cb, void* ctx);

/* Called by ip4_receive() when a UDP packet arrives.
 * ip_hdr: pointer to the enclosing IPv4 header (for source IP).
 * payload/len: UDP header + data. */
void udp_receive(const ip4_hdr_t* ip_hdr, const void* payload, size_t len);

/* Poll for an incoming UDP datagram on a local port (non-blocking).
 * Returns bytes copied into buf, or -1 if nothing available.
 * Used by dhcp.c and dns.c for simple request/response. */
int udp_recv_poll(uint16_t local_port, void* buf, int bufsz);

/* Blocking receive helper (spins until data arrives or timeout).
 * Registers a temporary listener on port, waits up to timeout_ms.
 * Returns number of bytes written to buf, or -1 on timeout.
 * NOT re-entrant for the same port. */
int udp_recv_blocking(uint16_t port, void* buf, uint16_t maxlen,
                      uint32_t timeout_ms);

#endif /* NET_UDP_H */
