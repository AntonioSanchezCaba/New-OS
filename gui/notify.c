/*
 * gui/notify.c - System notification popup system
 *
 * Toast notifications appear in the top-right corner, stack vertically,
 * and auto-dismiss after their TTL expires.
 */
#include <gui/notify.h>
#include <gui/theme.h>
#include <drivers/timer.h>
#include <string.h>
#include <memory.h>

#define NOTIF_W         260
#define NOTIF_H         56
#define NOTIF_PAD       6
#define NOTIF_MARGIN    8
#define NOTIF_ICON_W    4   /* Colored left stripe width */
#define NOTIF_TITLE_LEN 48
#define NOTIF_BODY_LEN  80

typedef struct {
    bool         active;
    notify_type_t type;
    char         title[NOTIF_TITLE_LEN];
    char         body[NOTIF_BODY_LEN];
    uint32_t     spawn_tick;
    uint32_t     duration;
    /* Fade-out: last 30 ticks */
    int          slot;
} notif_entry_t;

static notif_entry_t g_notifs[NOTIFY_MAX];
static int           g_notif_count = 0;

void notify_init(void)
{
    memset(g_notifs, 0, sizeof(g_notifs));
    g_notif_count = 0;
}

void notify_post(notify_type_t type, const char* title, const char* body)
{
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_notifs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Evict oldest */
        slot = 0;
        for (int i = 1; i < NOTIFY_MAX; i++) {
            if (g_notifs[i].spawn_tick < g_notifs[slot].spawn_tick)
                slot = i;
        }
    }

    notif_entry_t* n = &g_notifs[slot];
    n->active     = true;
    n->type       = type;
    n->spawn_tick = timer_get_ticks();
    n->duration   = NOTIFY_DURATION_NORMAL;
    strncpy(n->title, title ? title : "", NOTIF_TITLE_LEN - 1);
    n->title[NOTIF_TITLE_LEN - 1] = '\0';
    strncpy(n->body, body ? body : "", NOTIF_BODY_LEN - 1);
    n->body[NOTIF_BODY_LEN - 1] = '\0';

    g_notif_count++;
}

void notify_clear(void)
{
    memset(g_notifs, 0, sizeof(g_notifs));
    g_notif_count = 0;
}

void notify_tick(canvas_t* screen)
{
    uint32_t now = timer_get_ticks();
    int active_count = 0;

    /* Expire old notifications */
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_notifs[i].active) continue;
        uint32_t age = now - g_notifs[i].spawn_tick;
        if (age >= g_notifs[i].duration) {
            g_notifs[i].active = false;
            g_notif_count = (g_notif_count > 0) ? g_notif_count - 1 : 0;
        } else {
            active_count++;
        }
    }

    if (active_count == 0) return;

    const theme_t* th = theme_current();
    int margin = NOTIF_MARGIN;
    int slot_y = margin;

    for (int i = 0; i < NOTIFY_MAX; i++) {
        notif_entry_t* n = &g_notifs[i];
        if (!n->active) continue;

        uint32_t age  = now - n->spawn_tick;
        uint32_t left = n->duration - age;

        /* Fade effect: compute alpha-like offset (brighten or shift) */
        bool fading = (left < 30);

        int nx = screen->width - NOTIF_W - margin;
        int ny = slot_y;

        /* Background */
        uint32_t bg = fading ? rgba(0x20, 0x28, 0x40, 0xC0) : th->notif_bg;
        draw_rect(screen, nx, ny, NOTIF_W, NOTIF_H, bg);
        draw_rect_outline(screen, nx, ny, NOTIF_W, NOTIF_H, 1, th->notif_border);

        /* Colored left stripe */
        uint32_t stripe_col = (n->type == NOTIFY_ERROR) ? th->notif_error :
                              (n->type == NOTIFY_WARN)  ? th->notif_warn  :
                                                          th->notif_info;
        draw_rect(screen, nx, ny, NOTIF_ICON_W, NOTIF_H, stripe_col);

        /* Title */
        int tx = nx + NOTIF_ICON_W + NOTIF_PAD;
        draw_string(screen, tx, ny + 6, n->title, th->notif_text, rgba(0,0,0,0));

        /* Body */
        uint32_t body_col = th->text_secondary;
        if (th == &(*(theme_current()))) body_col = th->notif_text;
        /* Slightly lighter body text */
        draw_string(screen, tx, ny + 6 + FONT_H + 4, n->body, th->text_secondary, rgba(0,0,0,0));

        /* Progress bar at bottom */
        int bar_w = (int)((uint64_t)(NOTIF_W - 2) * left / n->duration);
        draw_rect(screen, nx + 1, ny + NOTIF_H - 3, NOTIF_W - 2, 2, th->notif_border);
        if (bar_w > 0)
            draw_rect(screen, nx + 1, ny + NOTIF_H - 3, bar_w, 2, stripe_col);

        slot_y += NOTIF_H + 4;
    }
}
