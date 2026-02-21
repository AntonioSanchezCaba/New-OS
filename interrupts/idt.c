/*
 * interrupts/idt.c - Interrupt Descriptor Table setup
 *
 * Creates a 256-entry IDT covering all CPU exceptions (0-31),
 * hardware IRQs (32-47), and the syscall vector (128/0x80).
 *
 * Each entry points to the corresponding ISR stub in isr.asm.
 */
#include <interrupts.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* The IDT: 256 entries of 16 bytes each */
static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));

/* IDT pointer for LIDT instruction */
static idt_ptr_t idt_ptr;

/* Exception message strings (for debugging output) */
const char* exception_messages[32] = {
    "Division By Zero",          /*  0 */
    "Debug",                     /*  1 */
    "Non Maskable Interrupt",    /*  2 */
    "Breakpoint",                /*  3 */
    "Overflow",                  /*  4 */
    "Bound Range Exceeded",      /*  5 */
    "Invalid Opcode",            /*  6 */
    "Device Not Available",      /*  7 */
    "Double Fault",              /*  8 */
    "Coprocessor Segment Overrun", /* 9 */
    "Invalid TSS",               /* 10 */
    "Segment Not Present",       /* 11 */
    "Stack-Segment Fault",       /* 12 */
    "General Protection Fault",  /* 13 */
    "Page Fault",                /* 14 */
    "Reserved",                  /* 15 */
    "x87 FPU Error",             /* 16 */
    "Alignment Check",           /* 17 */
    "Machine Check",             /* 18 */
    "SIMD FP Exception",         /* 19 */
    "Virtualization Exception",  /* 20 */
    "Reserved",                  /* 21 */
    "Reserved",                  /* 22 */
    "Reserved",                  /* 23 */
    "Reserved",                  /* 24 */
    "Reserved",                  /* 25 */
    "Reserved",                  /* 26 */
    "Reserved",                  /* 27 */
    "Reserved",                  /* 28 */
    "Reserved",                  /* 29 */
    "Security Exception",        /* 30 */
    "Reserved"                   /* 31 */
};

/* ISR stub symbols defined in isr.asm */
extern void isr0(void);   extern void isr1(void);   extern void isr2(void);
extern void isr3(void);   extern void isr4(void);   extern void isr5(void);
extern void isr6(void);   extern void isr7(void);   extern void isr8(void);
extern void isr9(void);   extern void isr10(void);  extern void isr11(void);
extern void isr12(void);  extern void isr13(void);  extern void isr14(void);
extern void isr15(void);  extern void isr16(void);  extern void isr17(void);
extern void isr18(void);  extern void isr19(void);  extern void isr20(void);
extern void isr21(void);  extern void isr22(void);  extern void isr23(void);
extern void isr24(void);  extern void isr25(void);  extern void isr26(void);
extern void isr27(void);  extern void isr28(void);  extern void isr29(void);
extern void isr30(void);  extern void isr31(void);

extern void irq0(void);   extern void irq1(void);   extern void irq2(void);
extern void irq3(void);   extern void irq4(void);   extern void irq5(void);
extern void irq6(void);   extern void irq7(void);   extern void irq8(void);
extern void irq9(void);   extern void irq10(void);  extern void irq11(void);
extern void irq12(void);  extern void irq13(void);  extern void irq14(void);
extern void irq15(void);

extern void isr128(void); /* Syscall vector */

/*
 * idt_set_gate - install one IDT descriptor.
 *
 * @num:      vector number (0-255)
 * @handler:  virtual address of the ISR stub
 * @selector: code segment selector (0x08 for kernel code)
 * @flags:    type_attr byte (e.g. IDT_FLAG_PRESENT | IDT_TYPE_INT32 for ring 0)
 */
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags)
{
    idt[num].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[num].selector    = selector;
    idt[num].ist         = 0;          /* Use legacy stack switching */
    idt[num].type_attr   = flags;
    idt[num].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[num].zero        = 0;
}

/*
 * idt_init - fill and load the IDT.
 */
