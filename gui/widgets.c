/*
 * gui/widgets.c - Aether OS GUI Widget Toolkit implementation
 *
 * Renders and handles events for Buttons, Labels, TextInputs,
 * CheckBoxes, ProgressBars, and Separators.
 */
#include <gui/widgets.h>
#include <gui/theme.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <drivers/timer.h>
#include <string.h>
#include <memory.h>

/* Cursor blink interval in timer ticks */
#define BLINK_INTERVAL  (TIMER_FREQ / 2)

/* Padding inside widgets */
#define BTN_PAD_X   10
#define INPUT_PAD_X  6
#define INPUT_PAD_Y  4
#define CHK_BOX_SIZE 16

/* =========================================================
 * Internal helpers
 * ========================================================= */

static int widget_alloc(widget_group_t* g)
{
    if (g->count >= WIDGET_GROUP_MAX) return -1;
    int idx = g->count++;
    widget_t* w = &g->widgets[idx];
    memset(w, 0, sizeof(*w));
    w->enabled = true;
    w->visible = true;
    return idx;
}

static bool point_in(const widget_t* w, int x, int y)
{
    return x >= w->x && x < w->x + w->w &&
           y >= w->y && y < w->y + w->h;
}

/* =========================================================
 * Drawing helpers for each widget type
 * ========================================================= */

static void draw_button(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();
    uint32_t bg, fg;

    if (!w->enabled) {
        bg = th->panel_border;
        fg = th->text_disabled;
    } else if (w->btn.pressed) {
        bg = th->btn_active;
        fg = th->text_on_accent;
    } else if (w->btn.hover) {
        bg = th->btn_hover;
        fg = th->text_on_accent;
    } else {
        bg = th->btn_normal;
        fg = th->text_on_accent;
    }

    draw_rect_rounded(c, w->x, w->y, w->w, w->h, 4, bg);

    /* Label centered */
    int tw = (int)strlen(w->btn.label) * FONT_W;
    int tx = w->x + (w->w - tw) / 2;
    int ty = w->y + (w->h - FONT_H) / 2;
    draw_string(c, tx, ty, w->btn.label, fg, rgba(0, 0, 0, 0));
}

static void draw_label(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();
    uint32_t fg = w->lbl.color ? w->lbl.color : th->text_primary;
    draw_string(c, w->x, w->y + (w->h - FONT_H) / 2,
                w->lbl.text, fg, rgba(0, 0, 0, 0));
}

static void draw_textinput(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();
    bool focused = w->input.cursor_vis || (w->enabled);

    /* Background */
    uint32_t bg  = th->login_field_bg;
    uint32_t brd = focused ? th->accent : th->login_field_border;
    int brd_w = focused ? 2 : 1;

    draw_rect(c, w->x, w->y, w->w, w->h, bg);
    draw_rect_outline(c, w->x, w->y, w->w, w->h, brd_w, brd);

    /* Text content */
    char display[WIDGET_TEXT_MAX];
    const char* src = w->input.buf;
    int len = w->input.len;

    if (len == 0 && w->input.placeholder[0]) {
        /* Show placeholder */
        strncpy(display, w->input.placeholder, WIDGET_TEXT_MAX - 1);
        display[WIDGET_TEXT_MAX - 1] = '\0';
        draw_string(c, w->x + INPUT_PAD_X,
                    w->y + (w->h - FONT_H) / 2,
                    display, th->text_disabled, rgba(0, 0, 0, 0));
        return;
    }

    if (w->input.password) {
        int i;
        for (i = 0; i < len && i < (int)(WIDGET_TEXT_MAX - 1); i++)
            display[i] = '*';
        display[i] = '\0';
    } else {
        /* Apply horizontal scroll */
        int scroll = w->input.scroll;
        if (scroll > len) scroll = len;
        strncpy(display, src + scroll, WIDGET_TEXT_MAX - 1);
        display[WIDGET_TEXT_MAX - 1] = '\0';
    }

    int tx = w->x + INPUT_PAD_X;
    int ty = w->y + (w->h - FONT_H) / 2;
    draw_string(c, tx, ty, display, th->text_primary, rgba(0, 0, 0, 0));

    /* Cursor blink */
    if (w->input.cursor_vis) {
        int visible_cur = w->input.cursor - w->input.scroll;
        if (visible_cur >= 0) {
            int cx = tx + visible_cur * FONT_W;
            draw_rect(c, cx, ty, 2, FONT_H, th->accent);
        }
    }
}

