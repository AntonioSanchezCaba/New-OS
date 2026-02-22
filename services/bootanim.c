/*
 * services/bootanim.c - Aureon OS Boot Animation
 *
 * A polished boot screen that runs between POST and the desktop.
 * Renders directly to the framebuffer before the compositor is up.
 *
 * Visual design:
 *   - Deep navy background (#0A0E1A)
 *   - Animated circular "loading ring" (sweep from 0° → 360°)
 *   - "Aureon" logotype rendered in white below the ring
 *   - Thin progress bar at the bottom edge
 *   - Smooth fade-in / fade-out transitions
 */
#include <services/bootanim.h>
#include <kernel/version.h>
#include <drivers/framebuffer.h>
#include <drivers/timer.h>
#include <gui/draw.h>
#include <kernel.h>
#include <string.h>

bootanim_state_t g_bootanim;

/* ── Colors ──────────────────────────────────────────────────────────── */
#define BA_BG        0xFF0A0E1Au
#define BA_RING_BG   0xFF1A2240u
#define BA_RING_FG   0xFF4A8EFFu
#define BA_RING_ACC  0xFF7BB8FFu
#define BA_TEXT      0xFFEEF2FFu
#define BA_SUBTEXT   0xFF8899CCu
#define BA_PROGRESS  0xFF4A8EFFu
#define BA_PROGRESS_BG 0xFF1A2240u

/* ── Integer-only sine approximation (for ring sweep) ────────────────── */
/* sin_table[i] = round(sin(i * PI/180) * 1024) for i in 0..359 */
static const int16_t sin_t[360] = {
       0,   17,   35,   52,   70,   87,  104,  121,  139,  156,
     173,  190,  207,  224,  241,  257,  273,  290,  306,  321,
     337,  352,  367,  382,  396,  411,  425,  438,  452,  465,
     477,  490,  502,  514,  525,  536,  547,  557,  567,  576,
     585,  594,  602,  609,  616,  623,  629,  634,  639,  644,
     648,  652,  655,  657,  659,  661,  662,  663,  663,  663,
     662,  661,  659,  656,  653,  650,  646,  642,  637,  631,
     625,  619,  612,  604,  596,  588,  579,  570,  560,  550,
     539,  528,  516,  504,  492,  479,  466,  452,  438,  424,
     409,  394,  379,  363,  347,  330,  313,  296,  279,  261,
     243,  225,  206,  187,  168,  149,  130,  110,   90,   70,
      50,   30,   10,  -10,  -30,  -50,  -70,  -90, -110, -130,
    -149, -168, -187, -206, -225, -243, -261, -279, -296, -313,
    -330, -347, -363, -379, -394, -409, -424, -438, -452, -466,
    -479, -492, -504, -516, -528, -539, -550, -560, -570, -579,
    -588, -596, -604, -612, -619, -625, -631, -637, -642, -646,
    -650, -653, -656, -659, -661, -662, -663, -663, -663, -662,
    -661, -659, -657, -655, -652, -648, -644, -639, -634, -629,
    -623, -616, -609, -602, -594, -585, -576, -567, -557, -547,
    -536, -525, -514, -502, -490, -477, -465, -452, -438, -425,
    -411, -396, -382, -367, -352, -337, -321, -306, -290, -273,
    -257, -241, -224, -207, -190, -173, -156, -139, -121, -104,
     -87,  -70,  -52,  -35,  -17,    0,   17,   35,   52,   70,
      87,  104,  121,  139,  156,  173,  190,  207,  224,  241,
     257,  273,  290,  306,  321,  337,  352,  367,  382,  396,
     411,  425,  438,  452,  465,  477,  490,  502,  514,  525,
     536,  547,  557,  567,  576,  585,  594,  602,  609,  616,
     623,  629,  634,  639,  644,  648,  652,  655,  657,  659,
     661,  662,  663,  663,  663,  662,  661,  659,  656,  653,
     650,  646,  642,  637,  631,  625,  619,  612,  604,  596,
     588,  579,  570,  560,  550,  539,  528,  516,  504,  492,
     479,  466,  452,  438,  424,  409,  394,  379,  363,  347,
     330,  313,  296,  279,  261,  243,  225,  206,  187,  168,
     149,  130,  110,   90,   70,   50,   30,   10,  -10,  -30,
     -50,  -70,  -90, -110, -130, -149, -168, -187, -206, -225,
     -243, -261, -279, -296, -313, -330, -347, -363
};

static inline int isin(int deg) { return sin_t[((deg % 360) + 360) % 360]; }
static inline int icos(int deg) { return sin_t[(((deg - 90) % 360) + 360) % 360]; }

