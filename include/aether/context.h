/*
 * include/aether/context.h — Context Engine
 *
 * The "context" is the user's current position in the spatial field.
 * Switching contexts = navigating to a different surface.
 *
 * Transitions are always animated: surfaces slide and fade smoothly.
 * There is no instant switching, no redraw flash, no state destruction.
 *
 * The context engine also manages "overview mode" — a bird's-eye view
 * of all surfaces laid out simultaneously.
 */
#pragma once
#include <types.h>
#include <aether/surface.h>

/* Number of animation frames for context transition (~200ms at 60Hz) */
#define CTX_TRANSITION_FRAMES  12

/* =========================================================
 * Context state
 * ========================================================= */
typedef struct {
    /* The ordered list of app surface IDs in the field */
    sid_t   field[SURFACE_MAX];
    int     field_count;

    /* Current position in field (index of active surface) */
    int     active_idx;

    /* Animation */
    bool    transitioning;
    int     trans_frame;
    int     trans_target;    /* target active_idx */
    int     trans_direction; /* +1 = forward, -1 = backward */

    /* Overview mode */
    bool    overview;
    int     overview_hover;  /* index under cursor in overview, or -1 */

    /* Overlay surfaces (launcher, etc.) — rendered on top */
    sid_t   overlays[4];
    int     overlay_count;
    bool    overlay_visible;
} context_state_t;

extern context_state_t g_ctx;

/* =========================================================
 * API
 * ========================================================= */
void context_init(void);

/* Add a surface to the field.  Returns its slot index. */
int  context_add_surface(sid_t sid);

/* Remove a surface from the field */
void context_remove_surface(sid_t sid);

/* Navigate by delta (usually ±1). Starts transition animation. */
void context_navigate(int delta);

/* Jump directly to a specific index (also animated) */
void context_goto(int idx);

/* Toggle overview mode */
void context_toggle_overview(void);

/* Register / show / hide an overlay surface */
void context_push_overlay(sid_t sid);
void context_pop_overlay(void);
bool context_overlay_visible(void);

/* Called every frame by ARE: advances animations */
void context_tick(void);

/* Get current active surface ID */
sid_t context_active(void);

/* Query slot index for a surface ID (-1 if not in field) */
int   context_slot_of(sid_t sid);
