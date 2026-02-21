/*
 * drivers/timer.c - PIT (Programmable Interval Timer) driver
 *
 * Programs the Intel 8253/8254 PIT channel 0 to fire IRQ0 at the
 * requested frequency. Each tick increments a global counter.
 *
 * The timer IRQ handler also:
 *   - Wakes sleeping processes whose sleep_until <= current tick
 *   - Calls scheduler_tick() to implement preemptive scheduling
 */
#include <drivers/timer.h>
#include <interrupts.h>
#include <scheduler.h>
#include <process.h>
#include <kernel.h>
#include <types.h>

/* Global tick counter */
static volatile uint64_t tick_count = 0;

/* Optional registered callback */
static void (*timer_callback)(uint64_t) = NULL;

/*
 * timer_irq_handler - called on each PIT tick (IRQ 0 -> INT 0x20).
 * The PIC EOI is sent by handle_irq() in handlers.c.
 */
void timer_irq_handler(void* regs_ptr)
{
    tick_count++;

    /* Invoke optional registered callback */
    if (timer_callback) {
        timer_callback(tick_count);
    }

    /* Wake any sleeping processes */
    extern process_t* process_list;
    process_t* proc = process_list;
    while (proc) {
        if (proc->state == PROC_STATE_SLEEPING &&
            proc->sleep_until <= tick_count) {
            scheduler_unblock(proc);
        }
        proc = proc->next;
    }

    /* Drive the round-robin scheduler */
    scheduler_tick((cpu_registers_t*)regs_ptr);
}

/*
 * timer_init - configure the PIT to fire at @freq Hz.
 * @freq: desired tick rate in Hz (e.g. 100 for 10ms ticks)
 */
void timer_init(uint32_t freq)
{
    /* Divisor calculation: PIT_BASE_FREQ / desired_freq */
    uint32_t divisor = PIT_BASE_FREQ / freq;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1)      divisor = 1;

    /* Send command: channel 0, lo+hi byte access, mode 3 (square wave) */
    outb(PIT_CMD, PIT_CMD_CH0 | PIT_CMD_LSB_MSB | PIT_CMD_MODE3 | PIT_CMD_BINARY);

    /* Send divisor (lo byte first, then hi byte) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Register our IRQ handler and enable IRQ0 */
    irq_register_handler(0, (irq_handler_t)timer_irq_handler);

    kinfo("PIT timer: %u Hz (divisor=%u)", freq, divisor);
}

uint64_t timer_ticks(void) { return tick_count; }
uint64_t timer_ms(void)    { return tick_count * 1000 / TIMER_FREQ; }

void timer_sleep_ticks(uint64_t ticks)
{
    if (!current_process) {
        /* No scheduler yet - busy wait */
        uint64_t end = tick_count + ticks;
        while (tick_count < end) cpu_pause();
        return;
    }

    current_process->sleep_until = tick_count + ticks;
    scheduler_block(current_process, PROC_STATE_SLEEPING);
    scheduler_yield();
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t ticks = (ms * TIMER_FREQ) / 1000;
    if (ticks == 0) ticks = 1;
    timer_sleep_ticks(ticks);
}

void timer_register_callback(void (*cb)(uint64_t ticks))
{
    timer_callback = cb;
}
