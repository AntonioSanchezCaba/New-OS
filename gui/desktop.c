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
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <gui/notify.h>
#include <services/splash.h>
#include <services/login.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/timer.h>
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

/* ---- Desktop state ---- */
static bool      gui_running       = false;
static canvas_t  g_screen;
static bool      startmenu_open    = false;
static int       g_workspace       = 0;    /* current virtual workspace 0-3 */
static uint32_t  g_frame_count     = 0;
static bool      g_first_frame     = true;

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

/* ---- Desktop background ---- */
static void draw_desktop_bg(void)
{
    const theme_t* th = theme_current();
    draw_gradient_v(&g_screen, 0, 0,
                    g_screen.width, g_screen.height - TASKBAR_H,
                    th->desktop_bg, th->desktop_bg2);

    /* Subtle grid */
    uint32_t grid = th->desktop_grid;
    for (int x = 0; x < g_screen.width; x += 48)
        draw_vline(&g_screen, x, 0, g_screen.height - TASKBAR_H, grid);
    for (int y = 0; y < g_screen.height - TASKBAR_H; y += 48)
        draw_hline(&g_screen, 0, y, g_screen.width, grid);

    /* OS watermark bottom-right */
    const char* wm_str = "Aether OS v0.1";
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
                         "Aether", th->btn_text, rgba(0,0,0,0));

    /* === Virtual workspace indicator === */
    int wx = 4 + START_BTN_W + 6;
    for (int i = 0; i < 4; i++) {
        bool active = (i == g_workspace);
        uint32_t wbg = active ? th->accent2 : rgba(0xFF,0xFF,0xFF, 0x18);
        draw_rect_rounded(screen, wx + i * 21, wy + 10, 18, TASKBAR_BTN_H - 10,
                          2, wbg);
        if (active) {
            draw_rect_outline(screen, wx + i * 21, wy + 10, 18, TASKBAR_BTN_H - 10,
                              1, th->accent);
        }
        char ws[2] = { '0' + (char)i, '\0' };
        draw_string_centered(screen, wx + i * 21, wy + 10, 18, TASKBAR_BTN_H - 10,
                             ws, th->taskbar_text, rgba(0,0,0,0));
    }

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

    /* Clock */
    uint32_t ticks = timer_get_ticks();
    uint32_t secs  = ticks / TIMER_FREQ;
    uint32_t m2    = (secs / 60) % 60;
    uint32_t h2    = (secs / 3600) % 24;
    uint32_t s2    =  secs % 60;
    char clock_str[12];
    clock_str[0] = '0' + (char)(h2/10); clock_str[1] = '0' + (char)(h2%10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (char)(m2/10); clock_str[4] = '0' + (char)(m2%10);
    clock_str[5] = ':';
    clock_str[6] = '0' + (char)(s2/10); clock_str[7] = '0' + (char)(s2%10);
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
static void _do_shutdown(void)        { cpu_cli(); cpu_halt(); }

static const menu_item_t g_menu_items[] = {
    { "Terminal",       "  >_ ", _launch_terminal    },
    { "File Manager",   "  [] ", _launch_files       },
    { "Text Editor",    "  == ", _launch_editor      },
    { "System Monitor", "  ## ", _launch_monitor     },
    { "Calculator",     "  +- ", _launch_calculator  },
    { "Clock",          "  O  ", _launch_clock       },
    { "Settings",       "  @@ ", _launch_settings    },
    { "Stress Test",    "  ~~ ", _launch_stress      },
    { NULL, NULL, NULL },  /* Separator */
    { "Shutdown",       "  X  ", _do_shutdown        },
};
#define MENU_ITEM_COUNT 10

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
                "Aether OS", th->win_title_text, rgba(0,0,0,0));
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
    for (int i = 0; i < 4; i++) {
        int wwx = wx + i * 21;
        if (mx >= wwx && mx < wwx + 18) {
            g_workspace = i;
            return;
        }
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
    taskbar_draw(&g_screen);
    draw_start_menu(&g_screen);
    notify_tick(&g_screen);
}

void desktop_init(void)
{
    notify_init();
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
    notify_post(NOTIFY_INFO, "Welcome to Aether OS",
                "Desktop environment loaded.");

    /* Launch initial apps */
    gui_launch_terminal();

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
        }
        /* Close menu on right-click */
        if ((cur_buttons & ~prev_buttons) & MOUSE_BTN_RIGHT) {
            startmenu_open = false;
        }
        prev_buttons = cur_buttons;

        /* Composite windows */
        wm_composite(&g_screen);

        /* Mouse cursor */
        mouse_draw_cursor(fb.back_buf, (int)fb.width, (int)fb.height);

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

/* New apps — these manage their own static state */
/* gui_launch_calculator, gui_launch_clock, gui_launch_stress_test
 * are defined in their respective .c files and declared in gui.h */

void gui_launch_terminal(void)
{
    wid_t wid = app_terminal_create();
    if (wid >= 0) taskbar_add(wid, "Terminal");
}

void gui_launch_filemanager(void)
{
    wid_t wid = app_filemanager_create();
    if (wid >= 0) taskbar_add(wid, "Files");
}

void gui_launch_texteditor(void)
{
    wid_t wid = app_texteditor_create();
    if (wid >= 0) taskbar_add(wid, "Editor");
}

void gui_launch_sysmonitor(void)
{
    wid_t wid = app_sysmonitor_create();
    if (wid >= 0) taskbar_add(wid, "Monitor");
}

void gui_launch_settings(void)
{
    wid_t wid = app_settings_create();
    if (wid >= 0) taskbar_add(wid, "Settings");
}
