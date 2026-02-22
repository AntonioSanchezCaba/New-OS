/*
 * include/aether/ui.h — Aether UI Scene Graph
 *
 * Surfaces render their content using a scene graph (tree of ui_node_t).
 * This replaces the widget-button model of classic toolkits.
 *
 * Node types:
 *   PANEL   — rectangular area, background color, optional border
 *   CARD    — elevated panel with shadow
 *   FLOW    — horizontal or vertical flex container
 *   TEXT    — rendered string with alignment
 *   LABEL   — static text (no input)
 *   BUTTON  — interactive text/icon element
 *   DIVIDER — separator line
 *   CUSTOM  — arbitrary render function
 *
 * Tree rendering:
 *   ui_node_render(root, canvas, 0, 0) recurses depth-first.
 *   Hit-testing: ui_node_hit(root, x, y) returns deepest hit node.
 *   Event delivery: ui_node_event(root, event) walks tree until consumed.
 */
#pragma once
#include <types.h>
#include <aether/vec.h>
#include <aether/input.h>
#include <gui/draw.h>

struct ui_node;

/* Input callback on a node */
typedef bool (*ui_event_fn)(struct ui_node* node,
                             const struct input_event* ev);

/* Custom render function */
typedef void (*ui_render_fn)(struct ui_node* node, canvas_t* c,
                              int abs_x, int abs_y);

/* =========================================================
 * Node types
 * ========================================================= */
typedef enum {
    UI_PANEL   = 0,
    UI_CARD    = 1,
    UI_FLOW    = 2,
    UI_TEXT    = 3,
    UI_LABEL   = 4,
    UI_BUTTON  = 5,
    UI_DIVIDER = 6,
    UI_CUSTOM  = 7,
} ui_node_type_t;

#define UI_MAX_CHILDREN  16
#define UI_TEXT_MAX      128

/* Alignment */
#define UI_ALIGN_LEFT    0
#define UI_ALIGN_CENTER  1
#define UI_ALIGN_RIGHT   2

/* Flow direction */
#define UI_FLOW_H  0   /* Horizontal */
#define UI_FLOW_V  1   /* Vertical */

/* =========================================================
 * Node
 * ========================================================= */
typedef struct ui_node {
    ui_node_type_t type;

    /* Geometry — relative to parent */
    int  x, y, w, h;

    /* Padding inside this node */
    int  pad_x, pad_y;

    /* Gap between children (FLOW) */
    int  gap;

    /* Visual */
    acolor_t  bg;
    acolor_t  fg;
    acolor_t  border_color;
    int       border;       /* Border thickness in px, 0=none */
    int       radius;       /* Corner radius */
    uint8_t   alpha;        /* Node opacity 0-255 */

    /* Content */
    char           text[UI_TEXT_MAX];
    int            text_align;   /* UI_ALIGN_* */
    int            flow_dir;     /* UI_FLOW_H / V */

    /* Custom render */
    ui_render_fn   render;

    /* Input */
    ui_event_fn    on_event;
    bool           hovered;
    bool           pressed;
    void*          userdata;

    /* Tree */
    struct ui_node* parent;
    struct ui_node* children[UI_MAX_CHILDREN];
    int             child_count;
} ui_node_t;

/* =========================================================
 * API
 * ========================================================= */

/* Allocate a node from the per-surface pool (max 256 nodes).
 * Returns NULL if pool exhausted. */
ui_node_t* ui_node_alloc(ui_node_type_t type);

/* Add child to parent */
void ui_node_add_child(ui_node_t* parent, ui_node_t* child);

/* Render the subtree rooted at node onto canvas */
void ui_node_render(ui_node_t* node, canvas_t* c, int ox, int oy);

/* Hit-test: returns deepest node at (x,y), or NULL */
ui_node_t* ui_node_hit(ui_node_t* root, int x, int y);

/* Deliver an input event to the tree.
 * Returns true if consumed. */
bool ui_node_event(ui_node_t* root, const struct input_event* ev,
                    int ox, int oy);

/* Auto-layout FLOW containers (sets children x/y from flow_dir + gap) */
void ui_node_layout(ui_node_t* node);

/* Free entire subtree (returns nodes to pool) */
void ui_node_free(ui_node_t* root);

/* Per-frame reset of hover/press states */
void ui_node_reset_states(ui_node_t* root);

/* Convenience constructors */
ui_node_t* ui_panel (int x, int y, int w, int h, acolor_t bg);
ui_node_t* ui_card  (int x, int y, int w, int h, acolor_t bg);
ui_node_t* ui_label (int x, int y, int w, int h, const char* text,
                      acolor_t fg, int align);
ui_node_t* ui_button(int x, int y, int w, int h, const char* text,
                      ui_event_fn on_ev, void* ud);
ui_node_t* ui_divider(int x, int y, int w, acolor_t color);
ui_node_t* ui_flow  (int x, int y, int w, int h, int dir, int gap);
ui_node_t* ui_custom(int x, int y, int w, int h,
                      ui_render_fn fn, void* ud);

/* Reset the node pool (called at start of each surface frame) */
void ui_pool_reset(void);
