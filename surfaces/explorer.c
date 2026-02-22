/*
 * surfaces/explorer.c — Aether File Explorer Surface
 *
 * Card-grid view of VFS directories.
 * Navigation: click cards, back button, keyboard arrows.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <aether/ui.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <gui/event.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* VFS path max (not defined in vfs.h) */
#ifndef VFS_PATH_MAX
#define VFS_PATH_MAX 512
#endif

/* =========================================================
 * Layout constants
 * ========================================================= */
#define EX_TITLE_H   40
#define EX_TOOLBAR_H 36
#define EX_CARD_W    120
#define EX_CARD_H    72
#define EX_CARD_GAP  12
#define EX_PAD       16
#define EX_MAX_ITEMS 128

/* Colours */
#define EX_BG         ACOLOR(0x10, 0x14, 0x1A, 0xFF)
#define EX_CARD_BG    ACOLOR(0x1C, 0x22, 0x2E, 0xFF)
#define EX_CARD_HOV   ACOLOR(0x25, 0x35, 0x50, 0xFF)
#define EX_CARD_SEL   ACOLOR(0x1A, 0x3A, 0x6A, 0xFF)
#define EX_TITLE_BG   ACOLOR(0x14, 0x18, 0x22, 0xFF)
#define EX_TITLE_FG   ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define EX_TEXT_FG    ACOLOR(0xCC, 0xCC, 0xCC, 0xFF)
#define EX_DIM_FG     ACOLOR(0x66, 0x77, 0x88, 0xFF)
#define EX_TOOLBAR_BG ACOLOR(0x16, 0x1A, 0x24, 0xFF)
#define EX_PATH_FG    ACOLOR(0x70, 0x90, 0xB0, 0xFF)
#define EX_BORDER     ACOLOR(0x28, 0x35, 0x4A, 0xFF)

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    char name[VFS_NAME_MAX];
    bool is_dir;
    uint32_t size;
} ex_entry_t;

typedef struct {
    char         cwd[VFS_PATH_MAX];
    ex_entry_t   items[EX_MAX_ITEMS];
    int          item_count;
    int          selected;
    int          hover;
    int          scroll_y;
    uint32_t     surf_w;
    uint32_t     surf_h;
    sid_t        sid;
    bool         dirty_items;
} ex_state_t;

static ex_state_t g_ex;

/* =========================================================
 * VFS listing
 * ========================================================= */
static void ex_list_dir(void)
{
    g_ex.item_count = 0;
    g_ex.selected   = -1;
    g_ex.scroll_y   = 0;

    vfs_node_t* dir_node = vfs_resolve_path(g_ex.cwd);
    if (!dir_node) return;

    vfs_dirent_t ent;
    uint32_t idx = 0;
    while (vfs_readdir(dir_node, idx, &ent) == 0 &&
           g_ex.item_count < EX_MAX_ITEMS) {
        /* Skip "." and ".." */
        if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
            (ent.name[1] == '.' && ent.name[2] == '\0'))) {
            idx++;
            continue;
        }
        strncpy(g_ex.items[g_ex.item_count].name, ent.name, VFS_NAME_MAX-1);
        g_ex.items[g_ex.item_count].name[VFS_NAME_MAX-1] = '\0';
        g_ex.items[g_ex.item_count].is_dir = (ent.type == VFS_TYPE_DIR);
        g_ex.items[g_ex.item_count].size   = 0;  /* size not in vfs_dirent_t */
        g_ex.item_count++;
        idx++;
    }
}

/* =========================================================
 * Card geometry
 * ========================================================= */
static void card_rect(int idx, int* ox, int* oy, int surf_w, int content_y)
{
    int cols = ((int)surf_w - 2 * EX_PAD + EX_CARD_GAP) / (EX_CARD_W + EX_CARD_GAP);
    if (cols < 1) cols = 1;
    int col = idx % cols;
    int row = idx / cols;
    *ox = EX_PAD + col * (EX_CARD_W + EX_CARD_GAP);
    *oy = content_y + EX_PAD + row * (EX_CARD_H + EX_CARD_GAP) - g_ex.scroll_y;
}

static int card_hit(int mx, int my, int surf_w, int content_y)
{
    for (int i = 0; i < g_ex.item_count; i++) {
        int cx, cy;
        card_rect(i, &cx, &cy, surf_w, content_y);
        if (mx >= cx && mx < cx + EX_CARD_W &&
            my >= cy && my < cy + EX_CARD_H)
            return i;
    }
    return -1;
}

