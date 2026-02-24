/*
 * include/aether/are.h — Aether Render Engine (ARE)
 *
 * The ARE is the core of Aether's graphical system.  It is NOT a
 * window manager, NOT a compositor in the X11 sense, and NOT a
 * display server.  It is a single-process spatial rendering engine.
 *
 * ARE responsibilities:
 *   1. Own the framebuffer and double-buffer
 *   2. Maintain the surface registry
 *   3. Run the main render loop (input → context → compose → flip)
 *   4. Route input to the correct recipient
 *   5. Manage surface lifecycle
 *
 * Main render loop:
 *   while (running) {
 *       input_poll();          — drain hardware queues
 *       are_dispatch_input();  — navigate or forward to surface
 *       context_tick();        — advance transition animations
 *       field_tick();          — advance background animations
 *       field_compose(screen); — blit all surfaces with transforms
 *       field_draw_nav_dots(screen);
 *       fb_flip();             — page flip
 *       scheduler_yield();
 *   }
 */
#pragma once
#include <types.h>
#include <aether/surface.h>
#include <aether/context.h>
#include <aether/input.h>

/* =========================================================
 * ARE version info
 * ========================================================= */
#define ARE_VERSION_MAJOR  1
#define ARE_VERSION_MINOR  0
#define ARE_NAME           "Aether Render Engine"

/* =========================================================
 * API
 * ========================================================= */

/* Called from kernel_main after memory + drivers are ready.
 * Initialises all ARE subsystems. */
void are_init(void);

/* Enter the main render loop (never returns normally). */
void are_run(void);

/* Stop the render loop (called from inside a surface callback). */
void are_shutdown(void);

/* Register a new surface and add it to the field.
 * Shorthand for surface_create() + context_add_surface(). */
sid_t are_add_surface(surface_type_t type,
                       uint32_t w, uint32_t h,
                       const char* title, const char* icon,
                       surface_render_fn render_fn,
                       surface_input_fn  input_fn,
                       surface_event_fn  close_fn,
                       void* userdata);

/* Remove and destroy a surface (with close animation). */
void are_remove_surface(sid_t id);

/* Push an overlay surface (launcher, alert, etc.) */
void are_push_overlay(sid_t id);
void are_pop_overlay(void);

/* Create a floating draggable window positioned at (x,y).
 * Pass x=-1, y=-1 to center automatically.
 * The title bar (FLOAT_TITLE_H px) is drawn by the compositor;
 * the render callback receives the body area only (w × h). */
sid_t are_add_float(uint32_t w, uint32_t h,
                    const char* title,
                    surface_render_fn render_fn,
                    surface_input_fn  input_fn,
                    void* userdata);

/* Blit one surface buffer onto destination with scale + alpha.
 * Public so surfaces can embed sub-canvases. */
void are_blit_surface(uint32_t* dst, int dst_w, int dst_h,
                       const uint32_t* src, int src_w, int src_h,
                       int x, int y, int blit_w, int blit_h,
                       uint8_t alpha);

/* Query screen dimensions */
uint32_t are_screen_w(void);
uint32_t are_screen_h(void);

/* Whether ARE is currently running */
bool are_running(void);
