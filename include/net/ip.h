/*
 * include/net/ip.h - IPv4 layer
 */
#ifndef NET_IP_H
#define NET_IP_H

#include <net/net.h>
#include <net/ethernet.h>

/* IPv4 header (20 bytes minimum) */
typedef struct {
    uint8_t  version_ihl;   /* Version (4) + IHL (5 = 20 bytes) */
    uint8_t  dscp_ecn;
    uint16_t total_len;     /* BE: header + payload */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;           /* Source IP, network byte order */
    uint32_t dst;           /* Destination IP, network byte order */
} __attribute__((packed)) ip4_hdr_t;

#define IP4_VERSION_IHL  0x45   /* Version=4, IHL=5 (20 bytes) */
#define IP4_DEFAULT_TTL  64

/* ARP header */
typedef struct {
    uint16_t htype;  /* 1 = Ethernet */
    uint16_t ptype;  /* 0x0800 = IPv4 */
    uint8_t  hlen;   /* 6 = MAC length */
    uint8_t  plen;   /* 4 = IP length */
    uint16_t oper;   /* 1=request, 2=reply */
    mac_addr_t sha;  /* Sender MAC */
    uint32_t   spa;  /* Sender IP */
    mac_addr_t tha;  /* Target MAC */
    uint32_t   tpa;  /* Target IP */
} __attribute__((packed)) arp_hdr_t;

/* ICMP header */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* IP layer API */
void ip4_init(void);
int  ip4_send(ip4_addr_t dst_ip, uint8_t protocol,
              const void* payload, size_t len);
void ip4_receive(const void* pkt, size_t len);

/* ARP */
void arp_init(void);
int  arp_resolve(ip4_addr_t ip, mac_addr_t* out_mac);
void arp_receive(const void* pkt, size_t len);
void arp_announce(void);

/* ICMP (ping) */
int  icmp_ping(ip4_addr_t target, uint16_t seq);
void icmp_receive(const ip4_hdr_t* ip_hdr, const void* payload, size_t len);

#endif /* NET_IP_H */
