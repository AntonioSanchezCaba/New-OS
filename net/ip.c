/*
 * net/ip.c - IPv4, ARP, and ICMP implementation
 */
#include <net/ip.h>
#include <net/ethernet.h>
#include <net/net.h>
#include <net/tcp.h>
#include <kernel.h>
#include <string.h>
#include <memory.h>

/* Sequence number of last received ICMP echo reply (updated by icmp_receive) */
volatile uint16_t g_icmp_reply_seq = 0;

/* =========================================================
 * ARP cache
 * ========================================================= */

#define ARP_CACHE_SIZE 16
#define ARP_TIMEOUT    5000   /* Ticks before entry expires */

typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   timestamp;
    bool       valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* Our next IP identification field (host byte order, monotonic) */
static uint16_t ip4_id = 1;

/* =========================================================
 * IP packet ID counter
 * ========================================================= */

void ip4_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));
    ip4_id = 1;
    kinfo("IPv4: initialized, local IP %u.%u.%u.%u",
          (net_iface.ip >>  0) & 0xFF,
          (net_iface.ip >>  8) & 0xFF,
          (net_iface.ip >> 16) & 0xFF,
          (net_iface.ip >> 24) & 0xFF);
}

/* =========================================================
 * ARP
 * ========================================================= */

void arp_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));
}

static arp_entry_t* arp_lookup(ip4_addr_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return &arp_cache[i];
    }
    return NULL;
}

static void arp_cache_add(ip4_addr_t ip, const mac_addr_t* mac)
{
    /* Find existing entry or free slot */
    int free_idx = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac.b, mac->b, 6);
            return;
        }
        if (!arp_cache[i].valid && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) free_idx = 0;  /* Evict first entry */
    arp_cache[free_idx].ip    = ip;
    memcpy(arp_cache[free_idx].mac.b, mac->b, 6);
    arp_cache[free_idx].valid = true;
}

/* Send ARP request for target_ip */
static void arp_send_request(ip4_addr_t target_ip)
{
    arp_hdr_t req;
    req.htype = htons(1);       /* Ethernet */
    req.ptype = htons(0x0800);  /* IPv4 */
    req.hlen  = 6;
    req.plen  = 4;
    req.oper  = htons(1);       /* Request */
    memcpy(req.sha.b, net_iface.mac.b, 6);
    req.spa = net_iface.ip;
    memset(req.tha.b, 0, 6);
    req.tpa = target_ip;

    eth_send(&eth_broadcast, ETH_TYPE_ARP, &req, sizeof(req));
}

/* Send ARP reply */
static void arp_send_reply(ip4_addr_t target_ip, const mac_addr_t* target_mac)
{
    arp_hdr_t rep;
    rep.htype = htons(1);
    rep.ptype = htons(0x0800);
    rep.hlen  = 6;
    rep.plen  = 4;
    rep.oper  = htons(2);      /* Reply */
    memcpy(rep.sha.b, net_iface.mac.b, 6);
    rep.spa = net_iface.ip;
    memcpy(rep.tha.b, target_mac->b, 6);
    rep.tpa = target_ip;

    eth_send(target_mac, ETH_TYPE_ARP, &rep, sizeof(rep));
}

void arp_receive(const void* pkt, size_t len)
{
    if (len < sizeof(arp_hdr_t)) return;
    const arp_hdr_t* arp = (const arp_hdr_t*)pkt;

    /* Only handle IPv4/Ethernet */
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != 0x0800) return;

    /* Cache sender's mapping */
    arp_cache_add(arp->spa, &arp->sha);

    uint16_t oper = ntohs(arp->oper);
    if (oper == 1 && arp->tpa == net_iface.ip) {
        /* ARP request for our IP - send reply */
        arp_send_reply(arp->spa, &arp->sha);
    }
    /* oper == 2 (reply) is already handled by cache update above */
}

int arp_resolve(ip4_addr_t ip, mac_addr_t* out_mac)
{
    /* Broadcast → broadcast MAC */
    if (ip == 0xFFFFFFFF) {
        memcpy(out_mac->b, eth_broadcast.b, 6);
        return 0;
    }

    arp_entry_t* entry = arp_lookup(ip);
    if (entry) {
        memcpy(out_mac->b, entry->mac.b, 6);
        return 0;
    }

    /* Send ARP request and wait briefly */
    arp_send_request(ip);

    for (int wait = 0; wait < 100000; wait++) {
        __asm__("pause");
        entry = arp_lookup(ip);
        if (entry) {
            memcpy(out_mac->b, entry->mac.b, 6);
            return 0;
        }
    }

    return -1;  /* ARP timeout */
}

void arp_announce(void)
{
    /* Gratuitous ARP: announce our IP to update neighbor caches */
    arp_hdr_t ann;
    ann.htype = htons(1);
    ann.ptype = htons(0x0800);
    ann.hlen  = 6;
    ann.plen  = 4;
    ann.oper  = htons(2);
    memcpy(ann.sha.b, net_iface.mac.b, 6);
    ann.spa = net_iface.ip;
    memcpy(ann.tha.b, eth_broadcast.b, 6);
    ann.tpa = net_iface.ip;
    eth_send(&eth_broadcast, ETH_TYPE_ARP, &ann, sizeof(ann));
}

