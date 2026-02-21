/*
 * include/input/input_svc.h — Aether OS Unified Input Service
 *
 * The input service is the single point of truth for all user input.
 * Hardware drivers (keyboard, mouse) post raw events to the input
 * service's port. The service translates and dispatches structured
 * input events to the focused surface via IPC.
 *
 * Registered as "aether.input" in the service bus.
 *
 * Event flow:
 *   keyboard IRQ → gui_post_key() → input_svc port
 *   mouse IRQ    → gui_post_mouse() → input_svc port
 *   input_svc    → dispatch to focused surface's notify_port
 */
#ifndef INPUT_INPUT_SVC_H
#define INPUT_INPUT_SVC_H

#include <types.h>
#include <kernel/ipc.h>
#include <display/compositor.h>

/* =========================================================
 * Input event payload structures
 * (embedded in ipc_msg_t.data[])
 * ========================================================= */

/* Keyboard event (MSG_INPUT_KEY_DOWN / MSG_INPUT_KEY_UP) */
typedef struct {
    int     keycode;
    uint8_t modifiers;
    char    ch;
    bool    down;
} input_key_evt_t;

/* Mouse movement event (MSG_INPUT_MOUSE_MOV) */
typedef struct {
    int     x, y;           /* Absolute screen position */
    int     dx, dy;         /* Delta since last event */
} input_mouse_mov_t;

/* Mouse button event (MSG_INPUT_MOUSE_BTN) */
typedef struct {
    int     x, y;
    uint8_t buttons;         /* Current button state bitmask */
    uint8_t changed;         /* Which buttons changed */
    bool    pressed;         /* True = press, false = release */
} input_mouse_btn_t;

/* Focus change event (MSG_INPUT_FOCUS_IN / MSG_INPUT_FOCUS_OUT) */
typedef struct {
    surf_id_t surf_id;
} input_focus_evt_t;

/* Registration request (MSG_INPUT_SUBSCRIBE) */
typedef struct {
    surf_id_t  surf_id;
    port_id_t  notify_port;  /* Where to deliver input events */
    uint32_t   event_mask;   /* Which event types to receive */
} input_subscribe_t;

/* Event type mask bits */
#define INPUT_MASK_KEYBOARD  0x01
#define INPUT_MASK_MOUSE     0x02
#define INPUT_MASK_FOCUS     0x04
#define INPUT_MASK_ALL       0x07

/* =========================================================
 * Input service state
 * ========================================================= */

#define INPUT_MAX_SUBSCRIBERS  32

typedef struct {
    surf_id_t  surf_id;
    port_id_t  notify_port;
    uint32_t   event_mask;
    bool       active;
} input_subscriber_t;

typedef struct {
    port_id_t  service_port;   /* "aether.input" IPC port */
    surf_id_t  focused_surf;   /* Currently focused surface */

    input_subscriber_t subs[INPUT_MAX_SUBSCRIBERS];
    uint32_t   sub_count;

    /* Raw event staging (from IRQ handlers, pre-dispatch) */
    ipc_msg_t  raw_queue[64];
    uint32_t   raw_head;
    uint32_t   raw_tail;
    uint32_t   raw_count;

    bool       initialized;
} input_svc_t;

extern input_svc_t g_input_svc;

/* =========================================================
 * API
 * ========================================================= */

void  input_svc_init(void);
void  input_svc_run(void);          /* Main loop (input service thread) */

/* Called by compositor when focus changes */
void  input_svc_set_focus(surf_id_t surf_id);

/* Subscribe a surface's notify port to input events */
void  input_svc_subscribe(surf_id_t surf_id, port_id_t port, uint32_t mask);
void  input_svc_unsubscribe(surf_id_t surf_id);

/* Handle one incoming IPC message */
void  input_svc_handle_msg(const ipc_msg_t* msg);

/* Post raw events from IRQ handlers (non-blocking, IRQ-safe) */
void  input_post_key(int keycode, uint8_t mods, char ch, bool down);
void  input_post_mouse(int x, int y, int dx, int dy,
                       uint8_t buttons, uint8_t prev_buttons);

#endif /* INPUT_INPUT_SVC_H */