void idt_init(void)
{
    /* Flags for a kernel-privilege interrupt gate (ring 0, present, int-gate) */
    uint8_t kflags = IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_TYPE_INT32;

    /* Flags for a user-accessible trap gate (ring 3, for int 0x80 syscall) */
    uint8_t uflags = IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_TYPE_TRAP32;

    /* CPU exception handlers (0-31) */
    idt_set_gate(0,  (uint64_t)isr0,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(1,  (uint64_t)isr1,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(2,  (uint64_t)isr2,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(3,  (uint64_t)isr3,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(4,  (uint64_t)isr4,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(5,  (uint64_t)isr5,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(6,  (uint64_t)isr6,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(7,  (uint64_t)isr7,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(8,  (uint64_t)isr8,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(9,  (uint64_t)isr9,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(10, (uint64_t)isr10, GDT_KERNEL_CODE, kflags);
    idt_set_gate(11, (uint64_t)isr11, GDT_KERNEL_CODE, kflags);
    idt_set_gate(12, (uint64_t)isr12, GDT_KERNEL_CODE, kflags);
    idt_set_gate(13, (uint64_t)isr13, GDT_KERNEL_CODE, kflags);
    idt_set_gate(14, (uint64_t)isr14, GDT_KERNEL_CODE, kflags);
    idt_set_gate(15, (uint64_t)isr15, GDT_KERNEL_CODE, kflags);
    idt_set_gate(16, (uint64_t)isr16, GDT_KERNEL_CODE, kflags);
    idt_set_gate(17, (uint64_t)isr17, GDT_KERNEL_CODE, kflags);
    idt_set_gate(18, (uint64_t)isr18, GDT_KERNEL_CODE, kflags);
    idt_set_gate(19, (uint64_t)isr19, GDT_KERNEL_CODE, kflags);
    idt_set_gate(20, (uint64_t)isr20, GDT_KERNEL_CODE, kflags);
    idt_set_gate(21, (uint64_t)isr21, GDT_KERNEL_CODE, kflags);
    idt_set_gate(22, (uint64_t)isr22, GDT_KERNEL_CODE, kflags);
    idt_set_gate(23, (uint64_t)isr23, GDT_KERNEL_CODE, kflags);
    idt_set_gate(24, (uint64_t)isr24, GDT_KERNEL_CODE, kflags);
    idt_set_gate(25, (uint64_t)isr25, GDT_KERNEL_CODE, kflags);
    idt_set_gate(26, (uint64_t)isr26, GDT_KERNEL_CODE, kflags);
    idt_set_gate(27, (uint64_t)isr27, GDT_KERNEL_CODE, kflags);
    idt_set_gate(28, (uint64_t)isr28, GDT_KERNEL_CODE, kflags);
    idt_set_gate(29, (uint64_t)isr29, GDT_KERNEL_CODE, kflags);
    idt_set_gate(30, (uint64_t)isr30, GDT_KERNEL_CODE, kflags);
    idt_set_gate(31, (uint64_t)isr31, GDT_KERNEL_CODE, kflags);

    /* Hardware IRQ handlers (32-47) */
    idt_set_gate(32, (uint64_t)irq0,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(33, (uint64_t)irq1,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(34, (uint64_t)irq2,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(35, (uint64_t)irq3,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(36, (uint64_t)irq4,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(37, (uint64_t)irq5,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(38, (uint64_t)irq6,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(39, (uint64_t)irq7,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(40, (uint64_t)irq8,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(41, (uint64_t)irq9,  GDT_KERNEL_CODE, kflags);
    idt_set_gate(42, (uint64_t)irq10, GDT_KERNEL_CODE, kflags);
    idt_set_gate(43, (uint64_t)irq11, GDT_KERNEL_CODE, kflags);
    idt_set_gate(44, (uint64_t)irq12, GDT_KERNEL_CODE, kflags);
    idt_set_gate(45, (uint64_t)irq13, GDT_KERNEL_CODE, kflags);
    idt_set_gate(46, (uint64_t)irq14, GDT_KERNEL_CODE, kflags);
    idt_set_gate(47, (uint64_t)irq15, GDT_KERNEL_CODE, kflags);

    /* Syscall vector (0x80) - ring 3 accessible trap gate */
    idt_set_gate(128, (uint64_t)isr128, GDT_KERNEL_CODE, uflags);

    /* Load the IDT */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)idt;
    __asm__ volatile("lidtq (%0)" :: "r"(&idt_ptr));

    kinfo("IDT loaded: %u entries at %p", IDT_ENTRIES, (void*)idt);
}
