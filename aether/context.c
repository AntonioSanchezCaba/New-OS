/*
 * aether/context.c — Context Engine (surface navigation)
 */
#include <aether/context.h>
#include <aether/surface.h>
#include <aether/field.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

context_state_t g_ctx;

/* =========================================================
 * Init
 * ========================================================= */
void context_init(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    for (int i = 0; i < SURFACE_MAX; i++)
        g_ctx.field[i] = SID_NONE;
    g_ctx.active_idx    = 0;
    g_ctx.overview      = false;
    g_ctx.overview_hover= -1;
    g_ctx.overlay_visible = false;
    kinfo("ARE: context engine initialized");
}

/* =========================================================
 * Field management
 * ========================================================= */
int context_add_surface(sid_t sid)
{
    if (g_ctx.field_count >= SURFACE_MAX) return -1;
    int idx = g_ctx.field_count;
    g_ctx.field[idx] = sid;
    g_ctx.field_count++;
    kinfo("ARE: surface %u added to field at slot %d", sid, idx);
    return idx;
}

void context_remove_surface(sid_t sid)
{
    for (int i = 0; i < g_ctx.field_count; i++) {
        if (g_ctx.field[i] != sid) continue;
        /* Shift remaining down */
        for (int j = i; j < g_ctx.field_count - 1; j++)
            g_ctx.field[j] = g_ctx.field[j+1];
        g_ctx.field[--g_ctx.field_count] = SID_NONE;
        if (g_ctx.active_idx >= g_ctx.field_count && g_ctx.active_idx > 0)
            g_ctx.active_idx--;
        return;
    }
}

sid_t context_active(void)
{
    if (g_ctx.field_count == 0) return SID_NONE;
    return g_ctx.field[g_ctx.active_idx];
}

int context_slot_of(sid_t sid)
{
    for (int i = 0; i < g_ctx.field_count; i++)
        if (g_ctx.field[i] == sid) return i;
    return -1;
}

/* =========================================================
 * Navigation
 * ========================================================= */
void context_navigate(int delta)
{
    if (g_ctx.field_count <= 1) return;
    int target = g_ctx.active_idx + delta;
    if (target < 0) target = 0;
    if (target >= g_ctx.field_count) target = g_ctx.field_count - 1;
    if (target == g_ctx.active_idx) return;
    context_goto(target);
}

void context_goto(int idx)
{
    if (idx < 0 || idx >= g_ctx.field_count) return;
    if (idx == g_ctx.active_idx && !g_ctx.transitioning) return;

    g_ctx.trans_target    = idx;
    g_ctx.trans_direction = (idx > g_ctx.active_idx) ? 1 : -1;
    g_ctx.trans_frame     = 0;
    g_ctx.transitioning   = true;

    /* Invalidate all surfaces so they repaint in new state */
    for (int i = 0; i < g_ctx.field_count; i++)
        surface_invalidate(g_ctx.field[i]);
}

/* =========================================================
 * Tick — advance transition animation
 * ========================================================= */
void context_tick(void)
{
    /* Advance transition */
    if (g_ctx.transitioning) {
        g_ctx.trans_frame++;
        if (g_ctx.trans_frame >= CTX_TRANSITION_FRAMES) {
            g_ctx.active_idx  = g_ctx.trans_target;
            g_ctx.transitioning = false;
            surface_invalidate(context_active());
        }
    }

    /* Update surface compositing targets based on context state */
    uint32_t sw = are_screen_w(), sh = are_screen_h();

    for (int i = 0; i < g_ctx.field_count; i++) {
        surface_t* s = surface_get(g_ctx.field[i]);
        if (!s) continue;

        /* Compute effective active index (lerp during transition) */
        int active = g_ctx.transitioning ? g_ctx.active_idx : g_ctx.active_idx;
        int slot_offset = i - active;

        /* During transition: blend toward target */
        if (g_ctx.transitioning) {
            int prog = (int)ease_progress(g_ctx.trans_frame,
                                           CTX_TRANSITION_FRAMES);
            /* Interpolate slot_offset toward final state */
            int final_slot = i - g_ctx.trans_target;
            slot_offset = slot_offset + ((final_slot - slot_offset) * prog) / 255;
        }

        if (g_ctx.overview) {
            /* Overview: all surfaces visible in a grid */
            int cols = FIELD_OVERVIEW_COLS;
            int row = i / cols, col = i % cols;
            int cell_w = (int)sw / cols;
            int cell_h = (int)sh / cols;  /* roughly square cells */
            int surf_w = cell_w - 16;
            int surf_h = cell_h - 16;
            s->tgt_pos.x = col * cell_w + 8;
            s->tgt_pos.y = row * cell_h + 8;
            s->tgt_w     = surf_w;
            s->tgt_h     = surf_h;
            s->tgt_alpha = (g_ctx.overview_hover == i) ? 255 : 200;
        } else {
            slot_geom_t g = field_slot_geom(slot_offset,
                                             s->buf_w, s->buf_h, sw, sh);
            s->tgt_pos  = g.pos;
            s->tgt_w    = g.w;
            s->tgt_h    = g.h;
            s->tgt_alpha= g.alpha;
        }

        /* Lerp current → target */
        int t;
        if (g_ctx.transitioning)
            t = (int)ease_progress(g_ctx.trans_frame, CTX_TRANSITION_FRAMES);
        else
            t = 255;  /* snap immediately when not transitioning */

        s->cur_pos.x = lrp(s->cur_pos.x, s->tgt_pos.x, t);
        s->cur_pos.y = lrp(s->cur_pos.y, s->tgt_pos.y, t);
        s->cur_w     = lrp(s->cur_w,     s->tgt_w,     t);
        s->cur_h     = lrp(s->cur_h,     s->tgt_h,     t);
        s->cur_alpha = (uint8_t)lrp(s->cur_alpha, s->tgt_alpha, t);
    }
}

/* =========================================================
 * Overview
 * ========================================================= */
void context_toggle_overview(void)
{
    g_ctx.overview = !g_ctx.overview;
    g_ctx.overview_hover = -1;
    /* Invalidate all surfaces */
    for (int i = 0; i < g_ctx.field_count; i++)
        surface_invalidate(g_ctx.field[i]);
}

/* =========================================================
 * Overlays
 * ========================================================= */
void context_push_overlay(sid_t sid)
{
    if (g_ctx.overlay_count >= 4) return;
    g_ctx.overlays[g_ctx.overlay_count++] = sid;
    g_ctx.overlay_visible = true;
}

void context_pop_overlay(void)
{
    if (g_ctx.overlay_count <= 0) return;
    g_ctx.overlay_count--;
    g_ctx.overlay_visible = (g_ctx.overlay_count > 0);
}

bool context_overlay_visible(void)
{
    return g_ctx.overlay_visible && g_ctx.overlay_count > 0;
}
