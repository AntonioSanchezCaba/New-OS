/*
 * apps/settings.c - Aether OS Settings Panel
 *
 * Three tabs:
 *   Display   — resolution info, theme toggle, accent picker
 *   System    — uptime, memory, process count, OS info
 *   About     — Aether OS identity, version, license
 */
#include <gui/window.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/timer.h>
#include <memory.h>
#include <process.h>
#include <string.h>
#include <kernel.h>

#define SET_W       520
#define SET_H       380
#define SET_SIDEBAR  140
#define SET_TAB_H    36
#define SET_ITEM_H   28

#define TAB_DISPLAY  0
#define TAB_SYSTEM   1
#define TAB_ABOUT    2
#define TAB_COUNT    3

typedef struct {
    wid_t wid;
    int   tab;
    uint32_t last_refresh;
} settings_t;

static settings_t* g_set = NULL;

/* ---- Formatting helpers ---- */

static void itoa_s(char* buf, int n)
{
    if (n < 0) { *buf++ = '-'; n = -n; }
    char tmp[16]; int i = 0;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (int k = i-1; k >= 0; k--) *buf++ = tmp[k];
    *buf = '\0';
}

static void fmt_mem_kb(char* buf, uint32_t kb)
{
    if (kb >= 1024) {
        itoa_s(buf, (int)(kb / 1024));
        strcat(buf, " MB");
    } else {
        itoa_s(buf, (int)kb);
        strcat(buf, " KB");
    }
}

/* ---- Draw helpers ---- */

static void set_section(canvas_t* c, int y, const char* label)
{
    const theme_t* th = theme_current();
    draw_rect(c, 0, y, c->width, SET_ITEM_H, th->panel_header);
    draw_string(c, 8, y + (SET_ITEM_H - FONT_H) / 2, label,
                th->text_on_accent, rgba(0,0,0,0));
}

static void set_row(canvas_t* c, int y, const char* key, const char* val, bool alt)
{
    const theme_t* th = theme_current();
    if (alt) draw_rect(c, 0, y, c->width, SET_ITEM_H, th->row_alt);
    draw_string(c, 8,  y + (SET_ITEM_H - FONT_H) / 2, key, th->text_secondary, rgba(0,0,0,0));
    draw_string(c, c->width / 2, y + (SET_ITEM_H - FONT_H) / 2, val,
                th->text_primary, rgba(0,0,0,0));
    draw_hline(c, 0, y + SET_ITEM_H - 1, c->width, th->panel_border);
}

/* ---- Toggle button ---- */
static void draw_toggle(canvas_t* c, int x, int y, int w, int h,
                         bool on, const char* label_off, const char* label_on)
{
    const theme_t* th = theme_current();
    uint32_t bg = on ? th->accent : th->panel_border;
    draw_rect_rounded(c, x, y, w, h, h/2, bg);
    /* Knob */
    int kx = on ? (x + w - h + 2) : (x + 2);
    draw_circle_filled(c, kx + (h-4)/2, y + h/2, h/2 - 2, COLOR_WHITE);

    int lx = on ? (x - (int)strlen(label_on) * FONT_W - 8)
                : (x + w + 8);
    const char* lbl = on ? label_on : label_off;
    draw_string(c, lx, y + (h - FONT_H) / 2, lbl,
                th->text_primary, rgba(0,0,0,0));
}

/* ---- Accent swatch ---- */
static void draw_accent_swatch(canvas_t* c, int x, int y, int r,
                                 uint32_t color, bool selected)
{
    draw_circle_filled(c, x, y, r, color);
    if (selected)
        draw_circle(c, x, y, r + 2, theme_current()->text_primary);
}

