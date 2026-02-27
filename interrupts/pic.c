/*
 * interrupts/pic.c - 8259 Programmable Interrupt Controller (PIC) driver
 *
 * Remaps IRQ 0-15 from their default BIOS vectors (0x08-0x0F and 0x70-0x77)
 * to vectors 0x20-0x2F, avoiding conflicts with CPU exception vectors 0-31.
 *
 * After remapping:
 *   IRQ 0  (timer)    -> INT 0x20
 *   IRQ 1  (keyboard) -> INT 0x21
 *   ...
 *   IRQ 15 (ATA2)     -> INT 0x2F
 */
#include <interrupts.h>
#include <kernel.h>
#include <types.h>

/* PIC initialization words */
#define ICW1_INIT    0x10  /* Initialization required */
#define ICW1_ICW4    0x01  /* ICW4 needed */
#define ICW4_8086    0x01  /* 8086/88 (MCS-80/85) mode */

void pic_init(void)
{
    /* Save existing interrupt masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    (void)mask1; (void)mask2;

    /* Start PIC initialization sequence (cascade mode) */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: Set interrupt vector offsets */
    outb(PIC1_DATA, 0x20);  /* PIC1 vectors: 0x20-0x27 */
    io_wait();
    outb(PIC2_DATA, 0x28);  /* PIC2 vectors: 0x28-0x2F */
    io_wait();

    /* ICW3: Configure cascade */
    outb(PIC1_DATA, 0x04);  /* PIC1: PIC2 is connected to IRQ2 (bit mask) */
    io_wait();
    outb(PIC2_DATA, 0x02);  /* PIC2: cascade identity = 2 */
    io_wait();

    /* ICW4: Set 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Restore masks (mask all IRQs initially; drivers unmask their own IRQ) */
    outb(PIC1_DATA, 0xFB);  /* Enable only IRQ2 (cascade) for now */
    outb(PIC2_DATA, 0xFF);  /* Mask all secondary IRQs */

    kinfo("PIC remapped: IRQ 0-7  -> INT 0x20-0x27");
    kinfo("              IRQ 8-15 -> INT 0x28-0x2F");
}

/*
 * pic_send_eoi - notify the PIC that the current interrupt has been handled.
 * Must be called at the end of every hardware IRQ handler.
 */
void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);  /* Notify slave PIC if IRQ >= 8 */
    }
    outb(PIC1_COMMAND, PIC_EOI);      /* Always notify master PIC */
}

/*
 * pic_set_mask - mask (disable) a specific IRQ line.
 * @irq: 0-15
 */
void pic_set_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port);
    value |= (1 << irq);
    outb(port, value);
}

/*
 * pic_clear_mask - unmask (enable) a specific IRQ line.
 * @irq: 0-15
 */
void pic_clear_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port);
    value &= ~(1 << irq);
    outb(port, value);
}

/*
 * pic_disable - mask all IRQs (e.g., when switching to APIC).
 */
void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
