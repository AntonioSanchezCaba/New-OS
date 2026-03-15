/*
 * include/drivers/e1000.h - Intel 82540EM (e1000) Ethernet driver
 */
#ifndef DRIVERS_E1000_H
#define DRIVERS_E1000_H

#include <types.h>
#include <net/net.h>

/* Driver statistics */
typedef struct {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_errors;
    uint32_t rx_errors;
} e1000_stats_t;

/* Global MAC address (populated by e1000_init) */
extern mac_addr_t e1000_mac;

/* e1000 driver API */
int  e1000_init(void);        /* Returns 0 on success, -1 if no device found */
int  e1000_send(const void* data, size_t len);
void e1000_receive_poll(void);
void e1000_get_mac(mac_addr_t* out);
void e1000_get_stats(e1000_stats_t* out);

/* Check if network link is up */
bool e1000_link_up(void);

/* Called from IRQ handler */
void e1000_irq_handler(void* regs);

#endif /* DRIVERS_E1000_H */
