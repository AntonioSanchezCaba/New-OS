/*
 * apps/clock.c — ARE Floating Clock Application
 *
 * Analog + digital clock rendered as a SURF_FLOAT draggable window.
 * Displays hours, minutes, second hand, and a digital HH:MM:SS readout.
 * Self-invalidates every second via tick delta tracking.
 *
 * Integer trig: sin_tab[deg] = sin(deg * PI/180) * 1024 (no libm, no SSE).
 * Timer: timer_get_ticks() at TIMER_FREQ=100 Hz → /100 = seconds.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>

/* =========================================================
 * Geometry (body area — ARE adds FLOAT_TITLE_H=26 above)
 * ========================================================= */
#define CLK_W       200
#define CLK_H       230
#define CLK_CX      (CLK_W / 2)
#define CLK_CY      105
#define CLK_R        82    /* outer rim radius */
#define CLK_R_FACE   78    /* inner face radius */

/* =========================================================
 * Colors
 * ========================================================= */
#define CLK_BG      ACOLOR(0x0A, 0x0E, 0x18, 0xFF)
#define CLK_FACE    ACOLOR(0x10, 0x18, 0x2C, 0xFF)
#define CLK_RIM     ACOLOR(0x30, 0x50, 0x80, 0xFF)
#define CLK_TICK    ACOLOR(0x50, 0x70, 0xA0, 0xFF)
#define CLK_HTICK   ACOLOR(0x80, 0xB0, 0xFF, 0xFF)
#define CLK_HOUR    ACOLOR(0xCC, 0xDD, 0xFF, 0xFF)
#define CLK_MIN     ACOLOR(0xDD, 0xEE, 0xFF, 0xFF)
#define CLK_SEC     ACOLOR(0xFF, 0x60, 0x40, 0xFF)
#define CLK_CENTER  ACOLOR(0xFF, 0xFF, 0xFF, 0xFF)
#define CLK_DIG_FG  ACOLOR(0xCC, 0xDD, 0xFF, 0xFF)
#define CLK_DIG_BG  ACOLOR(0x08, 0x0C, 0x18, 0xFF)
#define CLK_DIG_BD  ACOLOR(0x28, 0x38, 0x58, 0xFF)

/* =========================================================
 * Integer sin lookup: sin_tab[i] = sin(i°) × 1024
 * 360 entries, signed 16-bit values.
 * ========================================================= */
static const int16_t sin_tab[360] = {
       0,  17,  35,  52,  70,  87, 105, 122, 139, 156, 173, 190, 207, 224, 241,
     258, 275, 292, 309, 325, 342, 358, 374, 390, 406, 422, 438, 453, 469, 484,
     499, 514, 529, 544, 559, 573, 587, 601, 615, 629, 642, 656, 669, 681, 694,
     707, 719, 731, 743, 754, 766, 777, 788, 798, 809, 819, 829, 838, 848, 857,
     866, 874, 882, 891, 898, 906, 913, 920, 927, 933, 940, 946, 951, 957, 962,
     967, 971, 975, 979, 983, 987, 990, 993, 996, 998,1000,1002,1003,1004,1005,
    1006,1006,1006,1006,1006,1005,1004,1003,1002,1000, 998, 996, 993, 990, 987,
     983, 979, 975, 971, 967, 962, 957, 951, 946, 940, 933, 927, 920, 913, 906,
     898, 891, 882, 874, 866, 857, 848, 838, 829, 819, 809, 798, 788, 777, 766,
     754, 743, 731, 719, 707, 694, 681, 669, 656, 642, 629, 615, 601, 587, 573,
     559, 544, 529, 514, 499, 484, 469, 453, 438, 422, 406, 390, 374, 358, 342,
     325, 309, 292, 275, 258, 241, 224, 207, 190, 173, 156, 139, 122, 105,  87,
      70,  52,  35,  17,   0, -17, -35, -52, -70, -87,-105,-122,-139,-156,-173,
    -190,-207,-224,-241,-258,-275,-292,-309,-325,-342,-358,-374,-390,-406,-422,
    -438,-453,-469,-484,-499,-514,-529,-544,-559,-573,-587,-601,-615,-629,-642,
    -656,-669,-681,-694,-707,-719,-731,-743,-754,-766,-777,-788,-798,-809,-819,
    -829,-838,-848,-857,-866,-874,-882,-891,-898,-906,-913,-920,-927,-933,-940,
    -946,-951,-957,-962,-967,-971,-975,-979,-983,-987,-990,-993,-996,-998,
    -1000,-1002,-1003,-1004,-1005,-1006,-1006,-1006,-1006,-1006,-1005,-1004,
    -1003,-1002,-1000,-998,-996,-993,-990,-987,-983,-979,-975,-971,-967,-962,
    -957,-951,-946,-940,-933,-927,-920,-913,-906,-898,-891,-882,-874,-866,-857,
    -848,-838,-829,-819,-809,-798,-788,-777,-766,-754,-743,-731,-719,-707,-694,
    -681,-669,-656,-642,-629,-615,-601,-587,-573,-559,-544,-529,-514,-499,-484,
    -469,-453,-438,-422,-406,-390,-374,-358,-342,-325,-309,-292,-275,-258,-241,
    -224,-207,-190,-173,-156,-139,-122,-105,-87,-70,-52,-35,-17
};

