/*
 * include/net/net.h - Networking stack main header
 */
#ifndef NET_NET_H
#define NET_NET_H

#include <types.h>

/* MAC address */
typedef struct { uint8_t b[6]; } mac_addr_t;

/* IPv4 address (network byte order) */
typedef uint32_t ip4_addr_t;

/* IP address construction */
#define IP4(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Byte-order conversion */
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | (((x >> 16) & 0xFF) << 8) |
           (((x >> 8) & 0xFF) << 16) | ((x & 0xFF) << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Network buffer */
#define NET_BUF_SIZE 2048

typedef struct net_buf {
    uint8_t  data[NET_BUF_SIZE];
    size_t   len;
    uint16_t head;    /* Current read offset */
    struct net_buf* next;
} net_buf_t;

/* Network interface */
typedef struct {
    mac_addr_t mac;
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    bool       up;

    /* Driver callbacks */
    int (*send)(const void* data, size_t len);
    void (*poll)(void);
} net_iface_t;

extern net_iface_t net_iface;

/* Network stack initialization */
void net_init(void);

/* Called by NIC driver when a packet arrives */
void net_receive(const void* data, size_t len);

/* Packet transmission */
int net_send_raw(const void* data, size_t len);

/* Buffer pool */
net_buf_t* net_alloc_buf(void);
void       net_free_buf(net_buf_t* buf);

/* Internet checksum */
uint16_t net_checksum(const void* data, size_t len);

/* IP/protocol numbers */
#define PROTO_ICMP 0x01
#define PROTO_TCP  0x06
#define PROTO_UDP  0x11

/* Well-known ports */
#define PORT_HTTP  80
#define PORT_DNS   53

#endif /* NET_NET_H */
