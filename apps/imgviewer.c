/*
 * apps/imgviewer.c — AetherOS Image Viewer (ARE SURF_FLOAT)
 *
 * Supports BMP (24/32-bit uncompressed BI_RGB) and PPM (P6 binary).
 * Features:
 *   - Nearest-neighbour zoom (+/- keys or toolbar buttons, 10–800%)
 *   - Arrow-key and left-button drag panning
 *   - Fit-to-window and 1:1 pixel modes
 *   - Toolbar: [Zoom-] [Zoom+] [Fit] [1:1]
 *   - Status bar: filename, dimensions, zoom %
 *   - Opens via surface_imgviewer_open(path); singleton window.
 *
 * BMP loader: Windows BITMAPINFOHEADER (40-byte), 24/32-bit, uncompressed.
 * PPM loader: P6 binary magic, 8-bit channels, maxval=255.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <gui/event.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Geometry
 * ========================================================= */
#define IV_W          620
#define IV_H          460
#define IV_TB_H       28
#define IV_SB_H       20
#define IV_VIEW_Y     IV_TB_H
#define IV_VIEW_H     (IV_H - IV_TB_H - IV_SB_H)
#define IV_ZOOM_MIN   10
#define IV_ZOOM_MAX   800
#define IV_ZOOM_STEP  25
#define TB_BTN_W      36
#define TB_BTN_H      (IV_TB_H - 8)
#define TB_BTN_Y      4

/* Colours */
#define IV_BG         ACOLOR(0x0A, 0x0A, 0x12, 0xFF)
#define IV_TB_BG      ACOLOR(0x14, 0x18, 0x24, 0xFF)
#define IV_SB_BG      ACOLOR(0x10, 0x14, 0x1E, 0xFF)
#define IV_BTN_BG     ACOLOR(0x20, 0x28, 0x40, 0xFF)
#define IV_BTN_FG     ACOLOR(0xD0, 0xE0, 0xFF, 0xFF)
#define IV_SB_FG      ACOLOR(0x70, 0x80, 0xA0, 0xFF)
#define IV_GRID_A     ACOLOR(0x1A, 0x1A, 0x1A, 0xFF)
#define IV_GRID_B     ACOLOR(0x22, 0x22, 0x22, 0xFF)

/* =========================================================
 * BMP header
 * ========================================================= */
#pragma pack(push, 1)
typedef struct {
    uint16_t signature;
    uint32_t file_size;
    uint16_t reserved1, reserved2;
    uint32_t data_offset;
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes, bpp;
    uint32_t compression, image_size;
    int32_t  x_ppm, y_ppm;
    uint32_t clr_used, clr_important;
} bmp_hdr_t;
#pragma pack(pop)

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    sid_t     sid;
    bool      open;

    /* Loaded image */
    uint32_t* px;
    int       img_w, img_h;
    bool      has_image;
    char      path[128];

    /* View state */
    int       zoom;    /* percent: 10..800 */
    int       pan_x, pan_y;

    /* Drag */
    bool      dragging;
    int       drag_mx, drag_my;
    int       drag_px, drag_py;
} iv_state_t;

static iv_state_t g_iv;

/* =========================================================
 * Image loaders
 * ========================================================= */