/* 0° = 12 o'clock (top), values increase clockwise */
static inline int clk_sin(int d) { return sin_tab[((d % 360) + 360) % 360]; }
static inline int clk_cos(int d) { return sin_tab[((d + 90) % 360 + 360) % 360]; }

static inline void hand_pt(int deg, int r, int* ox, int* oy)
{
    *ox = CLK_CX + (clk_sin(deg) * r) / 1024;
    *oy = CLK_CY - (clk_cos(deg) * r) / 1024;
}

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    uint32_t last_sec;
    sid_t    sid;
    bool     open;
} clk_t;

static clk_t g_clk;

/* =========================================================
 * Helpers
 * ========================================================= */
static void draw_dot(canvas_t* c, int cx, int cy, int r, acolor_t col)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                draw_pixel(c, cx + dx, cy + dy, col);
}

/* =========================================================
 * Render callback
 * ========================================================= */
static void clk_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                       void* ud)
{
    (void)ud;
    canvas_t cv = { .pixels = pixels, .width = w, .height = h };

    /* Current uptime */
    uint64_t ticks    = timer_get_ticks();
    uint32_t total_s  = (uint32_t)(ticks / 100);   /* 100 Hz timer */
    uint32_t ss       = total_s % 60;
    uint32_t mm       = (total_s / 60) % 60;
    uint32_t hh12     = (total_s / 3600) % 12;

    /* Self-invalidate once per second */
    if (total_s != g_clk.last_sec) {
        g_clk.last_sec = total_s;
        surface_invalidate(id);
    }

    /* Background */
    draw_rect(&cv, 0, 0, (int)w, (int)h, CLK_BG);

    /* Clock face */
    draw_dot(&cv, CLK_CX, CLK_CY, CLK_R_FACE, CLK_FACE);

    /* Rim (approximated with line segments) */
    for (int deg = 0; deg < 360; deg += 3) {
        int x1, y1, x2, y2;
        hand_pt(deg,     CLK_R,  &x1, &y1);
        hand_pt(deg + 3, CLK_R,  &x2, &y2);
        draw_line(&cv, x1, y1, x2, y2, CLK_RIM);
    }

    /* Tick marks */
    for (int i = 0; i < 60; i++) {
        int deg    = i * 6;
        bool hour  = (i % 5 == 0);
        int inner  = CLK_R_FACE - (hour ? 14 : 6);
        int ox, oy, ix, iy;
        hand_pt(deg, CLK_R_FACE - 2, &ox, &oy);
        hand_pt(deg, inner,          &ix, &iy);
        draw_line(&cv, ox, oy, ix, iy, hour ? CLK_HTICK : CLK_TICK);
    }

    /* Hour hand (thick: 3-pixel) */
    {
        int deg = (int)(hh12 * 30 + mm / 2);
        int ex, ey;
        hand_pt(deg, CLK_R_FACE * 55 / 100, &ex, &ey);
        draw_line(&cv, CLK_CX - 1, CLK_CY, ex - 1, ey, CLK_HOUR);
        draw_line(&cv, CLK_CX,     CLK_CY, ex,     ey, CLK_HOUR);
        draw_line(&cv, CLK_CX + 1, CLK_CY, ex + 1, ey, CLK_HOUR);
    }

    /* Minute hand (2-pixel) */
    {
        int ex, ey;
        hand_pt((int)(mm * 6), CLK_R_FACE * 80 / 100, &ex, &ey);
        draw_line(&cv, CLK_CX - 1, CLK_CY, ex - 1, ey, CLK_MIN);
        draw_line(&cv, CLK_CX,     CLK_CY, ex,     ey, CLK_MIN);
    }

    /* Second hand (thin, red) + counterweight */
    {
        int ex, ey, tx, ty;
        hand_pt((int)(ss * 6),       CLK_R_FACE * 88 / 100, &ex, &ey);
        hand_pt((int)(ss * 6 + 180), CLK_R_FACE * 20 / 100, &tx, &ty);
        draw_line(&cv, tx, ty, ex, ey, CLK_SEC);
    }

    /* Center hub */
    draw_dot(&cv, CLK_CX, CLK_CY, 4, CLK_CENTER);

    /* Digital display below face */
    int dig_y = CLK_CY + CLK_R + 10;
    int dig_h = FONT_H + 8;
    int dig_w = 10 * FONT_W;   /* "HH:MM:SS" + padding */
    int dig_x = (CLK_W - dig_w) / 2;

    draw_rect(&cv, dig_x, dig_y, dig_w, dig_h, CLK_DIG_BG);
    draw_rect_outline(&cv, dig_x, dig_y, dig_w, dig_h, 1, CLK_DIG_BD);

    char tbuf[10];
    snprintf(tbuf, sizeof(tbuf), "%02u:%02u:%02u",
             (uint32_t)(total_s / 3600) % 24, mm, ss);
    draw_string(&cv, dig_x + 4, dig_y + 4, tbuf, CLK_DIG_FG, ACOLOR(0, 0, 0, 0));

    /* "Uptime" label */
    const char* lbl = "Uptime";
    draw_string(&cv,
                (CLK_W - (int)strlen(lbl) * FONT_W) / 2,
                dig_y + dig_h + 6,
                lbl, ACOLOR(0x50, 0x68, 0x90, 0xFF), ACOLOR(0, 0, 0, 0));
}

/* =========================================================
 * Input callback — clock is display-only
 * ========================================================= */
static void clk_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)id; (void)ev; (void)ud;
}

/* =========================================================
 * Close callback
 * ========================================================= */
static void clk_on_close(sid_t id, void* ud)
{
    (void)id; (void)ud;
    g_clk.open = false;
    g_clk.sid  = SID_NONE;
}

/* =========================================================
 * Public: open clock as floating ARE window
 * ========================================================= */
sid_t surface_clock_open(void)
{
    if (g_clk.open && g_clk.sid != SID_NONE)
        return g_clk.sid;

    g_clk.last_sec = 0;
    g_clk.open     = true;

    g_clk.sid = are_add_surface(SURF_FLOAT, CLK_W, CLK_H,
                                 "Clock", "T",
                                 clk_render, clk_input,
                                 clk_on_close, NULL);
    return g_clk.sid;
}
