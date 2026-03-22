/*
 * gui/desktop.c - Aether OS Desktop Environment
 *
 * Provides:
 *   - Gradient desktop background with subtle grid
 *   - Taskbar (bottom) with start button, app buttons, workspace indicator,
 *     notification area, and system clock
 *   - Start menu with app launcher grid + quick actions
 *   - Mouse-driven interaction
 *   - System notifications (via notify.h)
 *   - Virtual workspace indicator (4 workspaces, cosmetic)
 */
#include <gui/gui.h>
#include <kernel/version.h>
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <gui/notify.h>
#include <gui/wallpaper.h>
#include <gui/animation.h>
#include <gui/workspace.h>
#include <services/splash.h>
#include <services/login.h>
#include <drivers/framebuffer.h>
#include <drivers/cursor.h>
#include <drivers/mouse.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <process.h>
#include <scheduler.h>

/* ---- Layout constants ---- */
#define TASKBAR_H         38
#define TASKBAR_BTN_W     110
#define TASKBAR_BTN_H     (TASKBAR_H - 8)
#define START_BTN_W       72
#define WORKSPACE_W       84    /* 4 workspaces × 20 + gaps */
#define TRAY_W            110   /* clock + icons */

#define MENU_W            220
#define MENU_ITEM_H       30
#define MENU_ICON_W       24

/* ---- Desktop icon layout ---- */
#define ICON_SIZE         48
#define ICON_CELL_W       90
#define ICON_CELL_H       80
#define ICON_MARGIN_X     24
#define ICON_MARGIN_Y     20
#define ICON_LABEL_GAP     4

/* ---- Starfield ---- */
#define STAR_COUNT        120

/* ---- Desktop state ---- */
static bool      gui_running       = false;
static canvas_t  g_screen;
static bool      startmenu_open    = false;
static int       g_workspace       = 0;    /* current virtual workspace 0-3 */
static uint32_t  g_frame_count     = 0;
/* g_first_frame removed - unused */

/* ---- Taskbar entries ---- */
typedef struct {
    wid_t wid;
    char  label[24];
} taskbar_entry_t;

#define TASKBAR_MAX 14
static taskbar_entry_t taskbar_entries[TASKBAR_MAX];
static int             taskbar_count = 0;

void taskbar_add(wid_t wid, const char* label)
{
    if (taskbar_count >= TASKBAR_MAX) return;
    taskbar_entries[taskbar_count].wid = wid;
    strncpy(taskbar_entries[taskbar_count].label, label, 23);
    taskbar_entries[taskbar_count].label[23] = '\0';
    taskbar_count++;
}

static void taskbar_remove_closed(void)
{
    int j = 0;
    for (int i = 0; i < taskbar_count; i++) {
        window_t* w = wm_get_window(taskbar_entries[i].wid);
        if (w) taskbar_entries[j++] = taskbar_entries[i];
    }
    taskbar_count = j;
}

/* ---- Desktop icons ---- */
typedef struct {
    const char* label;
    uint32_t    icon_color;
} desktop_icon_t;

static const desktop_icon_t g_desktop_icons[] = {
    { "Terminal",  0xFF60C0FF },
    { "Files",     0xFFFFC060 },
    { "Editor",    0xFF80E080 },
    { "Monitor",   0xFFFF8060 },
    { "Settings",  0xFFB0B0C0 },
    { "Network",   0xFF60E0E0 },
    { "Calculator",0xFFE0A0FF },
    { "Images",    0xFF60FF90 },
};
#define DESKTOP_ICON_COUNT 8

/* Get icon position (single column on left side) */
static void icon_pos(int i, int* cx, int* cy)
{
    *cx = ICON_MARGIN_X + ICON_CELL_W / 2;
    *cy = ICON_MARGIN_Y + i * ICON_CELL_H + ICON_SIZE / 2;
}