static void draw_checkbox(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();
    int bx = w->x;
    int by = w->y + (w->h - CHK_BOX_SIZE) / 2;

    /* Box */
    uint32_t bg = w->chk.checked ? th->accent : th->login_field_bg;
    draw_rect(c, bx, by, CHK_BOX_SIZE, CHK_BOX_SIZE, bg);
    draw_rect_outline(c, bx, by, CHK_BOX_SIZE, CHK_BOX_SIZE, 1,
                      th->login_field_border);

    /* Checkmark */
    if (w->chk.checked) {
        draw_string(c, bx + 2, by + 1, "v", th->text_on_accent, rgba(0, 0, 0, 0));
    }

    /* Label */
    int tx = bx + CHK_BOX_SIZE + 6;
    int ty = w->y + (w->h - FONT_H) / 2;
    uint32_t fg = w->enabled ? th->text_primary : th->text_disabled;
    draw_string(c, tx, ty, w->chk.label, fg, rgba(0, 0, 0, 0));
}

static void draw_progressbar(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();

    /* Background track */
    draw_rect(c, w->x, w->y, w->w, w->h, th->panel_border);
    draw_rect_outline(c, w->x, w->y, w->w, w->h, 1, th->win_border);

    /* Fill */
    int max = (w->bar.max > 0) ? w->bar.max : 1;
    int fill_w = (w->w - 2) * w->bar.value / max;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > w->w - 2) fill_w = w->w - 2;

    uint32_t fill = w->bar.fill_color ? w->bar.fill_color : th->accent;
    if (fill_w > 0)
        draw_rect(c, w->x + 1, w->y + 1, fill_w, w->h - 2, fill);

    /* Percentage text */
    if (w->bar.show_text) {
        int pct = (w->bar.value * 100) / max;
        char buf[8];
        buf[0] = '0' + (char)(pct / 100);
        buf[1] = '0' + (char)((pct / 10) % 10);
        buf[2] = '0' + (char)(pct % 10);
        buf[3] = '%';
        buf[4] = '\0';
        /* Remove leading zeros */
        const char* txt = buf;
        while (*txt == '0' && *(txt + 1) != '%') txt++;
        int tw = (int)strlen(txt) * FONT_W;
        draw_string(c, w->x + (w->w - tw) / 2, w->y + (w->h - FONT_H) / 2,
                    txt, th->text_primary, rgba(0, 0, 0, 0));
    }
}

static void draw_separator(canvas_t* c, const widget_t* w)
{
    const theme_t* th = theme_current();
    if (w->sep.horizontal)
        draw_hline(c, w->x, w->y + w->h / 2, w->w, th->panel_border);
    else
        draw_vline(c, w->x + w->w / 2, w->y, w->h, th->panel_border);
}

/* =========================================================
 * Public API
 * ========================================================= */

void widget_group_init(widget_group_t* g)
{
    memset(g, 0, sizeof(*g));
    g->focused_idx = -1;
    g->hover_idx   = -1;
    g->dirty       = true;
}

int widget_add_button(widget_group_t* g, int x, int y, int w, int h,
                      const char* label, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_BUTTON;
    wg->id = id;
    wg->x = x; wg->y = y; wg->w = w; wg->h = h;
    strncpy(wg->btn.label, label, WIDGET_LABEL_MAX - 1);
    wg->btn.label[WIDGET_LABEL_MAX - 1] = '\0';
    return idx;
}