/* ---- Display tab ---- */
static void draw_tab_display(canvas_t* c)
{
    const theme_t* th = theme_current();
    int y = 4;

    set_section(c, y, "Display");
    y += SET_ITEM_H;

    char res[32];
    char wb[8], hb[8];
    itoa_s(wb, (int)fb.width);
    itoa_s(hb, (int)fb.height);
    strncpy(res, wb, sizeof(res));
    strncat(res, " × ", sizeof(res) - strlen(res) - 1);
    strncat(res, hb, sizeof(res) - strlen(res) - 1);
    set_row(c, y, "Resolution", res, false);   y += SET_ITEM_H;

    char bpp[16]; itoa_s(bpp, (int)fb.bpp); strncat(bpp, " bpp", 5);
    set_row(c, y, "Color Depth", bpp, true);   y += SET_ITEM_H;
    set_row(c, y, "Refresh Rate", "~60 Hz",    false); y += SET_ITEM_H;
    set_row(c, y, "Framebuffer", "Double-buffered", true); y += SET_ITEM_H + 8;

    /* Theme toggle */
    set_section(c, y, "Appearance");
    y += SET_ITEM_H + 8;

    bool dark = (theme_get_id() == THEME_DARK);
    draw_string(c, 8, y + 4, "Theme:", th->text_secondary, rgba(0,0,0,0));
    draw_toggle(c, 100, y, 48, 22, dark, "Light", "Dark");
    y += 34;

    /* Accent colors */
    draw_string(c, 8, y + 4, "Accent:", th->text_secondary, rgba(0,0,0,0));
    static const uint32_t swatches[] = {
        0xFF007ACC, 0xFF8040C0, 0xFF28A050, 0xFFC03030
    };
    int sx = 100;
    accent_id_t cur_accent = theme_get_accent();
    for (int i = 0; i < 4; i++) {
        draw_accent_swatch(c, sx + i * 32, y + 12, 10, swatches[i],
                           (accent_id_t)i == cur_accent);
    }
    y += 34;

    draw_string(c, 8, y,
                "Click accent circles or theme toggle in the",
                th->text_disabled, rgba(0,0,0,0));
    y += FONT_H + 2;
    draw_string(c, 8, y, "event handler to switch themes.",
                th->text_disabled, rgba(0,0,0,0));
}

/* ---- System tab ---- */
static void draw_tab_system(canvas_t* c)
{
    int y = 4;
    char buf[64];

    set_section(c, y, "System Resources");
    y += SET_ITEM_H;

    uint32_t total_kb = 256 * 1024;
    uint32_t free_kb  = (uint32_t)(pmm_free_frames_count() * 4);
    uint32_t used_kb  = (total_kb > free_kb) ? total_kb - free_kb : 0;
    uint32_t mem_pct  = used_kb * 100 / total_kb;

    char used_s[16], total_s[16];
    fmt_mem_kb(used_s, used_kb);
    fmt_mem_kb(total_s, total_kb);
    strncpy(buf, used_s, sizeof(buf));
    strncat(buf, " / ", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, total_s, sizeof(buf) - strlen(buf) - 1);
    set_row(c, y, "Memory Used", buf, false); y += SET_ITEM_H;

    /* Memory bar */
    int bx = 8, bw = c->width - 16, bh = 10;
    const theme_t* th = theme_current();
    draw_rect(c, bx, y, bw, bh, th->panel_border);
    int fill = (int)((uint64_t)mem_pct * (uint32_t)bw / 100);
    uint32_t bar_col = (mem_pct > 80) ? th->error :
                       (mem_pct > 50) ? th->warn  : th->ok;
    if (fill > 0) draw_rect(c, bx, y, fill, bh, bar_col);
    char pcts[8]; itoa_s(pcts, (int)mem_pct); strncat(pcts, "%", 2);
    draw_string(c, bx + bw + 4, y - 1, pcts, th->text_secondary, rgba(0,0,0,0));
    y += bh + 6;

    int nproc = 0;
    for (process_t* p = process_list; p; p = p->next) nproc++;
    char ps[8]; itoa_s(ps, nproc);
    set_row(c, y, "Processes", ps, false); y += SET_ITEM_H;

    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    char us[16], ms2[16], ss[16];
    itoa_s(us, (int)(secs / 3600));
    itoa_s(ms2, (int)((secs / 60) % 60));
    itoa_s(ss, (int)(secs % 60));
    strncpy(buf, us, sizeof(buf));
    strncat(buf, "h ", sizeof(buf)-strlen(buf)-1);
    strncat(buf, ms2, sizeof(buf)-strlen(buf)-1);
    strncat(buf, "m ", sizeof(buf)-strlen(buf)-1);
    strncat(buf, ss, sizeof(buf)-strlen(buf)-1);
    strncat(buf, "s", sizeof(buf)-strlen(buf)-1);
    set_row(c, y, "Uptime", buf, true); y += SET_ITEM_H + 8;

    set_section(c, y, "Hardware");
    y += SET_ITEM_H;
    set_row(c, y, "Architecture", "x86_64 (64-bit)", false); y += SET_ITEM_H;
    set_row(c, y, "Timer Freq", "100 Hz (PIT)", true);       y += SET_ITEM_H;
    set_row(c, y, "Framebuffer", "Linear 32bpp", false);     y += SET_ITEM_H;
    set_row(c, y, "Network", "Intel e1000 (emulated)", true);
}