/* Icon drawing: larger pixel-art style icon */
static void draw_desktop_icon_gfx(canvas_t* scr, int cx, int cy, int idx,
                                    uint32_t color, bool hovered)
{
    int sz = ICON_SIZE;
    int x = cx - sz / 2;
    int y = cy - sz / 2;

    /* Background: rounded glass panel */
    uint32_t bg = hovered ? rgba(0x30, 0x50, 0x80, 0xA0) : rgba(0x18, 0x28, 0x40, 0x60);
    draw_rect_rounded(scr, x, y, sz, sz, 8, bg);
    if (hovered) {
        draw_rect_rounded_outline(scr, x, y, sz, sz, 8, 1, rgba(0x60, 0xA0, 0xE0, 0xC0));
        /* Glow effect */
        draw_rect_alpha(scr, x-2, y-2, sz+4, sz+4, rgba(0x40, 0x80, 0xCC, 0x18));
    }

    /* Icon interior */
    uint32_t c1 = color;
    uint32_t c2 = (color & 0x00FFFFFF) | 0x90000000;
    switch (idx) {
    case 0: /* Terminal >_ */
        draw_rect_rounded(scr, x+6, y+6, sz-12, sz-12, 3, rgba(0x08, 0x10, 0x18, 0xC0));
        draw_line(scr, x+12, y+14, x+20, y+24, c1);
        draw_line(scr, x+20, y+24, x+12, y+34, c1);
        draw_hline(scr, x+22, y+34, 14, c1);
        break;
    case 1: /* Files - folder */
        draw_rect_rounded(scr, x+8, y+16, 32, 22, 3, c2);
        draw_rect(scr, x+8, y+12, 14, 6, c1);
        draw_rect_rounded(scr, x+8, y+16, 32, 20, 3, c1);
        draw_hline(scr, x+12, y+22, 24, rgba(0xFF,0xFF,0xFF,0x30));
        break;
    case 2: /* Editor - document with lines */
        draw_rect_rounded(scr, x+10, y+6, 28, 36, 3, rgba(0xFF,0xFF,0xFF,0xE0));
        draw_hline(scr, x+14, y+14, 20, c1);
        draw_hline(scr, x+14, y+20, 16, c2);
        draw_hline(scr, x+14, y+26, 18, c1);
        draw_hline(scr, x+14, y+32, 12, c2);
        break;
    case 3: /* Monitor - chart with bars */
        draw_rect_rounded(scr, x+6, y+8, 36, 26, 3, rgba(0x10,0x18,0x28,0xD0));
        draw_rect(scr, x+12, y+24, 5, 8, c1);
        draw_rect(scr, x+19, y+18, 5, 14, rgba(0x40,0xCC,0x60,0xFF));
        draw_rect(scr, x+26, y+14, 5, 18, c1);
        draw_rect(scr, x+33, y+20, 5, 12, rgba(0xFF,0xCC,0x40,0xFF));
        /* Stand */
        draw_rect(scr, x+20, y+36, 8, 3, rgba(0x80,0x90,0xA0,0xFF));
        draw_hline(scr, x+16, y+39, 16, rgba(0x80,0x90,0xA0,0xFF));
        break;
    case 4: /* Settings - gear */
        draw_circle(scr, x+24, y+24, 12, c1);
        draw_circle(scr, x+24, y+24, 11, c1);
        draw_circle_filled(scr, x+24, y+24, 6, c1);
        draw_circle_filled(scr, x+24, y+24, 3, bg);
        draw_rect(scr, x+23, y+10, 2, 5, c1);
        draw_rect(scr, x+23, y+33, 2, 5, c1);
        draw_rect(scr, x+10, y+23, 5, 2, c1);
        draw_rect(scr, x+33, y+23, 5, 2, c1);
        break;
    case 5: /* Network - globe/signal */
        draw_circle(scr, x+24, y+24, 14, c2);
        draw_circle(scr, x+24, y+24, 10, c1);
        draw_hline(scr, x+14, y+24, 20, c1);
        draw_vline(scr, x+24, y+14, 20, c1);
        draw_circle(scr, x+24, y+24, 6, c2);
        break;
    case 6: /* Calculator */
        draw_rect_rounded(scr, x+10, y+6, 28, 36, 4, c2);
        draw_rect(scr, x+14, y+10, 20, 8, rgba(0x20,0x30,0x40,0xFF));
        draw_string(scr, x+16, y+11, "123", c1, rgba(0,0,0,0));
        draw_rect(scr, x+14, y+22, 6, 5, c1);
        draw_rect(scr, x+22, y+22, 6, 5, c1);
        draw_rect(scr, x+30, y+22, 6, 5, rgba(0xFF,0x80,0x40,0xFF));
        draw_rect(scr, x+14, y+30, 6, 5, c1);
        draw_rect(scr, x+22, y+30, 6, 5, c1);
        draw_rect(scr, x+30, y+30, 6, 5, rgba(0x40,0xCC,0x60,0xFF));
        break;
    case 7: /* Images - landscape */
        draw_rect_rounded(scr, x+6, y+8, 36, 28, 3, rgba(0x30,0x60,0x90,0xE0));
        draw_line(scr, x+10, y+30, x+18, y+18, rgba(0x40,0xA0,0x40,0xFF));
        draw_line(scr, x+18, y+18, x+24, y+24, rgba(0x40,0xA0,0x40,0xFF));
        draw_line(scr, x+24, y+24, x+32, y+14, rgba(0x60,0xC0,0x60,0xFF));
        draw_line(scr, x+32, y+14, x+38, y+20, rgba(0x60,0xC0,0x60,0xFF));
        draw_circle_filled(scr, x+32, y+14, 3, 0xFFFFE060);
        /* Fill bottom */
        for (int fy = 26; fy <= 32; fy++)
            draw_hline(scr, x+8, y+fy, 32, rgba(0x30,0x80,0x30, (uint8_t)(0x40 + (fy-26)*0x18)));
        break;
    }
}

