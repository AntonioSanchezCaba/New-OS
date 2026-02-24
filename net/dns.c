/*
 * net/dns.c — DNS A-record resolver with 32-entry cache
 *
 * Builds a minimal DNS query packet, sends it to the configured server
 * on UDP port 53, then parses the response to extract the first A record.
 */
#include <net/dns.h>
#include <net/net.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <drivers/timer.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

#define DNS_PORT      53
#define DNS_CLIENT_PORT 5353
#define DNS_CACHE_SIZE 32

/* =========================================================
 * Cache
 * ========================================================= */
typedef struct {
    char     name[64];
    uint32_t addr;
    bool     valid;
} dns_cache_entry_t;

static dns_cache_entry_t g_cache[DNS_CACHE_SIZE];
static uint32_t          g_dns_server = 0;
static uint16_t          g_query_id   = 0x1234;

/* =========================================================
 * DNS packet helpers
 * ========================================================= */
#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;
#pragma pack(pop)

/* Encode hostname as DNS labels: "foo.bar" → \3foo\3bar\0 */
static int encode_name(const char* name, uint8_t* out, int max)
{
    int pos = 0;
    while (*name && pos < max - 2) {
        /* Find next label */
        int label_start = pos;
        out[pos++] = 0;  /* Length placeholder */
        int len = 0;
        while (*name && *name != '.' && pos < max - 1) {
            out[pos++] = (uint8_t)*name++;
            len++;
        }
        out[label_start] = (uint8_t)len;
        if (*name == '.') name++;
    }
    out[pos++] = 0;  /* Root label */
    return pos;
}

/* Build a DNS A query. Returns packet length. */
static int build_query(const char* hostname, uint8_t* buf, int buf_max)
{
    if (buf_max < 512) return -1;
    memset(buf, 0, 512);

    dns_header_t* h = (dns_header_t*)buf;
    h->id      = g_query_id++;
    h->flags   = 0x0001;   /* Standard query, recursion desired (big-endian) */
    h->qdcount = 0x0100;   /* 1 question (big-endian) */

    int pos = (int)sizeof(dns_header_t);
    pos += encode_name(hostname, buf + pos, buf_max - pos - 4);

    /* QTYPE A = 1, QCLASS IN = 1 */
    buf[pos++] = 0x00; buf[pos++] = 0x01;
    buf[pos++] = 0x00; buf[pos++] = 0x01;
    return pos;
}

/* Parse DNS response, extract first A record IPv4 address. */
static uint32_t parse_response(const uint8_t* buf, int len)
{
    if (len < (int)sizeof(dns_header_t)) return 0;
    const dns_header_t* h = (const dns_header_t*)buf;

    uint16_t ancount = (uint16_t)((h->ancount >> 8) | (h->ancount << 8));
    if (ancount == 0) return 0;

    /* Skip question section */
    int pos = (int)sizeof(dns_header_t);
    /* Skip QNAME */
    while (pos < len) {
        uint8_t l = buf[pos];
        if (l == 0) { pos++; break; }
        if ((l & 0xC0) == 0xC0) { pos += 2; break; }
        pos += l + 1;
    }
    pos += 4;  /* Skip QTYPE + QCLASS */

    /* Parse answer records */
    for (uint16_t i = 0; i < ancount && pos + 12 <= len; i++) {
        /* Skip NAME (may be pointer) */
        if ((buf[pos] & 0xC0) == 0xC0) pos += 2;
        else {
            while (pos < len && buf[pos]) pos++;
            pos++;
        }
        if (pos + 10 > len) break;

        uint16_t rtype  = (uint16_t)((buf[pos] << 8) | buf[pos+1]);  pos += 2;
        pos += 2;  /* class */
        pos += 4;  /* TTL */
        uint16_t rdlen  = (uint16_t)((buf[pos] << 8) | buf[pos+1]);  pos += 2;

        if (rtype == 1 && rdlen == 4 && pos + 4 <= len) {
            uint32_t addr =
                ((uint32_t)buf[pos]   << 24) |
                ((uint32_t)buf[pos+1] << 16) |
                ((uint32_t)buf[pos+2] <<  8) |
                 (uint32_t)buf[pos+3];
            return addr;  /* Network byte order */
        }
        pos += rdlen;
    }
    return 0;
}

/* =========================================================
 * Public API
 * ========================================================= */
void dns_set_server(uint32_t dns_ip) { g_dns_server = dns_ip; }
uint32_t dns_get_server(void)        { return g_dns_server; }

int dns_cache_count(void)
{
    int n = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; i++)
        if (g_cache[i].valid) n++;
    return n;
}

uint32_t dns_resolve(const char* hostname)
{
    if (!hostname || !hostname[0]) return 0;

    /* Check cache */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_cache[i].valid && strcmp(g_cache[i].name, hostname) == 0)
            return g_cache[i].addr;
    }

    /* Check if it's already an IP literal */
    uint32_t lit = 0;
    int parts = 0;
    const char* p = hostname;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            lit = (lit << 8) | (uint8_t)n;
            parts++;
            if (*p == '.') p++;
        } else { lit = 0; parts = 0; break; }
    }
    if (parts == 4) return lit;

    if (!g_dns_server) {
        klog_warn("DNS: no server configured");
        return 0;
    }

    /* Build and send query; save the query ID for response validation */
    uint16_t sent_id = g_query_id;
    uint8_t qbuf[512];
    int qlen = build_query(hostname, qbuf, sizeof(qbuf));
    if (qlen < 0) return 0;

    udp_send(g_dns_server, DNS_CLIENT_PORT, DNS_PORT,
             qbuf, qlen);

    /* Wait for response */
    uint32_t deadline = timer_get_ticks() + TIMER_FREQ * 3;
    while (timer_get_ticks() < deadline) {
        uint8_t rbuf[512];
        int n = udp_recv_poll(DNS_CLIENT_PORT, rbuf, (int)sizeof(rbuf));
        if (n < (int)sizeof(dns_header_t)) continue;

        /* Validate transaction ID before parsing */
        const dns_header_t* rh = (const dns_header_t*)rbuf;
        if (rh->id != sent_id) continue;

        uint32_t addr = parse_response(rbuf, n);
        if (!addr) continue;

        /* Cache result */
        for (int i = 0; i < DNS_CACHE_SIZE; i++) {
            if (!g_cache[i].valid) {
                g_cache[i].valid = true;
                strncpy(g_cache[i].name, hostname, 63);
                g_cache[i].addr = addr;
                break;
            }
        }
        kinfo("DNS: %s → %u.%u.%u.%u", hostname,
              (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
              (addr >>  8) & 0xFF,  addr        & 0xFF);
        return addr;
    }

    klog_warn("DNS: timeout resolving %s", hostname);
    return 0;
}
