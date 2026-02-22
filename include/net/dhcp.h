/*
 * include/net/dhcp.h — DHCP client (RFC 2131)
 *
 * Implements DORA (Discover → Offer → Request → Ack).
 * On success, configures the kernel IP stack with the assigned address,
 * subnet mask, default gateway, and primary DNS server.
 */
#pragma once
#include <types.h>

typedef struct {
    uint32_t ip;          /* Assigned IP address (network byte order)   */
    uint32_t netmask;     /* Subnet mask (network byte order)            */
    uint32_t gateway;     /* Default gateway (network byte order)        */
    uint32_t dns;         /* Primary DNS server (network byte order)     */
    uint32_t lease_sec;   /* Lease duration in seconds                   */
} dhcp_lease_t;

/* Performs DHCP discovery on the primary network interface.
 * Blocks for up to 4 seconds. Returns 0 on success, -1 on failure. */
int  dhcp_discover(void);

/* Renew an existing lease. Returns 0 on success. */
int  dhcp_renew(void);

/* Get current lease (valid only if dhcp_discover returned 0) */
const dhcp_lease_t* dhcp_get_lease(void);

/* Returns true if we have a valid DHCP lease */
bool dhcp_has_lease(void);
