/*
 * apps/imgviewer.c — AetherOS Image Viewer
 *
 * Supports BMP (24/32-bit uncompressed) and PPM (P6 binary) images
 * loaded from the VFS.  Renders into a GUI window with:
 *   - Nearest-neighbour zoom  (+/- keys, 0 = fit, 1 = 1:1)
 *   - Arrow-key + left-button drag panning
 *   - Toolbar: Zoom-, Zoom+, Fit, 1:1
 *   - Status bar: filename, image dimensions, zoom %
 *   - L key = load /sys/wallpapers/default.bmp
 *
 * Integration: call app_imgviewer_create() or app_imgviewer_open(path).
 * imgviewer_tick() is called from gui_run() for periodic redraws.
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <kernel/version.h>

/* =========================================================
 * Window dimensions
 * ========================================================= */
#define IV_W       640
#define IV_H       480
#define IV_SB_H     22
#define IV_TB_H     28
#define IV_VIEW_Y  (IV_TB_H)
#define IV_VIEW_H  (IV_H - IV_TB_H - IV_SB_H)
#define TB_BTN_H   (IV_TB_H - 8)
#define TB_BTN_Y   4

#define IV_ZOOM_MIN   10
#define IV_ZOOM_MAX  800
#define IV_ZOOM_STEP  25

/* =========================================================
 * App state
 * ========================================================= */
typedef struct {
    uint32_t* px;
    int       w, h;
} iv_img_t;

typedef struct {
    wid_t    wid;
    iv_img_t img;
    char     path[128];
    int      zoom;
    int      pan_x, pan_y;
    bool     loaded;
    bool     dragging;
    int      drag_mx, drag_my;
    int      drag_px, drag_py;
} iv_t;

static iv_t* g_iv = NULL;

/* =========================================================
 * Helpers
 * ========================================================= */
