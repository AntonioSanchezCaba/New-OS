/*
 * scheduler.h - Process scheduler interface
 *
 * Provides the round-robin preemptive scheduler.
 * The scheduler is invoked from the timer interrupt handler.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <types.h>
#include <process.h>
#include <interrupts.h>

/* Default time quantum in timer ticks (10ms per tick = 50ms quantum) */
#define SCHED_DEFAULT_QUANTUM 5

/* Scheduler API */
void scheduler_init(void);
void scheduler_add(process_t* proc);
void scheduler_remove(process_t* proc);
void scheduler_yield(void);
void scheduler_tick(cpu_registers_t* regs);
void scheduler_block(process_t* proc, proc_state_t reason);
void scheduler_unblock(process_t* proc);
process_t* scheduler_next(void);
uint64_t   scheduler_ticks(void);

#endif /* SCHEDULER_H */