static bool iv_load_bmp(const uint8_t* data, int len)
{
    if (len < (int)sizeof(bmp_hdr_t)) return false;
    const bmp_hdr_t* h = (const bmp_hdr_t*)data;
    if (h->signature != 0x4D42 || h->compression != 0) return false;
    if (h->bpp != 24 && h->bpp != 32) return false;

    int w = h->width;
    int rh = h->height;
    bool bottom_up = (rh > 0);
    int ih = bottom_up ? rh : -rh;
    if (w <= 0 || ih <= 0 || w > 4096 || ih > 4096) return false;

    uint32_t* px = (uint32_t*)kmalloc((size_t)(w * ih) * sizeof(uint32_t));
    if (!px) return false;

    int rb = (h->bpp == 24) ? (w * 3) : (w * 4);
    int rs = (rb + 3) & ~3;

    for (int y = 0; y < ih; y++) {
        int sy = bottom_up ? (ih - 1 - y) : y;
        int off = (int)h->data_offset + sy * rs;
        if (off + rs > len) break;
        const uint8_t* row = data + off;
        for (int x = 0; x < w; x++) {
            uint8_t b, g, r, a = 0xFF;
            if (h->bpp == 24) { b = row[x*3]; g = row[x*3+1]; r = row[x*3+2]; }
            else              { b = row[x*4]; g = row[x*4+1]; r = row[x*4+2]; a = row[x*4+3]; }
            px[y * w + x] = ((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
        }
    }

    if (g_iv.px) kfree(g_iv.px);
    g_iv.px    = px;
    g_iv.img_w = w;
    g_iv.img_h = ih;
    return true;
}

static bool iv_load_ppm(const uint8_t* data, int len)
{
    if (len < 7 || data[0] != 'P' || data[1] != '6') return false;
    int pos = 2;
    /* Parse integer fields with whitespace/comment skipping */
    int w = 0, ih = 0, maxval = 0;
    /* manual integer parsing */
    for (int field = 0; field < 3; field++) {
        while (pos < len && (data[pos]==' '||data[pos]=='\t'||
               data[pos]=='\r'||data[pos]=='\n'||data[pos]=='#')) {
            if (data[pos]=='#') while(pos<len&&data[pos]!='\n') pos++;
            else pos++;
        }
        int v = 0;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9')
            v = v * 10 + (data[pos++] - '0');
        if (field == 0) w = v;
        else if (field == 1) ih = v;
        else maxval = v;
    }
    if (pos < len) pos++;   /* skip one whitespace */
    if (w <= 0 || ih <= 0 || maxval != 255) return false;
    if (w > 4096 || ih > 4096) return false;
    if (pos + w * ih * 3 > len) return false;

    uint32_t* px = (uint32_t*)kmalloc((size_t)(w * ih) * sizeof(uint32_t));
    if (!px) return false;
    const uint8_t* pd = data + pos;
    for (int i = 0; i < w * ih; i++) {
        uint8_t r = pd[i*3], gv = pd[i*3+1], b = pd[i*3+2];
        px[i] = 0xFF000000|((uint32_t)r<<16)|((uint32_t)gv<<8)|b;
    }
    if (g_iv.px) kfree(g_iv.px);
    g_iv.px    = px;
    g_iv.img_w = w;
    g_iv.img_h = ih;
    return true;
}

static bool iv_load(const char* path)
{
    vfs_node_t* node = vfs_resolve_path(path);
    if (!node || (node->flags & VFS_DIRECTORY)) return false;

    int sz = (int)(node->size > 0 ? node->size : 4*1024*1024);
    if (sz > 16*1024*1024) return false;
    uint8_t* buf = (uint8_t*)kmalloc((size_t)sz);
    if (!buf) return false;

    int n = node->ops ? node->ops->read(node, 0, sz, (char*)buf) : -1;
    if (n <= 0) { kfree(buf); return false; }

    bool ok = false;
    if (n >= 2 && buf[0] == 'B' && buf[1] == 'M') ok = iv_load_bmp(buf, n);
    else if (n >= 2 && buf[0] == 'P' && buf[1] == '6') ok = iv_load_ppm(buf, n);
    kfree(buf);

    if (ok) {
        strncpy(g_iv.path, path, sizeof(g_iv.path) - 1);
        g_iv.path[sizeof(g_iv.path)-1] = '\0';
        g_iv.has_image = true;
        /* Fit to view by default */
        int vw = IV_W, vh = IV_VIEW_H;
        int zw = vw * 100 / g_iv.img_w;
        int zh = vh * 100 / g_iv.img_h;
        g_iv.zoom  = (zw < zh) ? zw : zh;
        if (g_iv.zoom < IV_ZOOM_MIN) g_iv.zoom = IV_ZOOM_MIN;
        if (g_iv.zoom > 100)         g_iv.zoom = 100;
        g_iv.pan_x = 0; g_iv.pan_y = 0;
    }
    return ok;
}

/* =========================================================
 * Toolbar button geometry
 * ========================================================= */
typedef struct { int x; const char* label; } iv_btn_t;
#define IV_NBTN 4
static iv_btn_t iv_btns[IV_NBTN] = {
    {  4, "-"   },
    { 44, "+"   },
    { 84, "Fit" },
    {128, "1:1" },
};

/* =========================================================
 * Render
 * ========================================================= */
static void iv_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                       void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = (int)w, .height = (int)h };

    /* Background */
    draw_rect(&c, 0, 0, (int)w, (int)h, IV_BG);

    /* Toolbar */
    draw_rect(&c, 0, 0, (int)w, IV_TB_H, IV_TB_BG);
    for (int i = 0; i < IV_NBTN; i++) {
        draw_rect(&c, iv_btns[i].x, TB_BTN_Y, TB_BTN_W, TB_BTN_H, IV_BTN_BG);
        int lw = (int)strlen(iv_btns[i].label) * FONT_W;
        draw_string(&c, iv_btns[i].x + (TB_BTN_W - lw)/2,
                    TB_BTN_Y + (TB_BTN_H - FONT_H)/2,
                    iv_btns[i].label, IV_BTN_FG, ACOLOR(0,0,0,0));
    }

    /* View area */
    int vx0 = 0, vy0 = IV_VIEW_Y;
    int vw = (int)w, vh = IV_VIEW_H;

    if (!g_iv.has_image) {
        /* Checkerboard + hint */
        for (int cy = 0; cy < vh; cy += 16)
            for (int cx = 0; cx < vw; cx += 16) {
                acolor_t col = ((cx/16 + cy/16) & 1) ? IV_GRID_B : IV_GRID_A;
                int bw = (cx + 16 <= vw) ? 16 : vw - cx;
                int bh = (cy + 16 <= vh) ? 16 : vh - cy;
                draw_rect(&c, vx0 + cx, vy0 + cy, bw, bh, col);
            }
        const char* hint = "No image loaded";
        draw_string(&c, vx0 + vw/2 - (int)strlen(hint)*FONT_W/2,
                    vy0 + vh/2 - FONT_H/2,
                    hint, IV_SB_FG, ACOLOR(0,0,0,0));
    } else {
        /* Scaled, panned image blit */
        int disp_w = g_iv.img_w * g_iv.zoom / 100;
        int disp_h = g_iv.img_h * g_iv.zoom / 100;
        int img_x  = vx0 + vw/2 - disp_w/2 + g_iv.pan_x;
        int img_y  = vy0 + vh/2 - disp_h/2 + g_iv.pan_y;

        /* Checkerboard background */
        for (int cy = 0; cy < vh; cy += 16)
            for (int cx = 0; cx < vw; cx += 16) {
                acolor_t col = ((cx/16 + cy/16) & 1) ? IV_GRID_B : IV_GRID_A;
                int bw = (cx + 16 <= vw) ? 16 : vw - cx;
                int bh = (cy + 16 <= vh) ? 16 : vh - cy;
                draw_rect(&c, vx0 + cx, vy0 + cy, bw, bh, col);
            }

        /* Nearest-neighbour scale blit */
        for (int dy = 0; dy < disp_h; dy++) {
            int py = img_y + dy;
            if (py < vy0 || py >= vy0 + vh) continue;
            int sy = dy * g_iv.img_h / disp_h;
            if (sy >= g_iv.img_h) continue;
            for (int dx = 0; dx < disp_w; dx++) {
                int px2 = img_x + dx;
                if (px2 < vx0 || px2 >= vx0 + vw) continue;
                int sx = dx * g_iv.img_w / disp_w;
                if (sx >= g_iv.img_w) continue;
                uint32_t sp = g_iv.px[sy * g_iv.img_w + sx];
                uint8_t  sa = (uint8_t)(sp >> 24);
                if (sa == 0) continue;
                if (sa == 0xFF) {
                    c.pixels[py * (int)w + px2] = sp;
                } else {
                    uint32_t dp = c.pixels[py * (int)w + px2];
                    uint8_t ia = 255 - sa;
                    uint8_t r = (uint8_t)(((sp>>16&0xFF)*sa + (dp>>16&0xFF)*ia)/255);
                    uint8_t g2= (uint8_t)(((sp>> 8&0xFF)*sa + (dp>> 8&0xFF)*ia)/255);
                    uint8_t b = (uint8_t)(((sp    &0xFF)*sa + (dp    &0xFF)*ia)/255);
                    c.pixels[py*(int)w+px2] = 0xFF000000|(r<<16)|(g2<<8)|b;
                }
            }
        }
    }

    /* Status bar */
    int sb_y = (int)h - IV_SB_H;
    draw_rect(&c, 0, sb_y, (int)w, IV_SB_H, IV_SB_BG);
    draw_rect(&c, 0, sb_y, (int)w, 1, ACOLOR(0x20,0x28,0x38,0xFF));
    if (g_iv.has_image) {
        char sbuf[80];
        snprintf(sbuf, sizeof(sbuf), "%s  %dx%d  %d%%",
                 g_iv.path, g_iv.img_w, g_iv.img_h, g_iv.zoom);
        draw_string(&c, 8, sb_y + (IV_SB_H - FONT_H)/2,
                    sbuf, IV_SB_FG, ACOLOR(0,0,0,0));
    } else {
        draw_string(&c, 8, sb_y + (IV_SB_H - FONT_H)/2,
                    "Open a .bmp or .ppm file from the terminal: open <path>",
                    IV_SB_FG, ACOLOR(0,0,0,0));
    }
}

