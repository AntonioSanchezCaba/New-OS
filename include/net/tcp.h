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

/* TCP socket state machine (RFC 793 §3.2) */
typedef enum {
    TCP_CLOSED     = 0,
    TCP_LISTEN,        /* Passive open: waiting for SYN               */
    TCP_SYN_RCVD,      /* Got SYN, sent SYN-ACK, waiting for final ACK */
    TCP_SYN_SENT,      /* Active open: SYN sent, waiting for SYN-ACK  */
    TCP_ESTABLISHED,   /* Full duplex data transfer                   */
    TCP_FIN_WAIT,      /* Active close: FIN sent, waiting for ACK     */
    TCP_CLOSE_WAIT,    /* Passive close: FIN received, ACK sent       */
    TCP_TIME_WAIT,     /* Waiting for 2×MSL before CLOSED             */
} tcp_state_t;

#define TCP_MAX_SOCKETS 16

/* Retransmit constants */
#define TCP_RTO_INITIAL   300   /* Initial RTO: 3 s at 100 Hz              */
#define TCP_RTO_MAX      6000   /* Max RTO: 60 s (exponential backoff cap)  */
#define TCP_MAX_RETRIES     5   /* Give up after 5 retransmissions          */
#define TCP_RTX_BUF      1460  /* One MSS segment for retransmission       */
#define TCP_MSS          1460  /* Maximum segment size (Ethernet MTU - hdrs)*/

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

    /* Retransmit state -------------------------------------------------- */
    uint8_t  tx_rtx_buf[TCP_RTX_BUF];  /* Copy of unACKed payload          */
    size_t   tx_rtx_len;                /* Payload bytes saved (0 = none)   */
    uint8_t  tx_rtx_flags;              /* TCP flags of the saved segment   */
    uint32_t tx_rtx_seq;                /* tx_seq when segment was sent     */
    uint32_t tx_rtx_end_seq;            /* First seq# after this segment    */
    uint32_t tx_rtx_time;               /* timer_get_ticks() when sent      */
    uint32_t tx_rto;                    /* Current RTO in ticks             */
    int      tx_rtx_count;              /* Number of retransmissions so far */
} tcp_socket_t;

/* TCP API */
void tcp_init(void);

/* Active open — sends SYN, waits for SYN-ACK.
 * Returns socket index ≥ 0 on success, -1 on timeout/error. */
int  tcp_connect(ip4_addr_t ip, uint16_t port);

/* Passive open — marks a TCP socket slot as LISTEN on @port.
 * The socket layer calls this when sock_listen() is invoked.
 * Returns socket index ≥ 0 on success, -1 if no free slot.
 * Incoming SYNs on @port cause a child socket to be created and
 * sock_notify_accept() to be called when the handshake completes. */
int  tcp_listen(uint16_t port);

int  tcp_send(int sock, const void* data, size_t len);
int  tcp_recv(int sock, void* buf, size_t len);
void tcp_close(int sock);
void tcp_receive(const ip4_hdr_t* ip_hdr, const void* segment, size_t len);

/* Called periodically (e.g. from the ARE render loop) to retransmit
 * unacknowledged segments whose RTO has expired. */
void tcp_tick(void);

#endif /* NET_TCP_H */
