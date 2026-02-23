/*
 * apps/clock.c - Aether OS Clock Application
 *
 * Displays a combined analog + digital clock.
 *   - Analog face with hour, minute, and second hands.
 *   - Digital readout (HH:MM:SS) below the face.
 *   - Date line showing elapsed days since boot (boot epoch placeholder).
 *   - Updates every second tick.
 */
#include <gui/gui.h>
#include <drivers/timer.h>
#include <string.h>
#include <memory.h>

/* Window geometry */
#define CLOCK_W  240
#define CLOCK_H  280

/* Analog face */
#define FACE_CX  (CLOCK_W / 2)
#define FACE_CY  110
#define FACE_R   90
#define FACE_R2  (FACE_R - 2)

/* Digital display */
#define DIG_Y   (FACE_CY + FACE_R + 14)

/* Clock state */
typedef struct {
    wid_t    wid;
    uint32_t last_sec;
} clock_app_t;

static clock_app_t g_clock;

/* =========================================================
 * Trig helpers (integer, no libm)
 * ========================================================= */

/* sin/cos approximation via lookup table — 360 entries */
/* sin_table[i] = sin(i * PI/180) * 1024 */
static const int16_t sin_table[] = {
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
    -946,-951,-957,-962,-967,-971,-975,-979,-983,-987,-990,-993,-996,-998,-1000,
    -1002,-1003,-1004,-1005,-1006,-1006,-1006,-1006,-1006,-1005,-1004,-1003,-1002,-1000,
     -998,-996,-993,-990,-987,-983,-979,-975,-971,-967,-962,-957,-951,-946,-940,
     -933,-927,-920,-913,-906,-898,-891,-882,-874,-866,-857,-848,-838,-829,-819,
     -809,-798,-788,-777,-766,-754,-743,-731,-719,-707,-694,-681,-669,-656,-642,
     -629,-615,-601,-587,-573,-559,-544,-529,-514,-499,-484,-469,-453,-438,-422,
     -406,-390,-374,-358,-342,-325,-309,-292,-275,-258,-241,-224,-207,-190,-173,
      -156,-139,-122,-105, -87, -70, -52, -35, -17,
};

static int isin(int deg)
{
    while (deg < 0)   deg += 360;
    while (deg >= 360) deg -= 360;
    return (int)sin_table[deg];
}

static int icos(int deg)
{
    return isin(deg + 90); /* cos(x) = sin(x + 90) */
}

/* =========================================================
 * Drawing helpers
 * ========================================================= */

/* Draw a thick clock hand */
static void draw_hand(canvas_t* c, int cx, int cy, int len,
                       int angle_deg, int thick, uint32_t col)
{
    /* angle_deg: 0 = 12-o'clock (top), increases clockwise */
    int x1 = cx + len * isin(angle_deg) / 1024;
    int y1 = cy - len * icos(angle_deg) / 1024;

    for (int t = -thick / 2; t <= thick / 2; t++) {
        int sx = cx + t * icos(angle_deg) / 1024;
        int sy = cy + t * isin(angle_deg) / 1024;
        draw_line(c, sx, sy, x1, y1, col);
    }
}

/* Draw filled circle by drawing horizontal scan lines */
static void draw_filled_circle_simple(canvas_t* c, int cx, int cy, int r, uint32_t col)
{
    draw_circle_filled(c, cx, cy, r, col);
}

/* =========================================================
 * Clock rendering
 * ========================================================= */