int widget_add_label(widget_group_t* g, int x, int y, int w, int h,
                     const char* text, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_LABEL;
    wg->id = id;
    wg->x = x; wg->y = y; wg->w = w; wg->h = h;
    strncpy(wg->lbl.text, text, WIDGET_TEXT_MAX - 1);
    wg->lbl.text[WIDGET_TEXT_MAX - 1] = '\0';
    return idx;
}

int widget_add_label_colored(widget_group_t* g, int x, int y, int w, int h,
                              const char* text, uint32_t id, uint32_t color)
{
    int idx = widget_add_label(g, x, y, w, h, text, id);
    if (idx >= 0) g->widgets[idx].lbl.color = color;
    return idx;
}

int widget_add_textinput(widget_group_t* g, int x, int y, int w, int h,
                          const char* placeholder, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_TEXTINPUT;
    wg->id = id;
    wg->x = x; wg->y = y; wg->w = w; wg->h = h;
    if (placeholder) {
        strncpy(wg->input.placeholder, placeholder, WIDGET_LABEL_MAX - 1);
        wg->input.placeholder[WIDGET_LABEL_MAX - 1] = '\0';
    }
    return idx;
}

int widget_add_checkbox(widget_group_t* g, int x, int y,
                         const char* label, bool checked, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_CHECKBOX;
    wg->id = id;
    wg->x = x; wg->y = y;
    wg->w = CHK_BOX_SIZE + 6 + (int)strlen(label) * FONT_W;
    wg->h = CHK_BOX_SIZE + 4;
    strncpy(wg->chk.label, label, WIDGET_LABEL_MAX - 1);
    wg->chk.label[WIDGET_LABEL_MAX - 1] = '\0';
    wg->chk.checked = checked;
    return idx;
}

int widget_add_progressbar(widget_group_t* g, int x, int y, int w, int h,
                            int value, int max, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_PROGRESSBAR;
    wg->id = id;
    wg->x = x; wg->y = y; wg->w = w; wg->h = h;
    wg->bar.value = value;
    wg->bar.max   = max;
    wg->bar.show_text = true;
    return idx;
}

int widget_add_separator(widget_group_t* g, int x, int y, int len,
                          bool horizontal, uint32_t id)
{
    int idx = widget_alloc(g);
    if (idx < 0) return -1;
    widget_t* wg = &g->widgets[idx];
    wg->type = WIDGET_SEPARATOR;
    wg->id = id;
    wg->x = x; wg->y = y;
    wg->w = horizontal ? len : 2;
    wg->h = horizontal ? 2 : len;
    wg->sep.horizontal = horizontal;
    return idx;
}

void widget_group_draw(canvas_t* c, widget_group_t* g)
{
    for (int i = 0; i < g->count; i++) {
        widget_t* w = &g->widgets[i];
        if (!w->visible) continue;

        switch (w->type) {
            case WIDGET_BUTTON:      draw_button(c, w);      break;
            case WIDGET_LABEL:       draw_label(c, w);       break;
            case WIDGET_TEXTINPUT:   draw_textinput(c, w);   break;
            case WIDGET_CHECKBOX:    draw_checkbox(c, w);    break;
            case WIDGET_PROGRESSBAR: draw_progressbar(c, w); break;
            case WIDGET_SEPARATOR:   draw_separator(c, w);   break;
            default: break;
        }
    }
    g->dirty = false;
}