/* ── Alpha blend pixel ───────────────────────────────────────────────── */
static inline uint32_t alpha_blend(uint32_t dst, uint32_t src, uint8_t a)
{
    uint32_t r = ((src >> 16) & 0xFF) * a / 255 + ((dst >> 16) & 0xFF) * (255 - a) / 255;
    uint32_t g = ((src >>  8) & 0xFF) * a / 255 + ((dst >>  8) & 0xFF) * (255 - a) / 255;
    uint32_t b = ( src        & 0xFF) * a / 255 + ( dst        & 0xFF) * (255 - a) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ── Draw anti-aliased ring arc ──────────────────────────────────────── */
static void draw_ring_arc(int cx, int cy, int r, int thickness,
                           int start_deg, int end_deg, uint32_t color)
{
    if (!fb_ready()) return;

    /* Clamp sweep */
    if (end_deg > start_deg + 360) end_deg = start_deg + 360;

    for (int deg = start_deg; deg <= end_deg; deg++) {
        /* Outer rim */
        int ox1 = cx + icos(deg)     * r     / 1024;
        int oy1 = cy + isin(deg)     * r     / 1024;
        int ox2 = cx + icos(deg)     * (r - thickness) / 1024;
        int oy2 = cy + isin(deg)     * (r - thickness) / 1024;

        /* Draw line from inner to outer radius */
        int dx = ox2 - ox1;
        int dy = oy2 - oy1;
        int steps = ABS(dx) > ABS(dy) ? ABS(dx) : ABS(dy);
        if (steps == 0) steps = 1;

        for (int s = 0; s <= steps; s++) {
            int px = ox1 + dx * s / steps;
            int py = oy1 + dy * s / steps;
            /* Fade tail of arc */
            uint8_t alpha = 255;
            if (end_deg - start_deg > 10) {
                int tail_len = 45;
                if (deg < start_deg + tail_len) {
                    alpha = (uint8_t)(255 * (deg - start_deg) / tail_len);
                }
            }
            uint32_t dst = fb_get_pixel(px, py);
            fb_put_pixel(px, py, alpha_blend(dst, color, alpha));
        }
    }
}

/* ── Draw filled circle (direct to back buffer) ──────────────────────── */
static void ba_fill_circle(int cx, int cy, int r, uint32_t color)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x*x + y*y <= r*r) {
                fb_put_pixel(cx + x, cy + y, color);
            }
        }
    }
}

/* ── Render one frame ────────────────────────────────────────────────── */
void bootanim_render(void)
{
    if (!fb_ready()) return;

    bootanim_state_t* st = &g_bootanim;
    uint32_t w = fb.width;
    uint32_t h = fb.height;

    /* Background */
    uint32_t bg = BA_BG;
    if (st->fade_alpha < 255) {
        /* Fade in: blend from black to bg */
        uint8_t a = (uint8_t)st->fade_alpha;
        bg = alpha_blend(0xFF000000u, BA_BG, a);
    }
    if (st->phase == BOOTANIM_PHASE_FADE_OUT) {
        /* Fade out: blend to black */
        uint8_t a = (uint8_t)st->fade_alpha;
        bg = alpha_blend(0xFF000000u, BA_BG, a);
    }

    /* Fill background */
    for (uint32_t i = 0; i < w * h; i++) {
        fb.back_buf[i] = bg;
    }

    /* Draw ring only after fade-in starts */
    if (st->phase >= BOOTANIM_PHASE_LOGO && st->phase < BOOTANIM_PHASE_FADE_OUT) {
        int cx = (int)w / 2;
        int cy = (int)h / 2 - 40;
        int ring_r = 60;
        int thickness = 8;

        /* Background ring track */
        draw_ring_arc(cx, cy, ring_r, thickness, 0, 359, BA_RING_BG);

        /* Animated sweep — angle progresses each tick */
        int sweep = (st->tick % 360);
        int start = sweep;
        int end   = sweep + 270;
        draw_ring_arc(cx, cy, ring_r, thickness, start, end, BA_RING_FG);

        /* Bright tip */
        int tip_x = cx + icos(end % 360) * ring_r / 1024;
        int tip_y = cy + isin(end % 360) * ring_r / 1024;
        ba_fill_circle(tip_x, tip_y, 5, BA_RING_ACC);
    }

    /* OS name */
    if (st->phase >= BOOTANIM_PHASE_TEXT) {
        canvas_t c = draw_main_canvas();
        int cx = (int)w / 2;
        int ty = (int)h / 2 + 30;

        const char* title    = OS_BOOT_LINE1;
        const char* subtitle = OS_BOOT_LINE2;

        int tw = (int)strlen(title)    * 10;
        int sw = (int)strlen(subtitle) * 7;
        draw_string(&c, cx - tw / 2, ty,      title,    BA_TEXT,    0);
        draw_string(&c, cx - sw / 2, ty + 20, subtitle, BA_SUBTEXT, 0);
    }

    /* Progress bar at bottom */
    if (st->phase >= BOOTANIM_PHASE_PROGRESS) {
        int bar_y      = (int)h - 12;
        int bar_h      = 4;
        int bar_margin = (int)w / 6;
        int bar_w      = (int)w - bar_margin * 2;
        int fill_w     = bar_w * st->progress / 100;

        /* Track */
        for (int y = bar_y; y < bar_y + bar_h; y++) {
            for (int x = bar_margin; x < bar_margin + bar_w; x++) {
                fb.back_buf[y * w + x] = BA_PROGRESS_BG;
            }
        }
        /* Fill */
        for (int y = bar_y; y < bar_y + bar_h; y++) {
            for (int x = bar_margin; x < bar_margin + fill_w; x++) {
                fb.back_buf[y * w + x] = BA_PROGRESS;
            }
        }

        /* Step label */
        if (st->step < st->num_steps && st->steps[st->step].label) {
            canvas_t c = draw_main_canvas();
            int lx = bar_margin;
            int ly = bar_y - 16;
            draw_string(&c, lx, ly, st->steps[st->step].label, BA_SUBTEXT, 0);
        }
    }

    fb_flip();
}

