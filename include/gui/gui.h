/*
 * include/gui/gui.h - Main GUI subsystem header
 *
 * Include this in any code that wants to use the desktop environment.
 * Pulls in the framebuffer, drawing, event, and window subsystems.
 */
#ifndef GUI_GUI_H
#define GUI_GUI_H

#include <types.h>
#include <drivers/framebuffer.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/window.h>

/* Target frame rate */
#define GUI_TARGET_FPS  60
#define GUI_FRAME_MS    (1000 / GUI_TARGET_FPS)

/* GUI initialization (called from kernel_main) */
void gui_init(void);

/* Main GUI render/event loop (called from the desktop kernel thread) */
void gui_run(void);

/* Whether the GUI has been successfully initialized */
bool gui_available(void);

/* Desktop / compositor */
void desktop_init(void);
void desktop_tick(void);        /* Called every frame */

/* Launch a GUI application (creates a kernel thread) */
void gui_launch_terminal(void);
void gui_launch_filemanager(void);
void gui_launch_texteditor(void);
void gui_launch_sysmonitor(void);

/* Taskbar */
void taskbar_draw(canvas_t* screen);
void taskbar_handle_mouse(int x, int y, bool clicked);

#endif /* GUI_GUI_H */