bool widget_group_handle_event(widget_group_t* g, const gui_event_t* evt,
                                uint32_t* out_id)
{
    if (out_id) *out_id = 0;

    if (evt->type == GUI_EVENT_MOUSE_MOVE) {
        int mx = evt->mouse.x;
        int my = evt->mouse.y;
        g->hover_idx = -1;
        for (int i = 0; i < g->count; i++) {
            widget_t* w = &g->widgets[i];
            if (!w->visible || !w->enabled) continue;
            if (w->type == WIDGET_BUTTON) {
                bool over = point_in(w, mx, my);
                if (over != w->btn.hover) {
                    w->btn.hover = over;
                    g->dirty = true;
                }
                if (over) g->hover_idx = i;
            }
        }
        return false;
    }

    if (evt->type == GUI_EVENT_MOUSE_DOWN) {
        int mx = evt->mouse.x;
        int my = evt->mouse.y;

        /* Unfocus all text inputs */
        for (int i = 0; i < g->count; i++) {
            if (g->widgets[i].type == WIDGET_TEXTINPUT)
                g->widgets[i].input.cursor_vis = false;
        }
        g->focused_idx = -1;

        for (int i = 0; i < g->count; i++) {
            widget_t* w = &g->widgets[i];
            if (!w->visible || !w->enabled) continue;
            if (!point_in(w, mx, my)) continue;

            if (w->type == WIDGET_BUTTON) {
                w->btn.pressed = true;
                g->dirty = true;
            } else if (w->type == WIDGET_TEXTINPUT) {
                g->focused_idx = i;
                w->input.cursor_vis = true;
                w->input.blink_tick = timer_get_ticks();
                g->dirty = true;
            } else if (w->type == WIDGET_CHECKBOX) {
                w->chk.checked = !w->chk.checked;
                g->dirty = true;
                if (out_id) *out_id = w->id;
                return true;
            }
        }
        return false;
    }

    if (evt->type == GUI_EVENT_MOUSE_UP) {
        int mx = evt->mouse.x;
        int my = evt->mouse.y;
        for (int i = 0; i < g->count; i++) {
            widget_t* w = &g->widgets[i];
            if (!w->visible || !w->enabled) continue;
            if (w->type == WIDGET_BUTTON && w->btn.pressed) {
                w->btn.pressed = false;
                g->dirty = true;
                if (point_in(w, mx, my)) {
                    if (out_id) *out_id = w->id;
                    return true;
                }
            }
        }
        return false;
    }

    if (evt->type == GUI_EVENT_KEY_DOWN) {
        if (g->focused_idx < 0) return false;
        widget_t* w = &g->widgets[g->focused_idx];
        if (w->type != WIDGET_TEXTINPUT) return false;

        int kc = evt->key.keycode;
        if (kc == KEY_BACKSPACE) {
            if (w->input.cursor > 0) {
                /* Shift left */
                int c = w->input.cursor;
                for (int i = c - 1; i < w->input.len; i++)
                    w->input.buf[i] = w->input.buf[i + 1];
                w->input.len--;
                w->input.cursor--;
                g->dirty = true;
            }
        } else if (kc == KEY_DELETE) {
            if (w->input.cursor < w->input.len) {
                int c = w->input.cursor;
                for (int i = c; i < w->input.len; i++)
                    w->input.buf[i] = w->input.buf[i + 1];
                w->input.len--;
                g->dirty = true;
            }
        } else if (kc == KEY_LEFT_ARROW) {
            if (w->input.cursor > 0) { w->input.cursor--; g->dirty = true; }
        } else if (kc == KEY_RIGHT_ARROW) {
            if (w->input.cursor < w->input.len) { w->input.cursor++; g->dirty = true; }
        } else if (kc == KEY_HOME) {
            w->input.cursor = 0; w->input.scroll = 0; g->dirty = true;
        } else if (kc == KEY_END) {
            w->input.cursor = w->input.len; g->dirty = true;
        } else if (kc == KEY_ENTER) {
            if (out_id) *out_id = w->id;
            return true;
        } else if (evt->key.ch >= 0x20 && evt->key.ch < 0x7F) {
            if (w->input.len < WIDGET_TEXT_MAX - 1) {
                int c = w->input.cursor;
                /* Shift right */
                for (int i = w->input.len; i > c; i--)
                    w->input.buf[i] = w->input.buf[i - 1];
                w->input.buf[c] = evt->key.ch;
                w->input.len++;
                w->input.buf[w->input.len] = '\0';
                w->input.cursor++;
                g->dirty = true;
            }
        }

        /* Adjust horizontal scroll so cursor stays visible */
        int visible_chars = (w->w - INPUT_PAD_X * 2) / FONT_W;
        if (visible_chars < 1) visible_chars = 1;
        while (w->input.cursor - w->input.scroll >= visible_chars)
            w->input.scroll++;
        while (w->input.cursor < w->input.scroll)
            w->input.scroll--;

        return false; /* Not an "action" event */
    }

    return false;
}