/* =========================================================
 * IPv4 send
 * ========================================================= */

int ip4_send(ip4_addr_t dst_ip, uint8_t protocol,
             const void* payload, size_t len)
{
    if (!net_iface.up) return -1;

    size_t pkt_len = sizeof(ip4_hdr_t) + len;
    if (pkt_len > NET_BUF_SIZE - sizeof(eth_hdr_t)) return -1;

    net_buf_t* buf = net_alloc_buf();
    if (!buf) return -1;

    ip4_hdr_t* ip = (ip4_hdr_t*)buf->data;
    ip->version_ihl = IP4_VERSION_IHL;
    ip->dscp_ecn    = 0;
    ip->total_len   = htons((uint16_t)pkt_len);
    ip->id          = htons(ip4_id++);
    ip->flags_frag  = 0;
    ip->ttl         = IP4_DEFAULT_TTL;
    ip->protocol    = protocol;
    ip->checksum    = 0;
    ip->src         = net_iface.ip;
    ip->dst         = dst_ip;
    ip->checksum    = net_checksum(ip, sizeof(ip4_hdr_t));

    if (payload && len > 0)
        memcpy(buf->data + sizeof(ip4_hdr_t), payload, len);

    /* Resolve destination MAC */
    ip4_addr_t next_hop = dst_ip;
    /* If not on same subnet, use gateway */
    if ((dst_ip & net_iface.netmask) != (net_iface.ip & net_iface.netmask))
        next_hop = net_iface.gateway;

    mac_addr_t dst_mac;
    int ret = arp_resolve(next_hop, &dst_mac);
    if (ret < 0) {
        net_free_buf(buf);
        return -1;
    }

    ret = eth_send(&dst_mac, ETH_TYPE_IP4, buf->data, pkt_len);
    net_free_buf(buf);
    return ret;
}

/* =========================================================
 * IPv4 receive
 * ========================================================= */

void ip4_receive(const void* pkt, size_t len)
{
    if (len < sizeof(ip4_hdr_t)) return;
    const ip4_hdr_t* ip = (const ip4_hdr_t*)pkt;

    /* Validate header */
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;

    /* Verify checksum */
    if (net_checksum(ip, ihl) != 0) return;

    /* Check destination */
    if (ip->dst != net_iface.ip && ip->dst != 0xFFFFFFFF) return;

    const void* payload = (const uint8_t*)pkt + ihl;
    size_t payload_len  = ntohs(ip->total_len) - ihl;

    switch (ip->protocol) {
    case PROTO_ICMP:
        icmp_receive(ip, payload, payload_len);
        break;
    case PROTO_TCP:
        tcp_receive(ip, payload, payload_len);
        break;
    case PROTO_UDP:
        udp_receive(ip, payload, payload_len);
        break;
    default:
        /* Unknown protocol - drop */
        break;
    }
}

/* =========================================================
 * ICMP
 * ========================================================= */

int icmp_ping(ip4_addr_t target, uint16_t seq)
{
    struct {
        icmp_hdr_t hdr;
        uint8_t    data[32];
    } pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type     = ICMP_ECHO_REQUEST;
    pkt.hdr.code     = 0;
    pkt.hdr.id       = htons(0x1234);
    pkt.hdr.seq      = htons(seq);
    pkt.hdr.checksum = 0;
    /* Fill data with pattern */
    for (int i = 0; i < 32; i++) pkt.data[i] = (uint8_t)i;
    pkt.hdr.checksum = net_checksum(&pkt, sizeof(pkt));

    return ip4_send(target, PROTO_ICMP, &pkt, sizeof(pkt));
}

void icmp_receive(const ip4_hdr_t* ip_hdr, const void* payload, size_t len)
{
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t* icmp = (const icmp_hdr_t*)payload;

    if (icmp->type == ICMP_ECHO_REQUEST) {
        /* Send ICMP echo reply */
        net_buf_t* rep = net_alloc_buf();
        if (!rep) return;

        icmp_hdr_t* reply = (icmp_hdr_t*)rep->data;
        memcpy(rep->data, payload, len);
        reply->type     = ICMP_ECHO_REPLY;
        reply->code     = 0;
        reply->checksum = 0;
        reply->checksum = net_checksum(rep->data, len);

        ip4_send(ip_hdr->src, PROTO_ICMP, rep->data, len);
        net_free_buf(rep);
    }
    if (icmp->type == ICMP_ECHO_REPLY) {
        /* Record last-received reply sequence so netconfig/ping can detect it */
        g_icmp_reply_seq = ntohs(icmp->seq);
    }
}

/* =========================================================
 * IP address configuration API (called by DHCP, netconfig)
 * ========================================================= */

void ip_set_addr(ip4_addr_t addr)
{
    net_iface.ip = addr;
    kinfo("IP: address set to %u.%u.%u.%u",
          (addr >>  0) & 0xFF, (addr >>  8) & 0xFF,
          (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
}

void ip_set_gateway(ip4_addr_t gw)
{
    net_iface.gateway = gw;
}

void ip_set_netmask(ip4_addr_t mask)
{
    net_iface.netmask = mask;
}

ip4_addr_t ip_get_addr(void)
{
    return net_iface.ip;
}
