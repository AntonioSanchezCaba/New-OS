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
#include <kernel/apkg.h>

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

/* Forward declarations for floating-window apps */
extern sid_t surface_calculator_open(void);
extern sid_t surface_clock_open(void);

/* =========================================================
 * App entries
 * field_slot >= 0: navigate to that ARE field slot.
 * field_slot == -1: call launch() to open a SURF_FLOAT window.
 * ========================================================= */
typedef sid_t (*app_launch_fn)(void);

typedef struct {
    const char*   name;
    const char*   icon;       /* ≤3-char abbreviation shown in icon cell */
    acolor_t      color;      /* icon background tint */
    int           field_slot; /* >= 0: field nav; -1: use launch fn */
    app_launch_fn launch;     /* used when field_slot == -1 */
} launcher_app_t;

static const launcher_app_t g_apps[] = {
    { "Terminal",    "T",  ACOLOR(0x20, 0x40, 0x80, 0xFF), 0,  NULL                    },
    { "Explorer",    "E",  ACOLOR(0x20, 0x70, 0x40, 0xFF), 1,  NULL                    },
    { "Sys Monitor", "M",  ACOLOR(0x70, 0x30, 0x80, 0xFF), 2,  NULL                    },
    { "Settings",    "S",  ACOLOR(0x60, 0x40, 0x20, 0xFF), 3,  NULL                    },
    { "Calculator",  "Cx", ACOLOR(0x40, 0x60, 0x20, 0xFF), -1, surface_calculator_open },
    { "Clock",       "Cl", ACOLOR(0x20, 0x60, 0x60, 0xFF), -1, surface_clock_open      },
};
#define LN_APP_COUNT  6

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

/* Total items = core apps + installed packages */
static int ln_total(void) { return LN_APP_COUNT + apkg_count(); }

/* Case-insensitive substring match */
static bool str_contains_ci(const char* name, const char* q, int qlen)
{
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

/* i < LN_APP_COUNT → core app; i >= LN_APP_COUNT → installed package */
static bool app_matches(int i)
{
    if (g_ln.search_len == 0) return true;
    const char* name;
    if (i < LN_APP_COUNT) {
        name = g_apps[i].name;
    } else {
        const apkg_record_t* p = apkg_get(i - LN_APP_COUNT);
        name = p ? p->name : "";
    }
    return str_contains_ci(name, g_ln.search, g_ln.search_len);
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

    int total = ln_total();
    for (int i = 0; i < total; i++) {
        if (!app_matches(i)) continue;

        int ix = grid_x0 + col * (LN_ITEM_W + LN_ITEM_GAP);
        int iy = row_y;
        /* Stop drawing if we've gone off the bottom */
        if (iy + LN_ITEM_H > (int)h) break;

        acolor_t ibg = (i == g_ln.selected) ? LN_ITEM_SEL
                     : (i == g_ln.hover)    ? LN_ITEM_HOV
                     : LN_ITEM_BG;
        draw_rect(&c, ix, iy, LN_ITEM_W, LN_ITEM_H, ibg);
        draw_rect_outline(&c, ix, iy, LN_ITEM_W, LN_ITEM_H, 1, LN_BORDER);

        const char* icon_char;
        acolor_t    icon_color;
        const char* name;

        if (i < LN_APP_COUNT) {
            /* Core surface app */
            icon_char  = g_apps[i].icon;
            icon_color = g_apps[i].color;
            name       = g_apps[i].name;
        } else {
            /* Installed package */
            const apkg_record_t* pkg = apkg_get(i - LN_APP_COUNT);
            icon_char  = "P";
            icon_color = ACOLOR(0x40, 0x60, 0x40, 0xFF);
            name       = pkg ? pkg->name : "?";
        }

        /* Icon area */
        int icx = ix + (LN_ITEM_W - LN_ICON_SZ) / 2;
        int icy = iy + 8;
        draw_rect(&c, icx, icy, LN_ICON_SZ, LN_ICON_SZ, icon_color);
        int icon_tx = icx + (LN_ICON_SZ - (int)strlen(icon_char) * FONT_W) / 2;
        int icon_ty = icy + (LN_ICON_SZ - FONT_H) / 2;
        draw_string(&c, icon_tx, icon_ty, icon_char, LN_ICON_FG, ACOLOR(0,0,0,0));

        /* Name */
        int nlx = ix + (LN_ITEM_W - (int)strlen(name)*FONT_W) / 2;
        int nly = iy + LN_ICON_SZ + 12;
        draw_string(&c, nlx, nly, name, LN_TEXT_FG, ACOLOR(0,0,0,0));

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

        /* Hover detection (core apps + installed packages) */
        g_ln.hover = -1;
        int col = 0, row_y = grid_y;
        int total = ln_total();
        for (int i = 0; i < total; i++) {
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
            are_pop_overlay();
            if (g_ln.selected < LN_APP_COUNT) {
                const launcher_app_t* app = &g_apps[g_ln.selected];
                if (app->field_slot >= 0) {
                    /* Navigate to existing field surface */
                    context_goto(app->field_slot);
                } else if (app->launch) {
                    /* Open as floating window */
                    app->launch();
                }
            } else {
                /* Installed package: launch via apkg_exec */
                const apkg_record_t* pkg = apkg_get(g_ln.selected - LN_APP_COUNT);
                if (pkg) apkg_exec(pkg->name);
            }
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
                are_pop_overlay();
                if (g_ln.selected < LN_APP_COUNT) {
                    const launcher_app_t* app = &g_apps[g_ln.selected];
                    if (app->field_slot >= 0)
                        context_goto(app->field_slot);
                    else if (app->launch)
                        app->launch();
                } else {
                    const apkg_record_t* pkg = apkg_get(g_ln.selected - LN_APP_COUNT);
                    if (pkg) apkg_exec(pkg->name);
                }
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
            int t = ln_total();
            if (g_ln.selected < t - 1) g_ln.selected++;
            surface_invalidate(id);
        } else if (kc == KEY_UP_ARROW) {
            int t = ln_total();
            g_ln.selected = (g_ln.selected - LN_GRID_COLS + t) % t;
            surface_invalidate(id);
        } else if (kc == KEY_DOWN_ARROW) {
            int t = ln_total();
            g_ln.selected = (g_ln.selected + LN_GRID_COLS) % t;
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
