/*
 * net/net.c - Network stack initialization, buffer pool, and receive dispatch
 */
#include <net/net.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <kernel.h>
#include <string.h>
#include <memory.h>

/* =========================================================
 * Global interface state
 * ========================================================= */

net_iface_t net_iface = {
    .mac      = {{ 0, 0, 0, 0, 0, 0 }},
    .ip       = IP4(10, 0, 2, 15),      /* QEMU default */
    .netmask  = IP4(255, 255, 255, 0),
    .gateway  = IP4(10, 0, 2, 2),
    .up       = false,
    .send     = NULL,
    .poll     = NULL,
};

/* =========================================================
 * Buffer pool (statically allocated)
 * ========================================================= */

#define NET_BUF_COUNT 64

static net_buf_t buf_pool[NET_BUF_COUNT];
static bool      buf_used[NET_BUF_COUNT];

net_buf_t* net_alloc_buf(void)
{
    for (int i = 0; i < NET_BUF_COUNT; i++) {
        if (!buf_used[i]) {
            buf_used[i] = true;
            memset(&buf_pool[i], 0, sizeof(buf_pool[i]));
            return &buf_pool[i];
        }
    }
    klog_warn("net: buffer pool exhausted");
    return NULL;
}

void net_free_buf(net_buf_t* buf)
{
    if (!buf) return;
    int idx = (int)(buf - buf_pool);
    if (idx >= 0 && idx < NET_BUF_COUNT)
        buf_used[idx] = false;
}

/* =========================================================
 * Internet checksum (16-bit one's complement)
 * ========================================================= */

uint16_t net_checksum(const void* data, size_t len)
{
    const uint16_t* p = (const uint16_t*)data;
    uint32_t acc = 0;
    while (len > 1) { acc += *p++; len -= 2; }
    if (len) acc += *(const uint8_t*)p;
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

/* =========================================================
 * Raw send (via NIC driver)
 * ========================================================= */

int net_send_raw(const void* data, size_t len)
{
    if (!net_iface.up || !net_iface.send) return -1;
    return net_iface.send(data, len);
}

/* =========================================================
 * Receive dispatch
 * ========================================================= */

void net_receive(const void* data, size_t len)
{
    /* Pass to Ethernet layer which demuxes by EtherType */
    eth_receive(data, len);
}

/* =========================================================
 * Initialization
 * ========================================================= */

void net_init(void)
{
    memset(buf_used, 0, sizeof(buf_used));
    arp_init();
    ip4_init();
    tcp_init();
    kinfo("Net: stack initialized (IP %u.%u.%u.%u)",
          (net_iface.ip >>  0) & 0xFF,
          (net_iface.ip >>  8) & 0xFF,
          (net_iface.ip >> 16) & 0xFF,
          (net_iface.ip >> 24) & 0xFF);
}
