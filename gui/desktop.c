/*
 * gui/desktop.c - Desktop environment, taskbar, and GUI main loop
 */
#include <gui/gui.h>
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <process.h>
#include <scheduler.h>

#define TASKBAR_H      36
#define TASKBAR_BTN_W  120
#define STARTMENU_W    160
#define STARTMENU_ITEM_H 28

static bool gui_running = false;
static canvas_t g_screen;

/* =========================================================
 * Desktop background
 * ========================================================= */

static void draw_desktop_bg(void)
{
    draw_gradient_v(&g_screen, 0, 0, g_screen.width, g_screen.height - TASKBAR_H,
                    COLOR_DESKTOP_BG, rgb(0x10, 0x18, 0x30));
    uint32_t grid_col = rgba(0xFF, 0xFF, 0xFF, 0x10);
    for (int x = 0; x < g_screen.width; x += 40)
        draw_vline(&g_screen, x, 0, g_screen.height - TASKBAR_H, grid_col);
    for (int y = 0; y < g_screen.height - TASKBAR_H; y += 40)
        draw_hline(&g_screen, 0, y, g_screen.width, grid_col);
    draw_string(&g_screen,
                g_screen.width - 8 * 8 - 8,
                g_screen.height - TASKBAR_H - FONT_H - 4,
                "NovOS  ", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
}

/* =========================================================
 * Taskbar
 * ========================================================= */

typedef struct { wid_t wid; char label[32]; } taskbar_entry_t;
#define TASKBAR_MAX_ENTRIES 16
static taskbar_entry_t taskbar_entries[TASKBAR_MAX_ENTRIES];
static int taskbar_entry_count = 0;
static bool startmenu_open = false;

static void taskbar_add(wid_t wid, const char* label)
{
    if (taskbar_entry_count >= TASKBAR_MAX_ENTRIES) return;
    taskbar_entries[taskbar_entry_count].wid = wid;
    strncpy(taskbar_entries[taskbar_entry_count].label, label, 31);
    taskbar_entries[taskbar_entry_count].label[31] = '\0';
    taskbar_entry_count++;
}

void taskbar_draw(canvas_t* screen)
{
    int y = screen->height - TASKBAR_H;
    int w = screen->width;

    draw_gradient_v(screen, 0, y, w, TASKBAR_H,
                    COLOR_TASKBAR_BG, rgb(0x08, 0x10, 0x20));
    draw_hline(screen, 0, y, w, rgb(0x40, 0x80, 0xC0));

    /* Start button */
    draw_rect_rounded(screen, 4, y + 4, 70, TASKBAR_H - 8, 4, COLOR_ACCENT);
    draw_string_centered(screen, 4, y + 4, 70, TASKBAR_H - 8,
                         "Start", COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* App buttons */
    int bx = 82;
    for (int i = 0; i < taskbar_entry_count; i++) {
        window_t* win = wm_get_window(taskbar_entries[i].wid);
        bool focused = (taskbar_entries[i].wid == wm_focused_wid);
        uint32_t btn_col = focused ? COLOR_BTN_HOVER : COLOR_BTN_NORMAL;
        if (win && (win->flags & WF_MINIMIZED)) btn_col = COLOR_MID_GREY;
        draw_rect_rounded(screen, bx, y + 4, TASKBAR_BTN_W, TASKBAR_H - 8, 3, btn_col);
        draw_string(screen, bx + 6, y + 10,
                    taskbar_entries[i].label, COLOR_TEXT_LIGHT, rgba(0,0,0,0));
        bx += TASKBAR_BTN_W + 4;
    }

    /* Clock */
    uint32_t ticks = timer_get_ticks();
    uint32_t secs  = ticks / TIMER_FREQ;
    uint32_t mins  = (secs / 60) % 60;
    uint32_t hours = (secs / 3600) % 24;
    char clock_str[16];
    clock_str[0] = '0' + (char)(hours / 10);
    clock_str[1] = '0' + (char)(hours % 10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (char)(mins / 10);
    clock_str[4] = '0' + (char)(mins % 10);
    clock_str[5] = ':';
    uint32_t s2 = secs % 60;
    clock_str[6] = '0' + (char)(s2 / 10);
    clock_str[7] = '0' + (char)(s2 % 10);
    clock_str[8] = '\0';
    draw_string(screen, w - 8 * 9, y + 10, clock_str, COLOR_TEXT_LIGHT, rgba(0,0,0,0));
}

void taskbar_handle_mouse(int mx, int my, bool clicked)
{
    int screen_h = (int)fb.height;
    int taskbar_y = screen_h - TASKBAR_H;

    if (my < taskbar_y || !clicked) return;

    if (mx >= 4 && mx < 74) { startmenu_open = !startmenu_open; return; }

    int bx = 82;
    for (int i = 0; i < taskbar_entry_count; i++) {
        if (mx >= bx && mx < bx + TASKBAR_BTN_W) {
            window_t* win = wm_get_window(taskbar_entries[i].wid);
            if (!win) { bx += TASKBAR_BTN_W + 4; continue; }
            if (win->flags & WF_MINIMIZED) {
                win->flags &= ~WF_MINIMIZED;
                win->flags |= WF_VISIBLE;
                wm_focus(win->id); wm_raise(win->id);
            } else if (taskbar_entries[i].wid == wm_focused_wid) {
                win->flags |= WF_MINIMIZED;
                win->flags &= ~WF_VISIBLE;
            } else {
                wm_focus(win->id); wm_raise(win->id);
            }
            return;
        }
        bx += TASKBAR_BTN_W + 4;
    }
}

static const char* start_menu_items[] = {
    "Terminal", "File Manager", "Text Editor", "Sys Monitor", "Shutdown"
};
#define STARTMENU_ITEM_COUNT 5

static void draw_start_menu(canvas_t* screen)
{
    if (!startmenu_open) return;
    int screen_h = screen->height;
    int menu_h = STARTMENU_ITEM_COUNT * STARTMENU_ITEM_H + 4;
    int menu_y = screen_h - TASKBAR_H - menu_h;
    draw_rect(screen, 4, menu_y, STARTMENU_W, menu_h, COLOR_TASKBAR_BG);
    draw_rect_outline(screen, 4, menu_y, STARTMENU_W, menu_h, 1, COLOR_ACCENT);
    for (int i = 0; i < STARTMENU_ITEM_COUNT; i++) {
        int iy = menu_y + 2 + i * STARTMENU_ITEM_H;
        draw_string(screen, 14, iy + 6, start_menu_items[i],
                    COLOR_TEXT_LIGHT, rgba(0,0,0,0));
        draw_hline(screen, 4, iy + STARTMENU_ITEM_H - 1, STARTMENU_W, COLOR_MID_GREY);
    }
}

static void check_start_menu_click(int mx, int my)
{
    if (!startmenu_open) return;
    int screen_h = (int)fb.height;
    int menu_h = STARTMENU_ITEM_COUNT * STARTMENU_ITEM_H + 4;
    int menu_y = screen_h - TASKBAR_H - menu_h;

    if (mx < 4 || mx > 4 + STARTMENU_W || my < menu_y || my > menu_y + menu_h) {
        startmenu_open = false;
        return;
    }
    int item = (my - menu_y - 2) / STARTMENU_ITEM_H;
    if (item < 0 || item >= STARTMENU_ITEM_COUNT) return;
    startmenu_open = false;
    switch (item) {
    case 0: gui_launch_terminal();    break;
    case 1: gui_launch_filemanager(); break;
    case 2: gui_launch_texteditor();  break;
    case 3: gui_launch_sysmonitor();  break;
    case 4: cpu_cli(); cpu_halt();    break;
    }
}

/* =========================================================
 * Desktop tick - called every frame
 * ========================================================= */

void desktop_tick(void)
{
    draw_desktop_bg();
    taskbar_draw(&g_screen);
    draw_start_menu(&g_screen);
}

void desktop_init(void)
{
    /* Nothing extra to init - gui_init() covers it */
}

/* =========================================================
 * GUI init and run
 * ========================================================= */

void gui_init(void)
{
    gui_event_queue_init();
    wm_init();
    gui_running = false;
    kinfo("GUI: initialized");
}

bool gui_available(void)
{
    return fb_ready();
}

/* =========================================================
 * GUI main loop (kernel thread)
 * ========================================================= */

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

    gui_launch_terminal();

    uint32_t last_ticks = 0;
    uint8_t  prev_buttons = 0;

    while (gui_running) {
        uint32_t now = timer_get_ticks();
        if (now - last_ticks < 2) {
            scheduler_yield();
            continue;
        }
        last_ticks = now;

        /* Desktop background */
        desktop_tick();

        /* Dispatch window events */
        wm_dispatch_events();

        /* Taskbar mouse handling */
        uint8_t cur_buttons = mouse.buttons;
        bool just_clicked   = (cur_buttons & ~prev_buttons) & MOUSE_BTN_LEFT;
        taskbar_handle_mouse(mouse.x, mouse.y, just_clicked);
        if (just_clicked) check_start_menu_click(mouse.x, mouse.y);
        prev_buttons = cur_buttons;

        /* Composite windows */
        wm_composite(&g_screen);

        /* Draw cursor */
        mouse_draw_cursor(fb.back_buf, (int)fb.width, (int)fb.height);

        /* Flip to display */
        fb_flip();

        scheduler_yield();
    }
}

/* =========================================================
 * App launchers
 * ========================================================= */

extern wid_t app_terminal_create(void);
extern wid_t app_filemanager_create(void);
extern wid_t app_texteditor_create(void);
extern wid_t app_sysmonitor_create(void);

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