/* =========================================================
 * Render
 * ========================================================= */
static void ex_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                      void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = w, .height = h };

    /* Background */
    draw_rect(&c, 0, 0, (int)w, (int)h, EX_BG);

    /* Title bar */
    draw_rect(&c, 0, 0, (int)w, EX_TITLE_H, EX_TITLE_BG);
    draw_string(&c, EX_PAD, (EX_TITLE_H - FONT_H) / 2,
                "File Explorer", EX_TITLE_FG, ACOLOR(0,0,0,0));

    /* Toolbar */
    int tb_y = EX_TITLE_H;
    draw_rect(&c, 0, tb_y, (int)w, EX_TOOLBAR_H, EX_TOOLBAR_BG);

    /* Back button */
    draw_rect(&c, EX_PAD, tb_y + 6, 40, EX_TOOLBAR_H - 12, EX_CARD_BG);
    draw_string(&c, EX_PAD + 10, tb_y + (EX_TOOLBAR_H - FONT_H) / 2,
                "<", EX_TEXT_FG, ACOLOR(0,0,0,0));

    /* Path */
    draw_string(&c, EX_PAD + 52, tb_y + (EX_TOOLBAR_H - FONT_H) / 2,
                g_ex.cwd, EX_PATH_FG, ACOLOR(0,0,0,0));

    /* Divider */
    int content_y = EX_TITLE_H + EX_TOOLBAR_H;
    draw_rect(&c, 0, content_y - 1, (int)w, 1, EX_BORDER);

    /* File cards */
    for (int i = 0; i < g_ex.item_count; i++) {
        int cx, cy;
        card_rect(i, &cx, &cy, w, content_y);
        if (cy + EX_CARD_H < 0 || cy > (int)h) continue;

        acolor_t bg = (i == g_ex.selected) ? EX_CARD_SEL
                    : (i == g_ex.hover)    ? EX_CARD_HOV
                    : EX_CARD_BG;

        /* Card background with rounded corners */
        draw_rect(&c, cx, cy, EX_CARD_W, EX_CARD_H, bg);
        draw_rect_outline(&c, cx, cy, EX_CARD_W, EX_CARD_H, 1, EX_BORDER);

        /* Icon character */
        const char* icon = g_ex.items[i].is_dir ? "[D]" : "[F]";
        acolor_t icon_color = g_ex.items[i].is_dir
                              ? ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
                              : ACOLOR(0xCC, 0xCC, 0xFF, 0xFF);
        int icon_x = cx + (EX_CARD_W - (int)strlen(icon) * FONT_W) / 2;
        draw_string(&c, icon_x, cy + 12, icon, icon_color, ACOLOR(0,0,0,0));

        /* Name (truncated) */
        char name_buf[20];
        strncpy(name_buf, g_ex.items[i].name, 18);
        name_buf[18] = '\0';
        if (strlen(g_ex.items[i].name) > 18) {
            name_buf[15] = '.'; name_buf[16] = '.'; name_buf[17] = '.';
        }
        int nx = cx + (EX_CARD_W - (int)strlen(name_buf) * FONT_W) / 2;
        draw_string(&c, nx, cy + 36, name_buf, EX_TEXT_FG, ACOLOR(0,0,0,0));

        /* Size or type */
        if (!g_ex.items[i].is_dir) {
            char size_buf[12];
            uint32_t sz = g_ex.items[i].size;
            if (sz >= 1024*1024)
                snprintf(size_buf, sizeof(size_buf), "%u MB", sz/1024/1024);
            else if (sz >= 1024)
                snprintf(size_buf, sizeof(size_buf), "%u KB", sz/1024);
            else
                snprintf(size_buf, sizeof(size_buf), "%u B", sz);
            int sx = cx + (EX_CARD_W - (int)strlen(size_buf) * FONT_W) / 2;
            draw_string(&c, sx, cy + 52, size_buf, EX_DIM_FG, ACOLOR(0,0,0,0));
        } else {
            int sx = cx + (EX_CARD_W - 3 * FONT_W) / 2;
            draw_string(&c, sx, cy + 52, "dir", EX_DIM_FG, ACOLOR(0,0,0,0));
        }
    }

    /* Empty state */
    if (g_ex.item_count == 0) {
        draw_string(&c, (int)w/2 - 4*FONT_W, (int)h/2,
                    "Empty", EX_DIM_FG, ACOLOR(0,0,0,0));
    }
}

