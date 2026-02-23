/*
 * userland/init.c - Init process (PID 1)
 *
 * The first user-space process launched by the kernel.
 * Responsible for:
 *   1. Mounting filesystems
 *   2. Printing the message of the day
 *   3. Launching the interactive shell
 *   4. Reaping zombie processes in an infinite loop
 */
#include <kernel.h>
#include <process.h>
#include <scheduler.h>
#include <fs/vfs.h>
#include <drivers/vga.h>
#include <drivers/timer.h>
#include <memory.h>
#include <types.h>
#include <string.h>

/* Forward declarations */
extern vfs_node_t* ramfs_create_root(void);
void shell_run(void);

/*
 * print_motd - display the message of the day from /etc/motd.
 */
static void print_motd(void)
{
    vfs_node_t* node = vfs_open("/etc/motd", O_RDONLY);
    if (!node) return;

    char buf[512];
    ssize_t n = vfs_read(node, 0, sizeof(buf) - 1, buf);
    if (n > 0) {
        buf[n] = '\0';
        vga_set_color(vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
        vga_puts(buf);
        vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }
    vfs_close(node);
}

/*
 * init_process_entry - kernel thread that acts as PID 1 (init).
 *
 * This runs in kernel mode (ring 0) but initializes the userland environment.
 * For a full OS, this would exec() a real /sbin/init binary.
 */
void init_process_entry(void)
{
    kinfo("Init process started (PID 1)");

    /* Mount RAM filesystem as root (kernel mounts it first; guard here) */
    if (!vfs_root()) {
        vfs_node_t* root = ramfs_create_root();
        vfs_mount_root(root);
    }

    /* Give the system a moment to settle */
    timer_sleep_ms(100);

    /* Print banner */
    vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    print_motd();

    /* Launch the shell */
    kinfo("Init: launching shell");
    shell_run();

    /* If shell exits, loop forever (init should never exit) */
    kinfo("Shell exited - system will halt");
    while (1) {
        timer_sleep_ms(1000);
    }
}