static void draw_desktop_icons(canvas_t* scr)
{
    int H = scr->height - TASKBAR_H;

    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int cx, cy;
        icon_pos(i, &cx, &cy);
        if (cy + ICON_SIZE/2 + FONT_H + ICON_LABEL_GAP >= H) continue;

        bool hov = (mouse.x >= cx - ICON_CELL_W/2 && mouse.x < cx + ICON_CELL_W/2 &&
                    mouse.y >= cy - ICON_SIZE/2 - 4 && mouse.y < cy + ICON_SIZE/2 + FONT_H + ICON_LABEL_GAP + 4 &&
                    mouse.y < H);

        draw_desktop_icon_gfx(scr, cx, cy, i, g_desktop_icons[i].icon_color, hov);

        /* Label below icon (with shadow) */
        int lw = draw_string_width(g_desktop_icons[i].label);
        int lx = cx - lw / 2;
        int ly = cy + ICON_SIZE / 2 + ICON_LABEL_GAP;
        draw_string(scr, lx+1, ly+1, g_desktop_icons[i].label,
                    rgba(0,0,0,0x80), rgba(0,0,0,0));
        uint32_t label_col = hov ? 0xFFFFFFFF : rgba(0xC0, 0xD8, 0xF0, 0xE0);
        draw_string(scr, lx, ly, g_desktop_icons[i].label, label_col, rgba(0,0,0,0));
    }
}

static void desktop_icon_click(int mx, int my)
{
    int H = g_screen.height - TASKBAR_H;
    if (my >= H) return;

    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int cx, cy;
        icon_pos(i, &cx, &cy);

        if (mx >= cx - ICON_CELL_W/2 && mx < cx + ICON_CELL_W/2 &&
            my >= cy - ICON_SIZE/2 - 4 && my < cy + ICON_SIZE/2 + FONT_H + ICON_LABEL_GAP + 4) {
            switch (i) {
            case 0: gui_launch_terminal();    break;
            case 1: gui_launch_filemanager(); break;
            case 2: gui_launch_texteditor();  break;
            case 3: gui_launch_sysmonitor();  break;
            case 4: gui_launch_settings();    break;
            case 5: gui_launch_netconfig();   break;
            case 6: gui_launch_calculator();  break;
            case 7: gui_launch_imgviewer();   break;
            }
            return;
        }
    }
}

/* ---- Starfield (static background stars) ---- */
static struct { int16_t x, y; uint8_t brightness; uint8_t size; } g_stars[STAR_COUNT];
static bool g_stars_inited = false;

static uint32_t star_rand_seed = 42;
static uint32_t star_rand(void) {
    star_rand_seed = star_rand_seed * 1103515245 + 12345;
    return (star_rand_seed >> 16) & 0x7FFF;
}

static void init_starfield(int W, int H)
{
    for (int i = 0; i < STAR_COUNT; i++) {
        g_stars[i].x = (int16_t)(star_rand() % (uint32_t)W);
        g_stars[i].y = (int16_t)(star_rand() % (uint32_t)(H - TASKBAR_H));
        g_stars[i].brightness = (uint8_t)(30 + star_rand() % 80);
        g_stars[i].size = (star_rand() % 10 < 2) ? 2 : 1; /* 20% bigger stars */
    }
    g_stars_inited = true;
}

static void draw_starfield(canvas_t* scr)
{
    if (!g_stars_inited) init_starfield(scr->width, scr->height);

    for (int i = 0; i < STAR_COUNT; i++) {
        uint8_t b = g_stars[i].brightness;
        /* Twinkle */
        int twinkle = (int)((g_frame_count + (uint32_t)i * 37) % 80);
        if (twinkle < 20) b = (uint8_t)(b + 30 > 255 ? 255 : b + 30);
        else if (twinkle > 60) b = (uint8_t)(b > 25 ? b - 20 : 5);

        uint32_t col = rgba(b, (uint8_t)(b + 20 > 255 ? 255 : b + 20), 0xFF, b);
        int sx = g_stars[i].x, sy = g_stars[i].y;

        if (g_stars[i].size == 2) {
            /* Larger star: 3x3 cross pattern */
            draw_pixel(scr, sx, sy, col);
            uint32_t dim = rgba((uint8_t)(b/2), (uint8_t)(b/2 + 10), (uint8_t)(b/2 + 30), (uint8_t)(b/2));
            draw_pixel(scr, sx-1, sy, dim);
            draw_pixel(scr, sx+1, sy, dim);
            draw_pixel(scr, sx, sy-1, dim);
            draw_pixel(scr, sx, sy+1, dim);
        } else {
            draw_pixel(scr, sx, sy, col);
        }
    }
}

