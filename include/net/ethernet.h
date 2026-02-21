/*
 * include/net/ethernet.h - Ethernet II frame layer
 */
#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <net/net.h>

/* Ethernet frame header (14 bytes) */
typedef struct {
    mac_addr_t dst;
    mac_addr_t src;
    uint16_t   ethertype;  /* Network byte order */
} __attribute__((packed)) eth_hdr_t;

/* Ethertypes (host byte order values stored as BE in header) */
#define ETH_TYPE_IP4  0x0800
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP6  0x86DD

/* Broadcast address */
extern const mac_addr_t eth_broadcast;

/* Ethernet layer API */
int  eth_send(const mac_addr_t* dst, uint16_t ethertype,
              const void* payload, size_t payload_len);
void eth_receive(const void* frame, size_t len);

#endif /* NET_ETHERNET_H */