/* =========================================================
 * Input
 * ========================================================= */
static void iv_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    bool dirty = false;

    if (ev->type == INPUT_POINTER) {
        int mx = ev->pointer.x, my = ev->pointer.y;
        bool btn_dn = (ev->pointer.buttons & IBTN_LEFT) &&
                     !(ev->pointer.prev_buttons & IBTN_LEFT);
        bool btn_up = !(ev->pointer.buttons & IBTN_LEFT) &&
                      (ev->pointer.prev_buttons & IBTN_LEFT);

        /* Toolbar clicks */
        if (btn_dn && my < IV_TB_H) {
            for (int i = 0; i < IV_NBTN; i++) {
                if (mx >= iv_btns[i].x && mx < iv_btns[i].x + TB_BTN_W) {
                    switch (i) {
                    case 0: g_iv.zoom -= IV_ZOOM_STEP; break;
                    case 1: g_iv.zoom += IV_ZOOM_STEP; break;
                    case 2:
                        if (g_iv.has_image) {
                            int zw = IV_W   * 100 / g_iv.img_w;
                            int zh = IV_VIEW_H * 100 / g_iv.img_h;
                            g_iv.zoom = (zw < zh) ? zw : zh;
                        }
                        g_iv.pan_x = 0; g_iv.pan_y = 0;
                        break;
                    case 3: g_iv.zoom = 100; g_iv.pan_x = 0; g_iv.pan_y = 0; break;
                    }
                    if (g_iv.zoom < IV_ZOOM_MIN) g_iv.zoom = IV_ZOOM_MIN;
                    if (g_iv.zoom > IV_ZOOM_MAX) g_iv.zoom = IV_ZOOM_MAX;
                    dirty = true;
                    break;
                }
            }
        }

        /* Pan drag */
        if (my >= IV_VIEW_Y && my < IV_VIEW_Y + IV_VIEW_H) {
            if (btn_dn) {
                g_iv.dragging = true;
                g_iv.drag_mx  = mx; g_iv.drag_my  = my;
                g_iv.drag_px  = g_iv.pan_x; g_iv.drag_py = g_iv.pan_y;
            }
        }
        if (g_iv.dragging && (ev->pointer.buttons & IBTN_LEFT)) {
            g_iv.pan_x = g_iv.drag_px + (mx - g_iv.drag_mx);
            g_iv.pan_y = g_iv.drag_py + (my - g_iv.drag_my);
            dirty = true;
        }
        if (btn_up) g_iv.dragging = false;

        /* Scroll wheel zoom */
        if (ev->pointer.scroll != 0 && my >= IV_VIEW_Y) {
            g_iv.zoom += ev->pointer.scroll * IV_ZOOM_STEP;
            if (g_iv.zoom < IV_ZOOM_MIN) g_iv.zoom = IV_ZOOM_MIN;
            if (g_iv.zoom > IV_ZOOM_MAX) g_iv.zoom = IV_ZOOM_MAX;
            dirty = true;
        }
    }

    if (ev->type == INPUT_KEY && ev->key.down) {
        switch (ev->key.keycode) {
        case '+': case '=':
            g_iv.zoom += IV_ZOOM_STEP;
            if (g_iv.zoom > IV_ZOOM_MAX) g_iv.zoom = IV_ZOOM_MAX;
            dirty = true; break;
        case '-':
            g_iv.zoom -= IV_ZOOM_STEP;
            if (g_iv.zoom < IV_ZOOM_MIN) g_iv.zoom = IV_ZOOM_MIN;
            dirty = true; break;
        case '0':
            g_iv.zoom = 100; g_iv.pan_x = 0; g_iv.pan_y = 0;
            dirty = true; break;
        case KEY_LEFT_ARROW:  g_iv.pan_x += 32; dirty = true; break;
        case KEY_RIGHT_ARROW: g_iv.pan_x -= 32; dirty = true; break;
        case KEY_UP_ARROW:    g_iv.pan_y += 32; dirty = true; break;
        case KEY_DOWN_ARROW:  g_iv.pan_y -= 32; dirty = true; break;
        }
    }

    if (dirty) surface_invalidate(id);
}

