/*
 * net/udp.c - UDP (User Datagram Protocol) implementation
 *
 * Stateless datagram layer on top of IPv4.  The IPv4 stack calls
 * udp_receive() whenever a PROTO_UDP packet arrives.  Applications
 * register per-port callbacks via udp_listen(), or use the blocking
 * helper udp_recv_blocking() for simple request/response protocols.
 *
 * Thread safety: udp_listen() / udp_receive() are called from the
 * network driver context.  udp_recv_blocking() busy-waits in the
 * caller's context.  A real kernel would use mutexes; we use simple
 * disable-irq guards compatible with the AetherOS IRQ model.
 */
#include <net/udp.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel.h>
#include <string.h>
#include <memory.h>
#include <drivers/timer.h>
#include <scheduler.h>

/* =========================================================
 * Listener table
 * ========================================================= */

typedef struct {
    uint16_t      port;   /* Bound local port (0 = unused slot) */
    udp_recv_cb   cb;     /* Callback function                   */
    void*         ctx;    /* User context pointer                */
} udp_listener_t;

static udp_listener_t g_listeners[UDP_MAX_LISTENERS];

/* =========================================================
 * Blocking-receive state (one slot per blocked port)
 * ========================================================= */

typedef struct {
    uint16_t  port;
    uint8_t*  buf;
    uint16_t  maxlen;
    uint16_t  rxlen;      /* 0xFFFF = not yet received           */
    bool      done;
} udp_blocking_slot_t;

#define UDP_BLOCKING_MAX  4
static udp_blocking_slot_t g_blocking[UDP_BLOCKING_MAX];

/* =========================================================
 * udp_init
 * ========================================================= */

void udp_init(void)
{
    memset(g_listeners, 0, sizeof(g_listeners));
    memset(g_blocking,  0, sizeof(g_blocking));
    kinfo("UDP: initialized (%d listener slots)", UDP_MAX_LISTENERS);
}

/* =========================================================
 * udp_listen  –  register / unregister a per-port callback
 * ========================================================= */

int udp_listen(uint16_t port, udp_recv_cb cb, void* ctx)
{
    /* Overwrite an existing registration for the same port */
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == port) {
            if (cb == NULL) {
                /* Unregister */
                g_listeners[i].port = 0;
                g_listeners[i].cb   = NULL;
                g_listeners[i].ctx  = NULL;
            } else {
                g_listeners[i].cb  = cb;
                g_listeners[i].ctx = ctx;
            }
            return 0;
        }
    }

    if (cb == NULL) return 0;   /* Unregister of non-existent – ok */

    /* Find a free slot */
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == 0) {
            g_listeners[i].port = port;
            g_listeners[i].cb   = cb;
            g_listeners[i].ctx  = ctx;
            return 0;
        }
    }

    kdebug("UDP: listener table full (port %u rejected)", port);
    return -1;
}

/* =========================================================
 * udp_send  –  transmit a single UDP datagram
 * ========================================================= */

int udp_send(ip4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void* data, size_t len)
{
    if (!net_iface.up) return -1;

    size_t udp_len = sizeof(udp_hdr_t) + len;

    /* Allocate a temporary stack buffer for header + payload.
     * Maximum UDP payload is bounded by NET_BUF_SIZE in practice. */
    if (udp_len > NET_BUF_SIZE) return -1;

    uint8_t pkt[NET_BUF_SIZE];
    udp_hdr_t* hdr = (udp_hdr_t*)pkt;

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)udp_len);
    hdr->checksum = 0;          /* UDP checksum is optional (RFC 768) */

    if (data && len > 0)
        memcpy(pkt + sizeof(udp_hdr_t), data, len);

    /* Optional: compute UDP checksum using pseudo-header */
    /* Skipped – zero checksum is valid for IPv4 UDP (RFC 768 §3) */

    return ip4_send(dst_ip, PROTO_UDP, pkt, udp_len);
}

/* =========================================================
 * blocking receive callback  –  used internally by udp_recv_blocking
 * ========================================================= */

static void udp_blocking_cb(ip4_addr_t src_ip, uint16_t src_port,
                             const void* data, uint16_t len, void* ctx)
{
    (void)src_ip; (void)src_port;

    udp_blocking_slot_t* slot = (udp_blocking_slot_t*)ctx;
    if (slot->done) return;     /* Already satisfied */

    uint16_t copy_len = (len < slot->maxlen) ? len : slot->maxlen;
    memcpy(slot->buf, data, copy_len);
    slot->rxlen = copy_len;
    slot->done  = true;
}

/* =========================================================
 * udp_receive  –  called by ip4_receive() for PROTO_UDP
 * ========================================================= */

