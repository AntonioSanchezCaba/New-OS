/*
 * include/drivers/mouse.h - PS/2 mouse driver
 */
#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <types.h>

/* Mouse button masks */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Mouse state */
typedef struct {
    int     x, y;          /* Current screen position */
    int     dx, dy;        /* Delta since last update */
    uint8_t buttons;       /* Button bitmask */
    uint8_t prev_buttons;  /* Previous buttons (for click detection) */
} mouse_state_t;

extern mouse_state_t mouse;

/* Mouse events */
typedef struct {
    int     x, y;
    int     dx, dy;
    uint8_t buttons;
    bool    left_clicked;   /* Left button just pressed */
    bool    left_released;
    bool    right_clicked;
} mouse_event_t;

/* Mouse driver API */
void mouse_init(void);
void mouse_irq_handler(void* regs);
void mouse_get_event(mouse_event_t* evt);
void mouse_set_bounds(int max_x, int max_y);

/* Cursor rendering */
void mouse_draw_cursor(uint32_t* back_buf, int fb_width, int fb_height);

#endif /* DRIVERS_MOUSE_H */
