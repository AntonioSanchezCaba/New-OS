/*
 * kernel/kernel.c - AetherOS Kernel Entry Point (v1.0.0 — Genesis)
 *
 * kernel_main() is the first C function called after the boot assembly
 * sets up long mode and switches to the higher-half virtual address.
 *
 * Initialization order:
 *   Phase 1  — Serial + VGA (output only)
 *   Phase 2  — PMM + VMM + Heap
 *   Phase 3  — CPU structures (GDT, IDT, PIC)
 *   Phase 4  — Drivers (timer, keyboard, ATA, framebuffer, mouse, PCI)
 *   Phase 5  — Aether Kernel Layer (cap, IPC, svcbus, secmon, buddy)
 *   Phase 6  — Core Services (compositor, input service, launcher)
 *   Phase 7  — Filesystem + networking
 *   Phase 8  — Process manager + scheduler
 *   Phase 9  — Enable interrupts + start userland
 */
#include <kernel.h>
#include <kernel/version.h>
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
#include <net/ip.h>
#include <gui/gui.h>

/* === Aether OS architectural layer === */
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/svcbus.h>
#include <kernel/secmon.h>
#include <kernel/diskman.h>
#include <kernel/users.h>
#include <kernel/apkg.h>
#include <mm/buddy.h>
#include <display/compositor.h>
#include <input/input_svc.h>
#include <services/launcher.h>
/* New subsystem drivers/GUI/net */
#include <drivers/uefi_gop.h>
#include <drivers/cursor.h>
#include <drivers/usb.h>
#include <gui/wallpaper.h>
#include <gui/animation.h>
#include <net/dhcp.h>

/* Aether Render Engine */
#include <aether/are.h>

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
    kinfo(OS_BANNER " — " OS_TAGLINE);
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
              fb_tag->framebuffer_width, fb_tag->framebuffer_height,
              fb_tag->framebuffer_bpp,
              (unsigned long long)fb_tag->framebuffer_addr);
        /* Try UEFI GOP via Multiboot2 first */
        gop_info_t gop_info;
        uefi_gop_init(mb2_info, &gop_info);
        fb_init(fb_tag);
        if (fb_ready()) {
            kinfo("Initializing PS/2 mouse...");
            mouse_init();
            kinfo("Initializing software cursor...");
            cursor_init();
            cursor_show();   /* make cursor visible from first frame */
            kinfo("Initializing wallpaper engine...");
            wallpaper_init();
            kinfo("Initializing window animations...");
            anim_init();
            kinfo("Initializing Aether Render Engine...");
            are_init();
        } else {
            klog_warn("Framebuffer init failed, GUI disabled");
        }
    } else {
        klog_warn("No framebuffer tag from bootloader, GUI disabled");
    }

    /* === Phase 5: Aether OS Architectural Layer === */
    kinfo("--- " OS_NAME " Kernel Layer ---");

    kinfo("Initializing capability security table...");
    cap_table_init();

    kinfo("Initializing security monitor...");
    secmon_init();

    kinfo("Initializing IPC engine...");
    ipc_init();

    kinfo("Initializing service bus...");
    svcbus_init();

    /* Buddy allocator: seed with 16MB starting above the kernel heap */
    {
        uint64_t buddy_base_phys = 0x2000000;  /* 32 MB mark */
        uint64_t buddy_base_virt = PHYS_TO_VIRT(buddy_base_phys);
        uint64_t buddy_size      = 16 * 1024 * 1024;  /* 16 MB */
        kinfo("Initializing buddy allocator (16 MB at phys 0x%llx)...",
              (unsigned long long)buddy_base_phys);
        buddy_init(buddy_base_phys, buddy_base_virt, buddy_size);
    }

    kinfo("--- " OS_NAME " Kernel Layer ready ---");
    kinfo("  Capabilities : active=%u", cap_count());
    kinfo("  IPC ports    : initialized");
    kinfo("  Service bus  : initialized");
    kinfo("  Buddy memory : %llu KB free",
          (unsigned long long)(buddy_free_bytes() / 1024));

    /* === Phase 4c: USB HID === */
    kinfo("Initializing USB (UHCI skeleton)...");
    usb_init();

    /* === Phase 4d: PCI bus and network === */
    kinfo("Scanning PCI bus...");
    pci_init();

    kinfo("Initializing e1000 Ethernet driver...");
    if (e1000_init() == 0) {
        kinfo("Initializing network stack...");
        net_init();
        arp_announce();
        kinfo("Starting DHCP discovery...");
        if (dhcp_discover() == 0)
            kinfo("DHCP: lease acquired");
        else
            klog_warn("DHCP: no lease (static IP or no DHCP server)");
    } else {
        kinfo("No e1000 NIC found, networking disabled");
    }

    /* === Phase 5: Filesystem + disk manager === */
    kinfo("Initializing VFS...");
    vfs_init();

    /* Mount ramfs as VFS root BEFORE diskman_init() calls vfs_mkdir("/mnt") */
    {
        extern vfs_node_t* ramfs_create_root(void);
        vfs_mount_root(ramfs_create_root());
        kinfo("VFS: ramfs mounted as root");
    }

    kinfo("Scanning disks and mounting partitions...");
    diskman_init();

    /* Mount proc filesystem at /proc */
    {
        extern void procfs_init(void);
        procfs_init();
    }

    /* === Phase 6: Process subsystem === */
    kinfo("Initializing process manager...");
    process_init();

    kinfo("Initializing scheduler...");
    scheduler_init();

    /* === Phase 6b: User accounts and package manager === */
    kinfo("Initializing user account system...");
    users_init();

    kinfo("Initializing package manager (apkg)...");
    apkg_init();

    /* === Phase 7: Syscall interface === */
    kinfo("Initializing system call interface...");
    syscall_init();

    /* === Phase 8: Enable interrupts and start scheduling === */
    kernel_state = KERNEL_STATE_RUNNING;
    kinfo("Kernel initialization complete. Enabling interrupts.");
    cpu_sti();

    /* === Phase 9: Launch userland === */
    init_userland();

    /* === Phase 10: Enter Aether Render Engine (replaces gui_run) === */
    kinfo("Entering Aether Render Engine...");
    are_run();  /* Returns only on are_shutdown() */

    /* Fallback idle loop (should not be reached during normal operation) */
    kinfo("ARE exited — entering idle loop");
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
        "    _____  __  __              ____   ____  \n"
        "   /  _  |/ _||_ |__  ___  _ / __ \\ / ___| \n"
        "  | |_| || |_  | '_ \\/ _ \\| |  |  \\\___ \\  \n"
        "  |  _  ||  _| | | | | __/| |  |__/ ___) | \n"
        "  |_| |_||_|   |_| |_|\\___||_|\\_____/____/  \n"
        "\n"
    );

    vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    vga_printf("  " OS_BANNER " | " OS_ARCH " | " OS_KERNEL_TYPE "\n");
    vga_printf("  " OS_TAGLINE "\n\n");

    vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

