/*
 * include/gui/gui.h - Aether OS GUI subsystem
 *
 * Include this in any code that uses the desktop environment.
 * Provides: framebuffer, drawing, events, windows, theme, notifications.
 */
#ifndef GUI_GUI_H
#define GUI_GUI_H

#include <types.h>
#include <drivers/framebuffer.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/window.h>
#include <gui/theme.h>
#include <gui/notify.h>

/* Target frame rate */
#define GUI_TARGET_FPS  60
#define GUI_FRAME_MS    (1000 / GUI_TARGET_FPS)

/* GUI initialization (called from kernel_main) */
void gui_init(void);

/* Main GUI render/event loop: splash → login → desktop */
void gui_run(void);

/* Whether the GUI has been successfully initialized */
bool gui_available(void);

/* Desktop / compositor */
void desktop_init(void);
void desktop_tick(void);        /* Called every frame */

/* Launch a GUI application */
void gui_launch_terminal(void);
void gui_launch_filemanager(void);
void gui_launch_texteditor(void);
void gui_launch_sysmonitor(void);
void gui_launch_settings(void);
void gui_launch_calculator(void);
void gui_launch_clock(void);
void gui_launch_stress_test(void);

/* Periodic ticks from desktop loop */
void clock_tick(void);
void stress_tick(void);

/* Taskbar */
void taskbar_add(wid_t wid, const char* label);
void taskbar_draw(canvas_t* screen);
void taskbar_handle_mouse(int x, int y, bool clicked);

#endif /* GUI_GUI_H */
