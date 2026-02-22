/*
 * drivers/timer.h - PIT (Programmable Interval Timer) driver interface
 */
#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H

#include <types.h>

/* PIT I/O ports */
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL1  0x41
#define PIT_CHANNEL2  0x42
#define PIT_CMD       0x43

/* PIT command byte fields */
#define PIT_CMD_BINARY    0x00
#define PIT_CMD_BCD       0x01
#define PIT_CMD_MODE0     0x00  /* Interrupt on terminal count */
#define PIT_CMD_MODE2     0x04  /* Rate generator */
#define PIT_CMD_MODE3     0x06  /* Square wave */
#define PIT_CMD_LSB_MSB   0x30  /* Access: lo byte then hi byte */
#define PIT_CMD_CH0       0x00

/* PIT base frequency */
#define PIT_BASE_FREQ   1193182

/* Timer frequency in Hz (100 = 10ms per tick) */
#define TIMER_FREQ      100

/* Timer API */
void     timer_init(uint32_t freq);
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
void     timer_sleep_ticks(uint64_t ticks);
void     timer_sleep_ms(uint64_t ms);
void     timer_irq_handler(void* regs);
void     timer_register_callback(void (*cb)(uint64_t ticks));

/* Alias used throughout drivers/services/networking */
static inline uint64_t timer_get_ticks(void) { return timer_ticks(); }

#endif /* DRIVERS_TIMER_H */
