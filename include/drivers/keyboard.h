/*
 * drivers/keyboard.h - PS/2 keyboard driver interface
 */
#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <types.h>

/* PS/2 keyboard I/O ports */
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_CMD_PORT     0x64

/* PS/2 keyboard scancode set 1 values (raw hardware scan codes)
 * Use KBD_SC_* in driver code that processes raw scan codes.
 * GUI-level translated keycodes are in include/gui/event.h as KEY_*. */
#define KBD_SC_ESCAPE    0x01
#define KBD_SC_BACKSPACE 0x0E
#define KBD_SC_TAB       0x0F
#define KBD_SC_ENTER     0x1C
#define KBD_SC_LCTRL     0x1D
#define KBD_SC_LSHIFT    0x2A
#define KBD_SC_RSHIFT    0x36
#define KBD_SC_LALT      0x38
#define KBD_SC_CAPSLOCK  0x3A
#define KBD_SC_F1        0x3B
#define KBD_SC_F2        0x3C
#define KBD_SC_F3        0x3D
#define KBD_SC_F4        0x3E
#define KBD_SC_F5        0x3F
#define KBD_SC_F6        0x40
#define KBD_SC_F7        0x41
#define KBD_SC_F8        0x42
#define KBD_SC_F9        0x43
#define KBD_SC_F10       0x44
#define KBD_SC_DELETE    0x53
#define KBD_SC_UP        0x48
#define KBD_SC_DOWN      0x50
#define KBD_SC_LEFT      0x4B
#define KBD_SC_RIGHT     0x4D

/* Modifier and non-printable scancodes (no gui/event.h conflict) */
#define KBD_SC_LCTRL_VAL    KBD_SC_LCTRL
#define KBD_SC_LSHIFT_VAL   KBD_SC_LSHIFT
#define KBD_SC_RSHIFT_VAL   KBD_SC_RSHIFT
#define KBD_SC_LALT_VAL     KBD_SC_LALT
#define KBD_SC_CAPSLOCK_VAL KBD_SC_CAPSLOCK

/* Key release flag: scancode | 0x80 */
#define KEY_RELEASED  0x80

/* Keyboard buffer size */
#define KBD_BUFFER_SIZE 256

/* Keyboard state flags */
typedef struct {
    bool shift_down;
    bool ctrl_down;
    bool alt_down;
    bool caps_lock;
} kbd_state_t;

/* Keyboard driver API */
void  keyboard_init(void);
char  keyboard_getchar(void);    /* Blocking: waits for keypress */
int   keyboard_available(void);  /* Returns 1 if char is available */
char  keyboard_read(void);       /* Non-blocking: returns 0 if no key */
void  keyboard_irq_handler(void* regs);

/* Read a line from keyboard into buffer (returns length) */
int   keyboard_readline(char* buf, int max_len);

/* Non-blocking raw event poll for Aether input system.
 * keycode uses gui/event.h KEY_* values (0x100..0x103 for arrows).
 * mods: MOD_SHIFT(0x01)|MOD_CTRL(0x02)|MOD_ALT(0x04).
 * Returns false if queue is empty. */
bool  keyboard_poll(int* keycode, uint8_t* mods, char* ch, bool* down);

extern kbd_state_t kbd_state;

#endif /* DRIVERS_KEYBOARD_H */
