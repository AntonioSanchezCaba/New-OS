/*
 * apps/texteditor.c - Simple graphical text editor
 *
 * Supports multi-line editing with keyboard navigation.
 * Files can be opened via VFS (placeholder) or used as scratch pad.
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <drivers/timer.h>

#define TE_W       620
#define TE_H       400
#define TE_COLS    ((TE_W - 50) / FONT_W)    /* ~71 chars */
#define TE_ROWS    (TE_H / FONT_H)            /* ~25 lines */
#define TE_MAX_LINES 1000
#define TE_MAX_LINE_LEN 256

#define TE_BG      rgb(0xFE, 0xFE, 0xFF)
#define TE_FG      COLOR_TEXT_DARK
#define TE_GUTTER  rgb(0xE8, 0xE8, 0xF0)
#define TE_GUTTER_FG rgb(0x88, 0x88, 0xA0)
#define TE_CURSOR_COL rgb(0x00, 0x60, 0xFF)
#define TE_SEL_BG  rgba(0x40, 0x80, 0xFF, 0x40)
#define TE_LINE_H  FONT_H
#define TE_GUTTER_W 40

typedef struct {
    wid_t wid;
    char  lines[TE_MAX_LINES][TE_MAX_LINE_LEN];
    int   line_count;
    int   cur_line;
    int   cur_col;
    int   scroll;             /* First visible line */
    char  filename[128];
    bool  modified;
    uint32_t last_blink;
    bool  cursor_vis;
} te_t;

static te_t* g_te = NULL;

