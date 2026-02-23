/*
 * surfaces/settings.c — Aether Settings Surface
 *
 * Tabbed settings: Display, Appearance, About.
 * Uses the UI scene graph for layout.
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
#include <kernel/version.h>

/* =========================================================
 * Layout
 * ========================================================= */
#define ST_TITLE_H   40
#define ST_TAB_H     36
#define ST_PAD       20
#define ST_ROW_H     32
#define ST_LABEL_W   160
#define ST_VAL_W     200

/* Colours */
#define ST_BG        ACOLOR(0x0E, 0x12, 0x1A, 0xFF)
#define ST_TITLE_BG  ACOLOR(0x12, 0x16, 0x20, 0xFF)
#define ST_TITLE_FG  ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define ST_TAB_BG    ACOLOR(0x16, 0x1C, 0x28, 0xFF)
#define ST_TAB_ACT   ACOLOR(0x1C, 0x30, 0x50, 0xFF)
#define ST_TAB_FG    ACOLOR(0xAA, 0xAA, 0xCC, 0xFF)
#define ST_TAB_AFG   ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define ST_ROW_BG    ACOLOR(0x14, 0x1A, 0x24, 0xFF)
#define ST_ROW_ALT   ACOLOR(0x12, 0x18, 0x22, 0xFF)
#define ST_TEXT_FG   ACOLOR(0xCC, 0xCC, 0xCC, 0xFF)
#define ST_DIM_FG    ACOLOR(0x60, 0x70, 0x80, 0xFF)
#define ST_VAL_FG    ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define ST_BORDER    ACOLOR(0x20, 0x28, 0x38, 0xFF)
#define ST_ACCENT    ACOLOR(0x30, 0x80, 0xFF, 0xFF)

/* =========================================================
 * State
 * ========================================================= */
#define ST_TAB_COUNT  3
static const char* const st_tab_names[ST_TAB_COUNT] = {
    "Display", "Appearance", "About"
};

typedef struct {
    int       tab;
    uint32_t  surf_w;
    uint32_t  surf_h;
    sid_t     sid;

    /* Display settings */
    int       brightness;   /* 0-100 */
    bool      fullscreen;

    /* Appearance */
    int       accent_idx;   /* 0=blue, 1=purple, 2=green, 3=orange */
} st_state_t;

static st_state_t g_st;

static const char* accent_names[] = { "Blue", "Purple", "Green", "Orange" };
static const acolor_t accent_colors[] = {
    ACOLOR(0x30, 0x80, 0xFF, 0xFF),
    ACOLOR(0xA0, 0x40, 0xFF, 0xFF),
    ACOLOR(0x20, 0xC0, 0x60, 0xFF),
    ACOLOR(0xFF, 0x80, 0x20, 0xFF),
};

/* =========================================================
 * Persistence (/etc/aether.conf)
 * ========================================================= */
#define ST_CONF_PATH  "/etc/aether.conf"

static void settings_save(void)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "brightness=%d\naccent=%d\n",
                     g_st.brightness, g_st.accent_idx);
    vfs_node_t* f = vfs_open(ST_CONF_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        vfs_create(ST_CONF_PATH, 0644);
        f = vfs_open(ST_CONF_PATH, O_WRONLY | O_TRUNC);
    }
    if (f) { vfs_write(f, 0, (size_t)n, buf); vfs_close(f); }
}

static void settings_load(void)
{
    vfs_node_t* f = vfs_open(ST_CONF_PATH, O_RDONLY);
    if (!f) return;
    char buf[128];
    ssize_t n = vfs_read(f, 0, sizeof(buf) - 1, buf);
    vfs_close(f);
    if (n <= 0) return;
    buf[n] = '\0';
    char* p = buf;
    while (*p) {
        char* lend = p;
        while (*lend && *lend != '\n') lend++;
        char saved = *lend; *lend = '\0';
        char* eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            int val = 0;
            const char* vp = eq + 1;
            while (*vp >= '0' && *vp <= '9') val = val * 10 + (*vp++ - '0');
            if (strcmp(p, "brightness") == 0 && val >= 0 && val <= 100)
                g_st.brightness = val;
            else if (strcmp(p, "accent") == 0 && val >= 0 && val < 4)
                g_st.accent_idx = val;
        }
        if (!saved) break;
        p = lend + 1;
    }
}

/* =========================================================
 * Render helpers
 * ========================================================= */
static void draw_setting_row(canvas_t* c, int x, int y, int w,
                              const char* label, const char* value,
                              bool alt)
{
    draw_rect(c, x, y, w, ST_ROW_H, alt ? ST_ROW_ALT : ST_ROW_BG);
    draw_string(c, x + ST_PAD, y + (ST_ROW_H - FONT_H)/2,
                label, ST_TEXT_FG, ACOLOR(0,0,0,0));
    draw_string(c, x + ST_LABEL_W, y + (ST_ROW_H - FONT_H)/2,
                value, ST_VAL_FG, ACOLOR(0,0,0,0));
    draw_rect(c, x, y + ST_ROW_H - 1, w, 1, ST_BORDER);
}

