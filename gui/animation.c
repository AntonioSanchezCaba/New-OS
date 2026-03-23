/*
 * gui/animation.c — Window animation engine (integer fixed-point)
 *
 * Easing functions (all 256-scaled, 64-entry lookup tables):
 *   ease_out_cubic  — smooth deceleration, used for open/close/restore
 *   ease_out_bounce — bouncy overshoot, used for shade/unshade
 *   ease_in_cubic   — smooth acceleration, used for minimize
 */
#include <gui/animation.h>
#include <gui/window.h>
#include <memory.h>
#include <string.h>

/* =========================================================
 * Ease-out cubic: f(t) = 1 - (1-t)^3, scaled to [0..256]
 * ========================================================= */
static const uint8_t ease_cubic[64] = {
      0,  4,  8, 12, 16, 20, 24, 28,
     32, 36, 40, 44, 47, 51, 55, 58,
     62, 65, 68, 72, 75, 78, 81, 84,
     87, 90, 92, 95, 98,100,103,105,
    107,110,112,114,116,118,120,122,
    124,126,128,130,131,133,135,136,
    138,139,141,142,143,145,146,147,
    148,150,151,152,153,154,155,156,
};

/* =========================================================
 * Ease-out bounce: overshoot landing effect, scaled to [0..256]
 * Approximates the classic bounce easing curve with 3 bounces.
 * ========================================================= */
static const uint8_t ease_bounce[64] = {
      0,  3,  7, 12, 18, 25, 32, 41,
     50, 60, 71, 83, 96,109,121,132,
    143,152,160,168,176,184,191,197,
    203,209,215,220,224,228,232,236,
    238,240,241,242,243,244,246,248,
    250,252,253,254,254,253,252,251,
    252,253,254,255,255,254,253,254,
    255,255,255,255,255,255,255,255,
};

/* =========================================================
 * Ease-in cubic: f(t) = t^3, scaled to [0..256]
 * ========================================================= */
static const uint8_t ease_in_cubic[64] = {
      0,  0,  0,  0,  0,  0,  0,  1,
      1,  2,  3,  4,  5,  7,  9, 12,
     14, 17, 21, 25, 29, 34, 39, 45,
     51, 58, 65, 73, 82, 91,101,112,
    123,135,147,160,173,186,200,213,
    226,234,238,242,244,246,248,250,
    251,252,253,254,254,255,255,255,
    255,255,255,255,255,255,255,255,
};

/* Apply an easing table: returns 256-scaled progress */
static int apply_table(const uint8_t* tbl, int frame, int duration)
{
    if (duration <= 0) return 256;
    if (frame >= duration) return 256;
    int idx = frame * 63 / duration;
    if (idx > 63) idx = 63;
    return (int)tbl[idx];
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
    /* Reuse existing slot for this window (prevents stacking) */
    for (int i = 0; i < ANIM_MAX; i++)
        if (g_anims[i].active && g_anims[i].wid == wid)
            return &g_anims[i];
    /* Grab first free slot */
    for (int i = 0; i < ANIM_MAX; i++)
        if (!g_anims[i].active)
            return &g_anims[i];
    return NULL;
}

