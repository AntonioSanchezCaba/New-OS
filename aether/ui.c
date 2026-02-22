/*
 * aether/ui.c — Aether UI Scene Graph
 *
 * Pool-allocated node tree.  Nodes never malloc individually;
 * all live in a flat pool reset each frame (or on demand).
 *
 * Rendering order: depth-first, parent before children.
 * Hit testing: deepest (last drawn) matching child wins.
 */
#include <aether/ui.h>
#include <aether/input.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Node pool
 * ========================================================= */
#define UI_POOL_MAX  256

static ui_node_t g_pool[UI_POOL_MAX];
static int       g_pool_used = 0;

void ui_pool_reset(void)
{
    g_pool_used = 0;
}

ui_node_t* ui_node_alloc(ui_node_type_t type)
{
    if (g_pool_used >= UI_POOL_MAX) {
        klog_warn("ui: node pool exhausted");
        return NULL;
    }
    ui_node_t* n = &g_pool[g_pool_used++];
    memset(n, 0, sizeof(*n));
    n->type  = type;
    n->alpha = 255;
    return n;
}

/* =========================================================
 * Tree construction
 * ========================================================= */
void ui_node_add_child(ui_node_t* parent, ui_node_t* child)
{
    if (!parent || !child) return;
    if (parent->child_count >= UI_MAX_CHILDREN) {
        klog_warn("ui: child limit reached");
        return;
    }
    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

/* =========================================================
 * Auto-layout (FLOW containers)
 * ========================================================= */
void ui_node_layout(ui_node_t* node)
{
    if (!node) return;
    if (node->type == UI_FLOW) {
        int cursor = (node->flow_dir == UI_FLOW_H) ? node->pad_x : node->pad_y;
        for (int i = 0; i < node->child_count; i++) {
            ui_node_t* c = node->children[i];
            if (!c) continue;
            if (node->flow_dir == UI_FLOW_H) {
                c->x = cursor;
                c->y = node->pad_y;
                cursor += c->w + node->gap;
            } else {
                c->x = node->pad_x;
                c->y = cursor;
                cursor += c->h + node->gap;
            }
        }
    }
    /* Recurse */
    for (int i = 0; i < node->child_count; i++)
        ui_node_layout(node->children[i]);
}

/* =========================================================
 * Rendering helpers
 * ========================================================= */

/* Draw rounded-corner filled rectangle */
static void draw_rounded_rect(canvas_t* c, int x, int y, int w, int h,
                               acolor_t color, int radius)
{
    if (!c || !c->pixels || w <= 0 || h <= 0) return;
    /* For simplicity: if radius==0 use fast fill, else software round */
    if (radius <= 0) {
        draw_rect(c, x, y, w, h, color);
        return;
    }
    /* Fill center */
    draw_rect(c, x + radius, y,          w - 2*radius, h,          color);
    draw_rect(c, x,          y + radius,  w,            h - 2*radius, color);
    /* Four corner circles (filled quarter circles) */
    for (int ry = 0; ry < radius; ry++) {
        for (int rx = 0; rx < radius; rx++) {
            int fx = radius - rx - 1;
            int fy = radius - ry - 1;
            if (fx*fx + fy*fy <= radius*radius) {
                /* Top-left */
                draw_pixel(c, x + rx,           y + ry,           color);
                /* Top-right */
                draw_pixel(c, x + w - 1 - rx,  y + ry,           color);
                /* Bottom-left */
                draw_pixel(c, x + rx,           y + h - 1 - ry,  color);
                /* Bottom-right */
                draw_pixel(c, x + w - 1 - rx,  y + h - 1 - ry,  color);
            }
        }
    }
}

/* Draw a shadow under a rect (dark translucent strip below+right) */
static void draw_shadow(canvas_t* c, int x, int y, int w, int h)
{
    /* Simple 3px shadow offset */
    acolor_t shad = ACOLOR(0, 0, 0, 80);
    draw_rect(c, x + 3, y + h,     w,     3,   shad);
    draw_rect(c, x + w, y + 3,     3,     h,   shad);
    draw_rect(c, x + w, y + h,     3,     3,   shad);
}

/* =========================================================
 * Render a single node (no children)
 * ========================================================= */
static void render_node_self(ui_node_t* node, canvas_t* c, int ax, int ay)
{
    if (!node || !c) return;
    int x = ax + node->x;
    int y = ay + node->y;
    int w = node->w;
    int h = node->h;

    switch (node->type) {
    case UI_CARD:
        draw_shadow(c, x, y, w, h);
        /* fallthrough */
    case UI_PANEL:
        draw_rounded_rect(c, x, y, w, h, node->bg, node->radius);
        if (node->border > 0)
            draw_rect_outline(c, x, y, w, h, 1, node->border_color);
        break;

    case UI_FLOW:
        if (ACOLOR_A(node->bg) > 0)
            draw_rounded_rect(c, x, y, w, h, node->bg, node->radius);
        break;

    case UI_TEXT:
    case UI_LABEL: {
        if (ACOLOR_A(node->bg) > 0)
            draw_rect(c, x, y, w, h, node->bg);
        /* Text alignment */
        int tx = x + node->pad_x;
        int ty = y + (h - FONT_H) / 2;
        int text_w = (int)strlen(node->text) * FONT_W;
        if (node->text_align == UI_ALIGN_CENTER)
            tx = x + (w - text_w) / 2;
        else if (node->text_align == UI_ALIGN_RIGHT)
            tx = x + w - node->pad_x - text_w;
        draw_string(c, tx, ty, node->text, node->fg, ACOLOR(0,0,0,0));
        break;
    }

    case UI_BUTTON: {
        acolor_t bg = node->bg;
        if (node->pressed)
            bg = ACOLOR(ACOLOR_R(bg)*3/4, ACOLOR_G(bg)*3/4,
                        ACOLOR_B(bg)*3/4, ACOLOR_A(bg));
        else if (node->hovered)
            bg = ACOLOR(
                (uint8_t)(ACOLOR_R(bg) + 20 > 255 ? 255 : ACOLOR_R(bg)+20),
                (uint8_t)(ACOLOR_G(bg) + 20 > 255 ? 255 : ACOLOR_G(bg)+20),
                (uint8_t)(ACOLOR_B(bg) + 20 > 255 ? 255 : ACOLOR_B(bg)+20),
                ACOLOR_A(bg));
        draw_rounded_rect(c, x, y, w, h, bg, node->radius ? node->radius : 4);
        if (node->border > 0)
            draw_rect_outline(c, x, y, w, h, 1, node->border_color);
        /* Centered text */
        int tx = x + (w - (int)strlen(node->text) * FONT_W) / 2;
        int ty = y + (h - FONT_H) / 2;
        draw_string(c, tx, ty, node->text, node->fg, ACOLOR(0,0,0,0));
        break;
    }

    case UI_DIVIDER:
        draw_rect(c, x, y, w, node->border > 0 ? node->border : 1,
                       node->border_color);
        break;

    case UI_CUSTOM:
        if (node->render)
            node->render(node, c, x, y);
        break;

    default: break;
    }
}

/* =========================================================
 * Recursive render
 * ========================================================= */
void ui_node_render(ui_node_t* node, canvas_t* c, int ox, int oy)
{
    if (!node) return;
    render_node_self(node, c, ox, oy);
    int ax = ox + node->x;
    int ay = oy + node->y;
    for (int i = 0; i < node->child_count; i++)
        ui_node_render(node->children[i], c, ax, ay);
}

/* =========================================================
 * Hit testing (deepest match)
 * ========================================================= */
ui_node_t* ui_node_hit(ui_node_t* root, int x, int y)
{
    if (!root) return NULL;
    /* Check from last child backward (top-most rendered last) */
    for (int i = root->child_count - 1; i >= 0; i--) {
        ui_node_t* c = root->children[i];
        if (!c) continue;
        int cx = root->x + c->x, cy = root->y + c->y;
        if (x >= cx && x < cx + c->w && y >= cy && y < cy + c->h) {
            ui_node_t* deep = ui_node_hit(c, x - root->x, y - root->y);
            return deep ? deep : c;
        }
    }
    return NULL;
}

/* =========================================================
 * Event delivery
 * ========================================================= */
bool ui_node_event(ui_node_t* root, const struct input_event* ev,
                   int ox, int oy)
{
    if (!root) return false;

    /* Depth-first: children first */
    int ax = ox + root->x;
    int ay = oy + root->y;
    for (int i = root->child_count - 1; i >= 0; i--) {
        if (ui_node_event(root->children[i], ev, ax, ay))
            return true;
    }

    /* Update hover/press state for pointer events */
    if (ev->type == INPUT_POINTER) {
        int px = ev->pointer.x, py = ev->pointer.y;
        root->hovered = (px >= ax && px < ax + root->w &&
                         py >= ay && py < ay + root->h);
        if (root->hovered)
            root->pressed = (ev->pointer.buttons & IBTN_LEFT) != 0;
        else
            root->pressed = false;
    }

    if (root->on_event)
        return root->on_event(root, ev);

    return false;
}

/* =========================================================
 * State reset
 * ========================================================= */
void ui_node_reset_states(ui_node_t* root)
{
    if (!root) return;
    root->hovered = false;
    root->pressed = false;
    for (int i = 0; i < root->child_count; i++)
        ui_node_reset_states(root->children[i]);
}

/* =========================================================
 * Free subtree (return to pool — pool reset handles memory)
 * ========================================================= */
void ui_node_free(ui_node_t* root)
{
    /* Pool-based: nothing to free individually.
     * Caller should call ui_pool_reset() at frame start. */
    (void)root;
}

/* =========================================================
 * Convenience constructors
 * ========================================================= */
ui_node_t* ui_panel(int x, int y, int w, int h, acolor_t bg)
{
    ui_node_t* n = ui_node_alloc(UI_PANEL);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h; n->bg = bg;
    return n;
}

ui_node_t* ui_card(int x, int y, int w, int h, acolor_t bg)
{
    ui_node_t* n = ui_node_alloc(UI_CARD);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h; n->bg = bg;
    n->radius = 6;
    return n;
}

ui_node_t* ui_label(int x, int y, int w, int h, const char* text,
                    acolor_t fg, int align)
{
    ui_node_t* n = ui_node_alloc(UI_LABEL);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->fg = fg; n->bg = ACOLOR(0, 0, 0, 0);
    n->text_align = align;
    n->pad_x = 4; n->pad_y = 2;
    strncpy(n->text, text ? text : "", UI_TEXT_MAX - 1);
    return n;
}

ui_node_t* ui_button(int x, int y, int w, int h, const char* text,
                     ui_event_fn on_ev, void* ud)
{
    ui_node_t* n = ui_node_alloc(UI_BUTTON);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->bg = ACOLOR(0x30, 0x50, 0x90, 0xFF);
    n->fg = ACOLOR(0xFF, 0xFF, 0xFF, 0xFF);
    n->border_color = ACOLOR(0x50, 0x80, 0xC0, 0xFF);
    n->border = 1;
    n->radius = 4;
    n->on_event = on_ev;
    n->userdata = ud;
    strncpy(n->text, text ? text : "", UI_TEXT_MAX - 1);
    return n;
}

ui_node_t* ui_divider(int x, int y, int w, acolor_t color)
{
    ui_node_t* n = ui_node_alloc(UI_DIVIDER);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = 1;
    n->border_color = color;
    n->border = 1;
    return n;
}

ui_node_t* ui_flow(int x, int y, int w, int h, int dir, int gap)
{
    ui_node_t* n = ui_node_alloc(UI_FLOW);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->flow_dir = dir;
    n->gap = gap;
    n->bg = ACOLOR(0, 0, 0, 0);
    return n;
}

ui_node_t* ui_custom(int x, int y, int w, int h,
                     ui_render_fn fn, void* ud)
{
    ui_node_t* n = ui_node_alloc(UI_CUSTOM);
    if (!n) return NULL;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->render = fn;
    n->userdata = ud;
    return n;
}
