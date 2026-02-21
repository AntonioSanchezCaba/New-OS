/*
 * apps/filemanager.c - Graphical file manager
 *
 * Displays the VFS directory tree in a two-pane view:
 *   Left: directory tree
 *   Right: file listing for selected directory
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

#define FM_W         600
#define FM_H         380
#define FM_PANEL_W   160     /* Left panel width */
#define FM_ITEM_H    20      /* Row height */
#define FM_MAX_ITEMS 32

#define FM_BG        COLOR_WIN_BG
#define FM_PANEL_BG  rgb(0xE0, 0xE8, 0xF0)
#define FM_SEL_COLOR COLOR_HIGHLIGHT
#define FM_FG        COLOR_TEXT_DARK
#define FM_DIR_FG    COLOR_BLUE

typedef struct {
    wid_t wid;
    char  current_path[256];
    char  items[FM_MAX_ITEMS][256];
    bool  is_dir[FM_MAX_ITEMS];
    int   item_count;
    int   selected;
    int   scroll;
} fm_t;

static fm_t* g_fm = NULL;

static void fm_load_dir(fm_t* f, const char* path)
{
    strncpy(f->current_path, path, 255);
    f->current_path[255] = '\0';
    f->item_count = 0;
    f->selected = 0;
    f->scroll = 0;

    vfs_node_t* dir = vfs_resolve_path(path);
    if (!dir || !dir->ops || !dir->ops->readdir) return;

    /* Add ".." first (unless at root) */
    if (strcmp(path, "/") != 0) {
        strncpy(f->items[f->item_count], "..", 255);
        f->is_dir[f->item_count] = true;
        f->item_count++;
    }

    for (int i = 0; f->item_count < FM_MAX_ITEMS; i++) {
        vfs_node_t* child = dir->ops->readdir(dir, i);
        if (!child) break;
        strncpy(f->items[f->item_count], child->name, 255);
        f->items[f->item_count][255] = '\0';
        f->is_dir[f->item_count] = (child->flags & VFS_FLAG_DIRECTORY) != 0;
        f->item_count++;
    }
}