/* ---- Central radial glow on desktop ---- */
static void draw_desktop_glow(canvas_t* scr)
{
    int cx = scr->width / 2;
    int cy = (scr->height - TASKBAR_H) / 2;
    int radius = 200;

    /* Draw soft radial glow - only sample every 2 pixels for speed */
    for (int y = cy - radius; y < cy + radius; y += 2) {
        if (y < 0 || y >= scr->height - TASKBAR_H) continue;
        for (int x = cx - radius; x < cx + radius; x += 2) {
            if (x < 0 || x >= scr->width) continue;
            int dx = x - cx, dy = y - cy;
            int dist2 = dx*dx + dy*dy;
            if (dist2 >= radius * radius) continue;
            int dist = 0;
            /* Fast integer sqrt approximation */
            { int t = dist2, r = 0, b = 1 << 14;
              while (b > t) b >>= 2;
              while (b) { if (t >= r + b) { t -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
              dist = r; }
            uint8_t alpha = (uint8_t)(12 * (radius - dist) / radius);
            uint32_t gcol = rgba(0x20, 0x40, 0x80, alpha);
            /* Write 2x2 block */
            draw_pixel(scr, x, y, gcol);
            draw_pixel(scr, x+1, y, gcol);
            draw_pixel(scr, x, y+1, gcol);
            draw_pixel(scr, x+1, y+1, gcol);
        }
    }
}

/* ---- Desktop clock/date widget (top-right) ---- */
static void draw_desktop_widget(canvas_t* scr)
{
    rtc_time_t t;
    rtc_get_time(&t);

    int W = scr->width;
    int widget_w = 200;
    int widget_h = 90;
    int wx = W - widget_w - 24;
    int wy = 20;

    /* Glass background */
    draw_rect_alpha(scr, wx, wy, widget_w, widget_h, rgba(0x08, 0x12, 0x20, 0x70));
    draw_rect_rounded_outline(scr, wx, wy, widget_w, widget_h, 8, 1,
                               rgba(0x40, 0x60, 0x90, 0x50));
    /* Top highlight */
    draw_hline(scr, wx+8, wy+1, widget_w-16, rgba(0x60, 0x80, 0xB0, 0x20));

    /* Time: HH:MM:SS */
    char time_str[9];
    time_str[0] = '0' + (char)(t.hour / 10);
    time_str[1] = '0' + (char)(t.hour % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (char)(t.minute / 10);
    time_str[4] = '0' + (char)(t.minute % 10);
    time_str[5] = ':';
    time_str[6] = '0' + (char)(t.second / 10);
    time_str[7] = '0' + (char)(t.second % 10);
    time_str[8] = '\0';

    /* Bold time (draw at multiple offsets) */
    int tw = draw_string_width(time_str);
    int tx = wx + (widget_w - tw) / 2;
    int ty = wy + 10;
    uint32_t tc = rgba(0xD0, 0xE8, 0xFF, 0xF0);
    draw_string(scr, tx, ty,     time_str, tc, rgba(0,0,0,0));
    draw_string(scr, tx+1, ty,   time_str, tc, rgba(0,0,0,0));

    /* Date: YYYY.MM.DD  DayName */
    static const char* day_names[] = {"Sunday","Monday","Tuesday","Wednesday",
                                       "Thursday","Friday","Saturday"};
    char date_str[32];
    int di = 0;
    date_str[di++] = '0' + (char)(t.year / 1000);
    date_str[di++] = '0' + (char)((t.year / 100) % 10);
    date_str[di++] = '0' + (char)((t.year / 10) % 10);
    date_str[di++] = '0' + (char)(t.year % 10);
    date_str[di++] = '.';
    date_str[di++] = '0' + (char)(t.month / 10);
    date_str[di++] = '0' + (char)(t.month % 10);
    date_str[di++] = '.';
    date_str[di++] = '0' + (char)(t.day / 10);
    date_str[di++] = '0' + (char)(t.day % 10);
    date_str[di] = '\0';

    int dw = draw_string_width(date_str);
    draw_string(scr, wx + (widget_w - dw) / 2, wy + 34,
                date_str, rgba(0xA0, 0xC0, 0xE0, 0xC0), rgba(0,0,0,0));

    /* Day name */
    const char* dn = (t.weekday < 7) ? day_names[t.weekday] : "Unknown";
    int dnw = draw_string_width(dn);
    draw_string(scr, wx + (widget_w - dnw) / 2, wy + 54,
                dn, rgba(0x80, 0xA0, 0xC8, 0xA0), rgba(0,0,0,0));

    /* Seconds progress bar */
    int bar_x = wx + 16;
    int bar_w = widget_w - 32;
    int bar_y = wy + widget_h - 12;
    draw_rect_rounded(scr, bar_x, bar_y, bar_w, 4, 2, rgba(0x20, 0x30, 0x50, 0x60));
    int fill_w = (int)t.second * bar_w / 60;
    if (fill_w > 0)
        draw_rect_rounded(scr, bar_x, bar_y, fill_w, 4, 2, rgba(0x30, 0x70, 0xBB, 0xA0));
}

/* ---- Welcome / system info panel (center) ---- */
static void draw_welcome_panel(canvas_t* scr)
{
    /* Only show when no windows are open */
    if (taskbar_count > 0) return;

    int W = scr->width;
    int H = scr->height - TASKBAR_H;
    int pw = 360;
    int ph = 180;
    int px = (W - pw) / 2;
    int py = (H - ph) / 2 - 20;

    /* Glass panel */
    draw_rect_alpha(scr, px, py, pw, ph, rgba(0x0A, 0x14, 0x24, 0x50));
    draw_rect_rounded_outline(scr, px, py, pw, ph, 10, 1, rgba(0x30, 0x50, 0x80, 0x40));

    /* Title */
    const char* title = "Welcome to " OS_NAME;
    int ttw = draw_string_width(title);
    draw_string(scr, px + (pw - ttw) / 2, py + 20,
                title, rgba(0xD0, 0xE8, 0xFF, 0xE0), rgba(0,0,0,0));

    /* Separator */
    draw_hline(scr, px + 30, py + 44, pw - 60, rgba(0x30, 0x50, 0x80, 0x40));

    /* Info lines */
    const char* lines[] = {
        OS_BANNER_SHORT,
        "",
        "Click desktop icons to launch apps",
        "or use the Aether menu (bottom-left)",
        "",
        "Right-click to close menus",
    };
    int ly = py + 54;
    for (int i = 0; i < 6; i++) {
        if (lines[i][0] == '\0') { ly += 8; continue; }
        int lw = draw_string_width(lines[i]);
        uint32_t lc = (i == 0) ? rgba(0x60, 0xA0, 0xE0, 0xD0) : rgba(0x90, 0xB0, 0xD0, 0xA0);
        draw_string(scr, px + (pw - lw) / 2, ly, lines[i], lc, rgba(0,0,0,0));
        ly += FONT_H + 2;
    }
}

/* ---- Desktop background ---- */
static void draw_desktop_bg(void)
{
    /* Use the wallpaper engine (falls back to gradient if no image loaded) */
    wallpaper_draw(&g_screen);

    /* Subtle radial glow in center */
    draw_desktop_glow(&g_screen);

    /* Starfield overlay */
    draw_starfield(&g_screen);

    /* Subtle grid pattern */
    for (int gy = 0; gy < g_screen.height - TASKBAR_H; gy += 48)
        draw_hline(&g_screen, 0, gy, g_screen.width, rgba(0x20, 0x40, 0x60, 0x08));
    for (int gx = 0; gx < g_screen.width; gx += 48)
        draw_vline(&g_screen, gx, 0, g_screen.height - TASKBAR_H, rgba(0x20, 0x40, 0x60, 0x08));

    /* OS watermark bottom-right, semi-transparent */
    const char* wm_str = OS_BANNER_SHORT;
    int wlen = (int)strlen(wm_str) * FONT_W;
    draw_string(&g_screen,
                g_screen.width  - wlen - 8,
                g_screen.height - TASKBAR_H - FONT_H - 6,
                wm_str, rgba(0xA0, 0xC0, 0xFF, 0x28), rgba(0,0,0,0));
}

/* ---- Taskbar ---- */
void taskbar_draw(canvas_t* screen)
{
    const theme_t* th = theme_current();
    int wy = screen->height - TASKBAR_H;
    int W  = screen->width;

    /* Background gradient */
    draw_gradient_v(screen, 0, wy, W, TASKBAR_H,
                    th->taskbar_bg, th->taskbar_bg2);
    draw_hline(screen, 0, wy, W, th->taskbar_border);

    /* === Start (Aether) button === */
    bool start_hov = (mouse.y >= wy && mouse.x >= 4 && mouse.x < 4 + START_BTN_W);
    uint32_t sbg = start_hov ? th->btn_hover : th->accent;
    draw_rect_rounded(screen, 4, wy + 4, START_BTN_W, TASKBAR_BTN_H, 3, sbg);
    draw_string_centered(screen, 4, wy + 4, START_BTN_W, TASKBAR_BTN_H,
                         OS_SHORT_NAME, th->btn_text, rgba(0,0,0,0));

    /* === Virtual workspace indicator (real workspace manager) === */
    int wx = 4 + START_BTN_W + 6;
    workspace_draw_indicator(screen, wx, wy + 4, WORKSPACE_W, TASKBAR_BTN_H);

    /* === App buttons === */
    int bx = wx + WORKSPACE_W + 4;
    int avail_w = W - bx - TRAY_W - 4;
    int max_buttons = avail_w / (TASKBAR_BTN_W + 3);

    for (int i = 0; i < taskbar_count && i < max_buttons; i++) {
        window_t* win = wm_get_window(taskbar_entries[i].wid);
        bool focused = (taskbar_entries[i].wid == wm_focused_wid);
        bool minimized = win && (win->flags & WF_MINIMIZED);

        uint32_t btn_bg;
        if (focused)    btn_bg = th->btn_hover;
        else if (minimized) btn_bg = th->panel_border;
        else            btn_bg = th->btn_normal;

        /* Button hover */
        if (mouse.y >= wy + 4 &&
            mouse.x >= bx && mouse.x < bx + TASKBAR_BTN_W)
            btn_bg = th->btn_hover;

        draw_rect_rounded(screen, bx, wy + 4, TASKBAR_BTN_W, TASKBAR_BTN_H,
                          3, btn_bg);

        /* Active indicator dot */
        if (!minimized && focused)
            draw_circle_filled(screen, bx + TASKBAR_BTN_W - 8, wy + TASKBAR_H - 8,
                               3, th->ok);

        draw_string(screen, bx + 6, wy + 4 + (TASKBAR_BTN_H - FONT_H) / 2,
                    taskbar_entries[i].label, th->btn_text, rgba(0,0,0,0));
        bx += TASKBAR_BTN_W + 3;
    }

    /* === System tray area === */
    int tray_x = W - TRAY_W;

    /* Separator */
    draw_vline(screen, tray_x - 4, wy + 6, TASKBAR_H - 12, th->taskbar_border);

    /* Small icons: network, sound placeholders */
    draw_rect_rounded(screen, tray_x + 2, wy + 11, 14, 14, 2,
                      rgba(0x60, 0xB0, 0xFF, 0x50));
    draw_rect_rounded(screen, tray_x + 18, wy + 11, 14, 14, 2,
                      rgba(0x60, 0xFF, 0x80, 0x50));

    /* Clock - use real RTC time */
    rtc_time_t rtc_now;
    rtc_get_time(&rtc_now);
    char clock_str[12];
    clock_str[0] = '0' + (char)(rtc_now.hour/10);
    clock_str[1] = '0' + (char)(rtc_now.hour%10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (char)(rtc_now.minute/10);
    clock_str[4] = '0' + (char)(rtc_now.minute%10);
    clock_str[5] = ':';
    clock_str[6] = '0' + (char)(rtc_now.second/10);
    clock_str[7] = '0' + (char)(rtc_now.second%10);
    clock_str[8] = '\0';
    draw_string(screen, tray_x + 36, wy + (TASKBAR_H - FONT_H) / 2,
                clock_str, th->taskbar_clock, rgba(0,0,0,0));
}

/* ---- Start menu ---- */
typedef struct {
    const char* name;
    const char* icon;
    void (*launch)(void);
} menu_item_t;

static void _launch_terminal(void)    { gui_launch_terminal();    startmenu_open = false; }
static void _launch_files(void)       { gui_launch_filemanager(); startmenu_open = false; }
static void _launch_editor(void)      { gui_launch_texteditor();  startmenu_open = false; }
static void _launch_monitor(void)     { gui_launch_sysmonitor();  startmenu_open = false; }
static void _launch_settings(void)    { gui_launch_settings();    startmenu_open = false; }
static void _launch_calculator(void)  { gui_launch_calculator();  startmenu_open = false; }
static void _launch_clock(void)       { gui_launch_clock();       startmenu_open = false; }
static void _launch_stress(void)      { gui_launch_stress_test(); startmenu_open = false; }
static void _launch_imgviewer(void)   { gui_launch_imgviewer();   startmenu_open = false; }
static void _launch_netconfig(void)   { gui_launch_netconfig();   startmenu_open = false; }
static void _do_shutdown(void)        { cpu_cli(); cpu_halt(); }

static const menu_item_t g_menu_items[] = {
    { "Terminal",       "  >_ ", _launch_terminal    },
    { "File Manager",   "  [] ", _launch_files       },
    { "Text Editor",    "  == ", _launch_editor      },
    { "Image Viewer",   "  [] ", _launch_imgviewer   },
    { "System Monitor", "  ## ", _launch_monitor     },
    { "Calculator",     "  +- ", _launch_calculator  },
    { "Clock",          "  O  ", _launch_clock       },
    { "Network",        "  ~~ ", _launch_netconfig   },
    { "Settings",       "  @@ ", _launch_settings    },
    { "Stress Test",    "  .. ", _launch_stress      },
    { NULL, NULL, NULL },  /* Separator */
    { "Shutdown",       "  X  ", _do_shutdown        },
};
#define MENU_ITEM_COUNT 12

static void draw_start_menu(canvas_t* screen)
{
    if (!startmenu_open) return;

    const theme_t* th = theme_current();
    int screen_h = screen->height;
    int menu_h   = MENU_ITEM_COUNT * MENU_ITEM_H + 8;
    int menu_y   = screen_h - TASKBAR_H - menu_h;
    int menu_x   = 4;

    /* Shadow */
    draw_rect(screen, menu_x + 3, menu_y + 3, MENU_W, menu_h,
              rgba(0,0,0,0x40));
    /* Background */
    draw_rect(screen, menu_x, menu_y, MENU_W, menu_h, th->taskbar_bg);
    draw_rect_outline(screen, menu_x, menu_y, MENU_W, menu_h, 1, th->accent);

    /* Header */
    draw_rect(screen, menu_x, menu_y, MENU_W, 28, th->win_title_bg);
    draw_string(screen, menu_x + 8, menu_y + (28 - FONT_H) / 2,
                OS_NAME, th->win_title_text, rgba(0,0,0,0));
    draw_hline(screen, menu_x, menu_y + 28, MENU_W, th->accent);

    int iy = menu_y + 32;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        const menu_item_t* m = &g_menu_items[i];

        if (!m->name) {
            /* Separator */
            draw_hline(screen, menu_x + 8, iy + 4, MENU_W - 16,
                       th->panel_border);
            iy += MENU_ITEM_H / 2;
            continue;
        }

        /* Hover highlight */
        bool hov = (mouse.y >= iy && mouse.y < iy + MENU_ITEM_H &&
                    mouse.x >= menu_x && mouse.x < menu_x + MENU_W);
        if (hov) {
            draw_rect(screen, menu_x + 2, iy, MENU_W - 4, MENU_ITEM_H,
                      th->selection);
        }

        /* Icon placeholder (colored square) */
        draw_rect_rounded(screen, menu_x + 6, iy + 5,
                          MENU_ICON_W - 4, MENU_ITEM_H - 10, 2,
                          hov ? th->accent : th->accent2);

        /* Label */
        uint32_t fg = hov ? th->selection_text : th->taskbar_text;
        draw_string(screen, menu_x + MENU_ICON_W + 8,
                    iy + (MENU_ITEM_H - FONT_H) / 2,
                    m->name, fg, rgba(0,0,0,0));

        iy += MENU_ITEM_H;
    }
}

static void check_start_menu_click(int mx, int my)
{
    if (!startmenu_open) return;
    int screen_h = (int)fb.height;
    int menu_h   = MENU_ITEM_COUNT * MENU_ITEM_H + 8;
    int menu_y   = screen_h - TASKBAR_H - menu_h;

    if (mx < 4 || mx > 4 + MENU_W || my < menu_y || my > menu_y + menu_h) {
        startmenu_open = false;
        return;
    }

    int iy = menu_y + 32;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        const menu_item_t* m = &g_menu_items[i];
        if (!m->name) { iy += MENU_ITEM_H / 2; continue; }
        if (my >= iy && my < iy + MENU_ITEM_H && m->launch) {
            m->launch();
            return;
        }
        iy += MENU_ITEM_H;
    }
}

/* ---- Taskbar mouse handling ---- */
void taskbar_handle_mouse(int mx, int my, bool clicked)
{
    int screen_h = (int)fb.height;
    int taskbar_y = screen_h - TASKBAR_H;
    if (my < taskbar_y || !clicked) return;

    /* Start button */
    if (mx >= 4 && mx < 4 + START_BTN_W) {
        startmenu_open = !startmenu_open;
        return;
    }

    /* Workspace switcher */
    int wx = 4 + START_BTN_W + 6;
    if (mx >= wx && mx < wx + WORKSPACE_W) {
        /* Divide the WORKSPACE_W region into 4 equal slots */
        int slot = (mx - wx) * 4 / WORKSPACE_W;
        if (slot >= 0 && slot < 4) {
            workspace_switch(slot);
            g_workspace = workspace_current();
        }
        return;
    }

    /* App buttons */
    int bx = wx + WORKSPACE_W + 4;
    for (int i = 0; i < taskbar_count; i++) {
        if (mx >= bx && mx < bx + TASKBAR_BTN_W) {
            window_t* win = wm_get_window(taskbar_entries[i].wid);
            if (!win) { bx += TASKBAR_BTN_W + 3; continue; }
            if (win->flags & WF_MINIMIZED) {
                win->flags &= ~WF_MINIMIZED;
                win->flags |=  WF_VISIBLE;
                wm_focus(win->id); wm_raise(win->id);
            } else if (taskbar_entries[i].wid == wm_focused_wid) {
                win->flags |=  WF_MINIMIZED;
                win->flags &= ~WF_VISIBLE;
            } else {
                wm_focus(win->id); wm_raise(win->id);
            }
            return;
        }
        bx += TASKBAR_BTN_W + 3;
    }
}

/* ---- Desktop tick (called every frame) ---- */
void desktop_tick(void)
{
    draw_desktop_bg();
    draw_desktop_icons(&g_screen);
    draw_welcome_panel(&g_screen);
    draw_desktop_widget(&g_screen);
    taskbar_draw(&g_screen);
    draw_start_menu(&g_screen);
    notify_tick(&g_screen);
    anim_tick();
}

void desktop_init(void)
{
    notify_init();
    workspace_init();
}

/* ---- GUI subsystem ---- */
void gui_init(void)
{
    gui_event_queue_init();
    wm_init();
    theme_init();
    gui_running = false;
    kinfo("GUI: initialized (theme: %s)", theme_current()->name);
}

bool gui_available(void)
{
    return fb_ready();
}

/* ---- GUI main loop ---- */
void gui_run(void)
{
    if (!fb_ready()) {
        klog_warn("GUI: framebuffer not ready");
        return;
    }

    gui_running = true;
    g_screen = draw_main_canvas();
    mouse_set_bounds(g_screen.width - 1, g_screen.height - 1);
    kinfo("GUI: main loop started (%dx%d)", g_screen.width, g_screen.height);

    /* Phase A: Boot splash */
    splash_run();

    /* Phase B: Login screen */
    login_run();
    kinfo("GUI: user '%s' logged in", login_username);

    /* Phase C: Desktop init & initial apps */
    desktop_init();
    notify_post(NOTIFY_INFO, OS_BOOT_WELCOME,
                "Desktop environment loaded.");

    /* Desktop is clean - user clicks icons or start menu to launch apps */

    uint32_t last_ticks = 0;
    uint8_t  prev_buttons = 0;

    while (gui_running) {
        uint32_t now = timer_get_ticks();
        /* Throttle to ~60 FPS (16ms @ 100Hz ≈ every 2 ticks) */
        if (now - last_ticks < 2) {
            scheduler_yield();
            continue;
        }
        last_ticks = now;
        g_frame_count++;

        /* Remove closed windows from taskbar */
        taskbar_remove_closed();

        /* Per-app periodic ticks */
        clock_tick();
        stress_tick();
        imgviewer_tick();
        netconfig_tick();

        /* Desktop background + taskbar + notifications */
        desktop_tick();

        /* Window events */
        wm_dispatch_events();

        /* Taskbar + start menu mouse */
        uint8_t cur_buttons = mouse.buttons;
        bool just_clicked   = (cur_buttons & ~prev_buttons) & MOUSE_BTN_LEFT;
        if (just_clicked) {
            taskbar_handle_mouse(mouse.x, mouse.y, true);
            check_start_menu_click(mouse.x, mouse.y);
            if (!startmenu_open)
                desktop_icon_click(mouse.x, mouse.y);
        }
        /* Close menu on right-click */
        if ((cur_buttons & ~prev_buttons) & MOUSE_BTN_RIGHT) {
            startmenu_open = false;
        }
        prev_buttons = cur_buttons;

        /* Composite windows */
        wm_composite(&g_screen);

        /* Software cursor: erase old position, composite new position */
        cursor_erase();
        cursor_render();

        /* Flip to display */
        fb_flip();

        scheduler_yield();
    }
}

/* ---- App launchers ---- */
extern wid_t app_terminal_create(void);
extern wid_t app_filemanager_create(void);
extern wid_t app_texteditor_create(void);
extern wid_t app_sysmonitor_create(void);
extern wid_t app_settings_create(void);
extern wid_t app_imgviewer_create(void);
extern wid_t app_netconfig_create(void);

#define LAUNCH(create_fn, label) do { \
    wid_t _w = (create_fn)();         \
    if (_w >= 0) { taskbar_add(_w, (label)); workspace_add_window(_w); } \
} while(0)

void gui_launch_terminal(void)    { LAUNCH(app_terminal_create,    "Terminal"); }
void gui_launch_filemanager(void) { LAUNCH(app_filemanager_create, "Files");    }
void gui_launch_texteditor(void)  { LAUNCH(app_texteditor_create,  "Editor");   }
void gui_launch_sysmonitor(void)  { LAUNCH(app_sysmonitor_create,  "Monitor");  }
void gui_launch_settings(void)    { LAUNCH(app_settings_create,    "Settings"); }
void gui_launch_imgviewer(void)   { LAUNCH(app_imgviewer_create,   "Images");   }
void gui_launch_netconfig(void)   { LAUNCH(app_netconfig_create,   "Network");  }
