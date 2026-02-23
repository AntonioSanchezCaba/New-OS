/*
 * net/tcp.c - TCP and UDP transport layer
 *
 * Implements a minimal TCP state machine supporting active open (connect),
 * send, receive, and close. Also provides stateless UDP send.
 */
#include <net/tcp.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel.h>
#include <string.h>
#include <memory.h>
#include <drivers/timer.h>
#include <scheduler.h>

/* =========================================================
 * Socket table
 * ========================================================= */

static tcp_socket_t sockets[TCP_MAX_SOCKETS];
static uint16_t     ephemeral_port = 49152;  /* Start of ephemeral range */

static uint32_t rand_seq(void)
{
    /* ISN based on timer ticks XOR some constants */
    return (uint32_t)(timer_get_ticks() * 0xDEADBEEF ^ 0x12345678);
}

void tcp_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    ephemeral_port = 49152;
    kinfo("TCP/UDP: initialized");
}

static tcp_socket_t* get_socket(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return NULL;
    if (!sockets[sock].used) return NULL;
    return &sockets[sock];
}

/* =========================================================
 * Pseudo-header checksum for TCP/UDP
 * ========================================================= */

static uint16_t tcp_checksum(ip4_addr_t src, ip4_addr_t dst, uint8_t proto,
                               const void* seg, size_t seg_len)
{
    /* Build pseudo-header */
    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    } __attribute__((packed)) pseudo;

    pseudo.src   = src;
    pseudo.dst   = dst;
    pseudo.zero  = 0;
    pseudo.proto = proto;
    pseudo.len   = htons((uint16_t)seg_len);

    uint32_t acc = 0;
    const uint16_t* p = (const uint16_t*)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) acc += p[i];

    p = (const uint16_t*)seg;
    size_t rem = seg_len;
    while (rem > 1) { acc += *p++; rem -= 2; }
    if (rem) acc += *(const uint8_t*)p;

    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

/* =========================================================
 * TCP send segment helper
 * ========================================================= */

static int tcp_send_segment(tcp_socket_t* s, uint8_t flags,
                             const void* data, size_t data_len)
{
    size_t seg_len = sizeof(tcp_hdr_t) + data_len;
    net_buf_t* buf = net_alloc_buf();
    if (!buf) return -1;

    tcp_hdr_t* hdr = (tcp_hdr_t*)buf->data;
    hdr->src_port   = htons(s->local_port);
    hdr->dst_port   = htons(s->remote_port);
    hdr->seq        = htonl(s->tx_seq);
    hdr->ack        = (flags & TCP_ACK) ? htonl(s->rx_seq) : 0;
    hdr->data_offset = (sizeof(tcp_hdr_t) / 4) << 4;
    hdr->flags      = flags;
    hdr->window     = htons(4096);
    hdr->checksum   = 0;
    hdr->urgent     = 0;

    if (data && data_len > 0)
        memcpy(buf->data + sizeof(tcp_hdr_t), data, data_len);

    hdr->checksum = tcp_checksum(net_iface.ip, s->remote_ip,
                                 PROTO_TCP, buf->data, seg_len);

    int ret = ip4_send(s->remote_ip, PROTO_TCP, buf->data, seg_len);
    net_free_buf(buf);

    if (ret == 0 && data_len > 0)
        s->tx_seq += (uint32_t)data_len;

    return ret;
}

/* =========================================================
 * TCP public API
 * ========================================================= */

int tcp_connect(ip4_addr_t ip, uint16_t port)
{
    /* Find free socket slot */
    int sock = -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) { sock = i; break; }
    }
    if (sock < 0) return -1;

    tcp_socket_t* s = &sockets[sock];
    memset(s, 0, sizeof(*s));
    s->used        = true;
    s->state       = TCP_SYN_SENT;
    s->local_ip    = net_iface.ip;
    s->remote_ip   = ip;
    s->local_port  = ephemeral_port++;
    s->remote_port = port;
    s->tx_seq      = rand_seq();

    /* Send SYN */
    int ret = tcp_send_segment(s, TCP_SYN, NULL, 0);
    if (ret < 0) {
        s->used = false;
        return -1;
    }
    s->tx_seq++;  /* SYN consumes one sequence number */

    /* Wait for SYN-ACK (poll with timeout) */
    for (uint32_t start = timer_get_ticks();
         timer_get_ticks() - start < TIMER_FREQ * 5; ) {
        if (net_iface.poll) net_iface.poll();
        if (s->state == TCP_ESTABLISHED) return sock;
        scheduler_yield();  /* yield instead of busy-spinning */
    }

    /* Timeout */
    s->used = false;
    return -1;
}

int tcp_send(int sock, const void* data, size_t len)
{
    tcp_socket_t* s = get_socket(sock);
    if (!s || s->state != TCP_ESTABLISHED) return -1;
    return tcp_send_segment(s, TCP_ACK | TCP_PSH, data, len);
}

int tcp_recv(int sock, void* buf, size_t len)
{
    tcp_socket_t* s = get_socket(sock);
    if (!s) return -1;

    /* Poll for data */
    size_t available = (s->rx_tail - s->rx_head + sizeof(s->rx_buf))
                       % sizeof(s->rx_buf);
    if (available == 0) return 0;

    size_t to_read = (available < len) ? available : len;
    uint8_t* dst = (uint8_t*)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = s->rx_buf[s->rx_head % sizeof(s->rx_buf)];
        s->rx_head++;
    }
    return (int)to_read;
}

