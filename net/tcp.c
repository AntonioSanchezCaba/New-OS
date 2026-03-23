/*
 * net/tcp.c - TCP and UDP transport layer
 *
 * Implements a minimal TCP state machine supporting active open (connect),
 * send, receive, and close.  Also provides stateless UDP send.
 *
 * Retransmit timer (RFC 793 §3.7):
 *   Each socket maintains a retransmit buffer for the most recent
 *   unacknowledged segment (SYN, data).  tcp_tick() is called from
 *   the ARE render loop (~50 Hz) and retransmits the segment when the
 *   current RTO expires.  RTO doubles on each retry (exponential backoff)
 *   and gives up after TCP_MAX_RETRIES attempts, resetting the socket.
 */
#include <net/tcp.h>
#include <net/ip.h>
#include <net/net.h>
#include <net/socket.h>
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
 * tcp_listen — passive open: park a socket in LISTEN state.
 * Called by sock_listen() via the BSD socket layer.
 * ========================================================= */
int tcp_listen(uint16_t port)
{
    /* Reuse existing LISTEN slot for the same port (idempotent) */
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].used &&
            sockets[i].state == TCP_LISTEN &&
            sockets[i].local_port == port)
            return i;
    }

    int sock = -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) { sock = i; break; }
    }
    if (sock < 0) {
        klog_warn("TCP: no free socket for listen on port %u", port);
        return -1;
    }

    tcp_socket_t* s = &sockets[sock];
    memset(s, 0, sizeof(*s));
    s->used        = true;
    s->state       = TCP_LISTEN;
    s->local_ip    = net_iface.ip;
    s->local_port  = port;
    s->remote_ip   = 0;
    s->remote_port = 0;

    kinfo("TCP: listening on port %u (slot %d)", port, sock);
    return sock;
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
    } pseudo;

    pseudo.src   = src;
    pseudo.dst   = dst;
    pseudo.zero  = 0;
    pseudo.proto = proto;
    pseudo.len   = htons((uint16_t)seg_len);

    uint32_t acc = 0;
    const uint16_t* p = (const uint16_t*)(const void*)&pseudo;
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

    if (ret == 0) {
        /* Advance tx_seq by data payload length first */
        if (data_len > 0)
            s->tx_seq += (uint32_t)data_len;

        /* Save segment for potential retransmission.
         * Only segments that consume sequence space: SYN, FIN, data.
         * Pure ACKs (no payload, no SYN/FIN) are not saved.
         *
         * tx_rtx_seq    = sequence number in the saved segment header
         * tx_rtx_end_seq = first sequence number AFTER this segment
         *                  (what the peer will ACK when it arrives)
         * For SYN or FIN: they each consume 1 extra sequence number.
         */
        bool consumes_seq = (flags & (TCP_SYN | TCP_FIN)) || (data_len > 0);
        if (consumes_seq) {
            size_t save_len = (data_len < TCP_RTX_BUF) ? data_len : TCP_RTX_BUF;
            uint32_t seg_seq = s->tx_seq - (uint32_t)data_len; /* seq sent */
            uint32_t end_seq = s->tx_seq; /* after data */
            if (flags & TCP_SYN) end_seq++; /* SYN consumes 1 seq num */
            if (flags & TCP_FIN) end_seq++; /* FIN consumes 1 seq num */

            s->tx_rtx_seq     = seg_seq;
            s->tx_rtx_end_seq = end_seq;
            s->tx_rtx_flags   = flags;
            s->tx_rtx_len     = save_len;
            if (data && save_len > 0)
                memcpy(s->tx_rtx_buf, data, save_len);
            if (s->tx_rtx_count == 0)
                s->tx_rto = TCP_RTO_INITIAL; /* reset on fresh send */
            s->tx_rtx_time    = (uint32_t)timer_get_ticks();
        }
    }

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

    /* Wait for SYN-ACK (poll with timeout).
     * tcp_tick() handles SYN retransmission if the SYN-ACK is delayed. */
    for (uint32_t start = timer_get_ticks();
         timer_get_ticks() - start < TIMER_FREQ * 5; ) {
        if (net_iface.poll) net_iface.poll();
        tcp_tick();   /* retransmit SYN if RTO expires during connect */
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

    /*
     * MSS-based segmentation: break large payloads into TCP_MSS chunks.
     * Each segment is sent independently; the retransmit buffer saves the
     * most recent unACKed segment for recovery via tcp_tick().
     * This enables bulk data transfer (e.g. HTTP responses) that previously
     * failed because the single 2048-byte retransmit buffer was too small.
     */
    const uint8_t* ptr  = (const uint8_t*)data;
    size_t         left = len;
    int            sent = 0;

    while (left > 0) {
        size_t chunk = (left > TCP_MSS) ? TCP_MSS : left;
        int rc = tcp_send_segment(s, TCP_ACK | TCP_PSH, ptr, chunk);
        if (rc < 0) {
            /* Partial send: return bytes delivered so far, or -1 if none */
            return (sent > 0) ? sent : -1;
        }
        ptr  += chunk;
        left -= chunk;
        sent += (int)chunk;

        /* If there are more segments to send and a retransmit is pending,
         * yield briefly so tcp_tick() can process any incoming ACKs and
         * clear the retransmit window before we enqueue the next segment. */
        if (left > 0 && s->tx_rtx_time != 0)
            scheduler_yield();
    }

    return sent;
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
    s->tx_rtx_time  = 0;   /* cancel any pending retransmit */
    s->tx_rtx_len   = 0;
    s->used  = false;
    s->state = TCP_CLOSED;
}

