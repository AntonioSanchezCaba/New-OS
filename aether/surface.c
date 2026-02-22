/*
 * aether/surface.c — Surface lifecycle management
 */
#include <aether/surface.h>
#include <aether/are.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Easing table (ease-out cubic, 0→64 → 0→255)
 * ========================================================= */
const uint8_t EASE_OUT_64[65] = {
      0,  11,  22,  33,  43,  54,  64,  74,  83,  93, 102, 111, 120, 128,
    136, 144, 152, 160, 167, 174, 181, 188, 194, 200, 206, 212, 217, 222,
    227, 232, 236, 240, 244, 247, 250, 253, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255
};

/* =========================================================
 * Registry
 * ========================================================= */
static surface_t g_surfaces[SURFACE_MAX];
static bool      g_initialized = false;

void surface_init(void)
{
    memset(g_surfaces, 0, sizeof(g_surfaces));
    for (int i = 0; i < SURFACE_MAX; i++)
        g_surfaces[i].id = SID_NONE;
    g_initialized = true;
    kinfo("ARE: surface registry initialized (max %d)", SURFACE_MAX);
}

/* =========================================================
 * Create
 * ========================================================= */
sid_t surface_create(surface_type_t type,
                      uint32_t w, uint32_t h,
                      const char* title, const char* icon,
                      surface_render_fn render_fn,
                      surface_input_fn  input_fn,
                      surface_event_fn  close_fn,
                      void* userdata)
{
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SURFACE_MAX; i++) {
        if (!(g_surfaces[i].flags & SF_ALIVE)) { slot = i; break; }
    }
    if (slot < 0) {
        klog_warn("ARE: surface registry full");
        return SID_NONE;
    }

    /* Allocate pixel buffer */
    size_t buf_bytes = (size_t)w * h * 4;
    uint32_t* buf = (uint32_t*)kmalloc(buf_bytes);
    if (!buf) {
        klog_warn("ARE: cannot allocate surface buffer (%zu bytes)", buf_bytes);
        return SID_NONE;
    }
    memset(buf, 0, buf_bytes);  /* start fully transparent */

    surface_t* s = &g_surfaces[slot];
    memset(s, 0, sizeof(*s));
    s->id        = (sid_t)slot;
    s->flags     = SF_ALIVE | SF_DIRTY;
    s->type      = type;
    s->buf       = buf;
    s->buf_w     = w;
    s->buf_h     = h;
    s->on_render = render_fn;
    s->on_input  = input_fn;
    s->on_close  = close_fn;
    s->userdata  = userdata;

    strncpy(s->title, title ? title : "Surface", sizeof(s->title)-1);
    strncpy(s->icon_char, icon ? icon : " ", sizeof(s->icon_char)-1);

    if (type == SURF_OVERLAY)
        s->flags |= SF_OVERLAY;

    /* Start invisible, field will animate to visible */
    s->cur_alpha = 0;
    s->tgt_alpha = 255;
    s->anim_total = CTX_TRANSITION_FRAMES;

    kinfo("ARE: created surface %u '%s' (%ux%u)", s->id, s->title, w, h);
    return s->id;
}

/* =========================================================
 * Destroy
 * ========================================================= */
void surface_destroy(sid_t id)
{
    surface_t* s = surface_get(id);
    if (!s) return;
    if (s->on_close) s->on_close(id, s->userdata);
    if (s->buf) { kfree(s->buf); s->buf = NULL; }
    s->flags = 0;
    s->id    = SID_NONE;
    kinfo("ARE: destroyed surface %u", id);
}

/* =========================================================
 * Lookup
 * ========================================================= */
surface_t* surface_get(sid_t id)
{
    if (id >= SURFACE_MAX) return NULL;
    surface_t* s = &g_surfaces[id];
    return (s->flags & SF_ALIVE) ? s : NULL;
}

void surface_invalidate(sid_t id)
{
    surface_t* s = surface_get(id);
    if (s) s->flags |= SF_DIRTY;
}

/* =========================================================
 * Tick (invoke render callback if dirty)
 * ========================================================= */
void surface_tick(sid_t id)
{
    surface_t* s = surface_get(id);
    if (!s || !s->on_render) return;
    if (!(s->flags & SF_DIRTY)) return;

    s->on_render(id, s->buf, s->buf_w, s->buf_h, s->userdata);
    s->flags &= ~SF_DIRTY;
}

/* =========================================================
 * Iteration
 * ========================================================= */
int surface_count(void)
{
    int n = 0;
    for (int i = 0; i < SURFACE_MAX; i++)
        if (g_surfaces[i].flags & SF_ALIVE) n++;
    return n;
}

surface_t* surface_nth(int n)
{
    int cnt = 0;
    for (int i = 0; i < SURFACE_MAX; i++) {
        if (!(g_surfaces[i].flags & SF_ALIVE)) continue;
        if (cnt == n) return &g_surfaces[i];
        cnt++;
    }
    return NULL;
}

/* =========================================================
 * canvas_t view
 * ========================================================= */
canvas_t surface_canvas(sid_t id)
{
    surface_t* s = surface_get(id);
    if (!s || !s->buf)
        return (canvas_t){ NULL, 0, 0 };
    return (canvas_t){ .pixels=s->buf, .width=s->buf_w, .height=s->buf_h };
}