static void draw_slider(canvas_t* c, int x, int y, int w, int val)
{
    /* Track */
    draw_rect(c, x, y + 8, w, 4, ST_BORDER);
    /* Fill */
    int fill = val * w / 100;
    draw_rect(c, x, y + 8, fill, 4, ST_ACCENT);
    /* Knob */
    int kx = x + fill - 6;
    draw_rect(c, kx, y + 4, 12, 12, ST_ACCENT);
}

/* =========================================================
 * Tab renderers
 * ========================================================= */
static void render_tab_display(canvas_t* c, int x, int y, int w, int h)
{
    (void)h;
    int ry = y;
    draw_setting_row(c, x, ry, w, "Resolution",
                     "Native (framebuffer)", false);
    ry += ST_ROW_H;

    draw_setting_row(c, x, ry, w, "Color depth", "32 bpp ARGB", true);
    ry += ST_ROW_H;

    draw_rect(c, x, ry, w, ST_ROW_H, ST_ROW_BG);
    draw_string(c, x + ST_PAD, ry + (ST_ROW_H - FONT_H)/2,
                "Brightness", ST_TEXT_FG, ACOLOR(0,0,0,0));
    draw_slider(c, x + ST_LABEL_W, ry + (ST_ROW_H - 20)/2,
                ST_VAL_W, g_st.brightness);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_st.brightness);
    draw_string(c, x + ST_LABEL_W + ST_VAL_W + 8,
                ry + (ST_ROW_H - FONT_H)/2, pct, ST_VAL_FG, ACOLOR(0,0,0,0));
    ry += ST_ROW_H;

    draw_rect(c, x, ry, w, 1, ST_BORDER);
}

static void render_tab_appearance(canvas_t* c, int x, int y, int w, int h)
{
    (void)h;
    int ry = y;
    draw_rect(c, x, ry, w, ST_ROW_H, ST_ROW_BG);
    draw_string(c, x + ST_PAD, ry + (ST_ROW_H - FONT_H)/2,
                "Accent Color", ST_TEXT_FG, ACOLOR(0,0,0,0));
    ry += ST_ROW_H;

    /* Color swatches */
    int sx = x + ST_PAD;
    for (int i = 0; i < 4; i++) {
        acolor_t col = accent_colors[i];
        bool sel = (i == g_st.accent_idx);
        draw_rect(c, sx, ry + 4, 48, 24, col);
        if (sel) draw_rect_outline(c, sx - 2, ry + 2, 52, 28, 2, ST_TEXT_FG);
        draw_string(c, sx + 4, ry + 28 + 4, accent_names[i],
                    ST_DIM_FG, ACOLOR(0,0,0,0));
        sx += 64;
    }
    ry += 56;

    draw_rect(c, x, ry, w, 1, ST_BORDER);
    ry += 12;

    draw_setting_row(c, x, ry, w, "Theme", "Aether Dark", false);
    ry += ST_ROW_H;
    draw_setting_row(c, x, ry, w, "Font", "Aether Mono 8pt", true);
}

static void render_tab_about(canvas_t* c, int x, int y, int w, int h)
{
    (void)w; (void)h;
    int ry = y + ST_PAD;

    /* Big OS name */
    draw_string(c, x + ST_PAD, ry, OS_NAME, ST_TITLE_FG, ACOLOR(0,0,0,0));
    ry += FONT_H + 4;
    draw_string(c, x + ST_PAD, ry, OS_BANNER_SHORT, ST_DIM_FG, ACOLOR(0,0,0,0));
    ry += FONT_H + 16;
    draw_rect(c, x + ST_PAD, ry, w - 2*ST_PAD, 1, ST_BORDER);
    ry += 12;

    struct { const char* label; const char* val; } info[] = {
        { "Kernel",      OS_BANNER_SHORT         },
        { "ARE",         "Aether Render Engine v1.0" },
        { "Arch",        "x86-64"                },
        { "Build",       __DATE__ " " __TIME__   },
        { "Author",      OS_AUTHOR               },
        { "License",     "Proprietary"           },
    };
    for (int i = 0; i < 6; i++) {
        draw_setting_row(c, x, ry, w - ST_PAD, info[i].label, info[i].val,
                         i & 1);
        ry += ST_ROW_H;
    }
}

/* =========================================================
 * Render callback
 * ========================================================= */
