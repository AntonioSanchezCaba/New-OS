/*
 * include/net/dns.h — Minimal DNS resolver (RFC 1035)
 *
 * Resolves A records (IPv4 addresses) via UDP port 53.
 * Uses the DNS server address from the DHCP lease.
 */
#pragma once
#include <types.h>

/* Resolve hostname → IPv4. Returns the address (network byte order),
 * or 0 on failure. Blocks for up to 3 seconds. */
uint32_t dns_resolve(const char* hostname);

/* Set DNS server address (called by DHCP). */
void dns_set_server(uint32_t dns_ip);

/* Get current DNS server address. */
uint32_t dns_get_server(void);

/* Cache entry count (for diagnostics). */
int dns_cache_count(void);