void widget_group_tick(widget_group_t* g)
{
    uint32_t now = timer_get_ticks();
    for (int i = 0; i < g->count; i++) {
        widget_t* w = &g->widgets[i];
        if (w->type != WIDGET_TEXTINPUT) continue;
        if (g->focused_idx != i) {
            w->input.cursor_vis = false;
            continue;
        }
        if (now - w->input.blink_tick >= (uint32_t)BLINK_INTERVAL) {
            w->input.cursor_vis = !w->input.cursor_vis;
            w->input.blink_tick = now;
            g->dirty = true;
        }
    }
}

widget_t* widget_find(widget_group_t* g, uint32_t id)
{
    for (int i = 0; i < g->count; i++) {
        if (g->widgets[i].id == id) return &g->widgets[i];
    }
    return NULL;
}

void widget_set_text(widget_group_t* g, uint32_t id, const char* text)
{
    widget_t* w = widget_find(g, id);
    if (!w) return;
    if (w->type == WIDGET_TEXTINPUT) {
        strncpy(w->input.buf, text, WIDGET_TEXT_MAX - 1);
        w->input.buf[WIDGET_TEXT_MAX - 1] = '\0';
        w->input.len    = (int)strlen(w->input.buf);
        w->input.cursor = w->input.len;
        w->input.scroll = 0;
    } else if (w->type == WIDGET_LABEL) {
        strncpy(w->lbl.text, text, WIDGET_TEXT_MAX - 1);
        w->lbl.text[WIDGET_TEXT_MAX - 1] = '\0';
    }
    g->dirty = true;
}

void widget_set_label(widget_group_t* g, uint32_t id, const char* label)
{
    widget_t* w = widget_find(g, id);
    if (!w) return;
    if (w->type == WIDGET_BUTTON) {
        strncpy(w->btn.label, label, WIDGET_LABEL_MAX - 1);
        w->btn.label[WIDGET_LABEL_MAX - 1] = '\0';
    } else if (w->type == WIDGET_CHECKBOX) {
        strncpy(w->chk.label, label, WIDGET_LABEL_MAX - 1);
        w->chk.label[WIDGET_LABEL_MAX - 1] = '\0';
    }
    g->dirty = true;
}

void widget_set_enabled(widget_group_t* g, uint32_t id, bool enabled)
{
    widget_t* w = widget_find(g, id);
    if (w) { w->enabled = enabled; g->dirty = true; }
}

void widget_set_visible(widget_group_t* g, uint32_t id, bool visible)
{
    widget_t* w = widget_find(g, id);
    if (w) { w->visible = visible; g->dirty = true; }
}

void widget_set_progress(widget_group_t* g, uint32_t id, int value)
{
    widget_t* w = widget_find(g, id);
    if (w && w->type == WIDGET_PROGRESSBAR) {
        w->bar.value = value;
        g->dirty = true;
    }
}

void widget_set_checked(widget_group_t* g, uint32_t id, bool checked)
{
    widget_t* w = widget_find(g, id);
    if (w && w->type == WIDGET_CHECKBOX) {
        w->chk.checked = checked;
        g->dirty = true;
    }
}

const char* widget_get_text(widget_group_t* g, uint32_t id)
{
    widget_t* w = widget_find(g, id);
    if (!w) return "";
    if (w->type == WIDGET_TEXTINPUT) return w->input.buf;
    if (w->type == WIDGET_LABEL)     return w->lbl.text;
    return "";
}

bool widget_get_checked(widget_group_t* g, uint32_t id)
{
    widget_t* w = widget_find(g, id);
    return (w && w->type == WIDGET_CHECKBOX) ? w->chk.checked : false;
}

int widget_get_progress(widget_group_t* g, uint32_t id)
{
    widget_t* w = widget_find(g, id);
    return (w && w->type == WIDGET_PROGRESSBAR) ? w->bar.value : 0;
}
