/*
 * input/input_svc.c — Aether OS Unified Input Service
 *
 * Receives raw input events from hardware drivers and dispatches
 * structured IPC events to the focused surface.
 *
 * Event flow:
 *   1. IRQ handler calls input_post_key() / input_post_mouse()
 *      → appends to raw_queue (lock-free, single-producer)
 *   2. input_svc_run() dispatches from raw_queue to subscribers
 *   3. Focused surface's notify_port receives MSG_INPUT_* messages
 *
 * Registered as "aether.input" in the service bus.
 */
#include <input/input_svc.h>
#include <kernel/ipc.h>
#include <kernel/svcbus.h>
#include <display/compositor.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* =========================================================
 * Global state
 * ========================================================= */

input_svc_t g_input_svc;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static input_subscriber_t* _find_sub(surf_id_t surf_id)
{
    for (int i = 0; i < INPUT_MAX_SUBSCRIBERS; i++) {
        if (g_input_svc.subs[i].active &&
            g_input_svc.subs[i].surf_id == surf_id)
            return &g_input_svc.subs[i];
    }
    return NULL;
}

static void _dispatch(uint32_t type, void* payload, size_t payload_len)
{
    /* Always dispatch to focused surface first */
    input_subscriber_t* focused = _find_sub(g_input_svc.focused_surf);
    if (!focused) return;

    ipc_msg_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type       = type;
    ev.sender_tid = 0; /* from input service */

    if (payload && payload_len <= IPC_MSG_DATA_MAX) {
        memcpy(ev.data, payload, payload_len);
        ev.data_len = (uint32_t)payload_len;
    }

    ipc_send(focused->notify_port, &ev);
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void input_svc_init(void)
{
    memset(&g_input_svc, 0, sizeof(g_input_svc));
    g_input_svc.focused_surf = SURF_INVALID;

    g_input_svc.service_port = ipc_port_create(0 /* kernel */);
    if (g_input_svc.service_port == PORT_INVALID) {
        klog_warn("INPUT: failed to create service port");
        return;
    }

    svcbus_register(SVC_INPUT, g_input_svc.service_port, 0, 1);
    g_input_svc.initialized = true;

    kinfo("INPUT: service ready on port %u", g_input_svc.service_port);
}

/* =========================================================
 * Focus management
 * ========================================================= */

void input_svc_set_focus(surf_id_t surf_id)
{
    if (g_input_svc.focused_surf == surf_id) return;

    /* Send FOCUS_OUT to old surface */
    if (g_input_svc.focused_surf != SURF_INVALID) {
        input_focus_evt_t ev = { .surf_id = g_input_svc.focused_surf };
        _dispatch(MSG_INPUT_FOCUS_OUT, &ev, sizeof(ev));
    }

    g_input_svc.focused_surf = surf_id;

    /* Send FOCUS_IN to new surface */
    if (surf_id != SURF_INVALID) {
        input_focus_evt_t ev = { .surf_id = surf_id };
        _dispatch(MSG_INPUT_FOCUS_IN, &ev, sizeof(ev));
    }
}

/* =========================================================
 * Subscription management
 * ========================================================= */

void input_svc_subscribe(surf_id_t surf_id, port_id_t port, uint32_t mask)
{
    /* Update if already subscribed */
    input_subscriber_t* existing = _find_sub(surf_id);
    if (existing) {
        existing->notify_port = port;
        existing->event_mask  = mask;
        return;
    }

    for (int i = 0; i < INPUT_MAX_SUBSCRIBERS; i++) {
        if (!g_input_svc.subs[i].active) {
            g_input_svc.subs[i].surf_id     = surf_id;
            g_input_svc.subs[i].notify_port = port;
            g_input_svc.subs[i].event_mask  = mask;
            g_input_svc.subs[i].active      = true;
            g_input_svc.sub_count++;
            return;
        }
    }
    klog_warn("INPUT: subscriber table full");
}

void input_svc_unsubscribe(surf_id_t surf_id)
{
    input_subscriber_t* sub = _find_sub(surf_id);
    if (!sub) return;
    sub->active = false;
    g_input_svc.sub_count--;
}

/* =========================================================
 * Raw event posting (called from IRQ context — must be fast)
 * ========================================================= */

void input_post_key(int keycode, uint8_t mods, char ch, bool down)
{
    if (!g_input_svc.initialized) return;
    if (g_input_svc.raw_count >= 64) return; /* Drop on overflow */

    ipc_msg_t* slot = &g_input_svc.raw_queue[g_input_svc.raw_tail % 64];
    memset(slot, 0, sizeof(*slot));
    slot->type = down ? MSG_INPUT_KEY_DOWN : MSG_INPUT_KEY_UP;
    input_key_evt_t* kev = (input_key_evt_t*)slot->data;
    kev->keycode   = keycode;
    kev->modifiers = mods;
    kev->ch        = ch;
    kev->down      = down;
    slot->data_len = sizeof(input_key_evt_t);

    g_input_svc.raw_tail = (g_input_svc.raw_tail + 1) % 64;
    g_input_svc.raw_count++;
}

void input_post_mouse(int x, int y, int dx, int dy,
                      uint8_t buttons, uint8_t prev_buttons)
{
    if (!g_input_svc.initialized) return;
    if (g_input_svc.raw_count >= 64) return;

    uint8_t changed = buttons ^ prev_buttons;
    uint32_t type   = changed ? MSG_INPUT_MOUSE_BTN : MSG_INPUT_MOUSE_MOV;

    ipc_msg_t* slot = &g_input_svc.raw_queue[g_input_svc.raw_tail % 64];
    memset(slot, 0, sizeof(*slot));
    slot->type = type;

    if (changed) {
        input_mouse_btn_t* bev = (input_mouse_btn_t*)slot->data;
        bev->x       = x;
        bev->y       = y;
        bev->buttons = buttons;
        bev->changed = changed;
        bev->pressed = (buttons & changed) != 0;
        slot->data_len = sizeof(input_mouse_btn_t);
    } else {
        input_mouse_mov_t* mev = (input_mouse_mov_t*)slot->data;
        mev->x  = x;  mev->y  = y;
        mev->dx = dx; mev->dy = dy;
        slot->data_len = sizeof(input_mouse_mov_t);
    }

    g_input_svc.raw_tail = (g_input_svc.raw_tail + 1) % 64;
    g_input_svc.raw_count++;
}

/* =========================================================
 * IPC message handler (subscriptions, config messages)
 * ========================================================= */

void input_svc_handle_msg(const ipc_msg_t* msg)
{
    switch (msg->type) {
    case MSG_INPUT_SUBSCRIBE: {
        if (msg->data_len < sizeof(input_subscribe_t)) break;
        input_subscribe_t* req = (input_subscribe_t*)msg->data;
        input_svc_subscribe(req->surf_id, req->notify_port, req->event_mask);
        break;
    }
    default:
        break;
    }
}

/* =========================================================
 * input_svc_run — service main loop (kernel thread)
 * ========================================================= */

void input_svc_run(void)
{
    if (!g_input_svc.initialized) {
        klog_warn("INPUT: not initialized — thread exiting");
        return;
    }

    kinfo("INPUT: service thread running");
    ipc_msg_t msg;

    for (;;) {
        /* Drain raw event queue → dispatch to focused surface */
        while (g_input_svc.raw_count > 0) {
            ipc_msg_t* raw = &g_input_svc.raw_queue[g_input_svc.raw_head % 64];

            /* Sync focus with compositor */
            surf_id_t comp_focus = g_compositor.focused_id;
            if (g_input_svc.focused_surf != comp_focus)
                input_svc_set_focus(comp_focus);

            /* Dispatch key events only to keyboard-subscribed surfaces */
            if (raw->type == MSG_INPUT_KEY_DOWN ||
                raw->type == MSG_INPUT_KEY_UP) {
                input_subscriber_t* sub = _find_sub(g_input_svc.focused_surf);
                if (sub && (sub->event_mask & INPUT_MASK_KEYBOARD))
                    ipc_send(sub->notify_port, raw);
            }

            /* Mouse events — dispatch to focused surface */
            if (raw->type == MSG_INPUT_MOUSE_BTN ||
                raw->type == MSG_INPUT_MOUSE_MOV) {
                input_subscriber_t* sub = _find_sub(g_input_svc.focused_surf);
                if (sub && (sub->event_mask & INPUT_MASK_MOUSE))
                    ipc_send(sub->notify_port, raw);
            }

            g_input_svc.raw_head = (g_input_svc.raw_head + 1) % 64;
            g_input_svc.raw_count--;
        }

        /* Service IPC messages (subscriptions etc.) */
        while (ipc_receive(g_input_svc.service_port, &msg,
                           IPC_TIMEOUT_NONE) == IPC_OK) {
            input_svc_handle_msg(&msg);
        }

        scheduler_yield();
    }
}
