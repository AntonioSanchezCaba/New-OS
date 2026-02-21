/*
 * drivers/keyboard.c - PS/2 keyboard driver (scancode set 1)
 *
 * Receives scancodes from the PS/2 controller (I/O port 0x60) via IRQ1,
 * translates them to ASCII characters, and stores them in a ring buffer.
 */
#include <drivers/keyboard.h>
#include <interrupts.h>
#include <kernel.h>
#include <types.h>

/* ============================================================
 * Scancode -> ASCII translation tables (scancode set 1)
 * ============================================================ */

/* Normal (unshifted) characters */
static const char kbd_us_normal[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   '-', 0,   0,   0,   '+', 0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0
};

/* Shifted characters */
static const char kbd_us_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"',  '~',
    0,    '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   '-', 0,   0,   0,   '+', 0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,
    0,    0
};

/* ============================================================
 * Ring buffer for keyboard input
 * ============================================================ */

static char   kbd_buffer[KBD_BUFFER_SIZE];
static int    kbd_buf_head = 0;   /* Write position */
static int    kbd_buf_tail = 0;   /* Read position */

kbd_state_t kbd_state = { false, false, false, false };

static inline bool buf_empty(void)
{
    return kbd_buf_head == kbd_buf_tail;
}

static inline bool buf_full(void)
{
    return ((kbd_buf_head + 1) % KBD_BUFFER_SIZE) == kbd_buf_tail;
}

static void buf_push(char c)
{
    if (!buf_full()) {
        kbd_buffer[kbd_buf_head] = c;
        kbd_buf_head = (kbd_buf_head + 1) % KBD_BUFFER_SIZE;
    }
}

static char buf_pop(void)
{
    if (buf_empty()) return 0;
    char c = kbd_buffer[kbd_buf_tail];
    kbd_buf_tail = (kbd_buf_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

/* ============================================================
 * IRQ handler
 * ============================================================ */

void keyboard_irq_handler(void* regs)
{
    (void)regs;

    /* Read scancode from PS/2 data port */
    uint8_t scancode = inb(KBD_DATA_PORT);

    /* Key release: scancode has bit 7 set */
    bool released = (scancode & KEY_RELEASED) != 0;
    uint8_t key   = scancode & ~KEY_RELEASED;

    /* Update modifier key state */
    switch (key) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            kbd_state.shift_down = !released;
            return;
        case KEY_LCTRL:
            kbd_state.ctrl_down = !released;
            return;
        case KEY_LALT:
            kbd_state.alt_down = !released;
            return;
        case KEY_CAPSLOCK:
            if (!released) {
                kbd_state.caps_lock = !kbd_state.caps_lock;
            }
            return;
    }

    if (released) return;  /* Ignore key-up events for regular keys */

    /* Translate scancode to ASCII */
    char c = 0;
    if (key < 128) {
        bool shift_active = kbd_state.shift_down ^
                            (kbd_state.caps_lock &&
                             ((key >= 0x10 && key <= 0x19) ||  /* q-p */
                              (key >= 0x1E && key <= 0x26) ||  /* a-l */
                              (key >= 0x2C && key <= 0x32)));  /* z-m */

        c = shift_active ? kbd_us_shift[key] : kbd_us_normal[key];
    }

    if (c) {
        buf_push(c);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void keyboard_init(void)
{
    /* Flush any pending data in the PS/2 buffer */
    while (inb(KBD_STATUS_PORT) & 0x01) {
        inb(KBD_DATA_PORT);
    }

    /* Register IRQ1 handler */
    irq_register_handler(1, (irq_handler_t)keyboard_irq_handler);

    kinfo("PS/2 keyboard initialized");
}

int keyboard_available(void)
{
    return !buf_empty();
}

char keyboard_read(void)
{
    return buf_pop();
}

char keyboard_getchar(void)
{
    /* Block until a character is available */
    while (buf_empty()) {
        cpu_halt();  /* Wait for IRQ */
    }
    return buf_pop();
}

/*
 * keyboard_readline - read a full line (up to max_len-1 chars) from keyboard.
 * Handles backspace and echoes characters to VGA.
 * Returns number of characters read (not including null terminator).
 */
int keyboard_readline(char* buf, int max_len)
{
    extern void vga_putchar(char);

    int len = 0;
    while (1) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            vga_putchar('\n');
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                vga_putchar('\b');
            }
        } else if (len < max_len - 1) {
            buf[len++] = c;
            vga_putchar(c);
        }
    }

    buf[len] = '\0';
    return len;
}
