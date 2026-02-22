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
 * Ring buffer for keyboard input (legacy char API)
 * ============================================================ */

static char   kbd_buffer[KBD_BUFFER_SIZE];
static int    kbd_buf_head = 0;   /* Write position */
static int    kbd_buf_tail = 0;   /* Read position */

kbd_state_t kbd_state = { false, false, false, false };

/* ============================================================
 * Raw event queue for Aether input system (keyboard_poll)
 * Keycodes use gui/event.h values:
 *   '\n'=enter, '\b'=backspace, '\t'=tab, 0x1B=ESC,
 *   0x100=UP, 0x101=DOWN, 0x102=LEFT, 0x103=RIGHT
 * ============================================================ */
#define KBD_RAW_SZ 64

typedef struct {
    int     keycode;
    uint8_t mods;
    char    ch;
    bool    down;
} kbd_raw_t;

static kbd_raw_t kbd_raw_buf[KBD_RAW_SZ];
static int       kbd_raw_head = 0;
static int       kbd_raw_tail = 0;
static bool      kbd_e0       = false;  /* E0 extended prefix seen */

static void raw_push(int keycode, uint8_t mods, char ch, bool down)
{
    int next = (kbd_raw_head + 1) % KBD_RAW_SZ;
    if (next == kbd_raw_tail) return;  /* full, drop */
    kbd_raw_buf[kbd_raw_head].keycode = keycode;
    kbd_raw_buf[kbd_raw_head].mods    = mods;
    kbd_raw_buf[kbd_raw_head].ch      = ch;
    kbd_raw_buf[kbd_raw_head].down    = down;
    kbd_raw_head = next;
}

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

    /* E0 extended key prefix */
    if (scancode == 0xE0) {
        kbd_e0 = true;
        return;
    }

    /* Build modifier byte for raw event */
    uint8_t mods = 0;
    if (kbd_state.shift_down) mods |= 0x01; /* MOD_SHIFT */
    if (kbd_state.ctrl_down)  mods |= 0x02; /* MOD_CTRL  */
    if (kbd_state.alt_down)   mods |= 0x04; /* MOD_ALT   */

    /* Key release: scancode has bit 7 set */
    bool released = (scancode & KEY_RELEASED) != 0;
    uint8_t key   = scancode & ~KEY_RELEASED;

    /* Handle E0-prefixed extended keys (arrows, etc.) */
    if (kbd_e0) {
        kbd_e0 = false;
        int kc = 0;
        switch (key) {
            case 0x48: kc = 0x100; break;  /* KEY_UP_ARROW    */
            case 0x50: kc = 0x101; break;  /* KEY_DOWN_ARROW  */
            case 0x4B: kc = 0x102; break;  /* KEY_LEFT_ARROW  */
            case 0x4D: kc = 0x103; break;  /* KEY_RIGHT_ARROW */
            case 0x1C: kc = '\n';  break;  /* Numpad Enter    */
            default: break;
        }
        if (kc) raw_push(kc, mods, (kc < 0x80 ? (char)kc : 0), !released);
        return;
    }

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

    /* Map special scancodes to gui-style keycodes */
    int gui_kc = 0;
    char gui_ch = 0;
    switch (key) {
        case KEY_ESCAPE:   gui_kc = 0x1B; gui_ch = 0x1B; break;
        case KEY_ENTER:    gui_kc = '\n';  gui_ch = '\n'; break;
        case KEY_BACKSPACE:gui_kc = '\b';  gui_ch = '\b'; break;
        case KEY_TAB:      gui_kc = '\t';  gui_ch = '\t'; break;
        default: break;
    }

    if (!gui_kc && key < 128) {
        bool shift_active = kbd_state.shift_down ^
                            (kbd_state.caps_lock &&
                             ((key >= 0x10 && key <= 0x19) ||  /* q-p */
                              (key >= 0x1E && key <= 0x26) ||  /* a-l */
                              (key >= 0x2C && key <= 0x32)));  /* z-m */
        gui_ch = shift_active ? kbd_us_shift[key] : kbd_us_normal[key];
        gui_kc = (int)(unsigned char)gui_ch;
    }

    /* Push to raw queue (both key-down and key-up) */
    if (gui_kc || gui_ch) {
        raw_push(gui_kc, mods, gui_ch, !released);
    }

    /* Legacy char buffer: key-down, printable/control chars only */
    if (!released && gui_ch) {
        buf_push(gui_ch);
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

/*
 * keyboard_poll - non-blocking raw event read for the Aether input system.
 * Returns true if an event was dequeued.
 * keycode: gui/event.h KEY_* value (0x100..0x103 for arrows)
 * mods:    MOD_SHIFT(0x01) | MOD_CTRL(0x02) | MOD_ALT(0x04)
 * ch:      ASCII char (0 for non-printable)
 * down:    true=press, false=release
 */
bool keyboard_poll(int* keycode, uint8_t* mods, char* ch, bool* down)
{
    if (kbd_raw_head == kbd_raw_tail) return false;
    kbd_raw_t* ev = &kbd_raw_buf[kbd_raw_tail];
    kbd_raw_tail = (kbd_raw_tail + 1) % KBD_RAW_SZ;
    *keycode = ev->keycode;
    *mods    = ev->mods;
    *ch      = ev->ch;
    *down    = ev->down;
    return true;
}