/* ── Subsystem API ───────────────────────────────────────────────────── */

void bootanim_start(void)
{
    memset(&g_bootanim, 0, sizeof(g_bootanim));
    g_bootanim.phase      = BOOTANIM_PHASE_FADE_IN;
    g_bootanim.running    = true;
    g_bootanim.fade_alpha = 0;

    kinfo("BOOTANIM: starting boot animation");
}

void bootanim_add_step(const char* label, int weight)
{
    bootanim_state_t* st = &g_bootanim;
    if (st->num_steps >= BOOTANIM_MAX_STEPS) return;

    st->steps[st->num_steps].label  = label;
    st->steps[st->num_steps].weight = weight;
    st->total_weight += weight;
    st->num_steps++;
}

void bootanim_step_done(void)
{
    bootanim_state_t* st = &g_bootanim;
    if (st->step < st->num_steps) {
        int done_weight = 0;
        for (int i = 0; i <= st->step; i++) {
            done_weight += st->steps[i].weight;
        }
        if (st->total_weight > 0) {
            st->progress = done_weight * 100 / st->total_weight;
        }
        st->step++;
    }
}

void bootanim_tick(void)
{
    bootanim_state_t* st = &g_bootanim;
    if (!st->running) return;

    st->tick++;

    switch (st->phase) {
    case BOOTANIM_PHASE_FADE_IN:
        st->fade_alpha += 255 / BOOTANIM_FADE_IN_TICKS;
        if (st->fade_alpha >= 255) {
            st->fade_alpha = 255;
            st->phase      = BOOTANIM_PHASE_LOGO;
        }
        break;

    case BOOTANIM_PHASE_LOGO:
        if (st->tick > BOOTANIM_FADE_IN_TICKS + BOOTANIM_LOGO_TICKS) {
            st->phase = BOOTANIM_PHASE_TEXT;
        }
        break;

    case BOOTANIM_PHASE_TEXT:
        if (st->tick > BOOTANIM_FADE_IN_TICKS + BOOTANIM_LOGO_TICKS +
                       BOOTANIM_TEXT_TICKS) {
            st->phase = BOOTANIM_PHASE_PROGRESS;
        }
        break;

    case BOOTANIM_PHASE_PROGRESS:
        if (st->progress >= 100) {
            st->phase = BOOTANIM_PHASE_FADE_OUT;
        }
        break;

    case BOOTANIM_PHASE_FADE_OUT:
        st->fade_alpha -= 255 / BOOTANIM_FADE_OUT_TICKS;
        if (st->fade_alpha <= 0) {
            st->fade_alpha = 0;
            st->phase      = BOOTANIM_PHASE_DONE;
            st->running    = false;
        }
        break;

    default:
        break;
    }

    bootanim_render();
}

bool bootanim_done(void)
{
    return g_bootanim.phase == BOOTANIM_PHASE_DONE || !g_bootanim.running;
}

void bootanim_skip(void)
{
    g_bootanim.phase   = BOOTANIM_PHASE_DONE;
    g_bootanim.running = false;
    /* Clear to black */
    if (fb_ready()) {
        fb_clear(0xFF000000u);
        fb_flip();
    }
}
