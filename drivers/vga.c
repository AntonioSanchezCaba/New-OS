/*
 * drivers/vga.c - VGA text mode driver (80x25)
 *
 * Provides character output, scrolling, cursor control, and printf-style
 * formatting to the standard 80x25 VGA text mode buffer at 0xB8000.
 */
#include <drivers/vga.h>
#include <kernel.h>
#include <types.h>
#include <stdarg.h>

extern int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);

/* VGA text buffer in kernel virtual address space */
static volatile uint16_t* const vga_buffer = (uint16_t*)(KERNEL_VMA_BASE + VGA_BUFFER);

/* Current cursor position */
static int vga_row = 0;
static int vga_col = 0;

/* Current color attribute */
uint8_t vga_current_color;

/* ============================================================
 * VGA hardware cursor (blinking block in text mode)
 * ============================================================ */

#define VGA_CTRL_REG  0x3D4
#define VGA_DATA_REG  0x3D5

static void vga_update_hw_cursor(void)
{
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb(VGA_CTRL_REG, 0x0F);
    outb(VGA_DATA_REG, (uint8_t)(pos & 0xFF));
    outb(VGA_CTRL_REG, 0x0E);
    outb(VGA_DATA_REG, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_enable_cursor(uint8_t start, uint8_t end)
{
    outb(VGA_CTRL_REG, 0x0A);
    outb(VGA_DATA_REG, (inb(VGA_DATA_REG) & 0xC0) | start);
    outb(VGA_CTRL_REG, 0x0B);
    outb(VGA_DATA_REG, (inb(VGA_DATA_REG) & 0xE0) | end);
}

void vga_disable_cursor(void)
{
    outb(VGA_CTRL_REG, 0x0A);
    outb(VGA_DATA_REG, 0x20);  /* Bit 5 = cursor disable */
}

/* ============================================================
 * Core operations
 * ============================================================ */

void vga_init(void)
{
    vga_current_color = vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_row = 0;
    vga_col = 0;
    vga_clear();
    vga_enable_cursor(14, 15);
}

void vga_clear(void)
{
    uint16_t blank = vga_make_entry(' ', vga_current_color);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_hw_cursor();
}

void vga_set_color(uint8_t color)
{
    vga_current_color = color;
}

void vga_set_cursor(int row, int col)
{
    vga_row = row;
    vga_col = col;
    vga_update_hw_cursor();
}

void vga_get_cursor(int* row, int* col)
{
    if (row) *row = vga_row;
    if (col) *col = vga_col;
}

/*
 * vga_scroll - scroll the screen up by one line.
 */
void vga_scroll(void)
{
    /* Move all lines up by one */
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            vga_buffer[(row - 1) * VGA_WIDTH + col] =
                vga_buffer[row * VGA_WIDTH + col];
        }
    }

    /* Clear the last line */
    uint16_t blank = vga_make_entry(' ', vga_current_color);
    for (int col = 0; col < VGA_WIDTH; col++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
    }

    vga_row = VGA_HEIGHT - 1;
}

/*
 * vga_putchar - write one character to the screen at the current position.
 * Handles newlines, carriage returns, backspace, and tab.
 */
void vga_putchar(char c)
{
    switch (c) {
        case '\n':
            vga_col = 0;
            vga_row++;
            break;

        case '\r':
            vga_col = 0;
            break;

        case '\b':
            if (vga_col > 0) {
                vga_col--;
                vga_buffer[vga_row * VGA_WIDTH + vga_col] =
                    vga_make_entry(' ', vga_current_color);
            }
            break;

        case '\t':
            vga_col = ALIGN_UP(vga_col + 1, 8);
            if (vga_col >= VGA_WIDTH) {
                vga_col = 0;
                vga_row++;
            }
            break;

        default:
            vga_buffer[vga_row * VGA_WIDTH + vga_col] =
                vga_make_entry(c, vga_current_color);
            vga_col++;
            break;
    }

    /* Wrap to next line if we reach the right edge */
    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    /* Scroll if we go past the bottom */
    if (vga_row >= VGA_HEIGHT) {
        vga_scroll();
    }

    vga_update_hw_cursor();
}

void vga_puts(const char* str)
{
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_printf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vga_puts(buf);
}
