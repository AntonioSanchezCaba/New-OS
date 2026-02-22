/*
 * aether/input.c — Aether Input Processing
 *
 * Drains PS/2 keyboard + mouse drivers, normalises events, and
 * recognises navigation gestures (Alt+Arrow, Alt+Tab, Super).
 */
#include <aether/input.h>
#include <gui/event.h>       /* KEY_* constants */
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Internal ring buffer
 * ========================================================= */
#define INPUT_QUEUE_SZ  128

static input_event_t g_queue[INPUT_QUEUE_SZ];
static int           g_head = 0;
static int           g_tail = 0;

static void queue_push(const input_event_t* ev)
{
    int next = (g_tail + 1) % INPUT_QUEUE_SZ;
    if (next == g_head) return;  /* full, drop */
    g_queue[g_tail] = *ev;
    g_tail = next;
}

bool input_pop(input_event_t* out)
{
    if (g_head == g_tail) return false;
    *out   = g_queue[g_head];
    g_head = (g_head + 1) % INPUT_QUEUE_SZ;
    return true;
}

/* =========================================================
 * State
 * ========================================================= */
static vec2_t  g_ptr    = { 0, 0 };
static uint8_t g_ptr_buttons = 0;
static uint8_t g_mods   = 0;  /* MOD_SHIFT | MOD_CTRL | MOD_ALT */

void input_init(void)
{
    g_head = g_tail = 0;
    kinfo("ARE: input system initialized");
}

vec2_t input_pointer_pos(void) { return g_ptr; }

/* =========================================================
 * Gesture key mapping
 *   Alt + Left       → SWIPE_LEFT  (prev surface)
 *   Alt + Right      → SWIPE_RIGHT (next surface)
 *   Alt + Up         → OVERVIEW
 *   Alt + Down       → FOCUS
 *   Alt + W / Super  → LAUNCHER
 *   Alt + Q          → CLOSE_SURFACE
 * ========================================================= */
bool input_is_nav_key(const key_event_t* k)
{
    if (!k->down) return false;
    bool alt = (k->mods & MOD_ALT) != 0;
    if (!alt) return false;
    switch (k->keycode) {
        case KEY_LEFT_ARROW: case KEY_RIGHT_ARROW:
        case KEY_UP_ARROW:   case KEY_DOWN_ARROW:
        case 'w': case 'W':  case 'q': case 'Q':
            return true;
    }
    return false;
}

bool input_key_to_gesture(const key_event_t* k, gesture_event_t* out)
{
    memset(out, 0, sizeof(*out));
    bool alt = (k->mods & MOD_ALT) != 0;
    if (!alt || !k->down) return false;

    switch (k->keycode) {
    case KEY_LEFT_ARROW:  out->type = GESTURE_SWIPE_RIGHT;    return true;
    case KEY_RIGHT_ARROW: out->type = GESTURE_SWIPE_LEFT;     return true;
    case KEY_UP_ARROW:    out->type = GESTURE_OVERVIEW;       return true;
    case KEY_DOWN_ARROW:  out->type = GESTURE_FOCUS;          return true;
    case 'w': case 'W':   out->type = GESTURE_LAUNCHER;       return true;
    case 'q': case 'Q':   out->type = GESTURE_CLOSE_SURFACE;  return true;
    }
    return false;
}

/* =========================================================
 * Poll — drain hardware drivers into queue
 * ========================================================= */
void input_poll(void)
{
    /* --- Keyboard --- */
    int key_event;
    /* The existing keyboard driver queues scan-code translated events.
     * We call keyboard_getchar() / keyboard_getevent() if available. */
    extern bool keyboard_poll(int* keycode, uint8_t* mods, char* ch, bool* down);

    int  keycode; uint8_t mods; char ch; bool down;
    while (keyboard_poll(&keycode, &mods, &ch, &down)) {
        input_event_t ev;
        ev.type          = INPUT_KEY;
        ev.key.keycode   = keycode;
        ev.key.mods      = mods;
        ev.key.ch        = ch;
        ev.key.down      = down;

        /* Update modifier state */
        g_mods = mods;

        /* Check for navigation gesture first */
        gesture_event_t gest;
        if (down && input_key_to_gesture(&ev.key, &gest)) {
            input_event_t gev;
            gev.type    = INPUT_GESTURE;
            gev.gesture = gest;
            queue_push(&gev);
        } else {
            queue_push(&ev);
        }
    }

    /* --- Mouse --- */
    {
        int prev_buttons = g_ptr_buttons;
        int mx = mouse.x, my = mouse.y;
        int dx = mx - g_ptr.x, dy = my - g_ptr.y;
        g_ptr_buttons = mouse.buttons;
        g_ptr.x = mx;
        g_ptr.y = my;

        if (dx || dy || g_ptr_buttons != prev_buttons) {
            input_event_t ev;
            ev.type                 = INPUT_POINTER;
            ev.pointer.x            = mx;
            ev.pointer.y            = my;
            ev.pointer.dx           = dx;
            ev.pointer.dy           = dy;
            ev.pointer.buttons      = g_ptr_buttons;
            ev.pointer.prev_buttons = (uint8_t)prev_buttons;
            ev.pointer.scroll       = 0;
            queue_push(&ev);
        }
    }
    (void)key_event;
}
