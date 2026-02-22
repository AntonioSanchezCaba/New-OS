/*
 * include/aether/field.h — Aether Field
 *
 * The Field is the spatial canvas that holds all surfaces.
 * It is NOT a desktop. It has no icons, no file grid, no wallpaper.
 *
 * The Field defines the layout geometry for surfaces based on their
 * slot offset relative to the active surface:
 *
 *   Slot -2  Slot -1  [Slot 0 = ACTIVE]  Slot +1  Slot +2
 *   ghost    partial   ┌───────────┐      partial   ghost
 *                      │  ACTIVE   │
 *                      └───────────┘
 *
 * The field also renders:
 *   - Dynamic background (deep-space gradient, animated particles)
 *   - Surface shadows
 *   - Navigation dots (bottom center)
 *   - Overview mode (all surfaces at small scale)
 */
#pragma once
#include <types.h>
#include <aether/vec.h>
#include <aether/surface.h>
#include <aether/context.h>
#include <gui/draw.h>

/* =========================================================
 * Field geometry constants
 * ========================================================= */
#define FIELD_SURF_MARGIN_X   32   /* pixels from screen edge */
#define FIELD_SURF_MARGIN_TOP 24   /* pixels from top */
#define FIELD_SURF_MARGIN_BOT 40   /* pixels from bottom (nav dots) */

/* Scale of adjacent surfaces (×256 = 100%) */
#define FIELD_SCALE_ACTIVE    256
#define FIELD_SCALE_ADJACENT  202  /* ~79% */
#define FIELD_SCALE_FAR       140  /* ~55% — barely visible */

/* Alpha of adjacent surfaces */
#define FIELD_ALPHA_ACTIVE    255
#define FIELD_ALPHA_ADJACENT  140
#define FIELD_ALPHA_FAR        40

/* Overview mode: all surfaces at this scale */
#define FIELD_SCALE_OVERVIEW  128  /* 50% */
#define FIELD_OVERVIEW_COLS     4

/* =========================================================
 * Slot geometry (computed each frame from context state)
 * ========================================================= */
typedef struct {
    vec2_t  pos;      /* top-left in screen space */
    int32_t w, h;     /* display dimensions after scaling */
    uint8_t alpha;
    int     slot;     /* relative slot offset from active */
} slot_geom_t;

/* =========================================================
 * Field state
 * ========================================================= */
typedef struct {
    /* Animated background particles */
    struct {
        int16_t x, y;
        uint8_t speed;
        uint8_t brightness;
    } particles[120];

    uint32_t tick;
} field_state_t;

/* =========================================================
 * API
 * ========================================================= */
void field_init(uint32_t screen_w, uint32_t screen_h);

/* Draw the field background onto canvas */
void field_draw_background(canvas_t* c);

/* Compute slot geometry for a surface at given slot offset */
slot_geom_t field_slot_geom(int slot_offset,
                             uint32_t surf_w, uint32_t surf_h,
                             uint32_t screen_w, uint32_t screen_h);

/* Compose all surfaces (in correct z-order) onto screen canvas */
void field_compose(canvas_t* c);

/* Draw navigation dots at bottom of screen */
void field_draw_nav_dots(canvas_t* c);

/* Draw overview layout (all surfaces small, tiled) */
void field_draw_overview(canvas_t* c);

/* Tick the animated background */
void field_tick(void);

/* Hit-test: in overview mode, which surface slot did the user click? */
int  field_overview_hit(int mx, int my,
                         uint32_t screen_w, uint32_t screen_h);
