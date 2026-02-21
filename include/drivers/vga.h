/*
 * drivers/vga.h - VGA text mode driver interface
 */
#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <types.h>

/* VGA text buffer dimensions */
#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_BUFFER  0xB8000

/* VGA color codes */
typedef enum {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW        = 14,
    VGA_COLOR_WHITE         = 15,
} vga_color_t;

/* Compose foreground/background color into a VGA attribute byte */
static inline uint8_t vga_make_color(vga_color_t fg, vga_color_t bg) {
    return (uint8_t)(fg | (bg << 4));
}

/* Combine a character and color attribute into a VGA cell (16-bit) */
static inline uint16_t vga_make_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

/* VGA driver API */
void vga_init(void);
void vga_clear(void);
void vga_set_color(uint8_t color);
void vga_putchar(char c);
void vga_puts(const char* str);
void vga_printf(const char* fmt, ...);
void vga_set_cursor(int row, int col);
void vga_get_cursor(int* row, int* col);
void vga_scroll(void);
void vga_enable_cursor(uint8_t start, uint8_t end);
void vga_disable_cursor(void);

/* Current VGA color attribute */
extern uint8_t vga_current_color;

#endif /* DRIVERS_VGA_H */
