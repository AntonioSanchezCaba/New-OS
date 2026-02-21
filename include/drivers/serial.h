/*
 * drivers/serial.h - Serial (UART 16550) port driver interface
 *
 * Used primarily for kernel debugging output.
 * COM1 = 0x3F8 at 115200 baud by default.
 */
#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <types.h>

/* COM port base addresses */
#define COM1_PORT   0x3F8
#define COM2_PORT   0x2F8
#define COM3_PORT   0x3E8
#define COM4_PORT   0x2E8

/* UART register offsets */
#define UART_DATA       0  /* Data register (R/W) */
#define UART_IER        1  /* Interrupt enable register */
#define UART_IIR        2  /* Interrupt identification register (R) */
#define UART_FCR        2  /* FIFO control register (W) */
#define UART_LCR        3  /* Line control register */
#define UART_MCR        4  /* Modem control register */
#define UART_LSR        5  /* Line status register */
#define UART_MSR        6  /* Modem status register */
#define UART_SCRATCH    7  /* Scratch register */

/* LSR bits */
#define UART_LSR_DATA_READY  0x01
#define UART_LSR_THRE        0x20  /* Transmit Holding Register Empty */

/* DLAB registers (when LCR bit 7 = 1) */
#define UART_DLL        0  /* Divisor latch low */
#define UART_DLH        1  /* Divisor latch high */

/* LCR bits */
#define UART_LCR_8N1    0x03  /* 8 data bits, no parity, 1 stop */
#define UART_LCR_DLAB   0x80  /* Divisor latch access */

/* Baud rate divisors (from 115200) */
#define UART_BAUD_115200  1
#define UART_BAUD_57600   2
#define UART_BAUD_9600    12

/* Serial driver API */
void   serial_init(uint16_t port, uint16_t baud_divisor);
void   serial_putchar(uint16_t port, char c);
void   serial_puts(uint16_t port, const char* str);
void   serial_printf(uint16_t port, const char* fmt, ...);
char   serial_getchar(uint16_t port);
bool   serial_has_data(uint16_t port);
bool   serial_tx_ready(uint16_t port);

/* Convenience wrappers for COM1 */
void   debug_putchar(char c);
void   debug_puts(const char* str);
void   debug_printf(const char* fmt, ...);

#endif /* DRIVERS_SERIAL_H */
