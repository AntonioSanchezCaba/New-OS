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

/* Keyboard scancode set 1 special keys */
#define KEY_ESCAPE    0x01
#define KEY_BACKSPACE 0x0E
#define KEY_TAB       0x0F
#define KEY_ENTER     0x1C
#define KEY_LCTRL     0x1D
#define KEY_LSHIFT    0x2A
#define KEY_RSHIFT    0x36
#define KEY_LALT      0x38
#define KEY_CAPSLOCK  0x3A
#define KEY_F1        0x3B
#define KEY_F2        0x3C
#define KEY_F3        0x3D
#define KEY_F4        0x3E
#define KEY_F5        0x3F
#define KEY_F6        0x40
#define KEY_F7        0x41
#define KEY_F8        0x42
#define KEY_F9        0x43
#define KEY_F10       0x44
#define KEY_DELETE    0x53
#define KEY_UP        0x48
#define KEY_DOWN      0x50
#define KEY_LEFT      0x4B
#define KEY_RIGHT     0x4D

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

extern kbd_state_t kbd_state;

#endif /* DRIVERS_KEYBOARD_H */
