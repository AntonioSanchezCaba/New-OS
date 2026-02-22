/*
 * surfaces/launcher.c — Aether Launcher Overlay
 *
 * Overlay surface (type SURF_OVERLAY).
 * Centered on screen, shows icons for all core surfaces.
 * Opened by Alt+W gesture, closed by same or Escape.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/context.h>
#include <aether/input.h>
#include <aether/field.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <gui/event.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Layout
 * ========================================================= */
#define LN_TITLE_H   44
#define LN_GRID_COLS 4
#define LN_ITEM_W    100
#define LN_ITEM_H    90
#define LN_ITEM_GAP  12
#define LN_PAD       20
#define LN_ICON_SZ   40
#define LN_SEARCH_H  36

/* Colours */
#define LN_BG         ACOLOR(0x0C, 0x10, 0x18, 0xE8)
#define LN_TITLE_FG   ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define LN_ITEM_BG    ACOLOR(0x1A, 0x22, 0x32, 0xFF)
#define LN_ITEM_HOV   ACOLOR(0x25, 0x38, 0x58, 0xFF)
#define LN_ITEM_SEL   ACOLOR(0x20, 0x48, 0x80, 0xFF)
#define LN_ICON_BG    ACOLOR(0x22, 0x40, 0x70, 0xFF)
#define LN_ICON_FG    ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define LN_TEXT_FG    ACOLOR(0xCC, 0xCC, 0xCC, 0xFF)
#define LN_SEARCH_BG  ACOLOR(0x14, 0x1C, 0x2A, 0xFF)
#define LN_SEARCH_FG  ACOLOR(0xDD, 0xDD, 0xFF, 0xFF)
#define LN_BORDER     ACOLOR(0x28, 0x38, 0x50, 0xFF)
#define LN_CURSOR     ACOLOR(0x40, 0x90, 0xFF, 0xFF)

/* =========================================================
 * App entries
 * ========================================================= */
typedef struct {
    const char* name;
    const char* icon;   /* 1-3 char abbreviation */
    acolor_t    color;  /* icon background tint */
    int         field_slot; /* index in context's field[] to jump to */
} launcher_app_t;

static const launcher_app_t g_apps[] = {
    { "Terminal",   "T",  ACOLOR(0x20, 0x40, 0x80, 0xFF), 0 },
    { "Explorer",   "E",  ACOLOR(0x20, 0x70, 0x40, 0xFF), 1 },
    { "Sys Monitor","M",  ACOLOR(0x70, 0x30, 0x80, 0xFF), 2 },
    { "Settings",   "S",  ACOLOR(0x60, 0x40, 0x20, 0xFF), 3 },
};
#define LN_APP_COUNT  4

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    int       hover;
    int       selected;
    char      search[64];
    int       search_len;
    uint32_t  surf_w;
    uint32_t  surf_h;
    sid_t     sid;
} ln_state_t;

static ln_state_t g_ln;

/* =========================================================
 * Filtering
 * ========================================================= */
