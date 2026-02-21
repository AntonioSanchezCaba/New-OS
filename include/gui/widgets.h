/*
 * include/gui/widgets.h - Aether OS GUI Widget Toolkit
 *
 * Provides a lightweight set of reusable UI widgets:
 *   Button, Label, TextInput, CheckBox, ProgressBar, ScrollView
 *
 * Usage:
 *   widget_group_t g;
 *   widget_group_init(&g);
 *   uint32_t btn_id = widget_add_button(&g, 10, 10, 100, 30, "Click Me", 1);
 *   widget_group_draw(&canvas, &g);
 *   // In event handler:
 *   uint32_t triggered;
 *   if (widget_group_handle_event(&g, &evt, &triggered)) {
 *       if (triggered == btn_id) { ... }
 *   }
 */
#ifndef GUI_WIDGETS_H
#define GUI_WIDGETS_H

#include <types.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>

/* ---- Widget types ---- */
typedef enum {
    WIDGET_BUTTON      = 0,
    WIDGET_LABEL       = 1,
    WIDGET_TEXTINPUT   = 2,
    WIDGET_CHECKBOX    = 3,
    WIDGET_PROGRESSBAR = 4,
    WIDGET_SEPARATOR   = 5,
} widget_type_t;

/* Max text buffer sizes */
#define WIDGET_TEXT_MAX    128
#define WIDGET_LABEL_MAX   128

/* Max widgets per group */
#define WIDGET_GROUP_MAX   64

/* ---- Widget structure ---- */
typedef struct {
    widget_type_t type;
    uint32_t      id;        /* User-assigned unique identifier */
    int           x, y;     /* Position relative to canvas */
    int           w, h;     /* Size in pixels */
    bool          enabled;
    bool          visible;

    union {
        /* WIDGET_BUTTON */
        struct {
            char  label[WIDGET_LABEL_MAX];
            bool  pressed;          /* Currently held down */
            bool  hover;            /* Mouse is over */
        } btn;

        /* WIDGET_LABEL */
        struct {
            char     text[WIDGET_TEXT_MAX];
            uint32_t color;         /* Text color (0 = use theme) */
            bool     bold;
        } lbl;

        /* WIDGET_TEXTINPUT */
        struct {
            char     buf[WIDGET_TEXT_MAX];
            int      len;
            int      cursor;        /* Cursor position */
            int      scroll;        /* Horizontal scroll offset (chars) */
            char     placeholder[WIDGET_LABEL_MAX];
            bool     password;      /* Mask with '*' */
            bool     cursor_vis;
            uint32_t blink_tick;
        } input;

        /* WIDGET_CHECKBOX */
        struct {
            char  label[WIDGET_LABEL_MAX];
            bool  checked;
        } chk;

        /* WIDGET_PROGRESSBAR */
        struct {
            int      value;         /* Current value */
            int      max;           /* Maximum value */
            uint32_t fill_color;    /* 0 = use theme accent */
            bool     show_text;     /* Show percentage */
        } bar;

        /* WIDGET_SEPARATOR — no extra data */
        struct {
            bool horizontal;        /* true=horiz, false=vert */
        } sep;
    };
} widget_t;

/* ---- Widget group (a set of widgets for one window) ---- */
typedef struct {
    widget_t widgets[WIDGET_GROUP_MAX];
    int      count;
    int      focused_idx;   /* -1 = none */
    int      hover_idx;     /* -1 = none */
    bool     dirty;         /* Needs redraw */
} widget_group_t;

/* ---- API ---- */

/* Initialize an empty widget group */
void widget_group_init(widget_group_t* g);

/* Add widgets — return the widget index (or -1 on failure) */
int widget_add_button(widget_group_t* g, int x, int y, int w, int h,
                      const char* label, uint32_t id);
int widget_add_label(widget_group_t* g, int x, int y, int w, int h,
                     const char* text, uint32_t id);
int widget_add_label_colored(widget_group_t* g, int x, int y, int w, int h,
                              const char* text, uint32_t id, uint32_t color);
int widget_add_textinput(widget_group_t* g, int x, int y, int w, int h,
                          const char* placeholder, uint32_t id);
int widget_add_checkbox(widget_group_t* g, int x, int y,
                         const char* label, bool checked, uint32_t id);
int widget_add_progressbar(widget_group_t* g, int x, int y, int w, int h,
                            int value, int max, uint32_t id);
int widget_add_separator(widget_group_t* g, int x, int y, int len,
                          bool horizontal, uint32_t id);

/* Draw all visible widgets in the group */
void widget_group_draw(canvas_t* c, widget_group_t* g);

/*
 * Handle a GUI event.
 * Returns true if an action was triggered (button clicked, checkbox toggled).
 * Sets *out_id to the widget ID that triggered the action.
 */
bool widget_group_handle_event(widget_group_t* g, const gui_event_t* evt,
                                uint32_t* out_id);

/* Per-frame tick (handles cursor blink etc.) */
void widget_group_tick(widget_group_t* g);

/* Find a widget by id */
widget_t* widget_find(widget_group_t* g, uint32_t id);

/* State setters */
void widget_set_text(widget_group_t* g, uint32_t id, const char* text);
void widget_set_label(widget_group_t* g, uint32_t id, const char* label);
void widget_set_enabled(widget_group_t* g, uint32_t id, bool enabled);
void widget_set_visible(widget_group_t* g, uint32_t id, bool visible);
void widget_set_progress(widget_group_t* g, uint32_t id, int value);
void widget_set_checked(widget_group_t* g, uint32_t id, bool checked);

/* State getters */
const char* widget_get_text(widget_group_t* g, uint32_t id);
bool        widget_get_checked(widget_group_t* g, uint32_t id);
int         widget_get_progress(widget_group_t* g, uint32_t id);

#endif /* GUI_WIDGETS_H */
