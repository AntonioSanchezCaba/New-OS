/*
 * apps/filemanager.c - Aether OS Graphical File Manager
 *
 * Two-pane layout:
 *   Left: Quick-access sidebar (home, root, common dirs)
 *   Right: File listing with icons, type, size
 *
 * Features:
 *   - Keyboard navigation (arrows, Enter, Backspace)
 *   - Single-click to select, double-click to open/navigate
 *   - Delete key to remove file
 *   - Toolbar: back, up, path bar, refresh
 *   - Status bar with item count + selection info
 *   - Theme-aware colors
 */
#include <gui/window.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <drivers/timer.h>

#define FM_W         640
#define FM_H         400
#define FM_SIDEBAR_W 148
#define FM_TOOLBAR_H 30
#define FM_ITEM_H    22
#define FM_STATUS_H  22
#define FM_MAX_ITEMS 64
#define FM_MAX_HIST  16

typedef struct {
    wid_t wid;

    /* Current directory state */
    char  cwd[256];
    char  items[FM_MAX_ITEMS][256];
    bool  is_dir[FM_MAX_ITEMS];
    int   item_count;
    int   selected;
    int   scroll;

    /* Back history */
    char  history[FM_MAX_HIST][256];
    int   hist_len;

    /* Double-click tracking */
    int      last_clicked;
    uint32_t last_click_tick;

    /* Delete confirm state */
    bool delete_confirm;
    int  delete_idx;
} fm_t;

static fm_t* g_fm = NULL;

/* ---- Sidebar items ---- */
static const struct { const char* label; const char* path; } g_sidebar[] = {
    { "/ Root",   "/" },
    { "  home",   "/home" },
    { "  user",   "/home/user" },
    { "  bin",    "/bin" },
    { "  etc",    "/etc" },
    { "  tmp",    "/tmp" },
    { "  proc",   "/proc" },
    { "  dev",    "/dev" },
    { NULL, NULL }
};
#define SIDEBAR_COUNT 8

/* ---- Load directory ---- */
static void fm_load_dir(fm_t* f, const char* path)
{
    /* Push to back history */
    if (f->hist_len < FM_MAX_HIST && strcmp(f->cwd, path) != 0 && f->cwd[0]) {
        strncpy(f->history[f->hist_len++], f->cwd, 255);
    }

    strncpy(f->cwd, path, 255);
    f->cwd[255] = '\0';
    f->item_count = 0;
    f->selected   = 0;
    f->scroll     = 0;
    f->delete_confirm = false;

    vfs_node_t* dir = vfs_resolve_path(path);
    if (!dir || !dir->ops || !dir->ops->readdir) return;

    /* ".." except at root */
    if (strcmp(path, "/") != 0) {
        strncpy(f->items[f->item_count], "..", 255);
        f->is_dir[f->item_count++] = true;
    }

    for (int i = 0; f->item_count < FM_MAX_ITEMS; i++) {
        vfs_node_t* child = dir->ops->readdir(dir, i);
        if (!child) break;
        strncpy(f->items[f->item_count], child->name, 255);
        f->items[f->item_count][255] = '\0';
        f->is_dir[f->item_count] = (child->flags & VFS_DIRECTORY) != 0;
        f->item_count++;
    }
}

static void fm_go_up(fm_t* f)
{
    char up[256];
    strncpy(up, f->cwd, 255);
    char* slash = strrchr(up, '/');
    if (slash && slash != up) *slash = '\0';
    else strcpy(up, "/");
    fm_load_dir(f, up);
}

static void fm_go_back(fm_t* f)
{
    if (f->hist_len <= 0) return;
    f->hist_len--;
    char prev[256];
    strncpy(prev, f->history[f->hist_len], 255);
    /* Don't add back to history when going back */
    char saved_cwd[256];
    strncpy(saved_cwd, f->cwd, 255);
    strncpy(f->cwd, prev, 255);
    f->item_count = 0; f->selected = 0; f->scroll = 0;
    f->delete_confirm = false;

    vfs_node_t* dir = vfs_resolve_path(prev);
    if (!dir || !dir->ops || !dir->ops->readdir) {
        strncpy(f->cwd, saved_cwd, 255);
        return;
    }
    if (strcmp(prev, "/") != 0) {
        strncpy(f->items[f->item_count], "..", 255);
        f->is_dir[f->item_count++] = true;
    }
    for (int i = 0; f->item_count < FM_MAX_ITEMS; i++) {
        vfs_node_t* child = dir->ops->readdir(dir, i);
        if (!child) break;
        strncpy(f->items[f->item_count], child->name, 255);
        f->items[f->item_count][255] = '\0';
        f->is_dir[f->item_count] = (child->flags & VFS_DIRECTORY) != 0;
        f->item_count++;
    }
}