/*
 * aether_compositor_thread — wraps compositor_run for the process API
 */
__attribute__((unused))
static void aether_compositor_thread(void)
{
    compositor_init();
    compositor_run();
}

/* aether_input_thread — wraps input_svc_run for the process API */
__attribute__((unused))
static void aether_input_thread(void)
{
    input_svc_init();
    input_svc_run();
}

/* aether_launcher_thread — wraps launcher_run for the process API */
__attribute__((unused))
static void aether_launcher_thread(void)
{
    launcher_init();
    launcher_run();
}

/*
 * init_userland - launch init process and all Aether OS services.
 *
 * Service launch order:
 *   1. init           — base process (manages system services)
 *   2. aether.display — compositor (owns framebuffer)
 *   3. aether.input   — input event dispatcher
 *   4. aether.launcher— application launcher / taskbar
 */
static void init_userland(void)
{
    extern void init_process_entry(void);

    kinfo("=== " OS_BANNER " — Starting Services ===");

    kinfo("Creating init process...");
    process_t* init = process_create("init", init_process_entry, false);
    if (!init) {
        kpanic("Failed to create init process!");
    }
    scheduler_add(init);
    kinfo("  [OK] init (PID %u)", init->pid);

    if (!gui_available()) {
        klog_warn("No framebuffer — running headless");
        return;
    }

    /*
     * ARE (Aether Render Engine) is the authoritative framebuffer owner
     * and input consumer.  The legacy compositor, input service, and
     * launcher service threads would conflict with ARE — both would write
     * to the framebuffer at 60 Hz and drain the same keyboard/mouse
     * queues.  Skip them when ARE is active.
     *
     * ARE is always active when fb_ready() == true (i.e. gui_available()).
     * Left in place for headless / regression builds that set ARE_DISABLE.
     */
    kinfo("ARE active — skipping legacy compositor/input/launcher threads");

    kinfo("=== " OS_NAME " — All services started ===");
    kinfo("  Service bus: %u registered services", svcbus_count());
    kinfo("  Capabilities: %u active", cap_count());
    secmon_dump_log(5);
}
