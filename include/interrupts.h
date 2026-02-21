/*
 * interrupts.h - Interrupt and exception handling interfaces
 *
 * Covers IDT setup, PIC remapping, interrupt registration, and the
 * CPU register frame saved on every interrupt/exception entry.
 */
#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <types.h>

/* ========== CPU Register Frame ========== */

/*
 * Layout of the register state saved on the stack when an interrupt fires.
 * The ISR stubs push these registers before calling the C handler.
 */
typedef struct {
    /* Saved by ISR stub (pushed in order) */
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx;
    uint64_t rcx, rbx, rax;

    /* Interrupt vector and optional error code (pushed by stub) */
    uint64_t int_no;
    uint64_t err_code;

    /* Pushed automatically by CPU on interrupt */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} PACKED cpu_registers_t;

/* ========== IDT ========== */

/* IDT entry types */
#define IDT_TYPE_TASK      0x5
#define IDT_TYPE_INT16     0x6
#define IDT_TYPE_TRAP16    0x7
#define IDT_TYPE_INT32     0xE   /* 64-bit interrupt gate */
#define IDT_TYPE_TRAP32    0xF   /* 64-bit trap gate */

#define IDT_FLAG_PRESENT   (1 << 7)
#define IDT_FLAG_RING0     (0 << 5)
#define IDT_FLAG_RING3     (3 << 5)

/* IDT entry descriptor (16 bytes) */
typedef struct {
    uint16_t offset_low;    /* bits 0-15 of handler address */
    uint16_t selector;      /* code segment selector */
    uint8_t  ist;           /* interrupt stack table (0 = legacy) */
    uint8_t  type_attr;     /* type and attributes */
    uint16_t offset_mid;    /* bits 16-31 */
    uint32_t offset_high;   /* bits 32-63 */
    uint32_t zero;
} PACKED idt_entry_t;

/* IDT pointer passed to LIDT instruction */
typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED idt_ptr_t;

/* Number of IDT entries */
#define IDT_ENTRIES 256

/* CPU exception vectors */
#define EXC_DIVIDE_BY_ZERO       0
#define EXC_DEBUG                1
#define EXC_NMI                  2
#define EXC_BREAKPOINT           3
#define EXC_OVERFLOW             4
#define EXC_BOUND_RANGE          5
#define EXC_INVALID_OPCODE       6
#define EXC_DEVICE_NOT_AVAILABLE 7
#define EXC_DOUBLE_FAULT         8
#define EXC_COPROCESSOR_SEG      9
#define EXC_INVALID_TSS          10
#define EXC_SEGMENT_NOT_PRESENT  11
#define EXC_STACK_FAULT          12
#define EXC_GENERAL_PROTECTION   13
#define EXC_PAGE_FAULT           14
#define EXC_FPU_ERROR            16
#define EXC_ALIGNMENT_CHECK      17
#define EXC_MACHINE_CHECK        18
#define EXC_SIMD_EXCEPTION       19

/* IRQ vectors (remapped to 0x20-0x2F) */
#define IRQ_BASE     0x20
#define IRQ_TIMER    (IRQ_BASE + 0)
#define IRQ_KEYBOARD (IRQ_BASE + 1)
#define IRQ_COM2     (IRQ_BASE + 3)
#define IRQ_COM1     (IRQ_BASE + 4)
#define IRQ_ATA1     (IRQ_BASE + 14)
#define IRQ_ATA2     (IRQ_BASE + 15)

/* Syscall interrupt vector */
#define INT_SYSCALL  0x80

/* IDT API */
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector,
                  uint8_t flags);

/* ========== PIC (8259) ========== */

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
void pic_disable(void);

/* ========== Interrupt handler registration ========== */

typedef void (*irq_handler_t)(cpu_registers_t* regs);

void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);

/* Main interrupt dispatcher (called from ISR stubs) */
void interrupt_dispatch(cpu_registers_t* regs);

/* Exception message table */
extern const char* exception_messages[32];

/* ========== GDT / TSS ========== */

/* GDT segment selectors */
#define GDT_NULL_SEG    0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* GDT entry */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED gdt_entry_t;

/* TSS (Task State Segment) for ring 0 stack on interrupt */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;       /* kernel stack pointer */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     /* interrupt stack table */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} PACKED tss_t;

/* GDT API */
void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);

#endif /* INTERRUPTS_H */