static bool app_matches(int i)
{
    if (g_ln.search_len == 0) return true;
    /* Case-insensitive substring match */
    const char* name = g_apps[i].name;
    const char* q    = g_ln.search;
    /* simple: check if q appears in name */
    int qlen = g_ln.search_len;
    int nlen = (int)strlen(name);
    for (int s = 0; s <= nlen - qlen; s++) {
        bool match = true;
        for (int k = 0; k < qlen; k++) {
            char a = name[s+k], b = q[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/* =========================================================
 * Render
 * ========================================================= */
static void ln_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                      void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = w, .height = h };

    /* Semi-transparent background */
    draw_rect(&c, 0, 0, (int)w, (int)h, LN_BG);

    /* Title */
    draw_string(&c, LN_PAD, (LN_TITLE_H - FONT_H)/2,
                "Launcher", LN_TITLE_FG, ACOLOR(0,0,0,0));

    /* Hint */
    const char* hint = "Alt+W or Esc to close";
    draw_string(&c, (int)w - (int)strlen(hint)*FONT_W - LN_PAD,
                (LN_TITLE_H - FONT_H)/2,
                hint, ACOLOR(0x40,0x50,0x60,0xFF), ACOLOR(0,0,0,0));

    draw_rect(&c, 0, LN_TITLE_H - 1, (int)w, 1, LN_BORDER);

    /* Search box */
    int sb_y = LN_TITLE_H + 4;
    draw_rect(&c, LN_PAD, sb_y, (int)w - 2*LN_PAD, LN_SEARCH_H, LN_SEARCH_BG);
    draw_rect_outline(&c, LN_PAD, sb_y, (int)w - 2*LN_PAD, LN_SEARCH_H, 1, LN_BORDER);

    char search_display[80];
    snprintf(search_display, sizeof(search_display), "Search: %s", g_ln.search);
    draw_string(&c, LN_PAD + 8, sb_y + (LN_SEARCH_H - FONT_H)/2,
                search_display, LN_SEARCH_FG, ACOLOR(0,0,0,0));
    /* Cursor blink */
    static uint32_t blink = 0;
    blink++;
    if ((blink / 30) % 2 == 0) {
        int cur_x = LN_PAD + 8 + (int)strlen(search_display) * FONT_W;
        draw_rect(&c, cur_x, sb_y + 4, 2, LN_SEARCH_H - 8, LN_CURSOR);
    }

    /* App grid */
    int grid_y = sb_y + LN_SEARCH_H + LN_PAD;
    int col = 0, row_y = grid_y;
    int total_w = LN_GRID_COLS * LN_ITEM_W + (LN_GRID_COLS-1) * LN_ITEM_GAP;
    int grid_x0 = ((int)w - total_w) / 2;

    for (int i = 0; i < LN_APP_COUNT; i++) {
        if (!app_matches(i)) continue;

        int ix = grid_x0 + col * (LN_ITEM_W + LN_ITEM_GAP);
        int iy = row_y;

        acolor_t ibg = (i == g_ln.selected) ? LN_ITEM_SEL
                     : (i == g_ln.hover)    ? LN_ITEM_HOV
                     : LN_ITEM_BG;
        draw_rect(&c, ix, iy, LN_ITEM_W, LN_ITEM_H, ibg);
        draw_rect_outline(&c, ix, iy, LN_ITEM_W, LN_ITEM_H, 1, LN_BORDER);

        /* Icon circle area */
        int icx = ix + (LN_ITEM_W - LN_ICON_SZ) / 2;
        int icy = iy + 8;
        draw_rect(&c, icx, icy, LN_ICON_SZ, LN_ICON_SZ, g_apps[i].color);
        int icon_tx = icx + (LN_ICON_SZ - (int)strlen(g_apps[i].icon) * FONT_W) / 2;
        int icon_ty = icy + (LN_ICON_SZ - FONT_H) / 2;
        draw_string(&c, icon_tx, icon_ty, g_apps[i].icon,
                    LN_ICON_FG, ACOLOR(0,0,0,0));

        /* Name */
        int nlx = ix + (LN_ITEM_W - (int)strlen(g_apps[i].name)*FONT_W) / 2;
        int nly = iy + LN_ICON_SZ + 12;
        draw_string(&c, nlx, nly, g_apps[i].name, LN_TEXT_FG, ACOLOR(0,0,0,0));

        col++;
        if (col >= LN_GRID_COLS) {
            col = 0;
            row_y += LN_ITEM_H + LN_ITEM_GAP;
        }
    }
}

/* =========================================================
 * Input
 * ========================================================= */
static void ln_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;

    if (ev->type == INPUT_POINTER) {
        int mx = ev->pointer.x;
        int my = ev->pointer.y;

        int sb_y   = LN_TITLE_H + 4;
        int grid_y = sb_y + LN_SEARCH_H + LN_PAD;
        int total_w = LN_GRID_COLS * LN_ITEM_W + (LN_GRID_COLS-1) * LN_ITEM_GAP;
        int grid_x0 = ((int)g_ln.surf_w - total_w) / 2;

        /* Hover detection */
        g_ln.hover = -1;
        int col = 0, row_y = grid_y;
        for (int i = 0; i < LN_APP_COUNT; i++) {
            if (!app_matches(i)) continue;
            int ix = grid_x0 + col * (LN_ITEM_W + LN_ITEM_GAP);
            int iy = row_y;
            if (mx >= ix && mx < ix + LN_ITEM_W &&
                my >= iy && my < iy + LN_ITEM_H)
                g_ln.hover = i;
            col++;
            if (col >= LN_GRID_COLS) { col = 0; row_y += LN_ITEM_H + LN_ITEM_GAP; }
        }

        /* Click */
        bool click = (ev->pointer.buttons & IBTN_LEFT) &&
                     !(ev->pointer.prev_buttons & IBTN_LEFT);
        if (click && g_ln.hover >= 0) {
            g_ln.selected = g_ln.hover;
            /* Jump to that surface in context and close overlay */
            context_goto(g_apps[g_ln.selected].field_slot);
            are_pop_overlay();
            surface_invalidate(id);
            return;
        }
        surface_invalidate(id);
    }

    if (ev->type == INPUT_KEY && ev->key.down) {
        char ch = ev->key.ch;
        int  kc = ev->key.keycode;

        if (kc == KEY_ESCAPE || ch == 0x1B) {
            are_pop_overlay();
            return;
        } else if (kc == KEY_ENTER || ch == '\n' || ch == '\r') {
            if (g_ln.selected >= 0) {
                context_goto(g_apps[g_ln.selected].field_slot);
                are_pop_overlay();
            }
        } else if (kc == KEY_BACKSPACE || ch == '\b') {
            if (g_ln.search_len > 0) {
                g_ln.search[--g_ln.search_len] = '\0';
                surface_invalidate(id);
            }
        } else if (ch >= 0x20 && ch < 0x7F && g_ln.search_len < 63) {
            g_ln.search[g_ln.search_len++] = ch;
            g_ln.search[g_ln.search_len] = '\0';
            surface_invalidate(id);
        } else if (kc == KEY_LEFT_ARROW) {
            if (g_ln.selected > 0) g_ln.selected--;
            surface_invalidate(id);
        } else if (kc == KEY_RIGHT_ARROW) {
            if (g_ln.selected < LN_APP_COUNT - 1) g_ln.selected++;
            surface_invalidate(id);
        } else if (kc == KEY_UP_ARROW) {
            g_ln.selected = (g_ln.selected - LN_GRID_COLS + LN_APP_COUNT) % LN_APP_COUNT;
            surface_invalidate(id);
        } else if (kc == KEY_DOWN_ARROW) {
            g_ln.selected = (g_ln.selected + LN_GRID_COLS) % LN_APP_COUNT;
            surface_invalidate(id);
        }
    }
}

/* =========================================================
 * Init
 * ========================================================= */
void surface_launcher_init(uint32_t w, uint32_t h)
{
    memset(&g_ln, 0, sizeof(g_ln));
    g_ln.surf_w  = w;
    g_ln.surf_h  = h;
    g_ln.hover   = -1;
    g_ln.selected = 0;

    g_ln.sid = are_add_surface(SURF_OVERLAY, w, h,
                               "Launcher", "L",
                               ln_render, ln_input, NULL, NULL);
}
