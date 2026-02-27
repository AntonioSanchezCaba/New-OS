/*
 * scheduler/scheduler.c - Preemptive round-robin process scheduler
 *
 * The scheduler maintains a circular ready queue of process_t structs.
 * On each timer tick, scheduler_tick() is called from the timer IRQ handler.
 * If the current process has exhausted its time quantum, the next process
 * in the ready queue is selected and a context switch is performed.
 *
 * Blocking/unblocking:
 *   scheduler_block()   - remove from ready queue, change state
 *   scheduler_unblock() - re-add to ready queue, mark READY
 *   scheduler_yield()   - voluntarily give up the CPU (reschedule immediately)
 */
#include <scheduler.h>
#include <process.h>
#include <interrupts.h>
#include <memory.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* Ready queue: circular linked list via a separate next_ready pointer
 * We re-use the process->next field conceptually via an array. */
#define MAX_READY 256

static process_t* ready_queue[MAX_READY];
static int ready_head  = 0;  /* Next to dequeue */
static int ready_tail  = 0;  /* Next to enqueue */
static int ready_count = 0;

/* Global tick counter (incremented by timer_irq_handler) */
static volatile uint64_t sched_ticks = 0;

/* The idle process (always schedulable) */
static process_t* idle_proc = NULL;

/* ============================================================
 * Ready queue helpers
 * ============================================================ */

static bool ready_queue_empty(void) { return ready_count == 0; }

static void ready_enqueue(process_t* proc)
{
    if (ready_count >= MAX_READY) {
        kwarn("Scheduler: ready queue full!");
        return;
    }
    ready_queue[ready_tail] = proc;
    ready_tail = (ready_tail + 1) % MAX_READY;
    ready_count++;
    proc->state = PROC_STATE_READY;
}

static process_t* ready_dequeue(void)
{
    if (ready_count == 0) return idle_proc;
    process_t* proc = ready_queue[ready_head];
    ready_head = (ready_head + 1) % MAX_READY;
    ready_count--;
    return proc;
}

/* ============================================================
 * scheduler_init - set up the scheduler and idle process
 * ============================================================ */

void scheduler_init(void)
{
    ready_head  = 0;
    ready_tail  = 0;
    ready_count = 0;
    sched_ticks = 0;

    /* Create the idle process (kernel task, runs when no one else is ready) */
    idle_proc = process_create("idle", idle_process, true);
    if (!idle_proc) {
        kpanic("scheduler_init: failed to create idle process");
    }
    /* Do NOT add idle to the ready queue; it is selected as fallback */

    kinfo("Scheduler initialized (quantum=%u ticks)", SCHED_DEFAULT_QUANTUM);
}

/* ============================================================
 * scheduler_add / scheduler_remove
 * ============================================================ */

void scheduler_add(process_t* proc)
{
    if (!proc) return;
    ready_enqueue(proc);
    kdebug("Scheduler: added PID=%u '%s'", proc->pid, proc->name);
}

void scheduler_remove(process_t* proc)
{
    /* Remove from ready queue by finding and shifting */
    for (int i = 0; i < ready_count; i++) {
        int idx = (ready_head + i) % MAX_READY;
        if (ready_queue[idx] == proc) {
            /* Shift remaining entries */
            for (int j = i; j < ready_count - 1; j++) {
                int a = (ready_head + j)     % MAX_READY;
                int b = (ready_head + j + 1) % MAX_READY;
                ready_queue[a] = ready_queue[b];
            }
            ready_tail = (ready_tail - 1 + MAX_READY) % MAX_READY;
            ready_count--;
            return;
        }
    }
}

/* ============================================================
 * scheduler_tick - called from timer IRQ; does preemptive switching
 * ============================================================ */

void scheduler_tick(cpu_registers_t* regs)
{
    (void)regs;
    sched_ticks++;

    if (!current_process) {
        /* No current process - select one and start it */
        process_t* next = ready_dequeue();
        if (!next) return;

        current_process = next;
        current_process->state = PROC_STATE_RUNNING;
        current_process->time_slice = SCHED_DEFAULT_QUANTUM;

        tss_set_kernel_stack(current_process->kernel_stack +
                             current_process->kernel_stack_size);
        vmm_switch_address_space(current_process->address_space);
        return;
    }

    /* Decrement time slice */
    if (current_process->time_slice > 0) {
        current_process->time_slice--;
        current_process->total_ticks++;

        if (current_process->time_slice > 0) {
            return; /* Still has time remaining */
        }
    }

    /* Time quantum exhausted: context switch to next process */
    process_t* old = current_process;

    /* Re-enqueue old process if it's still runnable */
    if (old->state == PROC_STATE_RUNNING) {
        old->state = PROC_STATE_READY;
        ready_enqueue(old);
    }

    /* Pick next process */
    process_t* next = ready_dequeue();
    if (!next) next = idle_proc;

    current_process = next;
    current_process->state = PROC_STATE_RUNNING;
    current_process->time_slice = SCHED_DEFAULT_QUANTUM;

    /* Update TSS kernel stack for the new process */
    tss_set_kernel_stack(current_process->kernel_stack +
                         current_process->kernel_stack_size);

    /* Switch address space if different */
    if (old->address_space != current_process->address_space) {
        vmm_switch_address_space(current_process->address_space);
    }

    /* Perform context switch (save old, restore new) */
    context_switch(&old->context, &current_process->context);
}

/* ============================================================
 * scheduler_yield - voluntarily give up the CPU
 * ============================================================ */

void scheduler_yield(void)
{
    /* Disable interrupts to safely manipulate the ready queue */
    cpu_cli();

    if (!current_process) {
        cpu_sti();
        return;
    }

    process_t* old = current_process;

    if (old->state == PROC_STATE_RUNNING) {
        old->state = PROC_STATE_READY;
        ready_enqueue(old);
    }

    process_t* next = ready_dequeue();
    if (!next) next = idle_proc;

    current_process = next;
    current_process->state = PROC_STATE_RUNNING;
    current_process->time_slice = SCHED_DEFAULT_QUANTUM;

    tss_set_kernel_stack(current_process->kernel_stack +
                         current_process->kernel_stack_size);

    if (old->address_space != current_process->address_space) {
        vmm_switch_address_space(current_process->address_space);
    }

    cpu_sti();
    context_switch(&old->context, &current_process->context);
}

/* ============================================================
 * scheduler_block / scheduler_unblock
 * ============================================================ */

void scheduler_block(process_t* proc, proc_state_t reason)
{
    if (!proc) return;
    proc->state = reason;
    scheduler_remove(proc);
}

void scheduler_unblock(process_t* proc)
{
    if (!proc) return;
    if (proc->state == PROC_STATE_READY ||
        proc->state == PROC_STATE_RUNNING) return;

    ready_enqueue(proc);
}

process_t* scheduler_next(void)
{
    if (ready_queue_empty()) return idle_proc;
    return ready_queue[ready_head];
}

uint64_t scheduler_ticks(void)
{
    return sched_ticks;
}
