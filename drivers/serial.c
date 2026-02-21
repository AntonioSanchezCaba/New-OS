/*
 * drivers/serial.c - 16550 UART serial port driver
 *
 * Used for kernel debug output to a host terminal via QEMU -serial stdio.
 * COM1 at 0x3F8 is initialized at 115200 baud, 8N1.
 */
#include <drivers/serial.h>
#include <kernel.h>
#include <types.h>
#include <stdarg.h>

extern int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

/*
 * serial_init - initialize a UART port.
 * @port:         base I/O address (e.g. COM1_PORT = 0x3F8)
 * @baud_divisor: 115200 / desired_baud (1 = 115200)
 */
void serial_init(uint16_t port, uint16_t baud_divisor)
{
    outb(port + UART_IER, 0x00);         /* Disable all interrupts */

    /* Set DLAB to configure baud rate */
    outb(port + UART_LCR, UART_LCR_DLAB);
    outb(port + UART_DLL, (uint8_t)(baud_divisor & 0xFF));
    outb(port + UART_DLH, (uint8_t)((baud_divisor >> 8) & 0xFF));

    /* 8N1, clear DLAB */
    outb(port + UART_LCR, UART_LCR_8N1);

    /* Enable and reset FIFOs, trigger at 14 bytes */
    outb(port + UART_FCR, 0xC7);

    /* Enable RTS/DTR */
    outb(port + UART_MCR, 0x0B);

    /* Enable receive interrupts (optional, not used for polling) */
    outb(port + UART_IER, 0x01);
}

bool serial_tx_ready(uint16_t port)
{
    return (inb(port + UART_LSR) & UART_LSR_THRE) != 0;
}

bool serial_has_data(uint16_t port)
{
    return (inb(port + UART_LSR) & UART_LSR_DATA_READY) != 0;
}

void serial_putchar(uint16_t port, char c)
{
    /* Wait until TX holding register is empty */
    while (!serial_tx_ready(port)) {
        cpu_pause();
    }
    outb(port + UART_DATA, (uint8_t)c);
}

void serial_puts(uint16_t port, const char* str)
{
    while (*str) {
        if (*str == '\n') serial_putchar(port, '\r');
        serial_putchar(port, *str++);
    }
}

void serial_printf(uint16_t port, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(port, buf);
}

char serial_getchar(uint16_t port)
{
    while (!serial_has_data(port)) {
        cpu_pause();
    }
    return (char)inb(port + UART_DATA);
}

/* Convenience wrappers for COM1 */
void debug_putchar(char c)               { serial_putchar(COM1_PORT, c); }
void debug_puts(const char* str)         { serial_puts(COM1_PORT, str); }
void debug_printf(const char* fmt, ...)  {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(COM1_PORT, buf);
}
