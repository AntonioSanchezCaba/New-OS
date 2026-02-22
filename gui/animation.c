/*
 * gui/animation.c — Window animation engine (integer fixed-point)
 *
 * Ease-out cubic: f(t) = 1 - (1-t)^3
 * Represented as a 64-entry lookup table scaled to [0..256].
 */
#include <gui/animation.h>
#include <gui/window.h>
#include <memory.h>
#include <string.h>

/* =========================================================
 * Ease-out cubic lookup table: easing[i] for i in [0..63]
 * easing[i] ≈ (1 - (1 - i/63)^3) * 256
 * Pre-computed (integer arithmetic):
 * ========================================================= */
static const uint8_t easing[64] = {
      0,  4,  8, 12, 16, 20, 24, 28,
     32, 36, 40, 44, 47, 51, 55, 58,
     62, 65, 68, 72, 75, 78, 81, 84,
     87, 90, 92, 95, 98,100,103,105,
    107,110,112,114,116,118,120,122,
    124,126,128,130,131,133,135,136,
    138,139,141,142,143,145,146,147,
    148,150,151,152,153,154,155,156,
};

/* Interpolate: returns 256-scaled progress through an animation */
static int ease(int frame, int duration)
{
    if (duration <= 0) return 256;
    if (frame >= duration) return 256;
    /* Map frame to [0..63] */
    int idx = frame * 63 / duration;
    if (idx > 63) idx = 63;
    return (int)easing[idx];
}

/* Lerp a→b using 256-scaled t */
static int lerp(int a, int b, int t)
{
    return a + (b - a) * t / 256;
}

/* =========================================================
 * Animation table
 * ========================================================= */
static anim_t g_anims[ANIM_MAX];

void anim_init(void)
{
    memset(g_anims, 0, sizeof(g_anims));
}

static anim_t* find_slot(wid_t wid)
{
    /* Try to reuse existing slot for this window */
    for (int i = 0; i < ANIM_MAX; i++)
        if (g_anims[i].active && g_anims[i].wid == wid)
            return &g_anims[i];
    /* Grab first free slot */
    for (int i = 0; i < ANIM_MAX; i++)
        if (!g_anims[i].active)
            return &g_anims[i];
    return NULL; /* table full */
}

void anim_start(wid_t wid, anim_type_t type)
{
    anim_t* a = find_slot(wid);
    if (!a) return;

    window_t* w = wm_get_window(wid);
    if (!w) return;

    memset(a, 0, sizeof(*a));
    a->active   = true;
    a->wid      = wid;
    a->type     = type;
    a->frame    = 0;

    a->src_x = w->x; a->src_y = w->y;
    a->src_w = w->width; a->src_h = w->height;
    a->dst_x = w->x; a->dst_y = w->y;
    a->dst_w = w->width; a->dst_h = w->height;

    switch (type) {
    case ANIM_OPEN:
        a->duration = ANIM_DURATION_OPEN;
        /* Start half-sized at centre */
        a->src_x = w->x + w->width / 4;
        a->src_y = w->y + w->height / 4;
        a->src_w = w->width  / 2;
        a->src_h = w->height / 2;
        break;
    case ANIM_CLOSE:
        a->duration = ANIM_DURATION_CLOSE;
        a->dst_x = w->x + w->width / 4;
        a->dst_y = w->y + w->height / 4;
        a->dst_w = w->width  / 2;
        a->dst_h = w->height / 2;
        break;
    case ANIM_RESTORE:
        a->duration = ANIM_DURATION_RESTORE;
        /* Start from zero size at centre */
        a->src_x = w->x + w->width / 2;
        a->src_y = w->y + w->height / 2;
        a->src_w = 0; a->src_h = 0;
        break;
    default:
        a->duration = ANIM_DURATION_OPEN;
        break;
    }
}

void anim_start_minimize(wid_t wid, int tx, int ty, int tw, int th)
{
    anim_t* a = find_slot(wid);
    if (!a) return;

    window_t* w = wm_get_window(wid);
    if (!w) return;

    memset(a, 0, sizeof(*a));
    a->active   = true;
    a->wid      = wid;
    a->type     = ANIM_MINIMIZE;
    a->frame    = 0;
    a->duration = ANIM_DURATION_MINIMIZE;

    a->src_x = w->x; a->src_y = w->y;
    a->src_w = w->width; a->src_h = w->height;
    a->dst_x = tx;   a->dst_y = ty;
    a->dst_w = tw;   a->dst_h = th;
    a->task_x = tx;  a->task_y = ty;
    a->task_w = tw;  a->task_h = th;
}

void anim_tick(void)
{
    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t* a = &g_anims[i];
        if (!a->active) continue;
        a->frame++;
        if (a->frame >= a->duration) {
            /* Animation complete */
            if (a->type == ANIM_CLOSE || a->type == ANIM_MINIMIZE) {
                window_t* w = wm_get_window(a->wid);
                if (w && a->type == ANIM_MINIMIZE) {
                    w->flags |=  WF_MINIMIZED;
                    w->flags &= ~WF_VISIBLE;
                }
            }
            a->active = false;
        }
    }
}

bool anim_any_active(void)
{
    for (int i = 0; i < ANIM_MAX; i++)
        if (g_anims[i].active) return true;
    return false;
}

void anim_cancel(wid_t wid)
{
    for (int i = 0; i < ANIM_MAX; i++)
        if (g_anims[i].active && g_anims[i].wid == wid)
            g_anims[i].active = false;
}

void anim_apply(wid_t wid, int* out_x, int* out_y,
                int* out_w, int* out_h, uint8_t* out_alpha)
{
    /* Default: no change */
    window_t* w = wm_get_window(wid);
    if (w) { *out_x = w->x; *out_y = w->y;
             *out_w = w->width; *out_h = w->height; }
    *out_alpha = 255;

    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t* a = &g_anims[i];
        if (!a->active || a->wid != wid) continue;

        int t = ease(a->frame, a->duration);

        *out_x = lerp(a->src_x, a->dst_x, t);
        *out_y = lerp(a->src_y, a->dst_y, t);
        *out_w = lerp(a->src_w, a->dst_w, t);
        *out_h = lerp(a->src_h, a->dst_h, t);
        if (*out_w < 1) *out_w = 1;
        if (*out_h < 1) *out_h = 1;

        switch (a->type) {
        case ANIM_OPEN:    *out_alpha = (uint8_t)t; break;
        case ANIM_CLOSE:   *out_alpha = (uint8_t)(255 - t); break;
        case ANIM_MINIMIZE:*out_alpha = (uint8_t)(255 - t); break;
        case ANIM_RESTORE: *out_alpha = (uint8_t)t; break;
        default:           *out_alpha = 255; break;
        }
        return;
    }
}
