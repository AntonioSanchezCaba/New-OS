/*
 * gui/workspace.c — Virtual workspace (desktop) manager
 *
 * Maintains 4 independent virtual workspaces.  Each workspace has a
 * list of window IDs that belong to it.  Switching workspaces hides all
 * windows on the departing workspace and reveals all windows on the new one.
 *
 * Integration:
 *   - Call workspace_init() in desktop_init()
 *   - Call workspace_add_window(wid) after every wm_create_window() call
 *   - Call workspace_remove_window(wid) when a window is closed
 *   - Call workspace_switch(n) from the taskbar workspace buttons
 *   - Draw the indicator via workspace_draw_indicator()
 */
#include <gui/workspace.h>
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/theme.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    wid_t wins[WS_MAX_WINDOWS];
    int   count;
} ws_t;

static ws_t g_ws[WS_COUNT];
static int  g_current = 0;

/* =========================================================
 * Init
 * ========================================================= */
void workspace_init(void)
{
    memset(g_ws, 0, sizeof(g_ws));
    for (int i = 0; i < WS_COUNT; i++) {
        for (int j = 0; j < WS_MAX_WINDOWS; j++)
            g_ws[i].wins[j] = -1;
        g_ws[i].count = 0;
    }
    g_current = 0;
    kinfo("WORKSPACE: initialized (%d virtual desktops)", WS_COUNT);
}

/* =========================================================
 * Helpers
 * ========================================================= */
static int find_window_in_ws(int ws, wid_t wid)
{
    for (int i = 0; i < WS_MAX_WINDOWS; i++)
        if (g_ws[ws].wins[i] == wid) return i;
    return -1;
}

static void set_ws_visible(int ws, bool visible)
{
    for (int i = 0; i < WS_MAX_WINDOWS; i++) {
        wid_t wid = g_ws[ws].wins[i];
        if (wid < 0) continue;
        window_t* win = wm_get_window(wid);
        if (!win) continue;

        if (visible) {
            /* Only show if not minimized by user */
            if (!(win->flags & WF_MINIMIZED))
                win->flags |= WF_VISIBLE;
        } else {
            /* Hide but do not set MINIMIZED flag (preserve minimize state) */
            win->flags &= ~WF_VISIBLE;
        }
    }
}

/* =========================================================
 * Switch workspace
 * ========================================================= */
bool workspace_switch(int n)
{
    if (n < 0 || n >= WS_COUNT || n == g_current) return false;

    kinfo("WORKSPACE: switching %d → %d", g_current, n);

    /* Hide current workspace windows */
    set_ws_visible(g_current, false);

    g_current = n;

    /* Show new workspace windows */
    set_ws_visible(g_current, true);

    return true;
}

int workspace_current(void) { return g_current; }

/* =========================================================
 * Add / remove windows
 * ========================================================= */
void workspace_add_window(wid_t wid)
{
    if (wid < 0) return;

    /* Already tracked? */
    for (int ws = 0; ws < WS_COUNT; ws++)
        if (find_window_in_ws(ws, wid) >= 0) return;

    ws_t* w = &g_ws[g_current];
    if (w->count >= WS_MAX_WINDOWS) {
        klog_warn("WORKSPACE: workspace %d is full", g_current);
        return;
    }

    /* Find a free slot */
    for (int i = 0; i < WS_MAX_WINDOWS; i++) {
        if (w->wins[i] < 0) {
            w->wins[i] = wid;
            w->count++;
            return;
        }
    }
}

void workspace_remove_window(wid_t wid)
{
    for (int ws = 0; ws < WS_COUNT; ws++) {
        int idx = find_window_in_ws(ws, wid);
        if (idx >= 0) {
            g_ws[ws].wins[idx] = -1;
            g_ws[ws].count--;
            return;
        }
    }
}

void workspace_move_window(wid_t wid, int target_ws)
{
    if (target_ws < 0 || target_ws >= WS_COUNT) return;

    /* Remove from wherever it is */
    for (int ws = 0; ws < WS_COUNT; ws++) {
        int idx = find_window_in_ws(ws, wid);
        if (idx >= 0) {
            g_ws[ws].wins[idx] = -1;
            g_ws[ws].count--;
            break;
        }
    }

    /* Add to target */
    ws_t* w = &g_ws[target_ws];
    for (int i = 0; i < WS_MAX_WINDOWS; i++) {
        if (w->wins[i] < 0) {
            w->wins[i] = wid;
            w->count++;
            break;
        }
    }

    /* Update visibility */
    window_t* win = wm_get_window(wid);
    if (win) {
        if (target_ws == g_current)
            win->flags |= WF_VISIBLE;
        else
            win->flags &= ~WF_VISIBLE;
    }
}

int workspace_window_count(int ws)
{
    if (ws < 0 || ws >= WS_COUNT) return 0;
    return g_ws[ws].count;
}

/* =========================================================
 * Taskbar indicator
 * ========================================================= */
void workspace_draw_indicator(void* canvas_ptr, int x, int y, int w, int h)
{
    canvas_t* c  = (canvas_t*)canvas_ptr;
    const theme_t* th = theme_current();

    /* Draw WS_COUNT small squares side by side */
    int sq   = h - 4;
    int gap  = (w - WS_COUNT * sq) / (WS_COUNT + 1);
    int ox   = x + gap;

    for (int i = 0; i < WS_COUNT; i++) {
        bool active = (i == g_current);
        bool has_wins = (g_ws[i].count > 0);

        uint32_t bg = active ? th->accent :
                      has_wins ? rgba(0xFF, 0xFF, 0xFF, 0x40) :
                                 rgba(0xFF, 0xFF, 0xFF, 0x18);

        draw_rect_rounded(c, ox, y + 2, sq, sq, 2, bg);

        if (active)
            draw_rect_outline(c, ox, y + 2, sq, sq, 1, th->accent2);

        /* Dot for each window on this workspace (up to 3) */
        int dots = g_ws[i].count;
        if (dots > 3) dots = 3;
        for (int d = 0; d < dots; d++) {
            int dx = ox + 2 + d * 3;
            int dy = y + sq - 2;
            draw_rect(c, dx, dy, 2, 2, has_wins ? th->ok : th->panel_border);
        }

        char ws_num[2] = { '0' + (char)i, '\0' };
        draw_string_centered(c, ox, y + 2, sq, sq - 4,
                             ws_num, active ? th->btn_text : th->taskbar_text,
                             rgba(0,0,0,0));

        ox += sq + gap;
    }
}
