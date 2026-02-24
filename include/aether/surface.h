/*
 * include/aether/surface.h — Surface abstraction
 *
 * A Surface is the fundamental render unit in Aether.
 * It is NOT a window: it has no title bar, no frame, no OS chrome.
 *
 * Each surface owns a pixel buffer it renders into.
 * The ARE composites surfaces onto the framebuffer with transforms.
 *
 * Surfaces are created by the field and owned by the ARE.
 * Applications write into surface buffers via callbacks.
 */
#pragma once
#include <types.h>
#include <aether/vec.h>

/* Maximum concurrent surfaces */
#define SURFACE_MAX   16

/* Surface identifier */
typedef uint32_t sid_t;
#define SID_NONE  0xFFFFFFFF

/* Surface type/category */
typedef enum {
    SURF_APP      = 0,   /* Regular application surface                */
    SURF_OVERLAY  = 1,   /* Modal overlay (launcher, dialogs)          */
    SURF_SYSTEM   = 2,   /* System-level (splash, lock)                */
    SURF_FLOAT    = 3,   /* Floating draggable window with title bar   */
} surface_type_t;

/* Surface lifecycle flags */
#define SF_ALIVE     (1<<0)   /* Surface is valid */
#define SF_DIRTY     (1<<1)   /* Buffer needs redraw */
#define SF_PINNED    (1<<2)   /* Cannot be removed from field */
#define SF_OVERLAY   (1<<3)   /* Rendered above all app surfaces */
#define SF_CLOSING   (1<<4)   /* Fade-out animation in progress */

/* Input callback (receives ARE-translated coordinates) */
struct input_event;
typedef void (*surface_input_fn)(sid_t id, const struct input_event* ev,
                                  void* userdata);

/* Render callback: surface writes into buf (w×h ARGB32) */
typedef void (*surface_render_fn)(sid_t id, uint32_t* buf,
                                   uint32_t w, uint32_t h, void* userdata);

/* Lifecycle callback */
typedef void (*surface_event_fn)(sid_t id, void* userdata);

/* =========================================================
 * Surface descriptor
 * ========================================================= */
typedef struct surface {
    sid_t          id;
    uint32_t       flags;
    surface_type_t type;

    /* Identity */
    char   title[48];     /* Shown in overview mode only */
    char   icon_char[4];  /* Single glyph icon (UTF-8, ≤3 bytes) */

    /* Pixel buffer (private — allocated by surface_create) */
    uint32_t* buf;
    uint32_t  buf_w;
    uint32_t  buf_h;

    /* Callbacks (set by surface module) */
    surface_render_fn on_render;
    surface_input_fn  on_input;
    surface_event_fn  on_close;
    void*             userdata;

    /* === Compositing state (managed by field/ARE) === */
    /* Target geometry (where the surface should end up) */
    vec2_t  tgt_pos;     /* Top-left screen coords */
    int32_t tgt_w, tgt_h;
    uint8_t tgt_alpha;

    /* Current geometry (animated toward target each frame) */
    vec2_t  cur_pos;
    int32_t cur_w, cur_h;
    uint8_t cur_alpha;

    /* Animation state */
    int     anim_frame;
    int     anim_total;
} surface_t;

/* =========================================================
 * API
 * ========================================================= */
void      surface_init(void);

/* Create a surface with a pixel buffer of given dimensions.
 * Returns SID_NONE on failure. */
sid_t     surface_create(surface_type_t type,
                          uint32_t w, uint32_t h,
                          const char* title, const char* icon,
                          surface_render_fn render_fn,
                          surface_input_fn  input_fn,
                          surface_event_fn  close_fn,
                          void* userdata);

void      surface_destroy(sid_t id);
surface_t* surface_get(sid_t id);

/* Mark surface as needing a render pass */
void      surface_invalidate(sid_t id);

/* Called by ARE each frame: invoke on_render if dirty */
void      surface_tick(sid_t id);

/* Iterate all alive surfaces */
int       surface_count(void);
surface_t* surface_nth(int n);   /* 0-based, alive only */

/* canvas_t compatible view into a surface's buffer */
#include <gui/draw.h>
canvas_t  surface_canvas(sid_t id);