static void st_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                      void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = w, .height = h };

    /* Background */
    draw_rect(&c, 0, 0, (int)w, (int)h, ST_BG);

    /* Title */
    draw_rect(&c, 0, 0, (int)w, ST_TITLE_H, ST_TITLE_BG);
    draw_string(&c, ST_PAD, (ST_TITLE_H - FONT_H)/2,
                "Settings", ST_TITLE_FG, ACOLOR(0,0,0,0));

    /* Tabs */
    int tab_y = ST_TITLE_H;
    draw_rect(&c, 0, tab_y, (int)w, ST_TAB_H, ST_TAB_BG);
    int tab_w = (int)w / ST_TAB_COUNT;
    for (int i = 0; i < ST_TAB_COUNT; i++) {
        int tx = i * tab_w;
        acolor_t tbg = (i == g_st.tab) ? ST_TAB_ACT : ST_TAB_BG;
        acolor_t tfg = (i == g_st.tab) ? ST_TAB_AFG : ST_TAB_FG;
        draw_rect(&c, tx, tab_y, tab_w, ST_TAB_H, tbg);
        int lx = tx + (tab_w - (int)strlen(st_tab_names[i]) * FONT_W) / 2;
        draw_string(&c, lx, tab_y + (ST_TAB_H - FONT_H)/2,
                    st_tab_names[i], tfg, ACOLOR(0,0,0,0));
        if (i < ST_TAB_COUNT - 1)
            draw_rect(&c, tx + tab_w - 1, tab_y, 1, ST_TAB_H, ST_BORDER);
    }
    /* Active tab underline */
    draw_rect(&c, g_st.tab * tab_w, tab_y + ST_TAB_H - 2,
                   tab_w, 2, ST_ACCENT);

    /* Content area */
    int content_y = ST_TITLE_H + ST_TAB_H + 8;
    switch (g_st.tab) {
    case 0: render_tab_display   (&c, 0, content_y, (int)w, (int)h - content_y); break;
    case 1: render_tab_appearance(&c, 0, content_y, (int)w, (int)h - content_y); break;
    case 2: render_tab_about     (&c, 0, content_y, (int)w, (int)h - content_y); break;
    }
}

/* =========================================================
 * Input callback
 * ========================================================= */
static void st_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    if (ev->type == INPUT_POINTER) {
        bool click = (ev->pointer.buttons & IBTN_LEFT) &&
                     !(ev->pointer.prev_buttons & IBTN_LEFT);
        if (click) {
            int my = ev->pointer.y;
            int mx = ev->pointer.x;
            /* Tab click */
            if (my >= ST_TITLE_H && my < ST_TITLE_H + ST_TAB_H) {
                int tab_w = (int)g_st.surf_w / ST_TAB_COUNT;
                int t = mx / tab_w;
                if (t >= 0 && t < ST_TAB_COUNT) {
                    g_st.tab = t;
                    surface_invalidate(id);
                }
            }
            /* Appearance tab: accent swatch click */
            if (g_st.tab == 1) {
                int content_y = ST_TITLE_H + ST_TAB_H + 8;
                int swatch_y  = content_y + ST_ROW_H + 4;
                int swatch_x  = ST_PAD;
                if (my >= swatch_y && my < swatch_y + 28) {
                    for (int i = 0; i < 4; i++) {
                        if (mx >= swatch_x && mx < swatch_x + 48) {
                            g_st.accent_idx = i;
                            settings_save();
                            surface_invalidate(id);
                            break;
                        }
                        swatch_x += 64;
                    }
                }
            }
            /* Display tab: brightness slider */
            if (g_st.tab == 0) {
                int content_y = ST_TITLE_H + ST_TAB_H + 8;
                int slider_y  = content_y + 2 * ST_ROW_H + (ST_ROW_H - 20)/2;
                if (my >= slider_y && my < slider_y + 20) {
                    int sx = ST_LABEL_W;
                    if (mx >= sx && mx < sx + ST_VAL_W) {
                        g_st.brightness = (mx - sx) * 100 / ST_VAL_W;
                        settings_save();
                        surface_invalidate(id);
                    }
                }
            }
        }
    }

    if (ev->type == INPUT_KEY && ev->key.down) {
        switch (ev->key.keycode) {
        case KEY_LEFT_ARROW:
        case KEY_TAB:
            g_st.tab = (g_st.tab - 1 + ST_TAB_COUNT) % ST_TAB_COUNT;
            surface_invalidate(id);
            break;
        case KEY_RIGHT_ARROW:
            g_st.tab = (g_st.tab + 1) % ST_TAB_COUNT;
            surface_invalidate(id);
            break;
        }
    }
}

/* =========================================================
 * Init
 * ========================================================= */
void surface_settings_init(uint32_t w, uint32_t h)
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.surf_w     = w;
    g_st.surf_h     = h;
    g_st.brightness = 80;  /* defaults — may be overridden by settings_load */
    g_st.accent_idx = 0;
    settings_load();       /* restore last saved settings */

    g_st.sid = are_add_surface(SURF_APP, w, h,
                               "Settings", "S",
                               st_render, st_input, NULL, NULL);
}