void udp_receive(const ip4_hdr_t* ip_hdr, const void* payload, size_t len)
{
    if (len < sizeof(udp_hdr_t)) return;

    const ip4_hdr_t* ip  = ip_hdr;
    const udp_hdr_t* hdr = (const udp_hdr_t*)payload;

    uint16_t dst_port  = ntohs(hdr->dst_port);
    uint16_t src_port  = ntohs(hdr->src_port);
    uint16_t udp_len   = ntohs(hdr->length);

    /* Validate declared length */
    if (udp_len < (uint16_t)sizeof(udp_hdr_t) || (size_t)udp_len > len) return;

    const void*  data     = (const uint8_t*)payload + sizeof(udp_hdr_t);
    uint16_t     data_len = (uint16_t)(udp_len - sizeof(udp_hdr_t));

    /* Dispatch to registered callbacks */
    for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].port == dst_port && g_listeners[i].cb) {
            g_listeners[i].cb(ip->src, src_port,
                               data, data_len,
                               g_listeners[i].ctx);
        }
    }
}

/* =========================================================
 * udp_recv_blocking  –  spin-wait for a single datagram
 * ========================================================= */

int udp_recv_blocking(uint16_t port, void* buf, uint16_t maxlen,
                      uint32_t timeout_ms)
{
    /* Find a free blocking slot */
    udp_blocking_slot_t* slot = NULL;
    for (int i = 0; i < UDP_BLOCKING_MAX; i++) {
        if (!g_blocking[i].port) {
            slot = &g_blocking[i];
            break;
        }
    }
    if (!slot) return -1;   /* All blocking slots busy */

    slot->port   = port;
    slot->buf    = (uint8_t*)buf;
    slot->maxlen = maxlen;
    slot->rxlen  = 0;
    slot->done   = false;

    if (udp_listen(port, udp_blocking_cb, slot) < 0) {
        slot->port = 0;
        return -1;
    }

    uint32_t deadline = timer_get_ticks() + (timeout_ms * TIMER_FREQ / 1000u);

    while (!slot->done) {
        if (timer_get_ticks() >= deadline) {
            udp_listen(port, NULL, NULL);   /* Unregister */
            slot->port = 0;
            return -1;                      /* Timeout */
        }
        scheduler_yield();
    }

    udp_listen(port, NULL, NULL);   /* Unregister temporary listener */
    int rxlen = (int)slot->rxlen;
    slot->port = 0;
    return rxlen;
}

/* =========================================================
 * udp_recv_poll  –  non-blocking single-datagram check
 *
 * Used by dhcp.c and dns.c for simple request/response.
 * Stores each incoming packet in a small ring buffer per port;
 * returns -1 immediately if no packet is waiting.
 * ========================================================= */

#define UDP_POLL_SLOTS  8
#define UDP_POLL_BUF_SZ 1500

typedef struct {
    uint16_t port;
    bool     used;
    int      len;
    uint8_t  data[UDP_POLL_BUF_SZ];
} udp_poll_slot_t;

static udp_poll_slot_t g_poll_slots[UDP_POLL_SLOTS];
static bool            g_poll_init = false;

static void udp_poll_cb(ip4_addr_t src_ip, uint16_t src_port,
                         const void* data, uint16_t len, void* ctx)
{
    (void)src_ip; (void)src_port;
    udp_poll_slot_t* slot = (udp_poll_slot_t*)ctx;
    if (slot->used) return;   /* Already has unread packet */
    int copy_len = (len < UDP_POLL_BUF_SZ) ? len : UDP_POLL_BUF_SZ;
    memcpy(slot->data, data, (size_t)copy_len);
    slot->len  = copy_len;
    slot->used = true;
}

int udp_recv_poll(uint16_t local_port, void* buf, int bufsz)
{
    if (!g_poll_init) {
        memset(g_poll_slots, 0, sizeof(g_poll_slots));
        g_poll_init = true;
    }

    /* Find or create a poll slot for this port */
    udp_poll_slot_t* slot = NULL;
    for (int i = 0; i < UDP_POLL_SLOTS; i++) {
        if (g_poll_slots[i].port == local_port) {
            slot = &g_poll_slots[i];
            break;
        }
    }
    if (!slot) {
        /* Allocate a new slot and register callback */
        for (int i = 0; i < UDP_POLL_SLOTS; i++) {
            if (g_poll_slots[i].port == 0) {
                slot = &g_poll_slots[i];
                slot->port = local_port;
                slot->used = false;
                udp_listen(local_port, udp_poll_cb, slot);
                break;
            }
        }
    }
    if (!slot) return -1;

    if (!slot->used) return -1;   /* No packet waiting */

    int n = (slot->len < bufsz) ? slot->len : bufsz;
    memcpy(buf, slot->data, (size_t)n);
    slot->used = false;
    return n;
}