void tcp_close(int sock)
{
    tcp_socket_t* s = get_socket(sock);
    if (!s) return;

    if (s->state == TCP_ESTABLISHED) {
        tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
        s->tx_seq++;
        s->state = TCP_FIN_WAIT;
    }
    s->used  = false;
    s->state = TCP_CLOSED;
}

/* =========================================================
 * TCP receive
 * ========================================================= */

void tcp_receive(const ip4_hdr_t* ip_hdr, const void* segment, size_t len)
{
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t* hdr = (const tcp_hdr_t*)segment;

    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    uint8_t  flags = hdr->flags;

    /* Find matching socket */
    tcp_socket_t* s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) continue;
        if (sockets[i].local_port  == dst_port &&
            sockets[i].remote_port == src_port &&
            sockets[i].remote_ip   == ip_hdr->src) {
            s = &sockets[i];
            break;
        }
    }
    if (!s) return;

    uint8_t  data_off = (hdr->data_offset >> 4) * 4;
    const uint8_t* data = (const uint8_t*)segment + data_off;
    size_t   data_len = (data_off <= len) ? len - data_off : 0;

    switch (s->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == s->tx_seq) {
                s->rx_seq = seq + 1;
                s->state  = TCP_ESTABLISHED;
                /* Send ACK */
                tcp_send_segment(s, TCP_ACK, NULL, 0);
            }
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) {
            s->state = TCP_CLOSED;
            s->used  = false;
            break;
        }
        if (flags & TCP_FIN) {
            s->rx_seq = seq + 1;
            s->state  = TCP_CLOSE_WAIT;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            /* Send FIN */
            tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
            s->tx_seq++;
            s->state = TCP_TIME_WAIT;
            break;
        }
        /* Data segment */
        if (data_len > 0 && seq == s->rx_seq) {
            /* Copy into RX ring buffer */
            for (size_t i = 0; i < data_len; i++) {
                size_t next = (s->rx_tail + 1) % sizeof(s->rx_buf);
                if (next != s->rx_head % sizeof(s->rx_buf)) {
                    s->rx_buf[s->rx_tail % sizeof(s->rx_buf)] = data[i];
                    s->rx_tail++;
                }
            }
            s->rx_seq += (uint32_t)data_len;
            /* Send ACK */
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT:
        if (flags & TCP_ACK) {
            s->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_TIME_WAIT:
        s->state = TCP_CLOSED;
        s->used  = false;
        break;

    default:
        break;
    }
}

/* =========================================================
 * UDP (stateless)
 * ========================================================= */

int udp_send(ip4_addr_t dst, uint16_t src_port, uint16_t dst_port,
             const void* data, size_t len)
{
    size_t seg_len = sizeof(udp_hdr_t) + len;
    net_buf_t* buf = net_alloc_buf();
    if (!buf) return -1;

    udp_hdr_t* hdr = (udp_hdr_t*)buf->data;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)seg_len);
    hdr->checksum = 0;  /* Optional for UDP */

    if (data && len > 0)
        memcpy(buf->data + sizeof(udp_hdr_t), data, len);

    int ret = ip4_send(dst, PROTO_UDP, buf->data, seg_len);
    net_free_buf(buf);
    return ret;
}

/* =========================================================
 * Simple single-slot UDP receive buffer per port
 * ========================================================= */
#define UDP_RX_SLOTS    4
#define UDP_RX_BUF_SZ   2048

typedef struct {
    uint16_t port;
    int      used;
    int      len;
    uint8_t  data[UDP_RX_BUF_SZ];
} udp_rx_slot_t;

static udp_rx_slot_t udp_rx[UDP_RX_SLOTS];

void udp_receive(const ip4_hdr_t* ip_hdr, const void* segment, size_t len)
{
    (void)ip_hdr;
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t* hdr = (const udp_hdr_t*)segment;
    uint16_t dst_port = ntohs(hdr->dst_port);
    size_t   payload_len = len - sizeof(udp_hdr_t);
    if (payload_len == 0 || payload_len > UDP_RX_BUF_SZ) return;
    const uint8_t* payload = (const uint8_t*)segment + sizeof(udp_hdr_t);

    /* Store in the first free slot matching this port (or any free slot) */
    for (int i = 0; i < UDP_RX_SLOTS; i++) {
        if (!udp_rx[i].used) {
            udp_rx[i].port = dst_port;
            udp_rx[i].len  = (int)payload_len;
            memcpy(udp_rx[i].data, payload, payload_len);
            udp_rx[i].used = 1;
            return;
        }
    }
}

int udp_recv_poll(uint16_t local_port, void* buf, int bufsz)
{
    for (int i = 0; i < UDP_RX_SLOTS; i++) {
        if (udp_rx[i].used && udp_rx[i].port == local_port) {
            int n = udp_rx[i].len < bufsz ? udp_rx[i].len : bufsz;
            memcpy(buf, udp_rx[i].data, (size_t)n);
            udp_rx[i].used = 0;
            return n;
        }
    }
    return -1;
}
