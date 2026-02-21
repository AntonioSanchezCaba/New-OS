/*
 * kernel/panic.c - Kernel panic handler
 *
 * When something unrecoverable happens in the kernel, kernel_panic() is
 * called. It disables interrupts, prints a diagnostic message with the
 * file/line of the failure, dumps a stack trace, and halts the CPU.
 */
#include <kernel.h>
#include <types.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <stdarg.h>

/* Provides vsnprintf for formatting the panic message */
extern int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

/*
 * kernel_panic - print panic message and halt system.
 *
 * This function is marked NORETURN because it never returns.
 * Called via the kpanic() macro which inserts __FILE__ and __LINE__.
 */
void NORETURN kernel_panic(const char* file, int line, const char* fmt, ...)
{
    /* Disable interrupts immediately to prevent further damage */
    cpu_cli();
    kernel_set_state(KERNEL_STATE_PANIC);

    /* Format the panic message */
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Print to VGA with red background */
    vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    vga_puts("\n\n");
    vga_puts("  *** KERNEL PANIC ***\n\n");

    vga_set_color(vga_make_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    vga_printf("  %s\n\n", msg);

    vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    vga_printf("  Location: %s:%d\n", file, line);
    vga_puts("  System halted.\n\n");

    /* Also output to serial for debugging */
    debug_printf("\n\n*** KERNEL PANIC ***\n");
    debug_printf("  Message:  %s\n", msg);
    debug_printf("  Location: %s:%d\n", file, line);

    /* Dump stack trace */
    kernel_dump_stack_trace();

    /* Halt forever */
    while (1) {
        cpu_cli();
        cpu_halt();
    }
}

/*
 * kernel_dump_stack_trace - walk the frame pointer chain and print addresses.
 *
 * Relies on the kernel being compiled with -fno-omit-frame-pointer so that
 * RBP is a valid frame pointer at each call site.
 */
void kernel_dump_stack_trace(void)
{
    /* Stack frame structure: [saved RBP] [return address] */
    struct stack_frame {
        struct stack_frame* prev;
        uint64_t ret_addr;
    };

    struct stack_frame* frame;
    __asm__ volatile("movq %%rbp, %0" : "=r"(frame));

    debug_puts("\nStack trace:\n");
    vga_puts("  Stack trace:\n");

    int depth = 0;
    while (frame && depth < 16) {
        /* Sanity check: frame must be in a valid kernel virtual address range */
        uint64_t faddr = (uint64_t)frame;
        if (faddr < KERNEL_VMA_BASE || faddr > (KERNEL_VMA_BASE + 0x40000000ULL)) {
            break;
        }

        debug_printf("    [%2d] 0x%016llx\n", depth, frame->ret_addr);
        vga_printf("    [%2d] 0x%016llx\n", depth, frame->ret_addr);

        frame = frame->prev;
        depth++;
    }

    if (depth == 0) {
        debug_puts("    (no frames available)\n");
        vga_puts("    (no frames available)\n");
    }
}