static void fmt_int(char* buf, size_t sz, int n)
{
    char tmp[16]; int i = 0;
    if (!sz) return;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    bool neg = (n < 0); if (neg) n = -n;
    while (n > 0) { tmp[i++] = '0' + (n%10); n /= 10; }
    int j = 0;
    if (neg && j < (int)sz-1) buf[j++] = '-';
    for (int k = i-1; k >= 0 && j < (int)sz-1; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

/* =========================================================
 * BMP loader (24/32-bit BI_RGB)
 * ========================================================= */
static int load_bmp(iv_img_t* im, const uint8_t* d, size_t sz)
{
    if (sz < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t poff = *(const uint32_t*)(d+10);
    int32_t  W    = *(const int32_t* )(d+18);
    int32_t  H    = *(const int32_t* )(d+22);
    uint16_t bpp  = *(const uint16_t*)(d+28);
    if (*(const uint32_t*)(d+30) != 0) return -1; /* BI_RGB only */
    if (bpp != 24 && bpp != 32) return -1;
    if (W <= 0 || W > 4096) return -1;
    bool flip = (H > 0); if (H < 0) H = -H;
    if (H > 4096) return -1;
    int Bpp = bpp/8, rb = (W*Bpp+3)&~3;
    if (poff + (uint32_t)(rb*H) > sz) return -1;
    im->px = (uint32_t*)kmalloc((size_t)(W*H)*4);
    if (!im->px) return -1;
    im->w = W; im->h = H;
    for (int row = 0; row < H; row++) {
        int sr = flip ? H-1-row : row;
        const uint8_t* s = d + poff + (size_t)(sr*rb);
        uint32_t* dst = im->px + row*W;
        for (int c = 0; c < W; c++) {
            uint8_t b=s[c*Bpp], g=s[c*Bpp+1], r=s[c*Bpp+2];
            uint8_t a = (bpp==32) ? s[c*Bpp+3] : 0xFF;
            dst[c] = rgba(r,g,b,a);
        }
    }
    return 0;
}

/* =========================================================
 * PPM loader (P6 binary)
 * ========================================================= */
static int ppm_int(const uint8_t* d, size_t sz, size_t* p)
{
    while (*p < sz && (d[*p] <= ' ' || d[*p] == '#')) {
        if (d[*p] == '#') while (*p < sz && d[*p] != '\n') (*p)++;
        else (*p)++;
    }
    int v = 0;
    while (*p < sz && d[*p] >= '0' && d[*p] <= '9')
        v = v*10 + (d[(*p)++]-'0');
    return v;
}

static int load_ppm(iv_img_t* im, const uint8_t* d, size_t sz)
{
    if (sz < 8 || d[0] != 'P' || d[1] != '6') return -1;
    size_t p = 2;
    int W=ppm_int(d,sz,&p), H=ppm_int(d,sz,&p), mx=ppm_int(d,sz,&p);
    if (W<=0||H<=0||mx<=0||mx>255||W>4096||H>4096) return -1;
    if (p < sz) p++;
    if (p + (size_t)(W*H*3) > sz) return -1;
    im->px = (uint32_t*)kmalloc((size_t)(W*H)*4);
    if (!im->px) return -1;
    im->w = W; im->h = H;
    const uint8_t* s = d+p;
    for (int i = 0; i < W*H; i++)
        im->px[i] = rgba(s[i*3], s[i*3+1], s[i*3+2], 0xFF);
    return 0;
}

/* =========================================================
 * Load from VFS
 * ========================================================= */
static int iv_load(iv_t* iv, const char* path)
{
    vfs_node_t* node = vfs_open(path, VFS_O_READ);
    if (!node) return -1;
    size_t sz = (size_t)vfs_size(node);
    if (!sz || sz > 8*1024*1024) { vfs_close(node); return -1; }
    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if (!buf) { vfs_close(node); return -1; }
    ssize_t n = vfs_read(node, buf, sz, 0);
    vfs_close(node);
    if (n != (ssize_t)sz) { kfree(buf); return -1; }

    if (iv->img.px) { kfree(iv->img.px); iv->img.px = NULL; }

    int ret = (buf[0]=='B' && buf[1]=='M') ? load_bmp(&iv->img,buf,sz)
            : (buf[0]=='P' && buf[1]=='6') ? load_ppm(&iv->img,buf,sz) : -1;
    kfree(buf);

    if (ret == 0) {
        strncpy(iv->path, path, sizeof(iv->path)-1);
        iv->zoom = 100; iv->pan_x = iv->pan_y = 0;
        iv->loaded = true;
        window_t* win = wm_get_window(iv->wid);
        if (win) {
            const char* fname = path;
            for (const char* q=path; *q; q++) if (*q=='/') fname=q+1;
            char t[80]; strncpy(t, fname, 52);
            strncat(t, " — " OS_NAME " Image Viewer", sizeof(t)-strlen(t)-1);
            strncpy(win->title, t, sizeof(win->title)-1);
        }
    }
    return ret;
}

static void iv_clamp(iv_t* iv)
{
    if (!iv->loaded) return;
    int dw=(iv->img.w*iv->zoom)/100, dh=(iv->img.h*iv->zoom)/100;
    int mx=dw-IV_W, my=dh-IV_VIEW_H;
    if (iv->pan_x<0) iv->pan_x=0; if (iv->pan_y<0) iv->pan_y=0;
    if (mx>0 && iv->pan_x>mx) iv->pan_x=mx; else if (mx<=0) iv->pan_x=0;
    if (my>0 && iv->pan_y>my) iv->pan_y=my; else if (my<=0) iv->pan_y=0;
}

static void iv_blit(canvas_t* dst, int dx, int dy, int dw, int dh,
                    const iv_img_t* src, int sox, int soy, int zoom)
{
    for (int row = 0; row < dh; row++) {
        int ay = dy+row;
        if (ay < 0 || ay >= (int)dst->height) continue;
        int sy = soy + (row*100)/zoom;
        if (sy < 0 || sy >= src->h) continue;
        for (int col = 0; col < dw; col++) {
            int ax = dx+col;
            if (ax < 0 || ax >= (int)dst->width) continue;
            int sx = sox + (col*100)/zoom;
            if (sx < 0 || sx >= src->w) continue;
            dst->pixels[ay*dst->width+ax] = src->px[sy*src->w+sx];
        }
    }
}

static void iv_draw(iv_t* iv, canvas_t* c)
{
    const theme_t* th = theme_current();

    /* Toolbar */
    draw_gradient_v(c, 0, 0, IV_W, IV_TB_H, th->panel_bg, th->panel_border);
    draw_hline(c, 0, IV_TB_H-1, IV_W, th->panel_border);
    const char* blabels[] = { "Zoom -","Zoom +","Fit","1:1" };
    int bx[]  = { 4, 60, 116, 160 };
    int bw[]  = { 52, 52, 40,  40 };
    for (int b = 0; b < 4; b++) {
        draw_rect_rounded(c, bx[b], TB_BTN_Y, bw[b], TB_BTN_H, 3, th->btn_normal);
        draw_string_centered(c, bx[b], TB_BTN_Y, bw[b], TB_BTN_H,
                             blabels[b], th->btn_text, rgba(0,0,0,0));
    }
    if (iv->loaded) {
        char zs[8]; fmt_int(zs,sizeof(zs),iv->zoom);
        strncat(zs,"%",sizeof(zs)-strlen(zs)-1);
        draw_string(c, 208, TB_BTN_Y+(TB_BTN_H-FONT_H)/2, zs,
                    th->panel_text, rgba(0,0,0,0));
    }

    /* Checkerboard */
    for (int ry=0; ry<IV_VIEW_H; ry+=16)
        for (int rx=0; rx<IV_W; rx+=16) {
            bool dk = (((rx>>4)+(ry>>4))&1);
            uint32_t tile = dk ? rgba(0x22,0x22,0x22,0xFF) : rgba(0x2C,0x2C,0x2C,0xFF);
            draw_rect(c, rx, IV_VIEW_Y+ry,
                      (rx+16<IV_W)?16:IV_W-rx,
                      (ry+16<IV_VIEW_H)?16:IV_VIEW_H-ry, tile);
        }

    if (iv->loaded) {
        int dw=(iv->img.w*iv->zoom)/100, dh=(iv->img.h*iv->zoom)/100;
        int ox=(dw<IV_W)?(IV_W-dw)/2:0, oy=(dh<IV_VIEW_H)?(IV_VIEW_H-dh)/2:0;
        int sox=(iv->pan_x*100)/iv->zoom, soy=(iv->pan_y*100)/iv->zoom;
        iv_blit(c, ox, IV_VIEW_Y+oy,
                (dw<IV_W)?dw:IV_W, (dh<IV_VIEW_H)?dh:IV_VIEW_H,
                &iv->img, sox, soy, iv->zoom);
    } else {
        draw_string_centered(c, 0, IV_VIEW_Y, IV_W, IV_VIEW_H,
            "No image — press L to load /sys/wallpapers/default.bmp",
            rgba(0x80,0x80,0x80,0xFF), rgba(0,0,0,0));
    }

    /* Status bar */
    int sby = IV_VIEW_Y+IV_VIEW_H;
    draw_rect(c, 0, sby, IV_W, IV_SB_H, th->panel_bg);
    draw_hline(c, 0, sby, IV_W, th->panel_border);
    char status[128];
    if (iv->loaded) {
        const char* fn=iv->path;
        for (const char* p=iv->path;*p;p++) if (*p=='/') fn=p+1;
        char ws[8],hs[8],zs[8];
        fmt_int(ws,sizeof(ws),iv->img.w);
        fmt_int(hs,sizeof(hs),iv->img.h);
        fmt_int(zs,sizeof(zs),iv->zoom);
        strncpy(status,fn,60);
        strncat(status,"   ",sizeof(status)-strlen(status)-1);
        strncat(status,ws,  sizeof(status)-strlen(status)-1);
        strncat(status,"x", sizeof(status)-strlen(status)-1);
        strncat(status,hs,  sizeof(status)-strlen(status)-1);
        strncat(status,"   ",sizeof(status)-strlen(status)-1);
        strncat(status,zs,  sizeof(status)-strlen(status)-1);
        strncat(status,"%", sizeof(status)-strlen(status)-1);
    } else {
        strncpy(status,"Keys: L=load  +/-=zoom  0=fit  1=1:1  Arrows=pan",sizeof(status)-1);
    }
    draw_string(c, 6, sby+(IV_SB_H-FONT_H)/2, status, th->panel_text, rgba(0,0,0,0));
}

/* =========================================================
 * Event handler
 * ========================================================= */
static void iv_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    iv_t* iv = g_iv;
    if (!iv || iv->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT: {
        canvas_t c = wm_client_canvas(wid);
        iv_draw(iv, &c);
        break;
    }
    case GUI_EVENT_KEY_DOWN:
        switch (evt->key.keycode) {
        case '+': case '=':
            iv->zoom += IV_ZOOM_STEP; if (iv->zoom>IV_ZOOM_MAX) iv->zoom=IV_ZOOM_MAX;
            iv_clamp(iv); wm_invalidate(wid); break;
        case '-':
            iv->zoom -= IV_ZOOM_STEP; if (iv->zoom<IV_ZOOM_MIN) iv->zoom=IV_ZOOM_MIN;
            iv_clamp(iv); wm_invalidate(wid); break;
        case '0':
            if (iv->loaded) {
                int zx=(IV_W*100)/iv->img.w, zy=(IV_VIEW_H*100)/iv->img.h;
                iv->zoom = (zx<zy)?zx:zy;
                if (iv->zoom<IV_ZOOM_MIN) iv->zoom=IV_ZOOM_MIN;
                iv->pan_x=iv->pan_y=0;
            }
            wm_invalidate(wid); break;
        case '1':
            iv->zoom=100; iv->pan_x=iv->pan_y=0; wm_invalidate(wid); break;
        case KEY_UP_ARROW:    iv->pan_y-=32; iv_clamp(iv); wm_invalidate(wid); break;
        case KEY_DOWN_ARROW:  iv->pan_y+=32; iv_clamp(iv); wm_invalidate(wid); break;
        case KEY_LEFT_ARROW:  iv->pan_x-=32; iv_clamp(iv); wm_invalidate(wid); break;
        case KEY_RIGHT_ARROW: iv->pan_x+=32; iv_clamp(iv); wm_invalidate(wid); break;
        case 'l': case 'L':
            iv_load(iv, "/sys/wallpapers/default.bmp"); wm_invalidate(wid); break;
        }
        break;

    case GUI_EVENT_MOUSE_DOWN: {
        int mx = evt->mouse.x, my = evt->mouse.y;
        if (my >= TB_BTN_Y && my < TB_BTN_Y+TB_BTN_H) {
            if      (mx >= 4   && mx < 56)  {
                iv->zoom -= IV_ZOOM_STEP; if (iv->zoom<IV_ZOOM_MIN) iv->zoom=IV_ZOOM_MIN;
                iv_clamp(iv); wm_invalidate(wid);
            } else if (mx >= 60  && mx < 112) {
                iv->zoom += IV_ZOOM_STEP; if (iv->zoom>IV_ZOOM_MAX) iv->zoom=IV_ZOOM_MAX;
                iv_clamp(iv); wm_invalidate(wid);
            } else if (mx >= 116 && mx < 156 && iv->loaded) {
                int zx=(IV_W*100)/iv->img.w, zy=(IV_VIEW_H*100)/iv->img.h;
                iv->zoom=(zx<zy)?zx:zy; if (iv->zoom<IV_ZOOM_MIN) iv->zoom=IV_ZOOM_MIN;
                iv->pan_x=iv->pan_y=0; wm_invalidate(wid);
            } else if (mx >= 160 && mx < 200) {
                iv->zoom=100; iv->pan_x=iv->pan_y=0; wm_invalidate(wid);
            }
        }
        if (my >= IV_VIEW_Y && my < IV_VIEW_Y+IV_VIEW_H) {
            iv->dragging=true; iv->drag_mx=mx; iv->drag_my=my;
            iv->drag_px=iv->pan_x; iv->drag_py=iv->pan_y;
        }
        break;
    }
    case GUI_EVENT_MOUSE_UP:
        iv->dragging = false; break;

    case GUI_EVENT_MOUSE_MOVE:
        if (iv->dragging) {
            iv->pan_x = iv->drag_px - (evt->mouse.x - iv->drag_mx);
            iv->pan_y = iv->drag_py - (evt->mouse.y - iv->drag_my);
            iv_clamp(iv); wm_invalidate(wid);
        }
        break;

    case GUI_EVENT_CLOSE:
        if (iv->img.px) kfree(iv->img.px);
        kfree(iv); g_iv = NULL;
        break;

    default: break;
    }
}

/* =========================================================
 * Public API
 * ========================================================= */
wid_t app_imgviewer_create(void) { return app_imgviewer_open(NULL); }

wid_t app_imgviewer_open(const char* path)
{
    if (g_iv) {
        if (path) iv_load(g_iv, path);
        wm_focus(g_iv->wid); wm_raise(g_iv->wid);
        return g_iv->wid;
    }
    g_iv = (iv_t*)kmalloc(sizeof(iv_t));
    if (!g_iv) return -1;
    memset(g_iv, 0, sizeof(iv_t));
    g_iv->zoom = 100;

    wid_t wid = wm_create_window("Image Viewer — " OS_NAME,
                                  80, 50, IV_W, IV_H, iv_on_event, NULL);
    if (wid < 0) { kfree(g_iv); g_iv = NULL; return -1; }
    g_iv->wid = wid;
    if (path) iv_load(g_iv, path);
    wm_invalidate(wid);
    return wid;
}

void imgviewer_tick(void)
{
    if (!g_iv) return;
    if (!wm_get_window(g_iv->wid)) { kfree(g_iv); g_iv = NULL; }
}