static void fm_redraw(wid_t wid)
{
    fm_t* f = g_fm;
    if (!f || f->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    /* Background */
    draw_rect(&c, 0, 0, c.width, c.height, FM_BG);

    /* Left panel */
    draw_rect(&c, 0, 0, FM_PANEL_W, c.height, FM_PANEL_BG);
    draw_vline(&c, FM_PANEL_W, 0, c.height, COLOR_MID_GREY);

    /* Panel header */
    draw_rect(&c, 0, 0, FM_PANEL_W, FM_ITEM_H, COLOR_WIN_BORDER);
    draw_string(&c, 4, 3, "Directories", COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Common root dirs */
    const char* dirs[] = { "/", "/bin", "/etc", "/home", "/tmp", NULL };
    for (int i = 0; dirs[i]; i++) {
        int y = FM_ITEM_H + i * FM_ITEM_H;
        bool sel = strcmp(f->current_path, dirs[i]) == 0;
        if (sel) draw_rect(&c, 0, y, FM_PANEL_W, FM_ITEM_H, FM_SEL_COLOR);
        draw_string(&c, 8, y + 3, dirs[i],
                    sel ? COLOR_TEXT_LIGHT : FM_DIR_FG, rgba(0,0,0,0));
    }

    /* Right panel: file list */
    int rx = FM_PANEL_W + 4;
    int rw = c.width - rx - 4;

    /* Toolbar: path bar */
    draw_rect(&c, rx, 0, rw, FM_ITEM_H, rgb(0xD0, 0xD8, 0xE8));
    draw_string(&c, rx + 4, 3, f->current_path, FM_FG, rgba(0,0,0,0));
    draw_hline(&c, rx, FM_ITEM_H, rw, COLOR_MID_GREY);

    /* Column headers */
    int hy = FM_ITEM_H + 1;
    draw_rect(&c, rx, hy, rw, FM_ITEM_H - 1, rgb(0xD8, 0xD8, 0xD8));
    draw_string(&c, rx + 4, hy + 3, "Name", FM_FG, rgba(0,0,0,0));
    draw_string(&c, rx + rw - 60, hy + 3, "Type", FM_FG, rgba(0,0,0,0));
    draw_hline(&c, rx, hy + FM_ITEM_H - 1, rw, COLOR_MID_GREY);

    /* File entries */
    int visible_rows = (c.height - FM_ITEM_H * 2) / FM_ITEM_H;
    for (int i = 0; i < visible_rows && (i + f->scroll) < f->item_count; i++) {
        int idx = i + f->scroll;
        int y = FM_ITEM_H * 2 + i * FM_ITEM_H;
        bool sel = (idx == f->selected);

        if (sel) draw_rect(&c, rx, y, rw, FM_ITEM_H, FM_SEL_COLOR);
        else if (i % 2 == 1) draw_rect(&c, rx, y, rw, FM_ITEM_H, rgb(0xF8,0xF8,0xFC));

        uint32_t fg = sel ? COLOR_TEXT_LIGHT :
                      (f->is_dir[idx] ? FM_DIR_FG : FM_FG);
        draw_string(&c, rx + 4, y + 3, f->items[idx], fg, rgba(0,0,0,0));
        draw_string(&c, rx + rw - 60, y + 3,
                    f->is_dir[idx] ? "[DIR]" : "file",
                    sel ? COLOR_TEXT_LIGHT : COLOR_MID_GREY, rgba(0,0,0,0));
    }

    /* Status bar */
    draw_hline(&c, 0, c.height - FM_ITEM_H, c.width, COLOR_MID_GREY);
    draw_rect(&c, 0, c.height - FM_ITEM_H + 1, c.width, FM_ITEM_H - 1,
              rgb(0xE8,0xE8,0xF0));
    char status[64];
    /* Format: "N items in /path" - manual */
    const char* prefix = "Items: ";
    int n = f->item_count;
    char nbuf[16];
    int ni = 0;
    if (n == 0) { nbuf[0]='0'; ni=1; }
    else { while (n > 0) { nbuf[ni++]='0'+(n%10); n/=10; } }
    /* Reverse nbuf */
    for (int a=0,b=ni-1; a<b; a++,b--) {
        char tmp=nbuf[a]; nbuf[a]=nbuf[b]; nbuf[b]=tmp;
    }
    nbuf[ni] = '\0';
    strncpy(status, prefix, 7);
    strncpy(status + 7, nbuf, 15);
    strcat(status, "  ");
    strncat(status, f->current_path, sizeof(status) - strlen(status) - 1);
    draw_string(&c, 4, c.height - FM_ITEM_H + 3, status, FM_FG, rgba(0,0,0,0));
}

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

        /* Left panel click */
        if (mx < FM_PANEL_W) {
            const char* dirs[] = { "/", "/bin", "/etc", "/home", "/tmp", NULL };
            int idx = (my - FM_ITEM_H) / FM_ITEM_H;
            if (idx >= 0 && dirs[idx]) {
                fm_load_dir(f, dirs[idx]);
                fm_redraw(wid);
            }
            break;
        }

        /* Right panel click */
        int row = (my - FM_ITEM_H * 2) / FM_ITEM_H;
        if (row >= 0 && (row + f->scroll) < f->item_count) {
            f->selected = row + f->scroll;
            fm_redraw(wid);
        }
        break;
    }

    case GUI_EVENT_KEY_DOWN:
        if (evt->key.keycode == KEY_UP_ARROW) {
            if (f->selected > 0) f->selected--;
            fm_redraw(wid);
        } else if (evt->key.keycode == KEY_DOWN_ARROW) {
            if (f->selected < f->item_count - 1) f->selected++;
            fm_redraw(wid);
        } else if (evt->key.ch == '\n' || evt->key.keycode == KEY_ENTER) {
            if (f->selected < f->item_count && f->is_dir[f->selected]) {
                /* Navigate into directory */
                char new_path[256];
                if (strcmp(f->items[f->selected], "..") == 0) {
                    strncpy(new_path, f->current_path, 255);
                    new_path[255] = '\0';
                    char* slash = strrchr(new_path, '/');
                    if (slash && slash != new_path) *slash = '\0';
                    else strcpy(new_path, "/");
                } else {
                    strncpy(new_path, f->current_path, 200);
                    new_path[200] = '\0';
                    if (strcmp(f->current_path, "/") != 0)
                        strncat(new_path, "/", sizeof(new_path) - strlen(new_path) - 1);
                    strncat(new_path, f->items[f->selected],
                            sizeof(new_path) - strlen(new_path) - 1);
                }
                fm_load_dir(f, new_path);
                fm_redraw(wid);
            }
        }
        break;

    case GUI_EVENT_CLOSE:
        kfree(f);
        g_fm = NULL;
        break;

    default: break;
    }
}

wid_t app_filemanager_create(void)
{
    if (g_fm) return g_fm->wid;

    fm_t* f = (fm_t*)kmalloc(sizeof(fm_t));
    if (!f) return -1;
    memset(f, 0, sizeof(fm_t));

    wid_t wid = wm_create_window("File Manager",
                                  100, 80, FM_W, FM_H,
                                  fm_on_event, NULL);
    if (wid < 0) { kfree(f); return -1; }

    f->wid = wid;
    g_fm = f;

    fm_load_dir(f, "/");
    fm_redraw(wid);
    return wid;
}