static void te_redraw(wid_t wid)
{
    te_t* t = g_te;
    if (!t || t->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    draw_rect(&c, 0, 0, c.width, c.height, TE_BG);

    /* Gutter */
    draw_rect(&c, 0, 0, TE_GUTTER_W, c.height, TE_GUTTER);
    draw_vline(&c, TE_GUTTER_W, 0, c.height, COLOR_LIGHT_GREY);

    int visible_rows = c.height / TE_LINE_H;

    for (int row = 0; row < visible_rows; row++) {
        int line_idx = t->scroll + row;
        int y = row * TE_LINE_H;

        /* Line number */
        if (line_idx < t->line_count) {
            char num[8];
            int n = line_idx + 1;
            int ni = 3;
            num[4] = '\0';
            num[3] = ' ';
            while (ni >= 0) {
                if (n > 0) { num[ni] = '0' + (n % 10); n /= 10; }
                else num[ni] = ' ';
                ni--;
            }
            draw_string(&c, 2, y, num + (line_idx >= 9 ? (line_idx >= 99 ? 1 : 2) : 3),
                        TE_GUTTER_FG, rgba(0,0,0,0));
        }

        /* Highlight current line */
        if (line_idx == t->cur_line) {
            draw_rect(&c, TE_GUTTER_W + 1, y, c.width - TE_GUTTER_W - 1, TE_LINE_H,
                      rgb(0xF0, 0xF4, 0xFF));
        }

        /* Line text */
        if (line_idx < t->line_count) {
            draw_string(&c, TE_GUTTER_W + 4, y, t->lines[line_idx], TE_FG, rgba(0,0,0,0));
        }
    }

    /* Cursor */
    if (t->cursor_vis && t->cur_line >= t->scroll &&
        t->cur_line < t->scroll + visible_rows) {
        int cy = (t->cur_line - t->scroll) * TE_LINE_H;
        int cx = TE_GUTTER_W + 4 + t->cur_col * FONT_W;
        draw_rect(&c, cx, cy, 2, TE_LINE_H, TE_CURSOR_COL);
    }

    /* Status bar */
    draw_hline(&c, 0, c.height - TE_LINE_H - 1, c.width, COLOR_LIGHT_GREY);
    draw_rect(&c, 0, c.height - TE_LINE_H, c.width, TE_LINE_H, TE_GUTTER);

    /* Status text: filename, line:col */
    char status[128];
    const char* fname = t->filename[0] ? t->filename : "[scratch]";
    const char* mod = t->modified ? "* " : "  ";
    /* Build status string manually */
    strncpy(status, mod, 3);
    strncat(status, fname, 60);
    strncat(status, "  Ln:", 6);
    /* Append line number */
    int ln = t->cur_line + 1;
    char lbuf[8]; int li = 0;
    if (ln == 0) { lbuf[0]='0'; li=1; }
    while (ln > 0) { lbuf[li++] = '0' + (ln%10); ln /= 10; }
    for (int a=0,b=li-1;a<b;a++,b--){char x=lbuf[a];lbuf[a]=lbuf[b];lbuf[b]=x;}
    lbuf[li]='\0';
    strncat(status, lbuf, 8);
    strncat(status, " Col:", 6);
    int cn = t->cur_col + 1;
    li = 0;
    if (cn == 0) { lbuf[0]='0'; li=1; }
    while (cn > 0) { lbuf[li++] = '0' + (cn%10); cn /= 10; }
    for (int a=0,b=li-1;a<b;a++,b--){char x=lbuf[a];lbuf[a]=lbuf[b];lbuf[b]=x;}
    lbuf[li]='\0';
    strncat(status, lbuf, 8);

    draw_string(&c, 4, c.height - TE_LINE_H + 2, status, TE_FG, rgba(0,0,0,0));
}

static void te_scroll_to_cursor(te_t* t)
{
    canvas_t c = wm_client_canvas(t->wid);
    int visible = (c.height - TE_LINE_H) / TE_LINE_H;
    if (t->cur_line < t->scroll) t->scroll = t->cur_line;
    if (t->cur_line >= t->scroll + visible) t->scroll = t->cur_line - visible + 1;
    if (t->scroll < 0) t->scroll = 0;
}

static void te_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    te_t* t = g_te;
    if (!t || t->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT:
        te_redraw(wid);
        break;

    case GUI_EVENT_KEY_DOWN: {
        uint8_t kc = evt->key.keycode;
        char ch = evt->key.ch;

        if (kc == KEY_UP_ARROW) {
            if (t->cur_line > 0) {
                t->cur_line--;
                int len = (int)strlen(t->lines[t->cur_line]);
                if (t->cur_col > len) t->cur_col = len;
            }
        } else if (kc == KEY_DOWN_ARROW) {
            if (t->cur_line < t->line_count - 1) {
                t->cur_line++;
                int len = (int)strlen(t->lines[t->cur_line]);
                if (t->cur_col > len) t->cur_col = len;
            }
        } else if (kc == KEY_LEFT_ARROW) {
            if (t->cur_col > 0) t->cur_col--;
            else if (t->cur_line > 0) {
                t->cur_line--;
                t->cur_col = (int)strlen(t->lines[t->cur_line]);
            }
        } else if (kc == KEY_RIGHT_ARROW) {
            int len = (int)strlen(t->lines[t->cur_line]);
            if (t->cur_col < len) t->cur_col++;
            else if (t->cur_line < t->line_count - 1) {
                t->cur_line++; t->cur_col = 0;
            }
        } else if (ch == '\n' || ch == '\r' || kc == KEY_ENTER) {
            /* Insert line break */
            if (t->line_count < TE_MAX_LINES - 1) {
                char* cur = t->lines[t->cur_line];
                char tail[TE_MAX_LINE_LEN];
                strncpy(tail, cur + t->cur_col, TE_MAX_LINE_LEN - 1);
                tail[TE_MAX_LINE_LEN - 1] = '\0';
                cur[t->cur_col] = '\0';
                /* Shift lines down */
                memmove(t->lines[t->cur_line + 2], t->lines[t->cur_line + 1],
                        (size_t)(t->line_count - t->cur_line - 1) * TE_MAX_LINE_LEN);
                t->cur_line++;
                t->line_count++;
                strncpy(t->lines[t->cur_line], tail, TE_MAX_LINE_LEN - 1);
                t->cur_col = 0;
                t->modified = true;
            }
        } else if (ch == '\b' || kc == KEY_BACKSPACE) {
            char* cur = t->lines[t->cur_line];
            if (t->cur_col > 0) {
                memmove(cur + t->cur_col - 1, cur + t->cur_col,
                        strlen(cur) - t->cur_col + 1);
                t->cur_col--;
                t->modified = true;
            } else if (t->cur_line > 0) {
                /* Merge with previous line */
                char* prev = t->lines[t->cur_line - 1];
                int prev_len = (int)strlen(prev);
                strncat(prev, cur, TE_MAX_LINE_LEN - prev_len - 1);
                memmove(t->lines[t->cur_line], t->lines[t->cur_line + 1],
                        (size_t)(t->line_count - t->cur_line) * TE_MAX_LINE_LEN);
                t->cur_line--;
                t->line_count--;
                t->cur_col = prev_len;
                t->modified = true;
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            char* cur = t->lines[t->cur_line];
            int len = (int)strlen(cur);
            if (len < TE_MAX_LINE_LEN - 1) {
                memmove(cur + t->cur_col + 1, cur + t->cur_col,
                        (size_t)(len - t->cur_col + 1));
                cur[t->cur_col] = ch;
                t->cur_col++;
                t->modified = true;
            }
        }

        te_scroll_to_cursor(t);
        te_redraw(wid);
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(t);
        g_te = NULL;
        break;

    default: break;
    }

    /* Cursor blink */
    uint32_t now = timer_get_ticks();
    if (now - t->last_blink >= TIMER_FREQ / 2) {
        t->cursor_vis = !t->cursor_vis;
        t->last_blink = now;
        te_redraw(wid);
    }
}

wid_t app_texteditor_create(void)
{
    if (g_te) return g_te->wid;

    te_t* t = (te_t*)kmalloc(sizeof(te_t));
    if (!t) return -1;
    memset(t, 0, sizeof(te_t));

    /* Start with one empty line */
    t->lines[0][0] = '\0';
    t->line_count = 1;
    t->cursor_vis = true;
    t->last_blink = timer_get_ticks();

    /* Welcome content */
    const char* welcome[] = {
        "Aether OS Text Editor  v0.1",
        "================================",
        "",
        "Arrow keys  : Navigate",
        "Type        : Insert text",
        "Backspace   : Delete character",
        "Enter       : New line",
        "",
        NULL
    };
    for (int i = 0; welcome[i] && t->line_count < TE_MAX_LINES - 1; i++) {
        strncpy(t->lines[t->line_count - 1], welcome[i], TE_MAX_LINE_LEN - 1);
        if (welcome[i+1]) {
            t->lines[t->line_count][0] = '\0';
            t->line_count++;
        }
    }
    t->cur_line = t->line_count - 1;
    t->cur_col = 0;
    t->modified = false;

    wid_t wid = wm_create_window("Text Editor — Aether OS",
                                  140, 100, TE_W, TE_H,
                                  te_on_event, NULL);
    if (wid < 0) { kfree(t); return -1; }

    t->wid = wid;
    g_te = t;

    te_redraw(wid);
    return wid;
}
