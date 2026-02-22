/*
 * services/launcher.c — Aether OS Application Launcher
 *
 * The launcher is the "shell" of Aether OS.  It is the first user-facing
 * service started at boot.  It provides:
 *
 *   1. Taskbar — bottom dock with app buttons, system clock, status
 *   2. App grid — full-screen launcher (opens on Start button)
 *   3. Permission prompts — capability request dialogs
 *   4. System notifications
 *
 * The launcher creates its surfaces via the compositor's IPC API and
 * receives input events via the input service's IPC API.
 *
 * For v0.1 the launcher runs on top of the existing window-manager
 * surfaces while also registering as "aether.launcher" in the service bus.
 */
#include <services/launcher.h>
#include <kernel/version.h>
#include <kernel/ipc.h>
#include <kernel/svcbus.h>
#include <kernel/cap.h>
#include <display/compositor.h>
#include <input/input_svc.h>
#include <gui/draw.h>
#include <gui/window.h>
#include <drivers/timer.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* Existing GUI apps */
extern wid_t app_terminal_create(void);
extern wid_t app_filemanager_create(void);
extern wid_t app_texteditor_create(void);
extern wid_t app_sysmonitor_create(void);

/* =========================================================
 * Layout constants
 * ========================================================= */

#define TASKBAR_H       36
#define TASKBAR_BTN_W   120
#define DOCK_PAD        4

#define GRID_COLS       4
#define GRID_ROWS       3
#define GRID_ICON_W     100
#define GRID_ICON_H     90
#define GRID_PAD        20

/* =========================================================
 * Launcher state
 * ========================================================= */

#define LAUNCHER_MAX_APPS  16
#define LAUNCHER_MAX_WINS  16

typedef struct {
    char    name[32];
    char    icon[4];  /* 2-char emoji-style label */
    uint32_t bg_color;
    void    (*launch)(void);
} app_def_t;

typedef struct {
    wid_t   wid;
    char    label[32];
    bool    active;
} win_entry_t;

typedef struct {
    port_id_t   service_port;
    port_id_t   notify_port;    /* For receiving events back */

    /* App grid */
    bool        grid_open;

    /* Window tracking */
    win_entry_t wins[LAUNCHER_MAX_WINS];
    int         win_count;

    /* Taskbar */
    canvas_t    screen;

    bool        initialized;
    bool        running;
} launcher_t;

static launcher_t g_launcher;

/* =========================================================
 * App definitions
 * ========================================================= */

static void launch_terminal(void)    { wid_t w = app_terminal_create();   if (w >= 0) launcher_track_window(w, "Terminal"); }
static void launch_files(void)       { wid_t w = app_filemanager_create(); if (w >= 0) launcher_track_window(w, "Files"); }
static void launch_editor(void)      { wid_t w = app_texteditor_create();  if (w >= 0) launcher_track_window(w, "Editor"); }
static void launch_monitor(void)     { wid_t w = app_sysmonitor_create();  if (w >= 0) launcher_track_window(w, "Monitor"); }

static const app_def_t g_apps[] = {
    { "Terminal",     ">_", rgb(0x14, 0x28, 0x14), launch_terminal },
    { "Files",        "[]", rgb(0x14, 0x28, 0x50), launch_files    },
    { "Text Editor",  "Ed", rgb(0x28, 0x14, 0x50), launch_editor   },
    { "Sys Monitor",  "Mn", rgb(0x50, 0x14, 0x14), launch_monitor  },
    { "Settings",     "St", rgb(0x28, 0x28, 0x28), NULL             },
    { "Network",      "Ne", rgb(0x14, 0x50, 0x50), NULL             },
};
#define N_APPS  ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

/* =========================================================
 * Window tracking
 * ========================================================= */

void launcher_track_window(wid_t wid, const char* label)
{
    for (int i = 0; i < LAUNCHER_MAX_WINS; i++) {
        if (!g_launcher.wins[i].active) {
            g_launcher.wins[i].wid    = wid;
            g_launcher.wins[i].active = true;
            strncpy(g_launcher.wins[i].label, label, 31);
            g_launcher.wins[i].label[31] = '\0';
            g_launcher.win_count++;
            return;
        }
    }
}

static void _untrack_closed_windows(void)
{
    for (int i = 0; i < LAUNCHER_MAX_WINS; i++) {
        if (!g_launcher.wins[i].active) continue;
        window_t* w = wm_get_window(g_launcher.wins[i].wid);
        if (!w) {
            g_launcher.wins[i].active = false;
            g_launcher.win_count--;
        }
    }
}

/* =========================================================
 * Taskbar rendering
 * ========================================================= */

