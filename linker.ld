/*
 * linker.ld - Kernel linker script for x86_64
 *
 * The kernel is physically loaded at 0x100000 (1MB) by GRUB, but the
 * code and data are linked at KERNEL_VMA_BASE + 0x100000 (higher half).
 *
 * The boot code (.multiboot2, .boot.text, .boot.data, .boot.bss) is
 * placed at its physical load address (0x100000) so it can run before
 * paging is enabled.
 *
 * All other sections are linked at the higher-half virtual address:
 *   Virtual:  0xFFFFFFFF80100000
 *   Physical: 0x100000
 *
 * AT(expr) in section definitions provides the physical Load Memory Address (LMA)
 * while the section address provides the Virtual Memory Address (VMA).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Kernel virtual base address */
KERNEL_VMA_BASE = 0xFFFFFFFF80000000;

/* Physical load base */
KERNEL_PHYS_BASE = 0x100000;

SECTIONS
{
    /*
     * ============================================================
     * Boot sections (VMA = LMA = physical address)
     * These run before paging is enabled.
     * ============================================================
     */
    . = KERNEL_PHYS_BASE;

    .multiboot2 : {
        KEEP(*(.multiboot2))
    }

    .boot.text : {
        KEEP(*(.boot.text))
    }

    .boot.data : {
        KEEP(*(.boot.data))
    }

    .boot.bss (NOLOAD) : {
        KEEP(*(.boot.bss))
    }

    /* Record the physical end of boot sections */
    boot_end = .;

    /*
     * ============================================================
     * Higher-half kernel sections
     * VMA = KERNEL_VMA_BASE + physical_offset
     * LMA (AT) = physical address
     * ============================================================
     */

    /* Jump to higher-half VMA. The AT() provides the physical LMA. */
    . = KERNEL_VMA_BASE + KERNEL_PHYS_BASE;

    .text ALIGN(0x1000) : AT(ADDR(.text) - KERNEL_VMA_BASE)
    {
        _kernel_text_start = .;
        *(.text*)
        _kernel_text_end = .;
    }

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KERNEL_VMA_BASE)
    {
        _kernel_rodata_start = .;
        *(.rodata*)
        _kernel_rodata_end = .;
    }

    .data ALIGN(0x1000) : AT(ADDR(.data) - KERNEL_VMA_BASE)
    {
        _kernel_data_start = .;
        *(.data*)
        _kernel_data_end = .;
    }

    .bss ALIGN(0x1000) : AT(ADDR(.bss) - KERNEL_VMA_BASE)
    {
        _kernel_bss_start = .;
        *(COMMON)
        *(.bss*)
        _kernel_bss_end = .;
    }

    /* End of kernel image */
    _kernel_end = .;

    /* Physical end addresses (for PMM to mark kernel as used) */
    _kernel_phys_start = KERNEL_PHYS_BASE;
    _kernel_phys_end   = _kernel_end - KERNEL_VMA_BASE;

    /*
     * Discard unneeded sections (debug, notes, etc.)
     * Comment these out to keep debug info for GDB.
     */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
