/*
 * include/gui/animation.h — Window animation system
 *
 * Provides smooth animations for window open, close, minimize, and restore.
 * All math uses integer fixed-point (no floating point).
 * Easing: ease-out cubic via lookup table.
 */
#pragma once
#include <gui/window.h>

typedef enum {
    ANIM_NONE      = 0,
    ANIM_OPEN      = 1,   /* Scale from centre, fade in  */
    ANIM_CLOSE     = 2,   /* Scale to centre, fade out   */
    ANIM_MINIMIZE  = 3,   /* Shrink to taskbar position  */
    ANIM_RESTORE   = 4,   /* Expand from taskbar         */
    ANIM_SHADE     = 5,   /* Roll-up to title bar        */
    ANIM_UNSHADE   = 6,   /* Unroll from title bar       */
} anim_type_t;

#define ANIM_DURATION_OPEN     12   /* frames */
#define ANIM_DURATION_CLOSE    10
#define ANIM_DURATION_MINIMIZE 14
#define ANIM_DURATION_RESTORE  14
#define ANIM_DURATION_SHADE    10   /* Roll-up to title bar */
#define ANIM_DURATION_UNSHADE  10   /* Unroll from title bar */
#define ANIM_MAX               32   /* Max concurrent animations */

typedef struct {
    bool        active;
    wid_t       wid;
    anim_type_t type;
    int         frame;          /* 0..duration */
    int         duration;

    /* Saved window geometry */
    int  src_x, src_y, src_w, src_h;
    int  dst_x, dst_y, dst_w, dst_h;

    /* Taskbar button target (for minimize/restore) */
    int  task_x, task_y, task_w, task_h;
} anim_t;

/* Initialise animation subsystem */
void anim_init(void);

/* Start an animation on a window */
void anim_start(wid_t wid, anim_type_t type);
void anim_start_minimize(wid_t wid, int task_x, int task_y,
                          int task_w, int task_h);

/* Advance all running animations by one frame (call per display frame) */
void anim_tick(void);

/* Returns true while any animation is running */
bool anim_any_active(void);

/* Cancel all animations on a window */
void anim_cancel(wid_t wid);

/* Apply animation to a window before compositing */
void anim_apply(wid_t wid, int* out_x, int* out_y,
                int* out_w, int* out_h, uint8_t* out_alpha);
