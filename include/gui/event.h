/*
 * include/gui/event.h - GUI event system
 *
 * Events flow from hardware drivers -> global event queue -> window manager
 * -> focused window callback.
 */
#ifndef GUI_EVENT_H
#define GUI_EVENT_H

#include <types.h>

/* Event types */
typedef enum {
    GUI_EVENT_NONE = 0,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_CHAR,         /* Printable character typed */
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_MOUSE_SCROLL,
    GUI_EVENT_PAINT,        /* Window needs to be redrawn */
    GUI_EVENT_RESIZE,
    GUI_EVENT_CLOSE,
    GUI_EVENT_FOCUS_IN,
    GUI_EVENT_FOCUS_OUT,
    GUI_EVENT_TIMER,
} gui_event_type_t;

/* Key codes (subset of PC scan codes, translated) */
#define KEY_ENTER      '\n'
#define KEY_TAB        '\t'
#define KEY_BACKSPACE  '\b'
#define KEY_ESCAPE     0x1B
#define KEY_UP_ARROW   0x100
#define KEY_DOWN_ARROW 0x101
#define KEY_LEFT_ARROW 0x102
#define KEY_RIGHT_ARROW 0x103
#define KEY_HOME       0x104
#define KEY_END        0x105
#define KEY_PAGE_UP    0x106
#define KEY_PAGE_DOWN  0x107
#define KEY_DELETE     0x108
#define KEY_F1         0x11B
#define KEY_F2         0x11C
#define KEY_F12        0x127

/* Modifier flags */
#define MOD_SHIFT  0x01
#define MOD_CTRL   0x02
#define MOD_ALT    0x04

/* GUI event structure */
typedef struct {
    gui_event_type_t type;

    union {
        /* Key event */
        struct {
            int     keycode;
            uint8_t modifiers;
            char    ch;         /* Printable character (0 if non-printable) */
        } key;

        /* Mouse event */
        struct {
            int     x, y;      /* Window-relative coordinates */
            int     dx, dy;    /* Delta movement */
            uint8_t buttons;   /* MOUSE_BTN_* bitmask */
            int     scroll;    /* +1 up, -1 down */
        } mouse;

        /* Resize event */
        struct {
            int w, h;
        } resize;
    };
} gui_event_t;

/* Event queue API */
#define GUI_EVENT_QUEUE_SIZE 256

void  gui_event_queue_init(void);
void  gui_event_push(const gui_event_t* evt);
bool  gui_event_pop(gui_event_t* evt);
int   gui_event_count(void);

/* Post events from keyboard/mouse drivers */
void  gui_post_key(int keycode, uint8_t mods, char ch, bool down);
void  gui_post_mouse(int x, int y, int dx, int dy, uint8_t buttons);

#endif /* GUI_EVENT_H */
