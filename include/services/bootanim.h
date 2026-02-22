/*
 * include/services/bootanim.h - Boot animation service
 *
 * Provides a polished boot screen displayed between POST (VGA text mode)
 * and the desktop login screen.
 *
 * Sequence:
 *   1. Fade-in from black
 *   2. Logo appears (animated circle sweep)
 *   3. "AetherOS" text fades in below logo
 *   4. Progress bar fills as kernel services initialize
 *   5. Fade-out to desktop
 *
 * The animation is rendered directly to the framebuffer back buffer and
 * flipped each tick by the kernel timer ISR callback.  It is designed to
 * run before the compositor is up (no window server needed).
 */
#ifndef SERVICES_BOOTANIM_H
#define SERVICES_BOOTANIM_H

#include <types.h>

/* Duration constants (in 10ms timer ticks) */
#define BOOTANIM_FADE_IN_TICKS   30   /* 300 ms */
#define BOOTANIM_LOGO_TICKS      60   /* 600 ms */
#define BOOTANIM_TEXT_TICKS      40   /* 400 ms */
#define BOOTANIM_PROGRESS_TICKS  80   /* 800 ms */
#define BOOTANIM_FADE_OUT_TICKS  30   /* 300 ms */
#define BOOTANIM_TOTAL_TICKS \
    (BOOTANIM_FADE_IN_TICKS + BOOTANIM_LOGO_TICKS + \
     BOOTANIM_TEXT_TICKS + BOOTANIM_PROGRESS_TICKS + \
     BOOTANIM_FADE_OUT_TICKS)

/* Animation phases */
typedef enum {
    BOOTANIM_PHASE_FADE_IN = 0,
    BOOTANIM_PHASE_LOGO,
    BOOTANIM_PHASE_TEXT,
    BOOTANIM_PHASE_PROGRESS,
    BOOTANIM_PHASE_FADE_OUT,
    BOOTANIM_PHASE_DONE,
} bootanim_phase_t;

/* Progress steps (registered by subsystems during init) */
#define BOOTANIM_MAX_STEPS  16

typedef struct {
    const char* label;     /* "Initializing memory..." */
    int         weight;    /* Relative progress weight */
} bootanim_step_t;

typedef struct {
    bootanim_phase_t phase;
    int              tick;           /* Ticks since animation start */
    int              progress;       /* 0..100 percent */
    int              step;           /* Current step index */
    bool             running;
    int              fade_alpha;     /* 0..255 */

    bootanim_step_t  steps[BOOTANIM_MAX_STEPS];
    int              num_steps;
    int              total_weight;
} bootanim_state_t;

extern bootanim_state_t g_bootanim;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Called from kernel_main() after framebuffer is up */
void bootanim_start(void);

/* Register a boot step (call before bootanim_start or in early init) */
void bootanim_add_step(const char* label, int weight);

/* Advance to the next step (called by each subsystem after init) */
void bootanim_step_done(void);

/* Called from timer ISR every 10 ms */
void bootanim_tick(void);

/* Returns true when animation is complete */
bool bootanim_done(void);

/* Render one frame to the framebuffer (called from bootanim_tick) */
void bootanim_render(void);

/* Force-complete animation (e.g. on keypress to skip) */
void bootanim_skip(void);

#endif /* SERVICES_BOOTANIM_H */
