/*
 * linker.ld - Kernel linker script for x86_64
 *
 * Boot sections (multiboot2, boot.text, boot.data, boot.bss) are placed
 * at their physical load address (0x100000) so they can run before paging.
 *
 * Higher-half kernel sections start immediately after the boot sections
 * in physical memory, but are linked at KERNEL_VMA_BASE + physical_offset.
 *
 * VMA layout:  0xFFFFFFFF80100000 + (phys_offset - 0x100000)
 * LMA layout:  physical address (starting at boot_end, page-aligned)
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA_BASE  = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x100000;

SECTIONS
{
    /* ========================================================
     * Boot sections — VMA = LMA = physical (no paging yet)
     * ======================================================== */
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

    /* Physical end of boot-time code/data */
    boot_end_phys = ALIGN(., 0x1000);

    /* ========================================================
     * Higher-half kernel sections
     * VMA = KERNEL_VMA_BASE + phys_offset
     * LMA = phys_offset  (AT() provides this)
     * ======================================================== */

    /*
     * Advance location counter to the higher-half VMA corresponding
     * to boot_end_phys in physical memory.
     */
    . = KERNEL_VMA_BASE + boot_end_phys;

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

    /* Physical addresses for PMM (mark kernel pages as used) */
    _kernel_phys_start = KERNEL_PHYS_BASE;
    _kernel_phys_end   = _kernel_end - KERNEL_VMA_BASE;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