static void clock_draw(wid_t wid)
{
    canvas_t cv = wm_client_canvas(wid);
    const theme_t* th = theme_current();

    /* Background */
    draw_rect(&cv, 0, 0, CLOCK_W, CLOCK_H, th->win_bg);

    /* ---- Analog face ---- */
    /* Outer bezel */
    draw_circle_filled(&cv, FACE_CX, FACE_CY, FACE_R + 4, th->win_border);
    /* Face background */
    draw_circle_filled(&cv, FACE_CX, FACE_CY, FACE_R, th->panel_bg);
    /* Inner ring */
    draw_circle(&cv, FACE_CX, FACE_CY, FACE_R, th->panel_border);

    /* Hour tick marks */
    for (int h = 0; h < 12; h++) {
        int angle = h * 30;
        int inner = (h % 3 == 0) ? FACE_R - 14 : FACE_R - 8;
        int x0 = FACE_CX + FACE_R2 * isin(angle) / 1024;
        int y0 = FACE_CY - FACE_R2 * icos(angle) / 1024;
        int x1 = FACE_CX + inner * isin(angle) / 1024;
        int y1 = FACE_CY - inner * icos(angle) / 1024;
        uint32_t col = (h % 3 == 0) ? th->text_primary : th->text_secondary;
        draw_line(&cv, x0, y0, x1, y1, col);
    }

    /* Get current time from system ticks */
    uint32_t secs  = timer_get_ticks() / TIMER_FREQ;
    uint32_t h2    = (secs / 3600) % 12;
    uint32_t m2    = (secs /   60) % 60;
    uint32_t s2    =  secs         % 60;

    /* Hour hand */
    int hour_angle = (int)(h2 * 30 + m2 / 2);
    draw_hand(&cv, FACE_CX, FACE_CY, FACE_R - 28, hour_angle, 5, th->text_primary);

    /* Minute hand */
    int min_angle = (int)(m2 * 6 + s2 / 10);
    draw_hand(&cv, FACE_CX, FACE_CY, FACE_R - 14, min_angle, 3, th->text_primary);

    /* Second hand (thin, accent color) */
    int sec_angle = (int)(s2 * 6);
    draw_hand(&cv, FACE_CX, FACE_CY, FACE_R - 10, sec_angle, 1, th->accent);

    /* Center dot */
    draw_filled_circle_simple(&cv, FACE_CX, FACE_CY, 5, th->accent);
    draw_filled_circle_simple(&cv, FACE_CX, FACE_CY, 2, th->win_bg);

    /* ---- Digital display ---- */
    char tstr[12];
    tstr[0] = '0' + (char)((h2 + 0) / 10 % 10);  /* h2 in 12h */
    /* Re-compute 24h for display */
    uint32_t h24  = (secs / 3600) % 24;
    uint32_t h12  = h24 % 12;
    if (h12 == 0) h12 = 12;
    tstr[0]  = '0' + (char)(h12 / 10);
    tstr[1]  = '0' + (char)(h12 % 10);
    tstr[2]  = ':';
    tstr[3]  = '0' + (char)(m2 / 10);
    tstr[4]  = '0' + (char)(m2 % 10);
    tstr[5]  = ':';
    tstr[6]  = '0' + (char)(s2 / 10);
    tstr[7]  = '0' + (char)(s2 % 10);
    tstr[8]  = ' ';
    tstr[9]  = (h24 < 12) ? 'A' : 'P';
    tstr[10] = 'M';
    tstr[11] = '\0';

    int tw = (int)strlen(tstr) * FONT_W;
    draw_string(&cv, CLOCK_W / 2 - tw / 2, DIG_Y,
                tstr, th->text_primary, rgba(0, 0, 0, 0));

    /* Uptime subtitle */
    uint32_t days  = secs / 86400;
    uint32_t uhrs  = (secs / 3600) % 24;
    uint32_t umins = (secs / 60) % 60;
    char ustr[48];
    /* Format: "Uptime: 0d 01:23" */
    ustr[0]  = 'U'; ustr[1]  = 'p'; ustr[2] = 't'; ustr[3] = 'i';
    ustr[4]  = 'm'; ustr[5]  = 'e'; ustr[6] = ':'; ustr[7] = ' ';
    ustr[8]  = '0' + (char)(days / 10); ustr[9]  = '0' + (char)(days % 10);
    ustr[10] = 'd'; ustr[11] = ' ';
    ustr[12] = '0' + (char)(uhrs / 10);  ustr[13] = '0' + (char)(uhrs % 10);
    ustr[14] = ':';
    ustr[15] = '0' + (char)(umins / 10); ustr[16] = '0' + (char)(umins % 10);
    ustr[17] = '\0';
    int uw = (int)strlen(ustr) * FONT_W;
    draw_string(&cv, CLOCK_W / 2 - uw / 2, DIG_Y + FONT_H + 6,
                ustr, th->text_secondary, rgba(0, 0, 0, 0));

    /* Bottom decoration bar */
    draw_hline(&cv, 20, DIG_Y + FONT_H * 2 + 14, CLOCK_W - 40, th->panel_border);

    const char* label = "Aether OS Clock";
    int lw = (int)strlen(label) * FONT_W;
    draw_string(&cv, CLOCK_W / 2 - lw / 2, DIG_Y + FONT_H * 2 + 20,
                label, th->text_disabled, rgba(0, 0, 0, 0));
}

/* =========================================================
 * Window event handler
 * ========================================================= */

static void clock_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    if (evt->type == GUI_EVENT_PAINT) {
        clock_draw(wid);
        return;
    }
    if (evt->type == GUI_EVENT_CLOSE) {
        g_clock.wid = -1;
        return;
    }
    if (evt->type == GUI_EVENT_TIMER) {
        /* Tick every second for redraw */
        uint32_t now = timer_get_ticks() / TIMER_FREQ;
        if (now != g_clock.last_sec) {
            g_clock.last_sec = now;
            wm_invalidate(wid);
        }
    }
}

/* =========================================================
 * Public launch
 * ========================================================= */

void gui_launch_clock(void)
{
    clock_app_t* c = &g_clock;
    if (c->wid > 0 && wm_get_window(c->wid)) {
        wm_raise(c->wid);
        return;
    }
    c->last_sec = timer_get_ticks() / TIMER_FREQ;
    c->wid = wm_create_window("Clock", 100, 100, CLOCK_W, CLOCK_H,
                               clock_on_event, NULL);
    if (c->wid > 0) {
        taskbar_add(c->wid, "Clock");
    }
}

/* Called by desktop_tick() every frame to ensure 1-sec updates */
void clock_tick(void)
{
    if (g_clock.wid <= 0) return;
    if (!wm_get_window(g_clock.wid)) { g_clock.wid = -1; return; }
    uint32_t now = timer_get_ticks() / TIMER_FREQ;
    if (now != g_clock.last_sec) {
        g_clock.last_sec = now;
        wm_invalidate(g_clock.wid);
    }
}
