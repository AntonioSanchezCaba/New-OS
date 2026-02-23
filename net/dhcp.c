/*
 * net/dhcp.c — DHCP client (RFC 2131 / RFC 2132)
 *
 * Sends a DHCPDISCOVER broadcast, waits for DHCPOFFER, sends DHCPREQUEST,
 * waits for DHCPACK. All communication is over UDP port 67→68.
 *
 * Uses the existing UDP/IP/Ethernet layers from the AetherOS network stack.
 */
#include <net/dhcp.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/ethernet.h>
#include <drivers/e1000.h>
#include <drivers/timer.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

/* =========================================================
 * DHCP packet structure (RFC 2131 §2)
 * ========================================================= */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363UL  /* Magic cookie */

/* DHCP op codes */
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5
#define DHCP_NAK       6

/* DHCP option codes */
#define DHCP_OPT_SUBNET       1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS          6
#define DHCP_OPT_HOSTNAME     12
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_REQ    55
#define DHCP_OPT_END          255

#pragma pack(push, 1)
typedef struct {
    uint8_t  op;          /* 1=request, 2=reply */
    uint8_t  htype;       /* 1=Ethernet */
    uint8_t  hlen;        /* 6 for MAC */
    uint8_t  hops;
    uint32_t xid;         /* Transaction ID */
    uint16_t secs;
    uint16_t flags;       /* 0x8000 = broadcast */
    uint32_t ciaddr;      /* Client IP (0 for discover) */
    uint32_t yiaddr;      /* 'Your' IP (from server)  */
    uint32_t siaddr;      /* Server IP */
    uint32_t giaddr;      /* Relay agent IP */
    uint8_t  chaddr[16];  /* Client MAC (padded) */
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} dhcp_packet_t;
#pragma pack(pop)

/* =========================================================
 * Internal state
 * ========================================================= */
static dhcp_lease_t g_lease;
static bool         g_has_lease = false;
static uint32_t     g_xid       = 0xDEAD1234;

/* =========================================================
 * Option helpers
 * ========================================================= */
static int opt_find(const uint8_t* opts, int opts_len,
                    uint8_t code, uint8_t* out, int out_max)
{
    int i = 0;
    while (i < opts_len) {
        if (opts[i] == DHCP_OPT_END) break;
        if (opts[i] == 0) { i++; continue; } /* Pad */
        uint8_t c = opts[i++];
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (c == code) {
            int copy = l < out_max ? l : out_max;
            memcpy(out, opts + i, (size_t)copy);
            return copy;
        }
        i += l;
    }
    return 0;
}

static int opt_append(uint8_t* opts, int pos, uint8_t code,
                       const void* val, uint8_t len)
{
    opts[pos++] = code;
    opts[pos++] = len;
    memcpy(opts + pos, val, len);
    return pos + len;
}

/* =========================================================
 * Build and send a DHCP packet
 * ========================================================= */
static void build_discover(dhcp_packet_t* pkt)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->op     = 1;       /* BOOTREQUEST */
    pkt->htype  = 1;       /* Ethernet */
    pkt->hlen   = 6;
    pkt->xid    = g_xid;
    pkt->flags  = 0x0080;  /* Broadcast flag (network order: big-endian) */
    pkt->magic  = 0x63538263UL; /* little-endian representation of magic */

    /* Our MAC */
    uint8_t* mac = e1000_mac.b;
    memcpy(pkt->chaddr, mac, 6);

    /* Options */
    int pos = 0;
    uint8_t mtype = DHCP_DISCOVER;
    pos = opt_append(pkt->options, pos, DHCP_OPT_MSG_TYPE, &mtype, 1);
    /* Parameter request list */
    uint8_t params[] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER,
                         DHCP_OPT_DNS, DHCP_OPT_LEASE_TIME };
    pos = opt_append(pkt->options, pos, DHCP_OPT_PARAM_REQ,
                     params, sizeof(params));
    /* Hostname */
    const char* hn = "aetheros";
    pos = opt_append(pkt->options, pos, DHCP_OPT_HOSTNAME,
                     hn, (uint8_t)strlen(hn));
    pkt->options[pos++] = DHCP_OPT_END;
}

static void build_request(dhcp_packet_t* pkt,
                           uint32_t offered_ip, uint32_t server_ip)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->op     = 1;
    pkt->htype  = 1;
    pkt->hlen   = 6;
    pkt->xid    = g_xid;
    pkt->flags  = 0x0080;
    pkt->magic  = 0x63538263UL;

    uint8_t* mac = e1000_mac.b;
    memcpy(pkt->chaddr, mac, 6);

    int pos = 0;
    uint8_t mtype = DHCP_REQUEST;
    pos = opt_append(pkt->options, pos, DHCP_OPT_MSG_TYPE, &mtype, 1);
    pos = opt_append(pkt->options, pos, 50, &offered_ip, 4);  /* Requested IP */
    pos = opt_append(pkt->options, pos, DHCP_OPT_SERVER_ID, &server_ip, 4);
    const char* hn = "aetheros";
    pos = opt_append(pkt->options, pos, DHCP_OPT_HOSTNAME,
                     hn, (uint8_t)strlen(hn));
    pkt->options[pos++] = DHCP_OPT_END;
}

