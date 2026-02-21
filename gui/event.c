/*
 * gui/event.c - GUI event queue
 */
#include <gui/event.h>
#include <kernel.h>
#include <string.h>

#define EVT_QUEUE_MASK  255u   /* 256 - 1 */

static gui_event_t evt_queue[256];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;

void gui_event_queue_init(void)
{
    evt_head = 0;
    evt_tail = 0;
    memset(evt_queue, 0, sizeof(evt_queue));
}

void gui_event_push(const gui_event_t* evt)
{
    if (evt_head - evt_tail >= 256) return;
    evt_queue[evt_head & EVT_QUEUE_MASK] = *evt;
    __sync_synchronize();
    evt_head++;
}

bool gui_event_pop(gui_event_t* out)
{
    if (evt_head == evt_tail) return false;
    *out = evt_queue[evt_tail & EVT_QUEUE_MASK];
    __sync_synchronize();
    evt_tail++;
    return true;
}

int gui_event_count(void)
{
    return (int)(evt_head - evt_tail);
}

void gui_post_key(int keycode, uint8_t mods, char ch, bool down)
{
    gui_event_t evt;
    evt.type          = down ? GUI_EVENT_KEY_DOWN : GUI_EVENT_KEY_UP;
    evt.key.keycode   = keycode;
    evt.key.modifiers = mods;
    evt.key.ch        = ch;
    gui_event_push(&evt);
}

void gui_post_mouse(int x, int y, int dx, int dy, uint8_t buttons)
{
    gui_event_t evt;
    evt.type          = GUI_EVENT_MOUSE_MOVE;
    evt.mouse.x       = x;
    evt.mouse.y       = y;
    evt.mouse.dx      = dx;
    evt.mouse.dy      = dy;
    evt.mouse.buttons = buttons;
    evt.mouse.scroll  = 0;
    gui_event_push(&evt);
}
