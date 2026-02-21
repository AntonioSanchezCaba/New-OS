/*
 * net/ethernet.c - Ethernet II frame layer
 *
 * Provides:
 *   eth_send()    - build and transmit an Ethernet frame
 *   eth_receive() - demux incoming Ethernet frames by EtherType
 */
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/net.h>
#include <kernel.h>
#include <string.h>
#include <memory.h>

const mac_addr_t eth_broadcast = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};

/* =========================================================
 * Transmit
 * ========================================================= */

int eth_send(const mac_addr_t* dst, uint16_t ethertype,
             const void* payload, size_t payload_len)
{
    if (!net_iface.up || !net_iface.send) return -1;

    size_t frame_len = sizeof(eth_hdr_t) + payload_len;
    if (frame_len > NET_BUF_SIZE) return -1;

    net_buf_t* buf = net_alloc_buf();
    if (!buf) return -1;

    eth_hdr_t* hdr = (eth_hdr_t*)buf->data;
    memcpy(hdr->dst.b, dst->b, 6);
    memcpy(hdr->src.b, net_iface.mac.b, 6);
    hdr->ethertype = htons(ethertype);

    if (payload && payload_len > 0)
        memcpy(buf->data + sizeof(eth_hdr_t), payload, payload_len);

    buf->len = (uint16_t)frame_len;

    int ret = net_iface.send(buf->data, buf->len);
    net_free_buf(buf);
    return ret;
}

/* =========================================================
 * Receive (called from net_receive → eth_receive)
 * ========================================================= */

void eth_receive(const void* frame, size_t len)
{
    if (len < sizeof(eth_hdr_t)) return;

    const eth_hdr_t* hdr = (const eth_hdr_t*)frame;
    uint16_t etype = ntohs(hdr->ethertype);

    const uint8_t* payload = (const uint8_t*)frame + sizeof(eth_hdr_t);
    size_t payload_len = len - sizeof(eth_hdr_t);

    switch (etype) {
    case ETH_TYPE_IP4:
        ip4_receive(payload, payload_len);
        break;
    case ETH_TYPE_ARP:
        arp_receive(payload, payload_len);
        break;
    default:
        /* Unknown EtherType - silently drop */
        break;
    }
}