/* ---- About tab ---- */
static void draw_tab_about(canvas_t* c)
{
    const theme_t* th = theme_current();
    int cx = c->width / 2;
    int y = 16;

    /* Logo */
    const char* logo[] = { " ___       _   _               ",
                            "| _ | ___ | |_| |__   ___ _ __ ",
                            "| |_) / _ \\| __| '_ \\ / _ \\ '__|",
                            "|  _ < __/ | |_| | | |  __/ |  ",
                            "|_| \\_\\___| \\__|_| |_|\\___|_|  ",
                            NULL };

    for (int i = 0; logo[i]; i++) {
        int lw = (int)strlen(logo[i]) * FONT_W;
        draw_string(c, cx - lw/2, y + i * FONT_H, logo[i],
                    th->accent, rgba(0,0,0,0));
    }
    y += 5 * FONT_H + 12;

    const struct { const char* k; const char* v; } info[] = {
        { "Version",      OS_VERSION " — " OS_RELEASE   },
        { "Architecture", "Hybrid Microkernel"          },
        { "Security",     "Capability-Based"            },
        { "IPC",          "Message-Passing Ports"       },
        { "Display",      "Compositor-First"            },
        { "Memory",       "Buddy + Free-List Allocator" },
        { "Network",      "Custom TCP/IP Stack"         },
        { "Tagline",      OS_TAGLINE                   },
        { NULL, NULL }
    };

    draw_hline(c, 8, y, c->width - 16, th->panel_border);
    y += 6;

    for (int i = 0; info[i].k; i++) {
        bool alt = (i % 2) == 1;
        if (alt) draw_rect(c, 0, y, c->width, FONT_H + 6, th->row_alt);
        draw_string(c, 12, y + 3, info[i].k, th->text_secondary, rgba(0,0,0,0));
        draw_string(c, cx - 20, y + 3, info[i].v, th->text_primary, rgba(0,0,0,0));
        y += FONT_H + 6;
    }

    draw_hline(c, 8, y + 4, c->width - 16, th->panel_border);
    y += 12;
    const char* copy = OS_PROJECT "  |  " OS_LICENSE "  |  " OS_YEAR;
    int cw = (int)strlen(copy) * FONT_W;
    draw_string(c, cx - cw/2, y, copy, th->text_disabled, rgba(0,0,0,0));
}

