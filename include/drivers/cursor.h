/*
 * include/drivers/cursor.h — Software cursor sprite system
 *
 * Renders a hardware-style cursor sprite in software by compositing it
 * on top of the framebuffer at the very end of each frame. Saves and
 * restores the background under the cursor to avoid flickering.
 */
#pragma once
#include <types.h>

/* Cursor types */
typedef enum {
    CURSOR_ARROW  = 0,   /* Standard arrow (default)           */
    CURSOR_HAND   = 1,   /* Pointing hand (links/buttons)      */
    CURSOR_TEXT   = 2,   /* I-beam (text fields)               */
    CURSOR_WAIT   = 3,   /* Spinning/hourglass                 */
    CURSOR_RESIZE_H = 4, /* Horizontal resize (←→)            */
    CURSOR_RESIZE_V = 5, /* Vertical resize (↕)               */
    CURSOR_RESIZE_D = 6, /* Diagonal resize (↖↘)             */
    CURSOR_CROSS  = 7,   /* Crosshair                          */
    CURSOR_NONE   = 8,   /* Hidden cursor                      */
    CURSOR_COUNT  = 9
} cursor_type_t;

/* Maximum cursor sprite dimensions */
#define CURSOR_MAX_W  20
#define CURSOR_MAX_H  24

void cursor_init(void);
void cursor_show(void);
void cursor_hide(void);
void cursor_set_type(cursor_type_t type);
void cursor_move(int x, int y);    /* Called by mouse driver */
void cursor_render(void);          /* Called at end of each frame */
void cursor_erase(void);           /* Restore saved background */

/* Query */
int         cursor_x(void);
int         cursor_y(void);
cursor_type_t cursor_get_type(void);
bool        cursor_is_visible(void);