static void _draw_taskbar(canvas_t* screen)
{
    int sw = screen->width;
    int sh = screen->height;
    int ty = sh - TASKBAR_H;

    /* Background */
    draw_gradient_v(screen, 0, ty, sw, TASKBAR_H,
                    rgb(0x12, 0x1E, 0x32),
                    rgb(0x08, 0x10, 0x1A));
    draw_hline(screen, 0, ty, sw, rgb(0x30, 0x60, 0xA0));

    /* Start / launcher button */
    bool hovered = false; /* TODO: track hover */
    uint32_t start_col = hovered ? COLOR_BTN_HOVER : COLOR_ACCENT;
    draw_rect_rounded(screen, DOCK_PAD, ty + 5, 72, TASKBAR_H - 10, 6, start_col);
    draw_string_centered(screen, DOCK_PAD, ty + 5, 72, TASKBAR_H - 10,
                         OS_SHORT_NAME, COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Window buttons */
    int bx = 84;
    for (int i = 0; i < LAUNCHER_MAX_WINS; i++) {
        if (!g_launcher.wins[i].active) continue;
        window_t* win = wm_get_window(g_launcher.wins[i].wid);
        bool focused = win && (g_launcher.wins[i].wid == wm_focused_wid);
        uint32_t bc  = focused ? COLOR_BTN_HOVER :
                       (win && (win->flags & WF_MINIMIZED)) ? COLOR_MID_GREY
                                                            : COLOR_BTN_NORMAL;
        draw_rect_rounded(screen, bx, ty + 5, TASKBAR_BTN_W, TASKBAR_H - 10, 4, bc);
        draw_string(screen, bx + 8, ty + 11,
                    g_launcher.wins[i].label, COLOR_TEXT_LIGHT, rgba(0,0,0,0));
        bx += TASKBAR_BTN_W + DOCK_PAD;
    }

    /* Clock */
    uint32_t ticks = timer_get_ticks();
    uint32_t secs  = ticks / TIMER_FREQ;
    uint32_t mins  = (secs / 60) % 60;
    uint32_t hours = (secs / 3600) % 24;
    char clk[10];
    clk[0] = '0' + (char)(hours / 10); clk[1] = '0' + (char)(hours % 10);
    clk[2] = ':';
    clk[3] = '0' + (char)(mins  / 10); clk[4] = '0' + (char)(mins  % 10);
    clk[5] = ':';
    uint32_t s2 = secs % 60;
    clk[6] = '0' + (char)(s2 / 10);   clk[7] = '0' + (char)(s2 % 10);
    clk[8] = '\0';

    /* System info area */
    int cx = sw - 9 * FONT_W - 8;
    draw_string(screen, cx, ty + 11, clk, COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Service count indicator */
    uint32_t nsvc = svcbus_count();
    char svc_s[16] = "svc:";
    char nb[8];
    int ni = 0;
    if (nsvc == 0) { nb[0]='0'; ni=1; }
    else { while (nsvc > 0) { nb[ni++] = '0' + (int)(nsvc % 10); nsvc /= 10; } }
    for (int a=0,b=ni-1;a<b;a++,b--){char t=nb[a];nb[a]=nb[b];nb[b]=t;}
    nb[ni]='\0';
    strncpy(svc_s + 4, nb, 8);
    draw_string(screen, cx - 7 * FONT_W - 8, ty + 11,
                svc_s, COLOR_MID_GREY, rgba(0,0,0,0));
}

/* =========================================================
 * App grid rendering
 * ========================================================= */

static void _draw_app_grid(canvas_t* screen)
{
    if (!g_launcher.grid_open) return;

    int sw = screen->width;
    int sh = screen->height;

    /* Dim overlay */
    draw_rect(screen, 0, 0, sw, sh - TASKBAR_H, rgba(0,0,0,0xCC));

    /* Grid title */
    draw_string_centered(screen, 0, 20, sw, FONT_H + 8,
                         OS_NAME " — Applications",
                         COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Grid of app tiles */
    int total_w = GRID_COLS * (GRID_ICON_W + GRID_PAD) - GRID_PAD;
    int start_x = (sw - total_w) / 2;
    int start_y = 60;

    for (int i = 0; i < N_APPS; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int ix  = start_x + col * (GRID_ICON_W + GRID_PAD);
        int iy  = start_y + row * (GRID_ICON_H + GRID_PAD);

        /* Tile background */
        draw_rect_rounded(screen, ix, iy, GRID_ICON_W, GRID_ICON_H,
                          8, g_apps[i].bg_color);
        draw_rect_outline(screen, ix, iy, GRID_ICON_W, GRID_ICON_H,
                          1, rgba(0xFF,0xFF,0xFF,0x30));

        /* Icon label (large centred text) */
        draw_string_centered(screen, ix, iy, GRID_ICON_W, GRID_ICON_H - FONT_H - 8,
                             g_apps[i].icon, COLOR_TEXT_LIGHT, rgba(0,0,0,0));

        /* App name */
        draw_string_centered(screen, ix, iy + GRID_ICON_H - FONT_H - 4,
                             GRID_ICON_W, FONT_H + 4,
                             g_apps[i].name, COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    }

    /* Close hint */
    draw_string_centered(screen, 0, sh - TASKBAR_H - FONT_H - 8,
                         sw, FONT_H, "Click anywhere to close",
                         COLOR_MID_GREY, rgba(0,0,0,0));
}

/* =========================================================
 * Mouse handler
 * ========================================================= */

static uint8_t prev_buttons = 0;

static void _handle_mouse_taskbar(int mx, int my, uint8_t buttons)
{
    int sw = g_launcher.screen.width;
    int sh = g_launcher.screen.height;
    int ty = sh - TASKBAR_H;

    bool clicked = (buttons & MOUSE_BTN_LEFT) && !(prev_buttons & MOUSE_BTN_LEFT);

    if (g_launcher.grid_open) {
        if (clicked) {
            /* Click on app tile? */
            int total_w = GRID_COLS * (GRID_ICON_W + GRID_PAD) - GRID_PAD;
            int start_x = (sw - total_w) / 2;
            int start_y = 60;
            for (int i = 0; i < N_APPS; i++) {
                int col = i % GRID_COLS;
                int row = i / GRID_COLS;
                int ix  = start_x + col * (GRID_ICON_W + GRID_PAD);
                int iy  = start_y + row * (GRID_ICON_H + GRID_PAD);
                if (mx >= ix && mx < ix + GRID_ICON_W &&
                    my >= iy && my < iy + GRID_ICON_H) {
                    g_launcher.grid_open = false;
                    if (g_apps[i].launch) g_apps[i].launch();
                    return;
                }
            }
            /* Click elsewhere closes grid */
            if (my < ty) g_launcher.grid_open = false;
        }
        return;
    }

    if (!clicked || my < ty) return;

    /* Start button */
    if (mx >= DOCK_PAD && mx < DOCK_PAD + 72) {
        g_launcher.grid_open = !g_launcher.grid_open;
        return;
    }

    /* Window buttons */
    int bx = 84;
    for (int i = 0; i < LAUNCHER_MAX_WINS; i++) {
        if (!g_launcher.wins[i].active) { continue; }
        if (mx >= bx && mx < bx + TASKBAR_BTN_W) {
            window_t* win = wm_get_window(g_launcher.wins[i].wid);
            if (!win) { g_launcher.wins[i].active = false; break; }
            if (win->flags & WF_MINIMIZED) {
                win->flags &= ~WF_MINIMIZED;
                win->flags |=  WF_VISIBLE;
                wm_focus(win->id);
                wm_raise(win->id);
            } else if (g_launcher.wins[i].wid == wm_focused_wid) {
                win->flags |= WF_MINIMIZED;
                win->flags &= ~WF_VISIBLE;
            } else {
                wm_focus(win->id);
                wm_raise(win->id);
            }
            return;
        }
        bx += TASKBAR_BTN_W + DOCK_PAD;
    }
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void launcher_init(void)
{
    memset(&g_launcher, 0, sizeof(g_launcher));
    g_launcher.grid_open = false;

    if (!fb_ready()) {
        klog_warn("LAUNCHER: framebuffer not ready");
        return;
    }

    g_launcher.screen = draw_main_canvas();

    g_launcher.service_port = ipc_port_create(0);
    if (g_launcher.service_port != PORT_INVALID)
        svcbus_register(SVC_LAUNCHER, g_launcher.service_port, 0, 1);

    g_launcher.initialized = true;
    kinfo("LAUNCHER: initialized");
}

/* =========================================================
 * launcher_run — main loop (kernel thread)
 * ========================================================= */

void launcher_run(void)
{
    if (!g_launcher.initialized) {
        klog_warn("LAUNCHER: not initialized — thread exiting");
        return;
    }

    g_launcher.running = true;
    kinfo("LAUNCHER: thread running");

    /* Open terminal at startup */
    launch_terminal();

    uint32_t last_frame = 0;

    for (;;) {
        uint32_t now = timer_get_ticks();
        if (now - last_frame >= 2) {
            last_frame = now;

            /* Clean up any windows that were closed */
            _untrack_closed_windows();

            /* Get screen canvas (re-read each frame in case of resize) */
            g_launcher.screen = draw_main_canvas();

            /* Draw taskbar over whatever the compositor drew */
            _draw_taskbar(&g_launcher.screen);

            /* Draw app grid if open */
            if (g_launcher.grid_open)
                _draw_app_grid(&g_launcher.screen);
        }

        /* Handle mouse */
        uint8_t cur = mouse.buttons;
        _handle_mouse_taskbar(mouse.x, mouse.y, cur);
        prev_buttons = cur;

        scheduler_yield();
    }
}
