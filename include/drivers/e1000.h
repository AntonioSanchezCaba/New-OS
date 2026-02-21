/*
 * include/drivers/e1000.h - Intel 82540EM (e1000) Ethernet driver
 */
#ifndef DRIVERS_E1000_H
#define DRIVERS_E1000_H

#include <types.h>
#include <net/net.h>

/* e1000 driver API */
int  e1000_init(void);        /* Returns 0 on success, -1 if no device found */
int  e1000_send(const void* data, size_t len);
void e1000_receive_poll(void);
void e1000_get_mac(mac_addr_t* out);

/* Called from IRQ handler */
void e1000_irq_handler(void* regs);

#endif /* DRIVERS_E1000_H */
