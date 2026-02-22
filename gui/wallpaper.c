/*
 * gui/wallpaper.c — Desktop wallpaper engine
 *
 * BMP loader supports: Windows BITMAPINFOHEADER (40-byte), 24-bit and 32-bit
 * uncompressed (BI_RGB). Bottom-up row order handled automatically.
 *
 * PPM loader supports: P6 binary (magic "P6"), 8-bit channels.
 *
 * All image data is stored in a heap-allocated pixel buffer (ARGB32).
 * wallpaper_draw() renders directly onto any canvas without an intermediate
 * surface — it scales with integer fixed-point arithmetic (16.16).
 */
#include <gui/wallpaper.h>
#include <gui/draw.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Internal state
 * ========================================================= */
static struct {
    wallpaper_mode_t  mode;
    wallpaper_scale_t scale;

    /* Solid / gradient */
    uint32_t          color_top;
    uint32_t          color_bot;

    /* Loaded image */
    uint32_t*         pixels;   /* ARGB32, heap-allocated */
    int               img_w;
    int               img_h;
} g_wp = {
    .mode      = WALLPAPER_GRADIENT,
    .color_top = 0xFF0D1B2A,
    .color_bot = 0xFF1A3A5C,
    .pixels    = NULL,
    .img_w     = 0,
    .img_h     = 0,
};

/* =========================================================
 * BMP loader
 * ========================================================= */
#pragma pack(push, 1)
typedef struct {
    uint16_t signature;      /* 'BM' = 0x4D42                  */
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t data_offset;    /* Offset to pixel data            */
    /* BITMAPINFOHEADER */
    uint32_t header_size;    /* 40 for BITMAPINFOHEADER         */
    int32_t  width;
    int32_t  height;         /* Negative = top-down             */
    uint16_t planes;
    uint16_t bpp;            /* 24 or 32                        */
    uint32_t compression;    /* 0 = BI_RGB                      */
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t clr_used;
    uint32_t clr_important;
} bmp_header_t;
#pragma pack(pop)

static int load_bmp(const uint8_t* data, int data_len)
{
    if (data_len < (int)sizeof(bmp_header_t)) return -1;
    const bmp_header_t* h = (const bmp_header_t*)data;

    if (h->signature != 0x4D42) return -1;
    if (h->compression != 0)     return -1;  /* Only BI_RGB */
    if (h->bpp != 24 && h->bpp != 32) return -1;

    int w = (int)h->width;
    int rh = (int)h->height;
    bool bottom_up = (rh > 0);
    int img_h = bottom_up ? rh : -rh;
    if (w <= 0 || img_h <= 0 || w > 4096 || img_h > 4096) return -1;

    uint32_t* px = (uint32_t*)kmalloc((size_t)(w * img_h) * sizeof(uint32_t));
    if (!px) return -1;

    int row_bytes = (h->bpp == 24) ? (w * 3) : (w * 4);
    /* BMP rows are padded to 4-byte boundary */
    int row_stride = (row_bytes + 3) & ~3;

    for (int y = 0; y < img_h; y++) {
        int src_y = bottom_up ? (img_h - 1 - y) : y;
        int row_off = (int)h->data_offset + src_y * row_stride;
        if (row_off + row_stride > data_len) break;
        const uint8_t* row = data + row_off;
        for (int x = 0; x < w; x++) {
            uint8_t b, g, r, a = 0xFF;
            if (h->bpp == 24) {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
            }
            px[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8)  |  (uint32_t)b;
        }
    }

    if (g_wp.pixels) kfree(g_wp.pixels);
    g_wp.pixels = px;
    g_wp.img_w  = w;
    g_wp.img_h  = img_h;
    return 0;
}

/* =========================================================
 * PPM loader (P6 binary)
 * ========================================================= */