/* =========================================================
 * Close
 * ========================================================= */
static void iv_on_close(sid_t id, void* ud)
{
    (void)id; (void)ud;
    if (g_iv.px) { kfree(g_iv.px); g_iv.px = NULL; }
    g_iv.open      = false;
    g_iv.has_image = false;
    g_iv.sid       = SID_NONE;
}

/* =========================================================
 * Public API
 * ========================================================= */
sid_t surface_imgviewer_open(const char* path)
{
    if (g_iv.open && g_iv.sid != SID_NONE) {
        /* Already open: load new image if path provided */
        if (path && path[0]) {
            iv_load(path);
            surface_invalidate(g_iv.sid);
        }
        return g_iv.sid;
    }

    memset(&g_iv, 0, sizeof(g_iv));
    g_iv.open = true;
    g_iv.zoom = 100;

    if (path && path[0]) iv_load(path);

    g_iv.sid = are_add_surface(SURF_FLOAT, IV_W, IV_H,
                                "Image Viewer", "I",
                                iv_render, iv_input,
                                iv_on_close, NULL);
    surface_invalidate(g_iv.sid);
    return g_iv.sid;
}

/* Legacy GUI API stubs — never called when ARE is active */
typedef int wid_t;
wid_t app_imgviewer_create(void) { surface_imgviewer_open(NULL); return -1; }
void  imgviewer_tick(void) {}
