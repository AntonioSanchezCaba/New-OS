/*
 * include/aether/input.h — Aether Input Model
 *
 * Input in Aether is NOT routed per-window.  All hardware events enter
 * the global input queue, are normalised by the ARE, then either:
 *
 *   a) Intercepted by the ARE as navigation gestures  (e.g. Alt+Arrow)
 *   b) Forwarded to the active surface's on_input callback
 *
 * This decouples input from the surface hierarchy entirely.
 * The model is future-ready: touch gestures, stylus, and spatial
 * controller events can be added by extending gesture_event_t.
 */
#pragma once
#include <types.h>
#include <aether/vec.h>

/* =========================================================
 * Event types
 * ========================================================= */
typedef enum {
    INPUT_NONE    = 0,
    INPUT_POINTER = 1,   /* Mouse move / click / scroll */
    INPUT_KEY     = 2,   /* Keyboard */
    INPUT_GESTURE = 3,   /* Synthesised high-level gesture */
} input_type_t;

/* Mouse button bitmask */
#define IBTN_LEFT    (1<<0)
#define IBTN_RIGHT   (1<<1)
#define IBTN_MIDDLE  (1<<2)

typedef struct {
    int32_t x, y;        /* Absolute screen coords */
    int32_t dx, dy;      /* Delta from last event */
    uint8_t buttons;     /* IBTN_* bitmask (current state) */
    uint8_t prev_buttons;
    int8_t  scroll;      /* +1 up / -1 down */
} pointer_event_t;

typedef struct {
    int     keycode;     /* KEY_* constants (from gui/event.h) */
    char    ch;          /* Printable char or 0 */
    uint8_t mods;        /* MOD_* bitmask */
    bool    down;        /* true = key down, false = key up */
} key_event_t;

/* Recognised gesture types (synthesised from raw input) */
typedef enum {
    GESTURE_NONE       = 0,
    GESTURE_SWIPE_LEFT,       /* Navigate to next surface */
    GESTURE_SWIPE_RIGHT,      /* Navigate to previous surface */
    GESTURE_OVERVIEW,         /* Show all surfaces (zoom out) */
    GESTURE_FOCUS,            /* Return to active surface (zoom in) */
    GESTURE_LAUNCHER,         /* Open/close launcher overlay */
    GESTURE_CLOSE_SURFACE,    /* Close active surface */
} gesture_type_t;

typedef struct {
    gesture_type_t type;
    int32_t        param;     /* Gesture-specific parameter */
} gesture_event_t;

/* =========================================================
 * Unified event
 * ========================================================= */
typedef struct input_event {
    input_type_t type;
    union {
        pointer_event_t pointer;
        key_event_t     key;
        gesture_event_t gesture;
    };
} input_event_t;

/* =========================================================
 * API
 * ========================================================= */
void input_init(void);

/* Called each frame: drain PS/2 keyboard + mouse drivers into queue */
void input_poll(void);

/* Pop next event. Returns false if queue empty. */
bool input_pop(input_event_t* out);

/* Query current pointer position */
vec2_t input_pointer_pos(void);

/* Check if a navigation gesture is being synthesised.
 * ARE calls this before forwarding events to surfaces. */
bool input_is_nav_key(const key_event_t* k);

/* Build a gesture event from a key event */
bool input_key_to_gesture(const key_event_t* k, gesture_event_t* out);
