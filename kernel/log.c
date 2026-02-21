/*
 * kernel/log.c - Kernel logging subsystem
 *
 * Provides kernel_log(), which is the backend for the kinfo/kdebug/kwarn/
 * kerror macros. Output goes to both VGA and the serial debug port.
 */
#include <kernel.h>
#include <types.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/timer.h>
#include <stdarg.h>

extern int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

/* Level names and VGA colors */
static const char* const level_names[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "PANIC"
};

static const uint8_t level_colors[] = {
    /* DEBUG */ 0,  /* set below */
    /* INFO  */ 0,
    /* WARN  */ 0,
    /* ERROR */ 0,
    /* PANIC */ 0,
};

/*
 * kernel_log - format and emit a log message.
 *
 * @level: LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, or LOG_PANIC
 * @file:  source file name (__FILE__)
 * @line:  source line number (__LINE__)
 * @fmt:   printf-style format string
 */
void kernel_log(int level, const char* file, int line, const char* fmt, ...)
{
    if (level < LOG_LEVEL) return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Get uptime ticks */
    uint64_t ticks = timer_ticks();
    uint32_t secs  = (uint32_t)(ticks / TIMER_FREQ);
    uint32_t ms    = (uint32_t)((ticks % TIMER_FREQ) * 1000 / TIMER_FREQ);

    /* Choose VGA color by level */
    uint8_t color;
    switch (level) {
        case LOG_DEBUG: color = vga_make_color(VGA_COLOR_DARK_GREY,   VGA_COLOR_BLACK); break;
        case LOG_INFO:  color = vga_make_color(VGA_COLOR_LIGHT_GREEN,  VGA_COLOR_BLACK); break;
        case LOG_WARN:  color = vga_make_color(VGA_COLOR_YELLOW,       VGA_COLOR_BLACK); break;
        case LOG_ERROR: color = vga_make_color(VGA_COLOR_LIGHT_RED,    VGA_COLOR_BLACK); break;
        default:        color = vga_make_color(VGA_COLOR_WHITE,        VGA_COLOR_RED);   break;
    }

    /* Print to VGA */
    uint8_t old_color = vga_current_color;
    vga_set_color(vga_make_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK));
    vga_printf("[%4u.%03u] ", secs, ms);
    vga_set_color(color);
    vga_printf("[%s] ", level_names[level]);
    vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    vga_printf("%s\n", msg);
    vga_set_color(old_color);

    /* Print to serial */
    debug_printf("[%4u.%03u] [%s] %s\n", secs, ms, level_names[level], msg);

    /* Unused: file/line (available for verbose mode) */
    (void)file;
    (void)line;
}