/* =========================================================
 * Thin UDP send helper — broadcast
 * ========================================================= */
static void dhcp_send(dhcp_packet_t* pkt)
{
    /* Broadcast: 255.255.255.255 */
    uint32_t bcast = 0xFFFFFFFF;
    udp_send(bcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             (const uint8_t*)pkt, sizeof(*pkt));
}

/* =========================================================
 * Wait for a DHCP reply with matching XID and message type
 * ========================================================= */
static int dhcp_wait(uint8_t want_type, dhcp_packet_t* out,
                      uint32_t timeout_ticks)
{
    uint32_t deadline = timer_get_ticks() + timeout_ticks;

    while (timer_get_ticks() < deadline) {
        /* Poll UDP receive buffer for port 68 */
        uint8_t buf[sizeof(dhcp_packet_t)];
        int n = udp_recv_poll(DHCP_CLIENT_PORT, buf, (int)sizeof(buf));
        if (n < (int)offsetof(dhcp_packet_t, options)) continue;

        dhcp_packet_t* p = (dhcp_packet_t*)buf;
        if (p->xid != g_xid) continue;
        if (p->op  != 2)     continue;  /* Must be BOOTREPLY */
        if (p->magic != 0x63538263UL) continue;

        /* Check message type option */
        uint8_t mtype = 0;
        opt_find(p->options,
                 n - (int)offsetof(dhcp_packet_t, options),
                 DHCP_OPT_MSG_TYPE, &mtype, 1);
        if (mtype != want_type) continue;

        memcpy(out, buf, sizeof(*out));
        return 0;
    }
    return -1; /* Timeout */
}

/* =========================================================
 * Public API
 * ========================================================= */
int dhcp_discover(void)
{
    g_xid++;
    dhcp_packet_t pkt, reply;

    /* Send DISCOVER */
    build_discover(&pkt);
    kinfo("DHCP: sending DISCOVER (xid=%08X)", g_xid);
    dhcp_send(&pkt);

    /* Wait for OFFER (up to 4 seconds) */
    if (dhcp_wait(DHCP_OFFER, &reply, TIMER_FREQ * 4) != 0) {
        klog_warn("DHCP: no OFFER received");
        return -1;
    }

    uint32_t offered_ip = reply.yiaddr;
    uint32_t server_ip  = 0;
    opt_find(reply.options,
             (int)sizeof(reply.options),
             DHCP_OPT_SERVER_ID, (uint8_t*)&server_ip, 4);

    kinfo("DHCP: got OFFER ip=%08X server=%08X", offered_ip, server_ip);

    /* Send REQUEST */
    build_request(&pkt, offered_ip, server_ip);
    dhcp_send(&pkt);

    /* Wait for ACK (up to 4 seconds) */
    if (dhcp_wait(DHCP_ACK, &reply, TIMER_FREQ * 4) != 0) {
        klog_warn("DHCP: no ACK received");
        return -1;
    }

    /* Parse lease options */
    memset(&g_lease, 0, sizeof(g_lease));
    g_lease.ip = reply.yiaddr;

    opt_find(reply.options, (int)sizeof(reply.options),
             DHCP_OPT_SUBNET,     (uint8_t*)&g_lease.netmask,   4);
    opt_find(reply.options, (int)sizeof(reply.options),
             DHCP_OPT_ROUTER,     (uint8_t*)&g_lease.gateway,   4);
    opt_find(reply.options, (int)sizeof(reply.options),
             DHCP_OPT_DNS,        (uint8_t*)&g_lease.dns,        4);
    opt_find(reply.options, (int)sizeof(reply.options),
             DHCP_OPT_LEASE_TIME, (uint8_t*)&g_lease.lease_sec,  4);

    /* Configure IP stack */
    ip_set_addr(g_lease.ip);
    ip_set_gateway(g_lease.gateway);
    ip_set_netmask(g_lease.netmask);

    g_has_lease = true;
    kinfo("DHCP: lease acquired ip=%08X gw=%08X mask=%08X dns=%08X",
          g_lease.ip, g_lease.gateway, g_lease.netmask, g_lease.dns);
    return 0;
}

int dhcp_renew(void) { return dhcp_discover(); }

const dhcp_lease_t* dhcp_get_lease(void)
{
    return g_has_lease ? &g_lease : NULL;
}

bool dhcp_has_lease(void) { return g_has_lease; }
