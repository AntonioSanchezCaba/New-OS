/*
 * include/net/tcp.h - TCP layer (simplified)
 */
#ifndef NET_TCP_H
#define NET_TCP_H

#include <net/net.h>
#include <net/ip.h>

/* TCP header (20 bytes, no options) */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;   /* Upper 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

/* TCP flag bits */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* UDP header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

/* Simple TCP socket state machine */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
    TCP_TIME_WAIT,
} tcp_state_t;

#define TCP_MAX_SOCKETS 16

/* TCP socket */
typedef struct {
    bool        used;
    tcp_state_t state;
    ip4_addr_t  local_ip;
    ip4_addr_t  remote_ip;
    uint16_t    local_port;
    uint16_t    remote_port;
    uint32_t    tx_seq;
    uint32_t    rx_seq;

    /* Receive buffer */
    uint8_t  rx_buf[4096];
    size_t   rx_head, rx_tail;
} tcp_socket_t;

/* TCP API */
void tcp_init(void);
int  tcp_connect(ip4_addr_t ip, uint16_t port);
int  tcp_send(int sock, const void* data, size_t len);
int  tcp_recv(int sock, void* buf, size_t len);
void tcp_close(int sock);
void tcp_receive(const ip4_hdr_t* ip_hdr, const void* segment, size_t len);

/* UDP API */
int  udp_send(ip4_addr_t dst, uint16_t src_port, uint16_t dst_port,
              const void* data, size_t len);
void udp_receive(const ip4_hdr_t* ip_hdr, const void* segment, size_t len);

/* Poll for an incoming UDP datagram on a local port.
 * Returns bytes copied into buf, or -1 if nothing available. */
int  udp_recv_poll(uint16_t local_port, void* buf, int bufsz);

#endif /* NET_TCP_H */
