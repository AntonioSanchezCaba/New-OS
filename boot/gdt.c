/*
 * boot/gdt.c - Global Descriptor Table and TSS initialization
 *
 * Sets up the final GDT in kernel virtual address space, installs the
 * 64-bit TSS, and provides tss_set_kernel_stack() for context switches.
 */
#include <interrupts.h>
#include <types.h>
#include <drivers/vga.h>
#include <kernel.h>

/* Our GDT: 5 standard segments + 1 TSS (which takes 2 entries in 64-bit) */
#define GDT_SIZE 7

/* GDT access byte flags */
#define GDT_ACCESS_PRESENT   (1 << 7)
#define GDT_ACCESS_DPL_RING0 (0 << 5)
#define GDT_ACCESS_DPL_RING3 (3 << 5)
#define GDT_ACCESS_S         (1 << 4)  /* Descriptor type: 1=code/data */
#define GDT_ACCESS_EX        (1 << 3)  /* Executable */
#define GDT_ACCESS_RW        (1 << 1)  /* Read/Write */
#define GDT_ACCESS_ACCESSED  (1 << 0)

/* Granularity byte flags */
#define GDT_GRAN_4K     (1 << 7)  /* Page (4KB) granularity */
#define GDT_GRAN_32BIT  (1 << 6)  /* 32-bit protected mode */
#define GDT_GRAN_64BIT  (1 << 5)  /* 64-bit long mode */

/* TSS access byte */
#define GDT_ACCESS_TSS_64  0x89  /* Present, DPL=0, 64-bit TSS Available */

/* GDT entries (7 total) */
static gdt_entry_t gdt[GDT_SIZE] __attribute__((aligned(8)));

/* GDT pointer */
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr;

/* Task State Segment */
static tss_t tss __attribute__((aligned(16)));

/* Helper: write a standard 8-byte GDT entry */
static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity)
{
    gdt[idx].base_low    = (base & 0xFFFF);
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = (limit & 0xFFFF);
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (granularity & 0xF0);
    gdt[idx].access      = access;
}

/*
 * Install the TSS descriptor (a 16-byte "system" descriptor in 64-bit mode).
 * Entries at index 5 and 6 together form the 64-bit TSS descriptor.
 */
static void gdt_set_tss(int idx, uint64_t base, uint32_t limit)
{
    /* Low 8 bytes (standard GDT layout) */
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].access      = GDT_ACCESS_TSS_64;
    gdt[idx].granularity = ((limit >> 16) & 0x0F);
    gdt[idx].base_high   = (base >> 24) & 0xFF;

    /* High 8 bytes (upper 32 bits of base address) */
    uint32_t* high_entry = (uint32_t*)&gdt[idx + 1];
    high_entry[0] = (uint32_t)(base >> 32);
    high_entry[1] = 0;
}

/* Load the TSS selector into the task register */
static inline void load_tr(uint16_t selector) {
    __asm__ volatile("ltr %0" :: "r"(selector));
}

/* Load our GDT */
static inline void load_gdt(void) {
    __asm__ volatile(
        "lgdtq (%0)\n\t"
        /* Reload CS with a far return */
        "pushq $0x08\n\t"           /* Kernel code selector */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload data segment registers */
        "movw $0x10, %%ax\n\t"      /* Kernel data selector */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :: "r"(&gdtr) : "rax", "memory"
    );
}

void gdt_init(void)
{
    /* Entry 0: Null segment (required) */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Entry 1 (0x08): Kernel code segment
     *   64-bit, DPL=0, execute/read, present
     */
    gdt_set_entry(1, 0, 0xFFFFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_DPL_RING0 |
                  GDT_ACCESS_S | GDT_ACCESS_EX | GDT_ACCESS_RW,
                  GDT_GRAN_64BIT | GDT_GRAN_4K);

    /* Entry 2 (0x10): Kernel data segment
     *   64-bit, DPL=0, read/write, present
     */
    gdt_set_entry(2, 0, 0xFFFFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_DPL_RING0 |
                  GDT_ACCESS_S | GDT_ACCESS_RW,
                  GDT_GRAN_32BIT | GDT_GRAN_4K);

    /* Entry 3 (0x18): User data segment
     *   DPL=3, read/write, present
     */
    gdt_set_entry(3, 0, 0xFFFFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_DPL_RING3 |
                  GDT_ACCESS_S | GDT_ACCESS_RW,
                  GDT_GRAN_32BIT | GDT_GRAN_4K);

    /* Entry 4 (0x20): User code segment
     *   64-bit, DPL=3, execute/read, present
     */
    gdt_set_entry(4, 0, 0xFFFFFFFF,
                  GDT_ACCESS_PRESENT | GDT_ACCESS_DPL_RING3 |
                  GDT_ACCESS_S | GDT_ACCESS_EX | GDT_ACCESS_RW,
                  GDT_GRAN_64BIT | GDT_GRAN_4K);

    /* Entry 5-6 (0x28): TSS descriptor (64-bit = 16 bytes = 2 GDT slots) */
    /* Initialize TSS */
    memset(&tss, 0, sizeof(tss));

    /* The IO permission map base points past the end of the TSS,
     * indicating no I/O permission bitmap (all ports require ring 0). */
    tss.iomap_base = sizeof(tss);

    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);

    /* Set up the GDT pointer */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;

    /* Load the new GDT */
    load_gdt();

    /* Load the TSS (selector = 0x28 = index 5 * 8) */
    load_tr(GDT_TSS);

    kinfo("GDT initialized: %d entries at %p", GDT_SIZE, (void*)gdt);
}

/*
 * tss_set_kernel_stack - update the TSS RSP0 field.
 *
 * This must be called on every context switch to ensure that when the
 * CPU transitions from ring 3 to ring 0 (on interrupt/syscall), it
 * uses the current process's kernel stack.
 */
void tss_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