static void fm_open_selected(fm_t* f)
{
    if (f->selected < 0 || f->selected >= f->item_count) return;
    if (!f->is_dir[f->selected]) return;

    if (strcmp(f->items[f->selected], "..") == 0) {
        fm_go_up(f);
        return;
    }
    char new_path[256];
    strncpy(new_path, f->cwd, 200);
    if (strcmp(f->cwd, "/") != 0)
        strncat(new_path, "/", sizeof(new_path) - strlen(new_path) - 1);
    strncat(new_path, f->items[f->selected],
            sizeof(new_path) - strlen(new_path) - 1);
    fm_load_dir(f, new_path);
}

static void fm_delete_selected(fm_t* f)
{
    if (f->selected < 0 || f->selected >= f->item_count) return;
    if (strcmp(f->items[f->selected], "..") == 0) return;

    char full_path[256];
    strncpy(full_path, f->cwd, 200);
    if (strcmp(f->cwd, "/") != 0) strncat(full_path, "/", 2);
    strncat(full_path, f->items[f->selected],
            sizeof(full_path) - strlen(full_path) - 1);

    vfs_unlink(full_path);
    fm_load_dir(f, f->cwd);
}

/* ---- Redraw ---- */
static void fm_redraw(wid_t wid)
{
    fm_t* f = g_fm;
    if (!f || f->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    const theme_t* th = theme_current();
    draw_rect(&c, 0, 0, c.width, c.height, th->win_bg);

    /* ---- Toolbar ---- */
    draw_rect(&c, 0, 0, c.width, FM_TOOLBAR_H, th->panel_bg);
    draw_hline(&c, 0, FM_TOOLBAR_H - 1, c.width, th->panel_border);

    /* Back button */
    bool can_back = f->hist_len > 0;
    uint32_t btn_bg = can_back ? th->btn_normal : th->panel_border;
    draw_rect_rounded(&c, 4, 4, 34, FM_TOOLBAR_H - 8, 3, btn_bg);
    draw_string_centered(&c, 4, 4, 34, FM_TOOLBAR_H - 8,
                         "<-", can_back ? th->btn_text : th->text_disabled,
                         rgba(0,0,0,0));

    /* Up button */
    bool can_up = strcmp(f->cwd, "/") != 0;
    uint32_t up_bg = can_up ? th->btn_normal : th->panel_border;
    draw_rect_rounded(&c, 42, 4, 34, FM_TOOLBAR_H - 8, 3, up_bg);
    draw_string_centered(&c, 42, 4, 34, FM_TOOLBAR_H - 8,
                         "Up", can_up ? th->btn_text : th->text_disabled,
                         rgba(0,0,0,0));

    /* Path bar */
    int path_x = 80, path_w = c.width - path_x - 50;
    draw_rect(&c, path_x, 5, path_w, FM_TOOLBAR_H - 10, th->win_bg);
    draw_rect_outline(&c, path_x, 5, path_w, FM_TOOLBAR_H - 10,
                      1, th->panel_border);
    draw_string(&c, path_x + 6, 5 + (FM_TOOLBAR_H - 10 - FONT_H) / 2,
                f->cwd, th->text_primary, rgba(0,0,0,0));

    /* Refresh button */
    int ref_x = path_x + path_w + 4;
    draw_rect_rounded(&c, ref_x, 4, 40, FM_TOOLBAR_H - 8, 3, th->btn_normal);
    draw_string_centered(&c, ref_x, 4, 40, FM_TOOLBAR_H - 8,
                         "Ref", th->btn_text, rgba(0,0,0,0));

    /* ---- Sidebar ---- */
    int sb_y = FM_TOOLBAR_H;
    int sb_h = c.height - FM_TOOLBAR_H - FM_STATUS_H;
    draw_rect(&c, 0, sb_y, FM_SIDEBAR_W, sb_h, th->panel_bg);
    draw_vline(&c, FM_SIDEBAR_W, sb_y, sb_h, th->panel_border);

    /* Sidebar header */
    draw_rect(&c, 0, sb_y, FM_SIDEBAR_W, FM_ITEM_H, th->panel_header);
    draw_string(&c, 6, sb_y + (FM_ITEM_H - FONT_H) / 2,
                "Places", th->text_on_accent, rgba(0,0,0,0));

    int sy = sb_y + FM_ITEM_H;
    for (int i = 0; i < SIDEBAR_COUNT && g_sidebar[i].label; i++) {
        bool active = strcmp(f->cwd, g_sidebar[i].path) == 0;
        if (active) draw_rect(&c, 0, sy, FM_SIDEBAR_W, FM_ITEM_H, th->selection);
        uint32_t fg = active ? th->selection_text : th->text_primary;
        draw_string(&c, 6, sy + (FM_ITEM_H - FONT_H) / 2,
                    g_sidebar[i].label, fg, rgba(0,0,0,0));
        sy += FM_ITEM_H;
    }

    /* ---- File list ---- */
    int fx = FM_SIDEBAR_W + 1;
    int fw = c.width - fx;
    int fy = FM_TOOLBAR_H;
    int fh = c.height - FM_TOOLBAR_H - FM_STATUS_H;

    /* Column headers */
    draw_rect(&c, fx, fy, fw, FM_ITEM_H, th->row_alt);
    draw_hline(&c, fx, fy + FM_ITEM_H - 1, fw, th->panel_border);
    draw_string(&c, fx + 28, fy + (FM_ITEM_H - FONT_H) / 2, "Name",
                th->text_secondary, rgba(0,0,0,0));
    draw_string(&c, fx + fw - 60, fy + (FM_ITEM_H - FONT_H) / 2, "Type",
                th->text_secondary, rgba(0,0,0,0));
    fy += FM_ITEM_H;

    /* File entries */
    int list_h  = fh - FM_ITEM_H;
    int visible = list_h / FM_ITEM_H;

    for (int i = 0; i < visible && (i + f->scroll) < f->item_count; i++) {
        int idx = i + f->scroll;
        int ey  = fy + i * FM_ITEM_H;
        bool sel = (idx == f->selected);

        if (sel) {
            draw_rect(&c, fx, ey, fw, FM_ITEM_H, th->selection);
        } else if (i % 2 == 1) {
            draw_rect(&c, fx, ey, fw, FM_ITEM_H, th->row_alt);
        }

        /* File icon (colored square) */
        uint32_t icon_col = f->is_dir[idx]
                            ? th->accent
                            : th->panel_border;
        draw_rect_rounded(&c, fx + 4, ey + 4, 16, FM_ITEM_H - 8, 2, icon_col);
        /* "/" indicator for dirs */
        if (f->is_dir[idx])
            draw_string_centered(&c, fx + 4, ey + 4, 16, FM_ITEM_H - 8,
                                 "/", th->text_on_accent, rgba(0,0,0,0));

        /* Name */
        uint32_t fg = sel ? th->selection_text :
                      (f->is_dir[idx] ? th->accent : th->text_primary);
        draw_string(&c, fx + 26, ey + (FM_ITEM_H - FONT_H) / 2,
                    f->items[idx], fg, rgba(0,0,0,0));

        /* Type label */
        const char* type_s = f->is_dir[idx] ? "Folder" : "File";
        draw_string(&c, fx + fw - 58, ey + (FM_ITEM_H - FONT_H) / 2,
                    type_s, sel ? th->selection_text : th->text_secondary,
                    rgba(0,0,0,0));

        draw_hline(&c, fx, ey + FM_ITEM_H - 1, fw, th->panel_border);
    }

    /* Delete confirmation banner */
    if (f->delete_confirm && f->delete_idx >= 0 && f->delete_idx < f->item_count) {
        int dy = c.height - FM_STATUS_H - 40;
        draw_rect(&c, fx, dy, fw, 40, th->error);
        char msg[128];
        snprintf(msg, sizeof(msg),
                  "Delete '%s'? Press Delete again to confirm, Esc to cancel.",
                  f->items[f->delete_idx]);
        draw_string(&c, fx + 8, dy + (40 - FONT_H) / 2, msg,
                    th->text_on_accent, rgba(0,0,0,0));
    }

    /* ---- Status bar ---- */
    int st_y = c.height - FM_STATUS_H;
    draw_hline(&c, 0, st_y, c.width, th->panel_border);
    draw_rect(&c, 0, st_y + 1, c.width, FM_STATUS_H - 1, th->panel_bg);

    char status[128];
    if (f->selected >= 0 && f->selected < f->item_count) {
        snprintf(status, sizeof(status),
                  "%d items   |   Selected: %s%s",
                  f->item_count,
                  f->items[f->selected],
                  f->is_dir[f->selected] ? "/" : "");
    } else {
        snprintf(status, sizeof(status), "%d items", f->item_count);
    }
    draw_string(&c, 8, st_y + (FM_STATUS_H - FONT_H) / 2,
                status, th->text_secondary, rgba(0,0,0,0));

    /* Right side: keyboard hint */
    const char* hint = "Enter: Open   Del: Delete   Backspace: Up";
    draw_string(&c, c.width - (int)strlen(hint) * FONT_W - 8,
                st_y + (FM_STATUS_H - FONT_H) / 2,
                hint, th->text_disabled, rgba(0,0,0,0));
}

/* ---- Event handler ---- */
static void fm_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    fm_t* f = g_fm;
    if (!f || f->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT:
        fm_redraw(wid);
        break;

    case GUI_EVENT_MOUSE_DOWN: {
        int mx = evt->mouse.x;
        int my = evt->mouse.y;

        f->delete_confirm = false;

        /* Toolbar buttons */
        if (my < FM_TOOLBAR_H) {
            if (mx >= 4 && mx < 38)  { fm_go_back(f); fm_redraw(wid); break; }
            if (mx >= 42 && mx < 76) { fm_go_up(f);   fm_redraw(wid); break; }
            /* Refresh */
            {
                canvas_t c2 = wm_client_canvas(wid);
                int path_w2 = c2.width - 80 - 50;
                int ref_x   = 80 + path_w2 + 4;
                if (mx >= ref_x && mx < ref_x + 40) {
                    fm_load_dir(f, f->cwd);
                    fm_redraw(wid);
                    break;
                }
            }
            break;
        }

        /* Sidebar clicks */
        if (mx < FM_SIDEBAR_W) {
            int iy = (my - FM_TOOLBAR_H - FM_ITEM_H) / FM_ITEM_H;
            if (iy >= 0 && iy < SIDEBAR_COUNT && g_sidebar[iy].path) {
                fm_load_dir(f, g_sidebar[iy].path);
                fm_redraw(wid);
            }
            break;
        }

        /* File list click */
        int fy_start = FM_TOOLBAR_H + FM_ITEM_H;
        int row = (my - fy_start) / FM_ITEM_H;
        if (row >= 0) {
            int idx = row + f->scroll;
            if (idx >= 0 && idx < f->item_count) {
                /* Double-click detection */
                uint32_t now = timer_get_ticks();
                bool dbl = (idx == f->last_clicked &&
                            now - f->last_click_tick < (uint32_t)(TIMER_FREQ / 3));
                f->last_clicked    = idx;
                f->last_click_tick = now;
                f->selected        = idx;

                if (dbl && f->is_dir[idx]) {
                    fm_open_selected(f);
                }
                fm_redraw(wid);
            }
        }
        break;
    }

    case GUI_EVENT_KEY_DOWN: {
        uint8_t kc = evt->key.keycode;
        char ch    = evt->key.ch;

        if (kc == KEY_UP_ARROW) {
            if (f->selected > 0) f->selected--;
            if (f->selected < f->scroll) f->scroll = f->selected;
            f->delete_confirm = false;
            fm_redraw(wid);
        } else if (kc == KEY_DOWN_ARROW) {
            if (f->selected < f->item_count - 1) f->selected++;
            /* Auto-scroll */
            canvas_t c = wm_client_canvas(wid);
            int visible = (c.height - FM_TOOLBAR_H - FM_STATUS_H - FM_ITEM_H * 2) / FM_ITEM_H;
            if (f->selected >= f->scroll + visible) f->scroll = f->selected - visible + 1;
            f->delete_confirm = false;
            fm_redraw(wid);
        } else if (kc == KEY_ENTER || ch == '\n') {
            fm_open_selected(f);
            fm_redraw(wid);
        } else if (kc == KEY_BACKSPACE || ch == '\b') {
            fm_go_up(f);
            fm_redraw(wid);
        } else if (kc == KEY_DELETE || ch == 127) {
            if (!f->delete_confirm) {
                /* First delete: ask for confirmation */
                f->delete_confirm = true;
                f->delete_idx     = f->selected;
                fm_redraw(wid);
            } else {
                /* Second delete: do it */
                fm_delete_selected(f);
                f->delete_confirm = false;
                fm_redraw(wid);
            }
        } else if (ch == 0x1B) {   /* Escape */
            f->delete_confirm = false;
            fm_redraw(wid);
        }
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(f);
        g_fm = NULL;
        break;

    default: break;
    }
}

wid_t app_filemanager_create(void)
{
    if (g_fm) { wm_raise(g_fm->wid); return g_fm->wid; }

    fm_t* f = (fm_t*)kmalloc(sizeof(fm_t));
    if (!f) return -1;
    memset(f, 0, sizeof(fm_t));
    f->last_clicked = -1;

    wid_t wid = wm_create_window("File Manager — " OS_NAME,
                                  90, 70, FM_W, FM_H,
                                  fm_on_event, NULL);
    if (wid < 0) { kfree(f); return -1; }

    f->wid = wid;
    g_fm   = f;

    fm_load_dir(f, "/");
    fm_redraw(wid);
    return wid;
}