void anim_start(wid_t wid, anim_type_t type)
{
    anim_t* a = find_slot(wid);
    if (!a) return;

    window_t* w = wm_get_window(wid);
    if (!w) return;

    memset(a, 0, sizeof(*a));
    a->active = true;
    a->wid    = wid;
    a->type   = type;
    a->frame  = 0;

    /* Default: geometry stays the same */
    a->src_x = w->x; a->src_y = w->y;
    a->src_w = w->w; a->src_h = w->h;
    a->dst_x = w->x; a->dst_y = w->y;
    a->dst_w = w->w; a->dst_h = w->h;

    switch (type) {
    case ANIM_OPEN:
        a->duration = ANIM_DURATION_OPEN;
        /* Scale up from half-size at centre */
        a->src_x = w->x + w->w / 4;
        a->src_y = w->y + w->h / 4;
        a->src_w = w->w / 2;
        a->src_h = w->h / 2;
        break;

    case ANIM_CLOSE:
        a->duration = ANIM_DURATION_CLOSE;
        /* Scale down to half-size at centre */
        a->dst_x = w->x + w->w / 4;
        a->dst_y = w->y + w->h / 4;
        a->dst_w = w->w / 2;
        a->dst_h = w->h / 2;
        break;

    case ANIM_RESTORE:
        a->duration = ANIM_DURATION_RESTORE;
        /* Expand from zero at centre */
        a->src_x = w->x + w->w / 2;
        a->src_y = w->y + w->h / 2;
        a->src_w = 0;
        a->src_h = 0;
        break;

    case ANIM_SHADE:
        /* Roll-up: client height collapses to 0, width/position unchanged */
        a->duration = ANIM_DURATION_SHADE;
        a->src_h    = w->h;   /* full client height */
        a->dst_h    = 0;      /* collapsed */
        /* x, y, w stay fixed */
        a->src_x = a->dst_x = w->x;
        a->src_y = a->dst_y = w->y;
        a->src_w = a->dst_w = w->w;
        break;

    case ANIM_UNSHADE:
        /* Unroll: client height expands from 0 back to saved height */
        a->duration = ANIM_DURATION_UNSHADE;
        a->src_h    = 0;       /* currently collapsed */
        a->dst_h    = w->saved_h > 0 ? w->saved_h : w->h;
        a->src_x = a->dst_x = w->x;
        a->src_y = a->dst_y = w->y;
        a->src_w = a->dst_w = w->w;
        /* Clear shaded flag immediately so compositor renders content */
        w->flags &= ~WF_SHADED;
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
    a->src_w = w->w; a->src_h = w->h;
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
            window_t* w = wm_get_window(a->wid);
            switch (a->type) {
            case ANIM_MINIMIZE:
                if (w) {
                    w->flags |=  WF_MINIMIZED;
                    w->flags &= ~WF_VISIBLE;
                }
                break;
            case ANIM_CLOSE:
                /* Compositor/caller handles window destruction */
                break;
            case ANIM_SHADE:
                /* Mark window as shaded: only title bar visible */
                if (w) {
                    w->saved_h  = w->h;     /* preserve full height */
                    w->flags   |= WF_SHADED;
                }
                break;
            case ANIM_UNSHADE:
                /* Restore full height from saved geometry */
                if (w && w->saved_h > 0) {
                    w->h = w->saved_h;
                    w->flags &= ~WF_SHADED;
                }
                break;
            default:
                break;
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
    /* Default: use window's current geometry */
    window_t* w = wm_get_window(wid);
    if (w) {
        *out_x = w->x; *out_y = w->y;
        *out_w = w->w; *out_h = w->h;
    }
    *out_alpha = 255;

    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t* a = &g_anims[i];
        if (!a->active || a->wid != wid) continue;

        switch (a->type) {
        /* --- Cubic easing: scale+fade --- */
        case ANIM_OPEN: {
            int t = apply_table(ease_cubic, a->frame, a->duration);
            *out_x = lerp(a->src_x, a->dst_x, t);
            *out_y = lerp(a->src_y, a->dst_y, t);
            *out_w = lerp(a->src_w, a->dst_w, t);
            *out_h = lerp(a->src_h, a->dst_h, t);
            *out_alpha = (uint8_t)t;
            break;
        }
        case ANIM_CLOSE: {
            int t = apply_table(ease_cubic, a->frame, a->duration);
            *out_x = lerp(a->src_x, a->dst_x, t);
            *out_y = lerp(a->src_y, a->dst_y, t);
            *out_w = lerp(a->src_w, a->dst_w, t);
            *out_h = lerp(a->src_h, a->dst_h, t);
            *out_alpha = (uint8_t)(256 - t);
            break;
        }
        case ANIM_RESTORE: {
            int t = apply_table(ease_cubic, a->frame, a->duration);
            *out_x = lerp(a->src_x, a->dst_x, t);
            *out_y = lerp(a->src_y, a->dst_y, t);
            *out_w = lerp(a->src_w, a->dst_w, t);
            *out_h = lerp(a->src_h, a->dst_h, t);
            *out_alpha = (uint8_t)t;
            break;
        }

        /* --- Ease-in cubic: shrink toward taskbar --- */
        case ANIM_MINIMIZE: {
            int t = apply_table(ease_in_cubic, a->frame, a->duration);
            *out_x = lerp(a->src_x, a->dst_x, t);
            *out_y = lerp(a->src_y, a->dst_y, t);
            *out_w = lerp(a->src_w, a->dst_w, t);
            *out_h = lerp(a->src_h, a->dst_h, t);
            *out_alpha = (uint8_t)(256 - t);
            break;
        }

        /* --- Bounce easing: height-only shade/unshade --- */
        case ANIM_SHADE: {
            int t = apply_table(ease_in_cubic, a->frame, a->duration);
            /* x, y, w are fixed; only h animates */
            *out_x = a->src_x;
            *out_y = a->src_y;
            *out_w = a->src_w;
            *out_h = lerp(a->src_h, a->dst_h, t);
            *out_alpha = 255;
            break;
        }
        case ANIM_UNSHADE: {
            int t = apply_table(ease_bounce, a->frame, a->duration);
            *out_x = a->src_x;
            *out_y = a->src_y;
            *out_w = a->src_w;
            *out_h = lerp(a->src_h, a->dst_h, t);
            if (*out_h < 1) *out_h = 1;
            *out_alpha = 255;
            break;
        }

        default:
            break;
        }

        if (*out_w < 1) *out_w = 1;
        if (*out_h < 1) *out_h = 1;
        return;
    }
}