static int parse_ppm_int(const uint8_t* data, int data_len, int* pos)
{
    /* Skip whitespace and comments */
    while (*pos < data_len &&
           (data[*pos] == ' '  || data[*pos] == '\t' ||
            data[*pos] == '\n' || data[*pos] == '\r' ||
            data[*pos] == '#')) {
        if (data[*pos] == '#')
            while (*pos < data_len && data[*pos] != '\n') (*pos)++;
        else (*pos)++;
    }
    int val = 0;
    while (*pos < data_len && data[*pos] >= '0' && data[*pos] <= '9') {
        val = val * 10 + (data[(*pos)++] - '0');
    }
    return val;
}

static int load_ppm(const uint8_t* data, int data_len)
{
    if (data_len < 7) return -1;
    if (data[0] != 'P' || data[1] != '6') return -1;

    int pos = 2;
    int w      = parse_ppm_int(data, data_len, &pos);
    int img_h  = parse_ppm_int(data, data_len, &pos);
    int maxval = parse_ppm_int(data, data_len, &pos);
    /* Skip exactly one whitespace byte */
    if (pos < data_len) pos++;

    if (w <= 0 || img_h <= 0 || maxval != 255) return -1;
    if (w > 4096 || img_h > 4096) return -1;
    if (pos + w * img_h * 3 > data_len) return -1;

    uint32_t* px = (uint32_t*)kmalloc((size_t)(w * img_h) * sizeof(uint32_t));
    if (!px) return -1;

    const uint8_t* pdata = data + pos;
    for (int i = 0; i < w * img_h; i++) {
        uint8_t r = pdata[i * 3 + 0];
        uint8_t g = pdata[i * 3 + 1];
        uint8_t b = pdata[i * 3 + 2];
        px[i] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    if (g_wp.pixels) kfree(g_wp.pixels);
    g_wp.pixels = px;
    g_wp.img_w  = w;
    g_wp.img_h  = img_h;
    return 0;
}

/* =========================================================
 * Bilinear-free scaled blit (nearest-neighbour, 16.16 fixed-point)
 * ========================================================= */
static void blit_scaled(canvas_t* c, int dst_x, int dst_y,
                         int dst_w, int dst_h)
{
    if (!g_wp.pixels || dst_w <= 0 || dst_h <= 0) return;

    /* Scale factors in 16.16 fixed-point */
    uint32_t sx = (uint32_t)(((uint32_t)g_wp.img_w << 16) / (uint32_t)dst_w);
    uint32_t sy = (uint32_t)(((uint32_t)g_wp.img_h << 16) / (uint32_t)dst_h);

    for (int y = 0; y < dst_h; y++) {
        int cy = dst_y + y;
        if (cy < 0 || cy >= c->height) continue;
        int src_y = (int)(((uint32_t)y * sy) >> 16);
        if (src_y >= g_wp.img_h) src_y = g_wp.img_h - 1;

        for (int x = 0; x < dst_w; x++) {
            int cx = dst_x + x;
            if (cx < 0 || cx >= c->width) continue;
            int src_x = (int)(((uint32_t)x * sx) >> 16);
            if (src_x >= g_wp.img_w) src_x = g_wp.img_w - 1;

            c->pixels[cy * c->width + cx] =
                g_wp.pixels[src_y * g_wp.img_w + src_x];
        }
    }
}

/* =========================================================
 * Public API
 * ========================================================= */
void wallpaper_init(void)
{
    g_wp.mode      = WALLPAPER_GRADIENT;
    g_wp.color_top = 0xFF0A1628;
    g_wp.color_bot = 0xFF1C3C64;
}

void wallpaper_set_color(uint32_t color)
{
    g_wp.mode      = WALLPAPER_SOLID;
    g_wp.color_top = color;
}

void wallpaper_set_gradient(uint32_t top, uint32_t bottom)
{
    g_wp.mode      = WALLPAPER_GRADIENT;
    g_wp.color_top = top;
    g_wp.color_bot = bottom;
}

int wallpaper_load(const char* path, wallpaper_scale_t scale)
{
    vfs_node_t* node = vfs_resolve_path(path);
    if (!node || !node->ops || !node->ops->read) return -1;
    if (node->flags & VFS_DIRECTORY) return -1;

    /* Read file into heap buffer */
    int size = (int)(node->size > 0 ? node->size : 4 * 1024 * 1024);
    if (size > 8 * 1024 * 1024) return -1;   /* 8MB sanity cap */
    uint8_t* buf = (uint8_t*)kmalloc((size_t)size);
    if (!buf) return -1;

    int n = node->ops->read(node, 0, size, (char*)buf);
    if (n <= 0) { kfree(buf); return -1; }

    int rc = -1;
    if (n >= 2 && buf[0] == 'B' && buf[1] == 'M')
        rc = load_bmp(buf, n);
    else if (n >= 2 && buf[0] == 'P' && buf[1] == '6')
        rc = load_ppm(buf, n);

    kfree(buf);
    if (rc == 0) {
        g_wp.mode  = WALLPAPER_IMAGE;
        g_wp.scale = scale;
        kinfo("WALLPAPER: loaded %s (%dx%d)", path, g_wp.img_w, g_wp.img_h);
    }
    return rc;
}

void wallpaper_draw(canvas_t* c)
{
    switch (g_wp.mode) {
    case WALLPAPER_SOLID:
        draw_rect(c, 0, 0, c->width, c->height, g_wp.color_top);
        break;

    case WALLPAPER_GRADIENT:
        draw_gradient_v(c, 0, 0, c->width, c->height,
                        g_wp.color_top, g_wp.color_bot);
        break;

    case WALLPAPER_IMAGE:
        if (!g_wp.pixels) {
            /* Fallback */
            draw_gradient_v(c, 0, 0, c->width, c->height,
                            g_wp.color_top, g_wp.color_bot);
            break;
        }
        switch (g_wp.scale) {
        case WP_SCALE_FILL:
            blit_scaled(c, 0, 0, c->width, c->height);
            break;
        case WP_SCALE_FIT: {
            int dw = c->width, dh = c->height;
            /* Maintain aspect ratio */
            int sw = g_wp.img_w * dh / g_wp.img_h;
            if (sw > dw) {
                int sh2 = g_wp.img_h * dw / g_wp.img_w;
                draw_rect(c, 0, 0, dw, (dh - sh2) / 2, 0xFF000000);
                draw_rect(c, 0, dh - (dh - sh2) / 2, dw, (dh - sh2) / 2, 0xFF000000);
                blit_scaled(c, 0, (dh - sh2) / 2, dw, sh2);
            } else {
                draw_rect(c, 0, 0, (dw - sw) / 2, dh, 0xFF000000);
                draw_rect(c, dw - (dw - sw) / 2, 0, (dw - sw) / 2, dh, 0xFF000000);
                blit_scaled(c, (dw - sw) / 2, 0, sw, dh);
            }
            break;
        }
        case WP_SCALE_CENTER: {
            draw_rect(c, 0, 0, c->width, c->height, 0xFF000000);
            int ox = (c->width  - g_wp.img_w) / 2;
            int oy = (c->height - g_wp.img_h) / 2;
            blit_scaled(c, ox, oy, g_wp.img_w, g_wp.img_h);
            break;
        }
        case WP_SCALE_TILE:
            for (int ty = 0; ty < c->height; ty += g_wp.img_h)
                for (int tx = 0; tx < c->width; tx += g_wp.img_w)
                    blit_scaled(c, tx, ty, g_wp.img_w, g_wp.img_h);
            break;
        }
        break;

    case WALLPAPER_TILED:
        /* Subtle dark grid pattern */
        draw_rect(c, 0, 0, c->width, c->height, 0xFF0D1B2A);
        for (int y = 0; y < c->height; y += 32)
            draw_hline(c, 0, y, c->width, 0x08FFFFFF);
        for (int x = 0; x < c->width; x += 32)
            draw_vline(c, x, 0, c->height, 0x08FFFFFF);
        break;
    }
}

wallpaper_mode_t wallpaper_get_mode(void) { return g_wp.mode; }
