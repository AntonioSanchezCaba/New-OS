/*
 * kernel/kernel.c - Kernel main entry point
 *
 * kernel_main() is the first C function called after the boot assembly
 * sets up long mode and switches to the higher-half virtual address.
 *
 * Initialization order matters: each subsystem may depend on earlier ones.
 */
#include <kernel.h>
#include <types.h>
#include <multiboot2.h>
#include <memory.h>
#include <interrupts.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/timer.h>
#include <drivers/keyboard.h>
#include <drivers/ata.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/pci.h>
#include <drivers/e1000.h>
#include <fs/vfs.h>
#include <net/net.h>
#include <gui/gui.h>

/* Kernel state */
static kernel_state_t kernel_state = KERNEL_STATE_BOOT;

kernel_state_t kernel_get_state(void) { return kernel_state; }
void kernel_set_state(kernel_state_t s) { kernel_state = s; }

/* Forward declarations */
static void print_banner(void);
static void init_userland(void);
void kmain_thread(void);

/*
 * kernel_main - called from boot.asm after entering 64-bit long mode.
 *
 * @mb2_info: virtual address of the Multiboot2 info structure.
 */
void kernel_main(struct multiboot2_info* mb2_info)
{
    /* === Phase 1: Essential output (before memory management) === */
    serial_init(COM1_PORT, UART_BAUD_115200);
    vga_init();
    print_banner();

    debug_puts("[boot] Kernel entered 64-bit long mode\n");
    kinfo("NovOS kernel starting...");
    kinfo("Multiboot2 info at %p", (void*)mb2_info);

    /* === Phase 2: Memory management === */
    kernel_state = KERNEL_STATE_INIT;

    kinfo("Initializing physical memory manager...");
    pmm_init(mb2_info);
    kinfo("PMM: %u MB free (%u frames)",
          (uint32_t)(pmm_free_frames_count() * PAGE_SIZE / (1024*1024)),
          (uint32_t)pmm_free_frames_count());

    kinfo("Initializing virtual memory manager...");
    vmm_init();

    kinfo("Initializing kernel heap...");
    heap_init(KERNEL_HEAP_START, 64 * 1024 * 1024); /* 64MB heap */

    /* === Phase 3: CPU structures === */
    kinfo("Initializing GDT/TSS...");
    gdt_init();

    kinfo("Initializing IDT...");
    idt_init();

    kinfo("Initializing PIC...");
    pic_init();

    /* === Phase 4: Drivers === */
    kinfo("Initializing timer (PIT at %u Hz)...", TIMER_FREQ);
    timer_init(TIMER_FREQ);

    kinfo("Initializing PS/2 keyboard...");
    keyboard_init();

    kinfo("Initializing ATA disk controller...");
    ata_init();

    /* === Phase 4b: Framebuffer and GUI input === */
    struct multiboot2_tag_framebuffer* fb_tag =
        (struct multiboot2_tag_framebuffer*)
        multiboot2_find_tag(mb2_info, MULTIBOOT2_TAG_FRAMEBUFFER);

    if (fb_tag) {
        kinfo("Framebuffer: %ux%u %ubpp at 0x%llx",
              fb_tag->width, fb_tag->height, fb_tag->bpp,
              (unsigned long long)fb_tag->addr);
        fb_init(fb_tag);
        if (fb_ready()) {
            kinfo("Initializing PS/2 mouse...");
            mouse_init();
            kinfo("Initializing GUI subsystem...");
            gui_init();
        } else {
            klog_warn("Framebuffer init failed, GUI disabled");
        }
    } else {
        klog_warn("No framebuffer tag from bootloader, GUI disabled");
    }

    /* === Phase 4c: PCI bus and network === */
    kinfo("Scanning PCI bus...");
    pci_init();

    kinfo("Initializing e1000 Ethernet driver...");
    if (e1000_init() == 0) {
        kinfo("Initializing network stack...");
        net_init();
        arp_announce();
    } else {
        kinfo("No e1000 NIC found, networking disabled");
    }

    /* === Phase 5: Filesystem === */
    kinfo("Initializing VFS...");
    vfs_init();

    /* === Phase 6: Process subsystem === */
    kinfo("Initializing process manager...");
    process_init();

    kinfo("Initializing scheduler...");
    scheduler_init();

    /* === Phase 7: Syscall interface === */
    kinfo("Initializing system call interface...");
    syscall_init();

    /* === Phase 8: Enable interrupts and start scheduling === */
    kernel_state = KERNEL_STATE_RUNNING;
    kinfo("Kernel initialization complete. Enabling interrupts.");
    cpu_sti();

    /* === Phase 9: Launch userland === */
    init_userland();

    /* The idle loop: scheduler will preempt this with real processes */
    kinfo("Entering idle loop");
    while (1) {
        cpu_halt();
    }
}

/*
 * print_banner - display the OS banner on the VGA screen.
 */
static void print_banner(void)
{
    vga_set_color(vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    vga_puts(
        "\n"
        "  _   _               ___  ____  \n"
        " | \\ | |  ___ __   __|   \\/ ___| \n"
        " |  \\| | / _ \\\\ \\ / / |) \\___ \\ \n"
        " | |\\  || (_) |\\ V /|___/ ___) |\n"
        " |_| \\_| \\___/  \\_/ |____||____/ \n"
        "\n"
    );

    vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    vga_printf("  NovOS v%s | x86_64 | Kernel at 0xFFFFFFFF80100000\n\n",
               KERNEL_VERSION);

    vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

/*
 * init_userland - create the init process and (optionally) the GUI thread.
 */
static void init_userland(void)
{
    extern void init_process_entry(void);

    kinfo("Creating init process...");
    process_t* init = process_create("init", init_process_entry, false);
    if (!init) {
        kpanic("Failed to create init process!");
    }
    scheduler_add(init);
    kinfo("Init process created (PID %u)", init->pid);

    /* Launch graphical desktop if framebuffer is available */
    if (gui_available()) {
        kinfo("Launching GUI thread...");
        process_t* gui_proc = process_create("gui", gui_run, false);
        if (gui_proc) {
            scheduler_add(gui_proc);
            kinfo("GUI thread created (PID %u)", gui_proc->pid);
        } else {
            klog_warn("Failed to create GUI thread");
        }
    }
}