/* ---- Main redraw ---- */
static void set_redraw(wid_t wid)
{
    settings_t* s = g_set;
    if (!s || s->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    const theme_t* th = theme_current();
    draw_rect(&c, 0, 0, c.width, c.height, th->win_bg);

    /* Tab bar */
    const char* tab_names[] = { " Display ", " System ", " About " };
    int tx = 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tw = (int)strlen(tab_names[i]) * FONT_W + 4;
        bool active = (s->tab == i);
        uint32_t bg  = active ? th->win_bg     : th->panel_bg;
        uint32_t fg  = active ? th->accent      : th->text_secondary;
        draw_rect(&c, tx, 0, tw, SET_TAB_H, bg);
        if (active) {
            draw_rect(&c, tx, SET_TAB_H - 2, tw, 2, th->accent);
        }
        draw_string_centered(&c, tx, 0, tw, SET_TAB_H, tab_names[i],
                             fg, rgba(0,0,0,0));
        draw_vline(&c, tx + tw - 1, 0, SET_TAB_H, th->panel_border);
        tx += tw;
    }
    draw_hline(&c, 0, SET_TAB_H, c.width, th->panel_border);

    /* Content area */
    canvas_t content = draw_sub_canvas(&c, 0, SET_TAB_H + 1,
                                        c.width, c.height - SET_TAB_H - 1);
    switch (s->tab) {
    case TAB_DISPLAY: draw_tab_display(&content); break;
    case TAB_SYSTEM:  draw_tab_system(&content);  break;
    case TAB_ABOUT:   draw_tab_about(&content);   break;
    }
}

/* ---- Event handler ---- */
static void set_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    settings_t* s = g_set;
    if (!s || s->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT:
        set_redraw(wid);
        break;

    case GUI_EVENT_MOUSE_DOWN: {
        int mx = evt->mouse.x;
        int my = evt->mouse.y;

        if (my < SET_TAB_H) {
            /* Tab switching */
            const char* tab_names[] = { " Display ", " System ", " About " };
            int tx = 0;
            for (int i = 0; i < TAB_COUNT; i++) {
                int tw = (int)strlen(tab_names[i]) * FONT_W + 4;
                if (mx >= tx && mx < tx + tw) {
                    s->tab = i;
                    set_redraw(wid);
                    break;
                }
                tx += tw;
            }
            break;
        }

        /* Display tab interactions */
        if (s->tab == TAB_DISPLAY) {
            /* Theme toggle at y ≈ 4 + 5*SET_ITEM_H + 8 + SET_ITEM_H + 8 */
            int toggle_y = 4 + 5 * SET_ITEM_H + 8 + SET_ITEM_H + 8 - SET_TAB_H;
            if (my >= toggle_y && my < toggle_y + 26 &&
                mx >= 92 && mx < 160) {
                theme_id_t new_id = (theme_get_id() == THEME_DARK)
                                    ? THEME_LIGHT : THEME_DARK;
                theme_set(new_id);
                set_redraw(wid);
                break;
            }
            /* Accent swatches */
            int accent_y = toggle_y + 34;
            if (my >= accent_y && my < accent_y + 26) {
                for (int i = 0; i < 4; i++) {
                    int sx = 100 + i * 32;
                    if (mx >= sx - 12 && mx < sx + 12) {
                        theme_set_accent((accent_id_t)i);
                        set_redraw(wid);
                        break;
                    }
                }
            }
        }
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(s);
        g_set = NULL;
        break;

    default: break;
    }

    /* Auto-refresh system tab */
    uint32_t now = timer_get_ticks();
    if (s->tab == TAB_SYSTEM && now - s->last_refresh >= 100) {
        s->last_refresh = now;
        set_redraw(wid);
    }
}

/* ---- Public create function ---- */
wid_t app_settings_create(void)
{
    if (g_set) return g_set->wid;

    settings_t* s = (settings_t*)kmalloc(sizeof(settings_t));
    if (!s) return -1;
    memset(s, 0, sizeof(settings_t));
    s->last_refresh = timer_get_ticks();

    wid_t wid = wm_create_window("Settings",
                                  180, 140, SET_W, SET_H,
                                  set_on_event, NULL);
    if (wid < 0) { kfree(s); return -1; }

    s->wid = wid;
    g_set  = s;

    set_redraw(wid);
    return wid;
}
