/*
 * kernel/kernel.c - AetherOS Kernel Entry Point (v1.0.0 — Genesis)
 *
 * kernel_main() is the first C function called after the boot assembly
 * sets up long mode and switches to the higher-half virtual address.
 *
 * Initialization order:
 *   Phase 1  — Serial + VGA (output only, no memory allocation)
 *   Phase 2  — CPU structures: GDT/TSS, IDT, PIC  ← MUST precede memory
 *              (any exception during PMM/VMM needs IDT to handle it;
 *               without IDT a page-fault triple-faults the machine)
 *   Phase 3  — Memory management: PMM, VMM, Heap
 *   Phase 4  — Drivers: timer, keyboard, ATA, framebuffer, mouse, PCI
 *   Phase 5  — Aether Kernel Layer: cap, IPC, svcbus, secmon, buddy
 *   Phase 6  — Filesystem + disk manager + networking
 *   Phase 7  — Process manager + scheduler + TTY
 *   Phase 8  — Syscall interface
 *   Phase 9  — Enable interrupts + launch userland
 *   Phase 10 — Boot animation + Aether Render Engine
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
#include <kernel/shm.h>
#include <kernel/tty.h>
#include <kernel/diskman.h>
#include <kernel/users.h>
#include <kernel/apkg.h>
#include <kernel/pkg.h>
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
/* Boot animation (plays before the ARE render engine starts) */
#include <services/bootanim.h>

/* Kernel state */
static kernel_state_t kernel_state = KERNEL_STATE_BOOT;

kernel_state_t kernel_get_state(void) { return kernel_state; }
void kernel_set_state(kernel_state_t s) { kernel_state = s; }

/* Forward declarations */
static void print_banner(void);
static void init_userland(void);
void kmain_thread(void);

/*
 * bootanim_timer_cb — adapts the PIT callback signature
 * (void cb(uint64_t)) to bootanim_tick() (void cb(void)).
 * Registered during the boot animation window and cleared
 * before are_run().
 */
static void bootanim_timer_cb(uint64_t ticks __attribute__((unused)))
{
    bootanim_tick();
}

/*
 * kernel_main - called from boot.asm after entering 64-bit long mode.
 *
 * @mb2_info: virtual address of the Multiboot2 info structure.
 */