/* =========================================================
 * Input
 * ========================================================= */
static void ex_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    int content_y = EX_TITLE_H + EX_TOOLBAR_H;

    if (ev->type == INPUT_POINTER) {
        int mx = ev->pointer.x;
        int my = ev->pointer.y;

        /* Back button hit */
        bool back_hit = (mx >= EX_PAD && mx < EX_PAD + 40 &&
                         my >= EX_TITLE_H + 6 && my < EX_TITLE_H + EX_TOOLBAR_H - 6);

        g_ex.hover = card_hit(mx, my, g_ex.surf_w, content_y);

        /* Scroll */
        if (ev->pointer.scroll != 0) {
            g_ex.scroll_y += ev->pointer.scroll * (EX_CARD_H + EX_CARD_GAP) / 3;
            if (g_ex.scroll_y < 0) g_ex.scroll_y = 0;
            surface_invalidate(id);
        }

        /* Left click */
        bool click = (ev->pointer.buttons & IBTN_LEFT) &&
                     !(ev->pointer.prev_buttons & IBTN_LEFT);
        if (click) {
            if (back_hit) {
                /* Navigate to parent */
                char* slash = strrchr(g_ex.cwd, '/');
                if (slash && slash != g_ex.cwd) {
                    *slash = '\0';
                } else {
                    strcpy(g_ex.cwd, "/");
                }
                ex_list_dir();
                surface_invalidate(id);
            } else if (g_ex.hover >= 0) {
                g_ex.selected = g_ex.hover;
                if (g_ex.items[g_ex.selected].is_dir) {
                    /* Enter directory */
                    char next[VFS_PATH_MAX];
                    if (strcmp(g_ex.cwd, "/") == 0)
                        snprintf(next, VFS_PATH_MAX, "/%s",
                                 g_ex.items[g_ex.selected].name);
                    else
                        snprintf(next, VFS_PATH_MAX, "%s/%s",
                                 g_ex.cwd, g_ex.items[g_ex.selected].name);
                    strncpy(g_ex.cwd, next, VFS_PATH_MAX-1);
                    ex_list_dir();
                }
                surface_invalidate(id);
            }
        }
    }

    if (ev->type == INPUT_KEY && ev->key.down) {
        /* Keyboard navigation */
        int cols = ((int)g_ex.surf_w - 2*EX_PAD + EX_CARD_GAP)
                  / (EX_CARD_W + EX_CARD_GAP);
        if (cols < 1) cols = 1;
        switch (ev->key.keycode) {
        case KEY_RIGHT_ARROW:
            if (g_ex.selected < g_ex.item_count - 1) g_ex.selected++;
            break;
        case KEY_LEFT_ARROW:
            if (g_ex.selected > 0) g_ex.selected--;
            break;
        case KEY_DOWN_ARROW:
            if (g_ex.selected + cols < g_ex.item_count)
                g_ex.selected += cols;
            break;
        case KEY_UP_ARROW:
            if (g_ex.selected - cols >= 0)
                g_ex.selected -= cols;
            break;
        case KEY_ENTER:
            if (g_ex.selected >= 0 && g_ex.items[g_ex.selected].is_dir) {
                char next[VFS_PATH_MAX];
                if (strcmp(g_ex.cwd, "/") == 0)
                    snprintf(next, VFS_PATH_MAX, "/%s",
                             g_ex.items[g_ex.selected].name);
                else
                    snprintf(next, VFS_PATH_MAX, "%s/%s",
                             g_ex.cwd, g_ex.items[g_ex.selected].name);
                strncpy(g_ex.cwd, next, VFS_PATH_MAX-1);
                ex_list_dir();
            }
            break;
        }
        surface_invalidate(id);
    }
}

/* =========================================================
 * Init
 * ========================================================= */
void surface_explorer_init(uint32_t w, uint32_t h)
{
    memset(&g_ex, 0, sizeof(g_ex));
    strcpy(g_ex.cwd, "/");
    g_ex.surf_w = w;
    g_ex.surf_h = h;
    g_ex.selected = -1;
    g_ex.hover = -1;

    ex_list_dir();

    g_ex.sid = are_add_surface(SURF_APP, w, h,
                               "Explorer", "E",
                               ex_render, ex_input, NULL, NULL);
}