/* =========================================================
 * Retransmit helper — re-sends the saved segment with the
 * original sequence number without updating socket state.
 * ========================================================= */

static void tcp_rtx_send(tcp_socket_t* s)
{
    size_t seg_len = sizeof(tcp_hdr_t) + s->tx_rtx_len;
    net_buf_t* buf = net_alloc_buf();
    if (!buf) return;

    tcp_hdr_t* hdr = (tcp_hdr_t*)buf->data;
    hdr->src_port    = htons(s->local_port);
    hdr->dst_port    = htons(s->remote_port);
    hdr->seq         = htonl(s->tx_rtx_seq);
    hdr->ack         = (s->tx_rtx_flags & TCP_ACK) ? htonl(s->rx_seq) : 0;
    hdr->data_offset = (sizeof(tcp_hdr_t) / 4) << 4;
    hdr->flags       = s->tx_rtx_flags;
    hdr->window      = htons(4096);
    hdr->checksum    = 0;
    hdr->urgent      = 0;

    if (s->tx_rtx_len > 0)
        memcpy(buf->data + sizeof(tcp_hdr_t), s->tx_rtx_buf, s->tx_rtx_len);

    hdr->checksum = tcp_checksum(net_iface.ip, s->remote_ip,
                                 PROTO_TCP, buf->data, seg_len);
    ip4_send(s->remote_ip, PROTO_TCP, buf->data, seg_len);
    net_free_buf(buf);
}

/* =========================================================
 * tcp_tick — retransmit timer, call periodically (~50 Hz).
 *
 * For every socket with a pending retransmit (tx_rtx_time != 0),
 * if the current RTO has expired, retransmit the saved segment.
 * RTO doubles on each retry (binary exponential backoff, RFC 793).
 * After TCP_MAX_RETRIES failures the socket is forcibly reset.
 * ========================================================= */

void tcp_tick(void)
{
    uint32_t now = (uint32_t)timer_get_ticks();

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t* s = &sockets[i];
        if (!s->used || s->tx_rtx_time == 0) continue;

        /* Has the RTO expired? */
        if (now - s->tx_rtx_time < s->tx_rto) continue;

        /* Give up after too many retransmissions */
        if (s->tx_rtx_count >= TCP_MAX_RETRIES) {
            klog_warn("TCP: slot %d max retries reached — resetting", i);
            s->tx_rtx_time  = 0;
            s->tx_rtx_len   = 0;
            s->tx_rtx_count = 0;
            s->state = TCP_CLOSED;
            s->used  = false;
            continue;
        }

        /* Retransmit the saved segment */
        s->tx_rtx_count++;
        s->tx_rto = (s->tx_rto * 2 < TCP_RTO_MAX) ? s->tx_rto * 2 : TCP_RTO_MAX;
        s->tx_rtx_time = now;

        kdebug("TCP: retransmit slot=%d attempt=%d rto=%u seq=%u",
               i, s->tx_rtx_count, s->tx_rto, s->tx_rtx_seq);
        tcp_rtx_send(s);
    }
}