void kernel_main(struct multiboot2_info* mb2_info)
{
    /* Earliest diagnostic: write '6' to VGA row 24 col 5 via identity map.
     * Boot page tables cover 0-6MB identity; 0xB8000 is within range. */
    *(volatile uint16_t*)((uintptr_t)0xB8000 + (24*80+6)*2) = (uint16_t)(0x4F00 | '6');

    /* === Phase 1: Essential output (no memory, no interrupts) === */
    serial_init(COM1_PORT, UART_BAUD_115200);   /* [STABLE] */
    vga_init();                                  /* [STABLE] */
    print_banner();

    debug_puts("[boot] Kernel entered 64-bit long mode\n");
    kinfo(OS_BANNER " — " OS_TAGLINE);
    kinfo("Multiboot2 info at %p", (void*)mb2_info);
    kinfo("[BOOT] Serial + VGA OK");

    kernel_state = KERNEL_STATE_INIT;

    /* === Phase 2: CPU structures BEFORE memory management ===
     *
     * GDT/IDT/PIC must be set up before pmm_init() runs.  The PMM and VMM
     * touch large memory regions; any CPU exception (page fault, GPF) during
     * that work requires a live IDT to dispatch the handler.  Without it the
     * CPU triple-faults and resets.
     */
    kinfo("Initializing GDT/TSS...");
    gdt_init();                                  /* [STABLE] */
    kinfo("[BOOT] GDT/TSS OK");

    kinfo("Initializing IDT (256 vectors)...");
    idt_init();                                  /* [STABLE] */
    kinfo("[BOOT] IDT OK");

    kinfo("Initializing PIC (8259A — IRQs remapped to 0x20-0x2F)...");
    pic_init();                                  /* [STABLE] */
    kinfo("[BOOT] PIC OK");

    /* === Phase 3: Memory management (IDT is live, exceptions are safe) === */
    kinfo("Initializing physical memory manager...");
    pmm_init(mb2_info);                          /* [STABLE] */
    kinfo("[BOOT] PMM OK — %u MB free (%u frames)",
          (uint32_t)(pmm_free_frames_count() * PAGE_SIZE / (1024*1024)),
          (uint32_t)pmm_free_frames_count());

    kinfo("Initializing virtual memory manager...");
    /*
     * NOTE: vmm.c intentionally includes process.h for COW fork semantics.
     * This is a design-level cross-layer dependency, not a bug.
     */
    vmm_init();                                  /* [STABLE] */
    kinfo("[BOOT] VMM OK");

    /* Inform the panic handler of the physical framebuffer address so any
     * kernel panic after this point paints a visible RED screen instead of
     * silently writing to the VGA text buffer (invisible in VBE mode).   */
    {
        struct multiboot2_tag_framebuffer* _early_fb =
            (struct multiboot2_tag_framebuffer*)
            multiboot2_find_tag(mb2_info, MULTIBOOT2_TAG_FRAMEBUFFER);
        if (_early_fb && _early_fb->framebuffer_type == 1 &&
            _early_fb->framebuffer_bpp == 32) {
            /* Map framebuffer so fb_paint_panic() can write to it safely.
             * vmm_init() only covers 0-128MB; MMIO at ~0xFD000000 needs
             * an explicit mapping.  fb_init() will re-remap as uncached. */
            {
                uint64_t _p0 = (uint64_t)_early_fb->framebuffer_addr & ~(uint64_t)0xFFF;
                uint64_t _pe = (_p0 + (uint64_t)_early_fb->framebuffer_pitch *
                                       _early_fb->framebuffer_height + 0xFFF)
                               & ~(uint64_t)0xFFF;
                for (uint64_t _a = _p0; _a < _pe && _a < 0x100000000ULL;
                     _a += PAGE_SIZE)
                    vmm_map_page(kernel_pml4, _a, _a,
                                 PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL);
            }
            fb_raw_setup((uintptr_t)_early_fb->framebuffer_addr,
                         _early_fb->framebuffer_width,
                         _early_fb->framebuffer_height,
                         _early_fb->framebuffer_pitch);
            kinfo("[BOOT] Raw fb registered: %ux%u @ phys 0x%llx",
                  _early_fb->framebuffer_width, _early_fb->framebuffer_height,
                  (unsigned long long)_early_fb->framebuffer_addr);
        } else {
            klog_warn("[BOOT] No 32bpp linear framebuffer in MB2 tag — panic will be text-only");
            if (_early_fb)
                klog_warn("[BOOT] MB2 fb tag type=%u bpp=%u",
                          _early_fb->framebuffer_type, _early_fb->framebuffer_bpp);
            else
                klog_warn("[BOOT] MB2 fb tag not found at all");
        }
    }

    /* Early framebuffer stripe test (pre-heap).
     * vmm_init() now only maps 0-128MB; device MMIO above that must be
     * mapped explicitly.  Map the framebuffer region with 4KB pages here
     * so that direct VRAM writes work before fb_init() runs.           */
    {
        struct multiboot2_tag_framebuffer* _fbt =
            (struct multiboot2_tag_framebuffer*)
            multiboot2_find_tag(mb2_info, MULTIBOOT2_TAG_FRAMEBUFFER);
        if (_fbt && _fbt->framebuffer_type == 1 &&
            _fbt->framebuffer_bpp == 32) {
            /* Map framebuffer pages (4KB, uncached) */
            uint64_t _fb_phys  = _fbt->framebuffer_addr;
            uint64_t _fb_size  = (uint64_t)_fbt->framebuffer_pitch *
                                  _fbt->framebuffer_height;
            uint64_t _fb_start = _fb_phys & ~(uint64_t)0xFFF;
            uint64_t _fb_end   = (_fb_phys + _fb_size + 0xFFF) & ~(uint64_t)0xFFF;
            for (uint64_t _a = _fb_start;
                 _a < _fb_end && _a < 0x100000000ULL; _a += PAGE_SIZE) {
                vmm_map_page(kernel_pml4, _a, _a,
                             PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL |
                             PTE_CACHE_DISABLE | PTE_WRITE_THROUGH);
            }
            kinfo("[BOOT] Framebuffer mapped: 0x%llx + %llu bytes",
                  (unsigned long long)_fb_phys, (unsigned long long)_fb_size);

            volatile uint32_t* _vram =
                (volatile uint32_t*)(uintptr_t)_fb_phys;
            uint32_t _w = _fbt->framebuffer_width;
            uint32_t _h = _fbt->framebuffer_height;
            uint32_t _p = _fbt->framebuffer_pitch / 4;
            for (uint32_t _y = 0; _y < _h; _y++)
                for (uint32_t _x = 0; _x < _w; _x++)
                    _vram[_y * _p + _x] = 0xFFFFFFFFu;
            __asm__ volatile("mfence" ::: "memory");
            kinfo("[BOOT] Early VRAM test: white screen written");
        }
    }

    kinfo("Initializing kernel heap (64 MB)...");
    heap_init(KERNEL_HEAP_START, 64 * 1024 * 1024); /* [STABLE] */
    kinfo("[BOOT] Heap OK");

    /* === Phase 4: Drivers === */
    kinfo("Initializing timer (PIT at %u Hz)...", TIMER_FREQ);
    /*
     * NOTE: timer.c depends on scheduler.h to call scheduler_tick() from
     * the IRQ0 handler.  This is an intentional driver→scheduler coupling
     * required for preemptive round-robin scheduling.
     */
    timer_init(TIMER_FREQ);                      /* [STABLE] */
    kinfo("[BOOT] Timer (PIT) OK — %u Hz", TIMER_FREQ);

    kinfo("Initializing PS/2 keyboard...");
    keyboard_init();                             /* [STABLE] */
    kinfo("[BOOT] Keyboard OK");

    kinfo("Initializing ATA disk controller...");
    ata_init();                                  /* [STABLE] */
    kinfo("[BOOT] ATA OK");

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
        uefi_gop_init(mb2_info, &gop_info);      /* [STABLE] */
        fb_init(fb_tag);                         /* [STABLE] */
        if (fb_ready()) {
            kinfo("[BOOT] Framebuffer OK — %ux%u @ %ubpp",
                  fb_tag->framebuffer_width, fb_tag->framebuffer_height,
                  fb_tag->framebuffer_bpp);
            /* Immediate VRAM smoke test: paint solid blue before any other
             * code runs, so the user sees colour the moment fb works.
             * This also ensures at least one frame is visible even if the
             * timer-based boot animation encounters issues. */
            fb_clear(0xFF1A3B70u);   /* medium-dark blue — clearly not black */
            fb_flip();
            kinfo("[BOOT] Framebuffer smoke test flushed (should see blue)");
            kinfo("Initializing PS/2 mouse...");
            mouse_init();                        /* [STABLE] */
            kinfo("Initializing software cursor...");
            cursor_init();                       /* [STABLE] */
            cursor_show();
            kinfo("Initializing wallpaper engine...");
            wallpaper_init();                    /* [PARTIAL] — static BMP only */
            kinfo("Initializing window animations...");
            anim_init();                         /* [PARTIAL] — alpha only */
            kinfo("Initializing Aether Render Engine...");
            are_init();                          /* [STABLE] */
            kinfo("[BOOT] GUI stack OK");
            /* Pre-register boot animation steps.  The animation itself
             * starts after cpu_sti() when the timer ISR fires.        */
            bootanim_add_step("Initializing hardware...",    1);
            bootanim_add_step("Starting kernel layer...",    1);
            bootanim_add_step("Loading filesystem...",       1);
            bootanim_add_step("Starting process manager...", 1);
            bootanim_add_step("Launching desktop...",        1);
        } else {
            klog_warn("Framebuffer init failed, GUI disabled");
        }
    } else {
        klog_warn("No framebuffer tag from bootloader, GUI disabled");
    }

    /* === Phase 5: Aether OS Architectural Layer === */
    kinfo("--- " OS_NAME " Kernel Layer ---");

    kinfo("Initializing capability security table...");
    cap_table_init();                            /* [STABLE] */

    kinfo("Initializing security monitor...");
    secmon_init();                               /* [STABLE] */

    kinfo("Initializing IPC engine...");
    ipc_init();                                  /* [STABLE] */

    kinfo("Initializing service bus...");
    svcbus_init();                               /* [STABLE] */

    kinfo("Initializing shared memory subsystem...");
    shm_init();                                  /* [STABLE] */

    /* Buddy allocator: seed with 16MB starting above the kernel heap */
    {
        uint64_t buddy_base_phys = 0x2000000;  /* 32 MB mark */
        uint64_t buddy_base_virt = (uint64_t)PHYS_TO_VIRT(buddy_base_phys);
        uint64_t buddy_size      = 16 * 1024 * 1024;  /* 16 MB */
        kinfo("Initializing buddy allocator (16 MB at phys 0x%llx)...",
              (unsigned long long)buddy_base_phys);
        buddy_init(buddy_base_phys, buddy_base_virt, buddy_size); /* [STABLE] */
    }

    kinfo("[BOOT] Kernel Layer OK — caps=%u  buddy=%llu KB",
          cap_count(), (unsigned long long)(buddy_free_bytes() / 1024));
    kinfo("  IPC ports    : initialized");
    kinfo("  Service bus  : initialized");

    /* === Phase 4c: USB HID === */
    kinfo("Initializing USB (UHCI skeleton)...");
    usb_init();                                  /* [STUB] — detects controller only */
    kinfo("[BOOT] USB stub OK");

    /* === Phase 4d: PCI bus and network === */
    kinfo("Scanning PCI bus...");
    pci_init();                                  /* [STABLE] */
    kinfo("[BOOT] PCI scan OK");

    kinfo("Initializing e1000 Ethernet driver...");
    if (e1000_init() == 0) {                     /* [STABLE] */
        kinfo("Initializing network stack...");
        net_init();                              /* [STABLE] */
        arp_announce();
        kinfo("[BOOT] Network stack OK");
        kinfo("Starting DHCP discovery...");
        if (dhcp_discover() == 0)                /* [PARTIAL] — IPv4 only */
            kinfo("[BOOT] DHCP lease acquired");
        else
            klog_warn("DHCP: no lease (static IP or no DHCP server)");
    } else {
        kinfo("[BOOT] No e1000 NIC found — networking disabled");
    }

    /* === Phase 6: Filesystem + disk manager === */
    kinfo("Initializing VFS...");
    vfs_init();                                  /* [STABLE] */

    /* Mount ramfs as VFS root BEFORE diskman_init() calls vfs_mkdir("/mnt") */
    {
        extern vfs_node_t* ramfs_create_root(void);
        vfs_mount_root(ramfs_create_root());     /* [STABLE] */
        kinfo("[BOOT] VFS + ramfs root OK");
    }

    kinfo("Scanning disks and mounting partitions...");
    diskman_init();                              /* [STABLE] */
    kinfo("[BOOT] Disk manager OK — %d partitions", diskman_count());

    /* Mount proc filesystem at /proc */
    {
        extern void procfs_init(void);
        procfs_init();                           /* [PARTIAL] — read-only stubs */
        kinfo("[BOOT] procfs OK");
    }

    /* === Phase 7: Process subsystem === */
    kinfo("Initializing process manager...");
    process_init();                              /* [STABLE] */
    kinfo("[BOOT] Process manager OK");

    kinfo("Initializing scheduler...");
    scheduler_init();                            /* [STABLE] */
    kinfo("[BOOT] Scheduler OK");

    kinfo("Initializing TTY subsystem...");
    tty_init();                                  /* [STABLE] */
    kinfo("[BOOT] TTY OK");

    /* === Phase 7b: User accounts and package manager === */
    kinfo("Initializing user account system...");
    users_init();                                /* [STABLE] */
    kinfo("[BOOT] User accounts OK");

    kinfo("Initializing package manager (apkg)...");
    apkg_init();                                 /* [PARTIAL] — install/list only */
    kinfo("Initializing package manager (pkg/.aur)...");
    pkg_init();                                  /* [STUB] — AUR resolver placeholder */
    kinfo("[BOOT] Package managers OK");

    /* === Phase 8: Syscall interface === */
    kinfo("Initializing system call interface...");
    syscall_init();                              /* [STABLE] — 50+ calls via int 0x80 */
    kinfo("[BOOT] Syscall interface OK");

    /* === Phase 9: Enable interrupts and start scheduling === */
    kernel_state = KERNEL_STATE_RUNNING;
    kinfo("[BOOT] ========================================");
    kinfo("[BOOT] All subsystems initialized. Enabling interrupts.");
    kinfo("[BOOT] ========================================");
    cpu_sti();

    /* === Phase 9: Launch userland === */
    init_userland();

    /* === Phase 9b: Boot animation ===
     *
     * Now that interrupts are enabled and the scheduler is running we can
     * let the PIT drive the animation at 100 Hz.  We distribute the five
     * pre-registered steps evenly across the PROGRESS phase window
     * (~800 ms) so the progress bar fills smoothly, then spin-wait for
     * the fade-out to complete before handing off to the render engine.
     */
    if (fb_ready()) {
        kinfo("Playing boot animation...");
        bootanim_start();
        timer_register_callback(bootanim_timer_cb);

        /* Advance one step every ~14 ticks (140 ms).
         * 5 steps × 14 ticks = 70 ticks ≈ fits the 80-tick PROGRESS phase. */
        for (int step = 0; step < 5; step++) {
            uint32_t t0 = (uint32_t)timer_ticks();
            while ((uint32_t)timer_ticks() - t0 < 14)
                scheduler_yield();
            bootanim_step_done();
        }

        /* Wait for the fade-out phase to complete */
        while (!bootanim_done())
            scheduler_yield();

        timer_register_callback(NULL);   /* release the callback slot */
        fb_clear(0xFF000000);            /* clean black for ARE handoff */
        fb_flip();
        kinfo("Boot animation complete.");
    }

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
        "  | |_| || |_  | '_ \\/ _ \\| |  |  \\___ \\  \n"
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
