/*
 * interrupts/handlers.c - Interrupt and exception dispatch
 *
 * interrupt_dispatch() is called from isr_common (isr.asm) with a pointer
 * to the cpu_registers_t frame saved on the kernel stack.
 *
 * It dispatches:
 *   - CPU exceptions (0-31) to their handlers
 *   - Hardware IRQs (32-47) to registered handlers
 *   - Syscall (128) to the syscall handler
 */
#include <interrupts.h>
#include <memory.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <kernel.h>
#include <drivers/vga.h>
#include <string.h>

/* Table of registered IRQ handlers */
static irq_handler_t irq_handlers[16] = { NULL };

/*
 * irq_register_handler - register a C function as the handler for an IRQ.
 * @irq:     IRQ line number (0-15)
 * @handler: function pointer to call when IRQ fires
 */
void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq >= 16) return;
    irq_handlers[irq] = handler;
    pic_clear_mask(irq);  /* Unmask the IRQ in the PIC */
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq >= 16) return;
    irq_handlers[irq] = NULL;
    pic_set_mask(irq);
}

/*
 * handle_exception - handle CPU exceptions (vectors 0-31).
 */
static void handle_exception(cpu_registers_t* regs)
{
    uint64_t vec = regs->int_no;

    if (vec == EXC_PAGE_FAULT) {
        /* Page fault: CR2 holds the faulting address */
        uint64_t fault_addr = read_cr2();
        vmm_handle_page_fault(fault_addr, regs->err_code);
        return;
    }

    /* Print exception info to VGA and serial */
    const char* name = (vec < 32) ? exception_messages[vec] : "Unknown";

    kerror("CPU Exception %llu: %s", vec, name);
    kerror("  Error code: 0x%016llx", regs->err_code);
    kerror("  RIP=0x%016llx  CS=0x%04llx  RFLAGS=0x%016llx",
           regs->rip, regs->cs, regs->rflags);
    kerror("  RSP=0x%016llx  SS=0x%04llx", regs->rsp, regs->ss);
    kerror("  RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx",
           regs->rax, regs->rbx, regs->rcx);
    kerror("  RDX=0x%016llx  RSI=0x%016llx  RDI=0x%016llx",
           regs->rdx, regs->rsi, regs->rdi);

    /* For user-space exceptions, kill the process rather than panic */
    if (regs->cs == (GDT_USER_CODE | 3)) {
        kwarn("Exception in user process (PID %u) - killing process",
              current_process ? current_process->pid : 0);
        if (current_process) {
            process_exit(current_process, -vec);
        }
        scheduler_yield();
        return;
    }

    /* Kernel exception: fatal panic */
    kpanic("Unhandled kernel exception %llu (%s) at RIP=0x%016llx",
           vec, name, regs->rip);
}

/*
 * handle_irq - dispatch a hardware IRQ to its registered handler.
 */
static void handle_irq(cpu_registers_t* regs)
{
    uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);

    /* Check for spurious IRQ (IRQ 7 or IRQ 15) */
    if (irq == 7) {
        uint8_t isr = inb(PIC1_COMMAND);
        if (!(isr & 0x80)) return; /* Spurious */
    }
    if (irq == 15) {
        uint8_t isr = inb(PIC2_COMMAND);
        if (!(isr & 0x80)) {
            pic_send_eoi(7); /* Send EOI to master only */
            return;
        }
    }

    /* Dispatch to registered handler */
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }

    /* Send End-Of-Interrupt signal to PIC */
    pic_send_eoi(irq);
}

/*
 * interrupt_dispatch - main C-level interrupt dispatcher.
 *
 * Called from isr_common in isr.asm with a pointer to the saved register
 * frame on the kernel stack.
 */
void interrupt_dispatch(cpu_registers_t* regs)
{
    uint64_t vec = regs->int_no;

    if (vec < 32) {
        /* CPU exception */
        handle_exception(regs);
    } else if (vec >= IRQ_BASE && vec < IRQ_BASE + 16) {
        /* Hardware IRQ */
        handle_irq(regs);
    } else if (vec == INT_SYSCALL) {
        /* System call via int 0x80 */
        syscall_handler(regs);
    } else {
        /* Unknown vector - ignore or warn */
        kwarn("Unknown interrupt vector %llu", vec);
    }
}