/* =========================================================
 * Retransmit-clear helper
 * Clear the retransmit buffer when an ACK covers our sent data.
 * ========================================================= */

static void tcp_rtx_clear_if_acked(tcp_socket_t* s, uint32_t ack_num)
{
    if (s->tx_rtx_time == 0) return;            /* nothing pending        */
    if (ack_num < s->tx_rtx_end_seq) return;    /* not fully covered yet  */
    /* All pending data acknowledged — clear retransmit state */
    s->tx_rtx_time  = 0;
    s->tx_rtx_len   = 0;
    s->tx_rtx_count = 0;
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

    /* Find matching connected socket (fully-qualified 4-tuple) */
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

    if (!s) {
        /* No connected socket — check for a SYN on a LISTEN socket */
        if (!(flags & TCP_SYN)) return;      /* Not a SYN: nothing to do */
        if (flags & TCP_ACK)    return;      /* SYN-ACK to us: ignore    */

        tcp_socket_t* listener = NULL;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (sockets[i].used &&
                sockets[i].state      == TCP_LISTEN &&
                sockets[i].local_port == dst_port) {
                listener = &sockets[i];
                break;
            }
        }
        if (!listener) return;   /* No listener on this port */

        /* Allocate a new socket for the incoming connection */
        int new_sock = -1;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (!sockets[i].used) { new_sock = i; break; }
        }
        if (new_sock < 0) {
            klog_warn("TCP: no free socket for incoming connection on port %u",
                      dst_port);
            return;
        }

        tcp_socket_t* ns = &sockets[new_sock];
        memset(ns, 0, sizeof(*ns));
        ns->used        = true;
        ns->state       = TCP_SYN_RCVD;
        ns->local_ip    = net_iface.ip;
        ns->local_port  = dst_port;
        ns->remote_ip   = ip_hdr->src;
        ns->remote_port = src_port;
        ns->rx_seq      = seq + 1;   /* SYN consumes one sequence number */
        ns->tx_seq      = rand_seq();

        /* Send SYN-ACK */
        tcp_send_segment(ns, TCP_SYN | TCP_ACK, NULL, 0);
        ns->tx_seq++;                /* SYN-ACK SYN consumes one sequence number */

        kdebug("TCP: SYN from %u.%u.%u.%u:%u → port %u  slot=%d SYN-ACK sent",
               (ip_hdr->src >>  0) & 0xFF, (ip_hdr->src >>  8) & 0xFF,
               (ip_hdr->src >> 16) & 0xFF, (ip_hdr->src >> 24) & 0xFF,
               src_port, dst_port, new_sock);
        return;
    }

    uint8_t  data_off = (hdr->data_offset >> 4) * 4;
    const uint8_t* data = (const uint8_t*)segment + data_off;
    size_t   data_len = (data_off <= len) ? len - data_off : 0;

    switch (s->state) {
    /* -------------------------------------------------------
     * TCP_SYN_RCVD: we sent SYN-ACK, waiting for the final ACK
     * to complete the three-way handshake (passive open).
     * ------------------------------------------------------- */
    case TCP_SYN_RCVD:
        if (flags & TCP_RST) {
            s->tx_rtx_time = 0;
            s->used  = false;
            s->state = TCP_CLOSED;
            break;
        }
        if ((flags & TCP_ACK) && ack == s->tx_seq) {
            tcp_rtx_clear_if_acked(s, ack);   /* clear SYN-ACK retransmit */
            s->state = TCP_ESTABLISHED;
            kdebug("TCP: connection ESTABLISHED on port %u (slot %d)",
                   s->local_port, (int)(s - sockets));
            /* Notify the BSD socket layer so accept() can dequeue it */
            sock_notify_accept(s->local_port,
                               s->remote_ip, s->remote_port,
                               (int)(s - sockets));
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == s->tx_seq) {
                tcp_rtx_clear_if_acked(s, ack);   /* clear SYN retransmit */
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
        /* ACK advances our send window — clear retransmit if fully covered */
        if ((flags & TCP_ACK) && s->tx_rtx_time != 0)
            tcp_rtx_clear_if_acked(s, ack);

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

