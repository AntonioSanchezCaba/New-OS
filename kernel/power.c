/*
 * kernel/power.c - System power management
 *
 * Shutdown: tries ACPI S5 via QEMU/VirtualBox magic ports, then
 *           falls back to keyboard controller reset.
 * Restart:  keyboard controller pulse (port 0x64 command 0xFE).
 * Sleep:    disables interrupts, halts CPU in a loop (wakes on NMI/reset).
 */
#include <kernel/power.h>
#include <kernel.h>

/* ── Shutdown ──────────────────────────────────────────────────────────── */

void power_shutdown(void)
{
    /* Disable interrupts to prevent further activity */
    cpu_cli();

    /* Method 1: QEMU ACPI shutdown (port 0x604) */
    outw(0x604, 0x2000);

    /* Method 2: VirtualBox ACPI shutdown (port 0xB004) */
    outw(0xB004, 0x2000);

    /* Method 3: Bochs/older QEMU shutdown (port 0x4004) */
    outw(0x4004, 0x3400);

    /* Method 4: Try the ISA bridge shutdown (Bochs) */
    outw(0x0604, 0x2000);

    /* If all ACPI methods fail, just halt */
    while (1) {
        cpu_halt();
    }
}

/* ── Restart ───────────────────────────────────────────────────────────── */

void power_restart(void)
{
    cpu_cli();

    /* Method 1: Keyboard controller reset pulse */
    /* Wait for input buffer to be empty */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);  /* Pulse CPU reset line */

    /* Method 2: Triple fault (load null IDT and trigger interrupt) */
    /* Create a null IDT descriptor */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };

    __asm__ volatile(
        "lidt %0\n"
        "int3\n"
        :: "m"(null_idt)
    );

    /* Should never reach here */
    while (1) {
        cpu_halt();
    }
}

/* ── Sleep ─────────────────────────────────────────────────────────────── */

void power_sleep(void)
{
    /*
     * In a real OS this would enter ACPI S1/S3 state.
     * For our bare-metal OS, we halt the CPU with interrupts enabled.
     * The system will wake on any hardware interrupt (keyboard, timer, mouse).
     */
    __asm__ volatile("sti; hlt");
}
