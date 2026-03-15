/*
 * services/login.c - AetherOS Sci-Fi Login Screen
 *
 * Professional dark-themed login with:
 *   - Vector-drawn 7-segment clock digits with neon glow
 *   - Date and system information panel
 *   - Frosted glass authentication card
 *   - 3D sphere avatar with specular highlights
 *   - Smooth rounded password field and button
 *   - Background grid and radial glow effects
 */
#include <services/login.h>
#include <kernel/version.h>
#include <kernel/power.h>
#include <gui/draw.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <drivers/mouse.h>
#include <drivers/cursor.h>
#include <scheduler.h>
#include <kernel/users.h>
#include <string.h>
#include <memory.h>

/* Arrow key keycodes from gui/event.h (avoid include to prevent type conflicts) */
#define KEY_UP_ARROW   0x100
#define KEY_DOWN_ARROW 0x101

char login_username[64] = "user";

/* ── Extern font data for AA text rendering ────────────────────────────── */
extern const uint8_t font_data[256][16];

/* ── 7-segment clock constants ─────────────────────────────────────────── */
#define DIGIT_W      44    /* width of one digit cell */
#define DIGIT_H      80    /* height of one digit cell */
#define SEG_T         8    /* segment thickness */
#define SEG_GAP       4    /* gap from digit edge to segment tip */
#define SEG_R         4    /* rounded corner radius of segments */
#define DIGIT_SPACE  56    /* horizontal step per digit (DIGIT_W + gap) */
#define COLON_W      24    /* width of colon separator */
#define GLOW_EXPAND   3    /* glow halo size around segments */

/* Segment bit masks (bit 6=a, 5=b, 4=c, 3=d, 2=e, 1=f, 0=g) */
#define S_A 0x40  /* top horizontal */
#define S_B 0x20  /* top-right vertical */
#define S_C 0x10  /* bottom-right vertical */
#define S_D 0x08  /* bottom horizontal */
#define S_E 0x04  /* bottom-left vertical */
#define S_F 0x02  /* top-left vertical */
#define S_G 0x01  /* middle horizontal */

static const uint8_t seg_table[10] = {
    S_A|S_B|S_C|S_D|S_E|S_F,       /* 0 */
    S_B|S_C,                         /* 1 */
    S_A|S_B|S_D|S_E|S_G,           /* 2 */
    S_A|S_B|S_C|S_D|S_G,           /* 3 */
    S_B|S_C|S_F|S_G,               /* 4 */
    S_A|S_C|S_D|S_F|S_G,           /* 5 */
    S_A|S_C|S_D|S_E|S_F|S_G,      /* 6 */
    S_A|S_B|S_C,                    /* 7 */
    S_A|S_B|S_C|S_D|S_E|S_F|S_G,  /* 8 */
    S_A|S_B|S_C|S_D|S_F|S_G,      /* 9 */
};

/* ── Layout constants ──────────────────────────────────────────────────── */
#define CARD_W        400
#define CARD_H        340
#define CARD_RADIUS    16
#define FIELD_W       300
#define FIELD_H        40
#define FIELD_RADIUS    8
#define FIELD_PAD      40
#define BTN_W         300
#define BTN_H          44
#define BTN_RADIUS      8
#define AVATAR_R       48
#define GRID_SPACING   40

/* ── Color palette ─────────────────────────────────────────────────────── */
#define C_BG_TOP        rgb(0x0B, 0x14, 0x26)
#define C_BG_BOT        rgb(0x06, 0x0C, 0x1A)
#define C_GRID          rgba(0x20, 0x40, 0x70, 0x18)
#define C_CLOCK         rgb(0x70, 0xD0, 0xF8)
#define C_CLOCK_GLOW    rgba(0x40, 0x90, 0xD0, 0x28)
#define C_CLOCK_DIM     rgb(0x25, 0x50, 0x70)
#define C_DATE          rgb(0x60, 0xB0, 0xD8)
#define C_INFO          rgb(0x50, 0x80, 0xA8)
#define C_INFO_DIM      rgb(0x38, 0x60, 0x80)
#define C_CARD_BG       rgba(0x18, 0x28, 0x40, 0xCC)
#define C_CARD_BORDER   rgba(0x40, 0x70, 0xA0, 0x60)
#define C_CARD_SHINE    rgba(0x60, 0x90, 0xC0, 0x20)
#define C_AVATAR_RING   rgba(0x40, 0x80, 0xC0, 0x80)
#define C_NAME          rgb(0xD0, 0xE8, 0xF8)
#define C_SUBTEXT       rgb(0x60, 0x88, 0xA8)
#define C_FIELD_BG      rgba(0x0C, 0x18, 0x2C, 0xE0)
#define C_FIELD_BORDER  rgba(0x30, 0x58, 0x80, 0x80)
#define C_FIELD_FOCUS   rgba(0x40, 0xA0, 0xE0, 0xFF)
#define C_TEXT          rgb(0xC8, 0xE0, 0xF0)
#define C_TEXT_DIM      rgb(0x50, 0x70, 0x90)
#define C_CURSOR_COL    rgb(0x60, 0xD0, 0xFF)
#define C_BTN_TOP       rgb(0x28, 0x78, 0xC0)
#define C_BTN_BOT       rgb(0x1C, 0x5C, 0x98)
#define C_BTN_TEXT      rgb(0xF0, 0xF8, 0xFF)
#define C_ERROR_COL     rgb(0xE0, 0x50, 0x50)
#define C_FOOTER        rgb(0x50, 0x78, 0x98)
#define C_FOOTER_HI     rgb(0x70, 0xA0, 0xC8)

/* ── Field state ──────────────────────────────────────────────────────── */
#define FIELD_PASS  0
#define FIELD_COUNT 1

typedef struct {
    char buf[64];
    int  len;
    bool active;
} field_t;

static field_t  g_fields[FIELD_COUNT];
static bool     g_error      = false;
static uint32_t g_blink_tick = 0;
static bool     g_cursor_vis = true;

/* Cached hit-test positions */
static int g_field_x, g_field_y;
static int g_btn_x, g_btn_y;

/* Password visibility toggle */
static bool g_show_password = false;

/* Footer button hit-test rectangles */
typedef struct { int x, y, w, h; } hitbox_t;
static hitbox_t g_hit_shutdown;
static hitbox_t g_hit_restart;
static hitbox_t g_hit_sleep;
static hitbox_t g_hit_eye;    /* password show/hide toggle */

/* ── Timezone setup wizard state ─────────────────────────────────────── */
#define TZ_WIZARD_NONE    0
#define TZ_WIZARD_CLOCK   1   /* asking: is RTC UTC or local? */
#define TZ_WIZARD_REGION  2   /* selecting region/continent */
#define TZ_WIZARD_CITY    3   /* selecting city within region */
#define TZ_WIZARD_DONE    4

static int  g_tz_wizard = TZ_WIZARD_NONE;
static int  g_tz_clock_sel   = 0;   /* 0=local, 1=UTC */
static int  g_tz_region_sel  = 0;   /* cursor in region list */
static int  g_tz_city_sel    = 0;   /* cursor in city list */

/* Timezone database: region -> city list with UTC offset in minutes */
typedef struct { const char* name; int16_t offset; } tz_city_t;
typedef struct {
    const char* name;
    const tz_city_t* cities;
    int count;
} tz_region_t;

static const tz_city_t tz_americas[] = {
    { "New York (EST)",     -300 },
    { "Chicago (CST)",      -360 },
    { "Denver (MST)",       -420 },
    { "Los Angeles (PST)",  -480 },
    { "Anchorage (AKST)",   -540 },
    { "Honolulu (HST)",     -600 },
    { "Santo Domingo (AST)",-240 },
    { "San Juan, PR (AST)", -240 },
    { "Havana (CST)",       -300 },
    { "Kingston (EST)",     -300 },
    { "Port-au-Prince (EST)",-300 },
    { "Panama City (EST)",  -300 },
    { "San Jose, CR (CST)", -360 },
    { "Guatemala City",     -360 },
    { "Tegucigalpa (CST)",  -360 },
    { "Managua (CST)",      -360 },
    { "San Salvador (CST)", -360 },
    { "Mexico City (CST)",  -360 },
    { "Bogota (COT)",       -300 },
    { "Lima (PET)",         -300 },
    { "Caracas (VET)",      -240 },
    { "Santiago (CLT)",     -240 },
    { "Sao Paulo (BRT)",    -180 },
    { "Buenos Aires (ART)", -180 },
    { "Montevideo (UYT)",   -180 },
    { "Toronto (EST)",      -300 },
    { "Vancouver (PST)",    -480 },
};

static const tz_city_t tz_europe[] = {
    { "London (GMT)",          0 },
    { "Paris (CET)",         +60 },
    { "Berlin (CET)",        +60 },
    { "Madrid (CET)",        +60 },
    { "Rome (CET)",          +60 },
    { "Lisbon (WET)",          0 },
    { "Moscow (MSK)",       +180 },
    { "Istanbul (TRT)",     +180 },
    { "Athens (EET)",       +120 },
    { "Helsinki (EET)",     +120 },
    { "Bucharest (EET)",    +120 },
    { "Warsaw (CET)",        +60 },
    { "Amsterdam (CET)",     +60 },
    { "Zurich (CET)",        +60 },
    { "Stockholm (CET)",     +60 },
    { "Dublin (GMT)",          0 },
    { "Kyiv (EET)",         +120 },
};

static const tz_city_t tz_asia[] = {
    { "Tokyo (JST)",        +540 },
    { "Shanghai (CST)",     +480 },
    { "Hong Kong (HKT)",    +480 },
    { "Singapore (SGT)",    +480 },
    { "Mumbai (IST)",       +330 },
    { "Dubai (GST)",        +240 },
    { "Seoul (KST)",        +540 },
    { "Bangkok (ICT)",      +420 },
    { "Taipei (CST)",       +480 },
    { "Jakarta (WIB)",      +420 },
};

static const tz_city_t tz_africa[] = {
    { "Cairo (EET)",        +120 },
    { "Lagos (WAT)",         +60 },
    { "Nairobi (EAT)",      +180 },
    { "Johannesburg (SAST)",+120 },
    { "Casablanca (WET)",      0 },
    { "Accra (GMT)",           0 },
    { "Addis Ababa (EAT)",  +180 },
    { "Dar es Salaam (EAT)",+180 },
    { "Kinshasa (WAT)",      +60 },
    { "Tunis (CET)",         +60 },
    { "Algiers (CET)",       +60 },
};

static const tz_city_t tz_oceania[] = {
    { "Sydney (AEST)",      +600 },
    { "Melbourne (AEST)",   +600 },
    { "Brisbane (AEST)",    +600 },
    { "Auckland (NZST)",    +720 },
    { "Perth (AWST)",       +480 },
    { "Fiji (FJT)",         +720 },
    { "Adelaide (ACST)",    +570 },
    { "Guam (ChST)",        +600 },
};

static const tz_city_t tz_utc[] = {
    { "UTC+0",     0 },
    { "UTC+1",   +60 },
    { "UTC+2",  +120 },
    { "UTC+3",  +180 },
    { "UTC+4",  +240 },
    { "UTC+5",  +300 },
    { "UTC+6",  +360 },
    { "UTC+7",  +420 },
    { "UTC+8",  +480 },
    { "UTC+9",  +540 },
    { "UTC+10", +600 },
    { "UTC+11", +660 },
    { "UTC+12", +720 },
    { "UTC-1",   -60 },
    { "UTC-2",  -120 },
    { "UTC-3",  -180 },
    { "UTC-4",  -240 },
    { "UTC-5",  -300 },
    { "UTC-6",  -360 },
    { "UTC-7",  -420 },
    { "UTC-8",  -480 },
    { "UTC-9",  -540 },
    { "UTC-10", -600 },
    { "UTC-11", -660 },
    { "UTC-12", -720 },
};

#define TZ_REGION_COUNT 6
static const tz_region_t tz_regions[TZ_REGION_COUNT] = {
    { "Americas & Caribbean", tz_americas, 27 },
    { "Europe",   tz_europe,   17 },
    { "Asia",     tz_asia,     10 },
    { "Africa",   tz_africa,   11 },
    { "Oceania",  tz_oceania,   8 },
    { "Manual UTC Offset", tz_utc, 25 },
};

/* ── Helper: integer square root ──────────────────────────────────────── */
static int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (n / y + y) / 2; }
    return x;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 7-SEGMENT VECTOR CLOCK
 * ═══════════════════════════════════════════════════════════════════════ */

/* Draw a single 7-segment digit at (x,y) */
static void draw_digit(canvas_t* scr, int x, int y, int digit, uint32_t color)
{
    if (digit < 0 || digit > 9) return;
    uint8_t segs = seg_table[digit];
    int half = DIGIT_H / 2;

    /* Extract glow color from main color */
    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >>  8) & 0xFF;
    uint8_t cb =  color        & 0xFF;
    uint32_t glow = rgba(cr, cg, cb, 0x28);
    int ge = GLOW_EXPAND;

    /* Inactive segment ghost (very faint, shows the "LCD glass") */
    uint32_t ghost = rgba(cr, cg, cb, 0x0C);
    /* Draw all 7 ghost segments first */
    draw_rect_alpha(scr, x + SEG_GAP, y, DIGIT_W - 2*SEG_GAP, SEG_T, ghost);
    draw_rect_alpha(scr, x + SEG_GAP, y + DIGIT_H - SEG_T, DIGIT_W - 2*SEG_GAP, SEG_T, ghost);
    draw_rect_alpha(scr, x + SEG_GAP, y + half - SEG_T/2, DIGIT_W - 2*SEG_GAP, SEG_T, ghost);
    draw_rect_alpha(scr, x, y + SEG_GAP, SEG_T, half - 2*SEG_GAP, ghost);
    draw_rect_alpha(scr, x + DIGIT_W - SEG_T, y + SEG_GAP, SEG_T, half - 2*SEG_GAP, ghost);
    draw_rect_alpha(scr, x, y + half + SEG_GAP, SEG_T, half - 2*SEG_GAP, ghost);
    draw_rect_alpha(scr, x + DIGIT_W - SEG_T, y + half + SEG_GAP, SEG_T, half - 2*SEG_GAP, ghost);

    /* --- Glow pass (slightly larger, semi-transparent) --- */
    if (segs & S_A)
        draw_rect_alpha(scr, x + SEG_GAP - ge, y - ge,
                        DIGIT_W - 2*SEG_GAP + 2*ge, SEG_T + 2*ge, glow);
    if (segs & S_D)
        draw_rect_alpha(scr, x + SEG_GAP - ge, y + DIGIT_H - SEG_T - ge,
                        DIGIT_W - 2*SEG_GAP + 2*ge, SEG_T + 2*ge, glow);
    if (segs & S_G)
        draw_rect_alpha(scr, x + SEG_GAP - ge, y + half - SEG_T/2 - ge,
                        DIGIT_W - 2*SEG_GAP + 2*ge, SEG_T + 2*ge, glow);
    if (segs & S_F)
        draw_rect_alpha(scr, x - ge, y + SEG_GAP - ge,
                        SEG_T + 2*ge, half - 2*SEG_GAP + 2*ge, glow);
    if (segs & S_B)
        draw_rect_alpha(scr, x + DIGIT_W - SEG_T - ge, y + SEG_GAP - ge,
                        SEG_T + 2*ge, half - 2*SEG_GAP + 2*ge, glow);
    if (segs & S_E)
        draw_rect_alpha(scr, x - ge, y + half + SEG_GAP - ge,
                        SEG_T + 2*ge, half - 2*SEG_GAP + 2*ge, glow);
    if (segs & S_C)
        draw_rect_alpha(scr, x + DIGIT_W - SEG_T - ge, y + half + SEG_GAP - ge,
                        SEG_T + 2*ge, half - 2*SEG_GAP + 2*ge, glow);

    /* --- Sharp segment pass (rounded rectangles) --- */
    if (segs & S_A) draw_rect_rounded(scr, x + SEG_GAP, y,
                        DIGIT_W - 2*SEG_GAP, SEG_T, SEG_R, color);
    if (segs & S_D) draw_rect_rounded(scr, x + SEG_GAP, y + DIGIT_H - SEG_T,
                        DIGIT_W - 2*SEG_GAP, SEG_T, SEG_R, color);
    if (segs & S_G) draw_rect_rounded(scr, x + SEG_GAP, y + half - SEG_T/2,
                        DIGIT_W - 2*SEG_GAP, SEG_T, SEG_R, color);
    if (segs & S_F) draw_rect_rounded(scr, x, y + SEG_GAP,
                        SEG_T, half - 2*SEG_GAP, SEG_R, color);
    if (segs & S_B) draw_rect_rounded(scr, x + DIGIT_W - SEG_T, y + SEG_GAP,
                        SEG_T, half - 2*SEG_GAP, SEG_R, color);
    if (segs & S_E) draw_rect_rounded(scr, x, y + half + SEG_GAP,
                        SEG_T, half - 2*SEG_GAP, SEG_R, color);
    if (segs & S_C) draw_rect_rounded(scr, x + DIGIT_W - SEG_T, y + half + SEG_GAP,
                        SEG_T, half - 2*SEG_GAP, SEG_R, color);
}

/* Draw colon separator (two filled circles with glow) */
static void draw_colon(canvas_t* scr, int x, int y, uint32_t color)
{
    int cx = x + COLON_W / 2;
    int dot_r = SEG_T / 2 + 1;
    int y1 = y + DIGIT_H * 30 / 100;
    int y2 = y + DIGIT_H * 70 / 100;

    /* Glow */
    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >>  8) & 0xFF;
    uint8_t cb =  color        & 0xFF;
    draw_circle_filled(scr, cx, y1, dot_r + 2, rgba(cr, cg, cb, 0x20));
    draw_circle_filled(scr, cx, y2, dot_r + 2, rgba(cr, cg, cb, 0x20));

    /* Solid dots */
    draw_circle_filled(scr, cx, y1, dot_r, color);
    draw_circle_filled(scr, cx, y2, dot_r, color);
}

/* Draw full HH:MM clock */
static void draw_clock(canvas_t* scr, int x, int y, int hours, int minutes,
                        bool blink_colon)
{
    uint32_t col = C_CLOCK;
    uint32_t col_dim = C_CLOCK_DIM;

    /* Hours */
    draw_digit(scr, x, y, hours / 10, col);
    draw_digit(scr, x + DIGIT_SPACE, y, hours % 10, col);

    /* Colon (blinking) */
    int colon_x = x + 2 * DIGIT_SPACE;
    if (blink_colon)
        draw_colon(scr, colon_x, y, col);
    else
        draw_colon(scr, colon_x, y, col_dim);

    /* Minutes */
    int min_x = colon_x + COLON_W;
    draw_digit(scr, min_x, y, minutes / 10, col);
    draw_digit(scr, min_x + DIGIT_SPACE, y, minutes % 10, col);
}

/* ═══════════════════════════════════════════════════════════════════════
 * ANTI-ALIASED SCALED TEXT
 * ═══════════════════════════════════════════════════════════════════════ */

/* Draw a character scaled up with edge anti-aliasing fringe */
static void draw_char_scaled_aa(canvas_t* c, int x, int y, char ch,
                                 int scale, uint32_t color)
{
    const uint8_t* glyph = font_data[(uint8_t)ch];
    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >>  8) & 0xFF;
    uint8_t cb =  color        & 0xFF;

    /* Pass 1: AA fringe (1px border around each scaled block, semi-transparent) */
    uint32_t fringe = rgba(cr, cg, cb, 0x50);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        uint8_t bits_above = (row > 0) ? glyph[row - 1] : 0;
        uint8_t bits_below = (row < 15) ? glyph[row + 1] : 0;

        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px = x + col * scale;
            int py = y + row * scale;

            /* Check neighbors to only fringe exposed edges */
            bool has_left  = (col > 0) && (bits & (0x80 >> (col - 1)));
            bool has_right = (col < 7) && (bits & (0x80 >> (col + 1)));
            bool has_above = (bits_above & (0x80 >> col)) != 0;
            bool has_below = (bits_below & (0x80 >> col)) != 0;

            /* Top fringe */
            if (!has_above)
                draw_rect_alpha(c, px, py - 1, scale, 1, fringe);
            /* Bottom fringe */
            if (!has_below)
                draw_rect_alpha(c, px, py + scale, scale, 1, fringe);
            /* Left fringe */
            if (!has_left)
                draw_rect_alpha(c, px - 1, py, 1, scale, fringe);
            /* Right fringe */
            if (!has_right)
                draw_rect_alpha(c, px + scale, py, 1, scale, fringe);
        }
    }

    /* Pass 2: Solid character blocks */
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        if (!bits) continue;
        int py = y + row * scale;
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col * scale;
                draw_rect(c, px, py, scale, scale, color);
            }
        }
    }
}

/* Draw scaled string with AA */
static void draw_string_scaled_aa(canvas_t* c, int x, int y, const char* str,
                                   int scale, uint32_t color)
{
    while (*str) {
        draw_char_scaled_aa(c, x, y, *str++, scale, color);
        x += 8 * scale;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * BACKGROUND AND DECORATIONS
 * ═══════════════════════════════════════════════════════════════════════ */

static void draw_grid(canvas_t* scr, int W, int H)
{
    for (int gy = GRID_SPACING; gy < H; gy += GRID_SPACING)
        draw_rect_alpha(scr, 0, gy, W, 1, C_GRID);
    for (int gx = GRID_SPACING; gx < W; gx += GRID_SPACING)
        draw_rect_alpha(scr, gx, 0, 1, H, C_GRID);
}

static void draw_glow(canvas_t* scr, int cx, int cy, int radius,
                       uint8_t gr, uint8_t gg, uint8_t gb, int max_alpha)
{
    int x0 = cx - radius; if (x0 < 0) x0 = 0;
    int y0 = cy - radius; if (y0 < 0) y0 = 0;
    int x1 = cx + radius; if (x1 > scr->width)  x1 = scr->width;
    int y1 = cy + radius; if (y1 > scr->height) y1 = scr->height;

    for (int y = y0; y < y1; y++) {
        int dy = y - cy;
        for (int x = x0; x < x1; x++) {
            int dx = x - cx;
            int dist = isqrt(dx * dx + dy * dy);
            if (dist < radius) {
                uint8_t alpha = (uint8_t)(max_alpha * (radius - dist) / radius);
                uint32_t src = ((uint32_t)alpha << 24) |
                               ((uint32_t)gr << 16) |
                               ((uint32_t)gg << 8) | gb;
                uint32_t* px = &scr->pixels[y * scr->stride + x];
                *px = fb_blend(*px, src);
            }
        }
    }
}

static void draw_sparkle(canvas_t* scr, int cx, int cy, int size)
{
    for (int i = -size; i <= size; i++) {
        int dist = i < 0 ? -i : i;
        uint8_t alpha = (uint8_t)((size - dist) * 100 / size);
        uint32_t col = rgba(0xB0, 0xD0, 0xE8, alpha);
        int py = cy + i;
        if ((unsigned)cx < (unsigned)scr->width && (unsigned)py < (unsigned)scr->height) {
            uint32_t* px = &scr->pixels[py * scr->stride + cx];
            *px = fb_blend(*px, col);
        }
        int px_x = cx + i;
        if ((unsigned)px_x < (unsigned)scr->width && (unsigned)cy < (unsigned)scr->height) {
            uint32_t* px = &scr->pixels[cy * scr->stride + px_x];
            *px = fb_blend(*px, col);
        }
    }
    int ds = size * 55 / 100;
    for (int i = -ds; i <= ds; i++) {
        int dist = i < 0 ? -i : i;
        uint8_t alpha = (uint8_t)((ds - dist) * 50 / ds);
        uint32_t col = rgba(0xB0, 0xD0, 0xE8, alpha);
        int px1 = cx + i, py1 = cy + i;
        if ((unsigned)px1 < (unsigned)scr->width && (unsigned)py1 < (unsigned)scr->height)
            scr->pixels[py1 * scr->stride + px1] =
                fb_blend(scr->pixels[py1 * scr->stride + px1], col);
        int px2 = cx + i, py2 = cy - i;
        if ((unsigned)px2 < (unsigned)scr->width && (unsigned)py2 < (unsigned)scr->height)
            scr->pixels[py2 * scr->stride + px2] =
                fb_blend(scr->pixels[py2 * scr->stride + px2], col);
    }
    if ((unsigned)cx < (unsigned)scr->width && (unsigned)cy < (unsigned)scr->height)
        scr->pixels[cy * scr->stride + cx] = rgb(0xE0, 0xF0, 0xFF);
}

/* ═══════════════════════════════════════════════════════════════════════
 * FROSTED GLASS CARD
 * ═══════════════════════════════════════════════════════════════════════ */

static inline bool in_rounded_rect(int col, int row, int w, int h, int r)
{
    if (col < r && row < r) {
        int dx = r - col - 1, dy = r - row - 1;
        return dx * dx + dy * dy <= r * r;
    }
    if (col >= w - r && row < r) {
        int dx = col - (w - r), dy = r - row - 1;
        return dx * dx + dy * dy <= r * r;
    }
    if (col < r && row >= h - r) {
        int dx = r - col - 1, dy = row - (h - r);
        return dx * dx + dy * dy <= r * r;
    }
    if (col >= w - r && row >= h - r) {
        int dx = col - (w - r), dy = row - (h - r);
        return dx * dx + dy * dy <= r * r;
    }
    return true;
}

static void draw_glass_card(canvas_t* scr, int x, int y, int w, int h, int r)
{
    /* Multi-layer shadow */
    for (int s = 8; s >= 1; s--) {
        uint8_t sa = (uint8_t)(16 / s);
        uint32_t shadow = rgba(0, 0, 0, sa);
        int sx = x + 4, sy = y + 6;
        int sr = r + s;
        draw_rect_alpha(scr, sx + sr, sy, w - 2*sr, h + 2*s, shadow);
        draw_rect_alpha(scr, sx, sy + sr, sr, h + 2*s - 2*sr, shadow);
        draw_rect_alpha(scr, sx + w - sr, sy + sr, sr, h + 2*s - 2*sr, shadow);
    }

    /* Card body (alpha blended) */
    for (int row = 0; row < h; row++) {
        int ry = y + row;
        if (ry < 0 || ry >= scr->height) continue;
        for (int col = 0; col < w; col++) {
            int rx = x + col;
            if (rx < 0 || rx >= scr->width) continue;
            if (in_rounded_rect(col, row, w, h, r)) {
                uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                *px = fb_blend(*px, C_CARD_BG);
            }
        }
    }

    /* Border */
    for (int col = r; col < w - r; col++) {
        int rx = x + col;
        if (rx < 0 || rx >= scr->width) continue;
        if (y >= 0 && y < scr->height) {
            uint32_t* px = &scr->pixels[y * scr->stride + rx];
            *px = fb_blend(*px, C_CARD_BORDER);
        }
        int by = y + h - 1;
        if (by >= 0 && by < scr->height) {
            uint32_t* px = &scr->pixels[by * scr->stride + rx];
            *px = fb_blend(*px, C_CARD_BORDER);
        }
    }
    for (int row = r; row < h - r; row++) {
        int ry = y + row;
        if (ry < 0 || ry >= scr->height) continue;
        if (x >= 0 && x < scr->width) {
            uint32_t* px = &scr->pixels[ry * scr->stride + x];
            *px = fb_blend(*px, C_CARD_BORDER);
        }
        int bx = x + w - 1;
        if (bx >= 0 && bx < scr->width) {
            uint32_t* px = &scr->pixels[ry * scr->stride + bx];
            *px = fb_blend(*px, C_CARD_BORDER);
        }
    }
    /* Corner arc border */
    for (int row = 0; row < r; row++) {
        for (int col = 0; col < r; col++) {
            int dx = r - col - 1, dy = r - row - 1;
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - 1) * (r - 1)) {
                int corners[4][2] = {
                    { x + col, y + row }, { x + w - 1 - col, y + row },
                    { x + col, y + h - 1 - row }, { x + w - 1 - col, y + h - 1 - row }
                };
                for (int c = 0; c < 4; c++) {
                    int px = corners[c][0], py = corners[c][1];
                    if ((unsigned)px < (unsigned)scr->width &&
                        (unsigned)py < (unsigned)scr->height) {
                        uint32_t* p = &scr->pixels[py * scr->stride + px];
                        *p = fb_blend(*p, C_CARD_BORDER);
                    }
                }
            }
        }
    }

    /* Top edge glassmorphism shine */
    for (int col = r; col < w - r; col++) {
        int rx = x + col, ry = y + 1;
        if ((unsigned)rx < (unsigned)scr->width && (unsigned)ry < (unsigned)scr->height) {
            uint32_t* px = &scr->pixels[ry * scr->stride + rx];
            *px = fb_blend(*px, C_CARD_SHINE);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 3D SPHERE AVATAR
 * ═══════════════════════════════════════════════════════════════════════ */

static void draw_sphere_avatar(canvas_t* scr, int cx, int cy, int rad)
{
    /* Outer glow ring */
    for (int dy = -(rad + 4); dy <= (rad + 4); dy++) {
        for (int dx = -(rad + 4); dx <= (rad + 4); dx++) {
            int d2 = dx * dx + dy * dy;
            int outer = (rad + 4) * (rad + 4);
            int inner = (rad + 1) * (rad + 1);
            if (d2 <= outer && d2 >= inner) {
                int px = cx + dx, py = cy + dy;
                if ((unsigned)px < (unsigned)scr->width &&
                    (unsigned)py < (unsigned)scr->height) {
                    uint32_t* p = &scr->pixels[py * scr->stride + px];
                    *p = fb_blend(*p, C_AVATAR_RING);
                }
            }
        }
    }

    /* Main sphere with per-pixel shading */
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > rad * rad) continue;
            int px = cx + dx, py = cy + dy;
            if ((unsigned)px >= (unsigned)scr->width ||
                (unsigned)py >= (unsigned)scr->height)
                continue;

            int dist = isqrt(d2) * 255 / rad;

            /* Light from upper-left */
            int lx = dx + rad * 35 / 100;
            int ly = dy + rad * 45 / 100;
            int ldist = isqrt(lx * lx + ly * ly) * 255 / rad;
            if (ldist > 255) ldist = 255;

            /* Base dark teal */
            int br = 0x10 + (0x30 - 0x10) * (255 - dist) / 255;
            int bg = 0x20 + (0x58 - 0x20) * (255 - dist) / 255;
            int bb = 0x38 + (0x80 - 0x38) * (255 - dist) / 255;

            /* Specular from light */
            if (ldist < 120) {
                int spec = (120 - ldist) * 200 / 120;
                br += spec * (0xC0 - br) / 255;
                bg += spec * (0xE8 - bg) / 255;
                bb += spec * (0xF8 - bb) / 255;
            }

            /* Edge darkening */
            if (dist > 200) {
                int fade = (dist - 200) * 3;
                if (fade > 255) fade = 255;
                br = br * (255 - fade) / 255;
                bg = bg * (255 - fade) / 255;
                bb = bb * (255 - fade) / 255;
            }

            if (br > 255) br = 255;
            if (bg > 255) bg = 255;
            if (bb > 255) bb = 255;

            scr->pixels[py * scr->stride + px] =
                rgb((uint8_t)br, (uint8_t)bg, (uint8_t)bb);
        }
    }

    /* Bright specular spot */
    int hx = cx - rad * 30 / 100;
    int hy = cy - rad * 35 / 100;
    int hr = rad * 18 / 100;
    if (hr < 2) hr = 2;
    for (int dy = -hr; dy <= hr; dy++) {
        for (int dx = -hr; dx <= hr; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > hr * hr) continue;
            int px = hx + dx, py = hy + dy;
            if ((unsigned)px < (unsigned)scr->width &&
                (unsigned)py < (unsigned)scr->height) {
                int ldist = isqrt(d2) * 255 / hr;
                uint8_t alpha = (uint8_t)((255 - ldist) * 160 / 255);
                uint32_t* p = &scr->pixels[py * scr->stride + px];
                *p = fb_blend(*p, rgba(0xFF, 0xFF, 0xFF, alpha));
            }
        }
    }
}

/* ── Safe pixel plot helpers (used by icons + password field) ────────── */
static inline void px_blend(canvas_t* s, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)s->width && (unsigned)y < (unsigned)s->height)
        s->pixels[y * s->stride + x] = fb_blend(s->pixels[y * s->stride + x], c);
}
static inline void px_set(canvas_t* s, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)s->width && (unsigned)y < (unsigned)s->height)
        s->pixels[y * s->stride + x] = c;
}

/* ═══════════════════════════════════════════════════════════════════════
 * PASSWORD FIELD
 *
 * Reference layout:  [ ⇧ | ● ● ● ● ● ● ● ●  _ ]
 *   - Caps Lock up-arrow icon on the left
 *   - Vertical separator
 *   - Password bullet dots
 *   - Blinking cursor
 * ═══════════════════════════════════════════════════════════════════════ */

/* Draw the Caps Lock up-arrow icon (outlined hollow arrow) */
static void draw_capslock_icon(canvas_t* scr, int cx, int cy, uint32_t color)
{
    /* Arrow dimensions */
    int arrow_w = 14;     /* total width of arrowhead base */
    int arrow_tip_h = 10; /* height from tip to shoulder */
    int shaft_w = 6;      /* shaft width */
    int shaft_h = 5;      /* shaft height below shoulder */
    int base_h = 3;       /* base bar height */
    int gap = 2;          /* gap between shaft and base */

    int top_y = cy - (arrow_tip_h + shaft_h + gap + base_h) / 2;

    /* --- Arrowhead (outlined triangle) --- */
    /* Draw filled then cut interior */
    for (int row = 0; row < arrow_tip_h; row++) {
        /* Width at this row: starts at 1 at top, widens to arrow_w */
        int half = (arrow_w * row) / (2 * (arrow_tip_h - 1));
        if (row == 0) half = 0;
        int lx = cx - half;
        int rx = cx + half;
        int py = top_y + row;

        /* Draw outline only: left edge, right edge, and top row */
        if (row < 2 || row == arrow_tip_h - 1) {
            /* Full row for top and bottom of arrowhead */
            for (int x = lx; x <= rx; x++)
                px_blend(scr, x, py, color);
        } else {
            /* Left and right edges (2px thick) */
            px_blend(scr, lx, py, color);
            px_blend(scr, lx + 1, py, color);
            px_blend(scr, rx, py, color);
            px_blend(scr, rx - 1, py, color);
        }
    }

    /* --- Shaft (hollow rectangle) --- */
    int shaft_y = top_y + arrow_tip_h;
    int shaft_x = cx - shaft_w / 2;
    draw_rect_rounded_outline(scr, shaft_x, shaft_y, shaft_w, shaft_h, 1, 1, color);

    /* --- Base bar (solid) --- */
    int base_y = shaft_y + shaft_h + gap;
    int base_x = cx - shaft_w / 2;
    draw_rect_rounded(scr, base_x, base_y, shaft_w, base_h, 1, color);
}

static void draw_password_field(canvas_t* scr, int x, int y, int w, int h,
                                 field_t* f, bool focused)
{
    /* Field background (alpha blended, rounded) */
    for (int row = 0; row < h; row++) {
        int ry = y + row;
        if (ry < 0 || ry >= scr->height) continue;
        for (int col = 0; col < w; col++) {
            int rx = x + col;
            if (rx < 0 || rx >= scr->width) continue;
            if (in_rounded_rect(col, row, w, h, FIELD_RADIUS)) {
                uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                *px = fb_blend(*px, C_FIELD_BG);
            }
        }
    }

    /* Border */
    uint32_t brd = focused ? C_FIELD_FOCUS : C_FIELD_BORDER;
    draw_rect_rounded_outline(scr, x, y, w, h, FIELD_RADIUS, 1, brd);

    /* Focus glow at bottom */
    if (focused) {
        draw_rect_alpha(scr, x + FIELD_RADIUS, y + h - 2,
                        w - 2 * FIELD_RADIUS, 2,
                        rgba(0x40, 0xA0, 0xE0, 0x60));
    }

    /* ── Caps Lock up-arrow icon (left zone) ─────────────────────── */
    int icon_cx = x + 20;
    int icon_cy = y + h / 2;
    uint32_t icon_col = focused ? rgba(0x50, 0x90, 0xC0, 0xC0)
                                : rgba(0x40, 0x60, 0x80, 0x80);
    draw_capslock_icon(scr, icon_cx, icon_cy, icon_col);

    /* ── Vertical separator ──────────────────────────────────────── */
    int sep_x = x + 38;
    draw_rect_alpha(scr, sep_x, y + 6, 1, h - 12,
                    rgba(0x40, 0x70, 0xA0, 0x40));

    /* ── Eye toggle icon (right side of field, centered) ────── */
    {
        int eye_zone = 36;  /* clickable zone width */
        int eye_cx = x + w - eye_zone / 2;
        int eye_cy = y + h / 2;
        uint32_t c_on  = C_FIELD_FOCUS;              /* active blue */
        uint32_t c_off = rgba(0x60, 0x80, 0xA0, 0xB0); /* dimmer */
        uint32_t eye_col = g_show_password ? c_on : c_off;

        /* Draw almond-shaped eye using parametric curves.
         * Upper lid:  y = -A * (1 - (x/R)^2)
         * Lower lid:  y = +A * (1 - (x/R)^2)
         * with R=10 (half-width) and A=5 (half-height) */
        int R = 10, A = 5;
        for (int dx = -R; dx <= R; dx++) {
            /* Quadratic lid curves */
            int frac = (R * R - dx * dx);  /* 0..R^2 */
            int dy_lid = A * frac / (R * R);
            if (dy_lid < 1 && (dx > -R && dx < R)) dy_lid = 1;

            /* Top lid pixel */
            int px_x = eye_cx + dx;
            int py_top = eye_cy - dy_lid;
            int py_bot = eye_cy + dy_lid;
            if ((unsigned)px_x < (unsigned)scr->width) {
                if ((unsigned)py_top < (unsigned)scr->height)
                    scr->pixels[py_top * scr->stride + px_x] = eye_col;
                if ((unsigned)py_bot < (unsigned)scr->height)
                    scr->pixels[py_bot * scr->stride + px_x] = eye_col;
            }
        }

        /* Iris: filled circle radius 3 */
        draw_circle_filled(scr, eye_cx, eye_cy, 3, eye_col);
        /* Pupil: dark dot */
        draw_circle_filled(scr, eye_cx, eye_cy, 1,
                           rgba(0x10, 0x18, 0x28, 0xFF));
        /* Iris ring */
        draw_circle(scr, eye_cx, eye_cy, 3, eye_col);

        /* Diagonal slash when hidden */
        if (!g_show_password) {
            for (int i = -8; i <= 8; i++) {
                int sx = eye_cx + i;
                int sy = eye_cy - (i * 6) / 8;
                /* Draw 2px wide for visibility */
                for (int t = 0; t <= 1; t++) {
                    int px_sx = sx + t;
                    if ((unsigned)px_sx < (unsigned)scr->width &&
                        (unsigned)sy < (unsigned)scr->height)
                        scr->pixels[sy * scr->stride + px_sx] = eye_col;
                }
            }
        }

        /* Store hitbox */
        g_hit_eye = (hitbox_t){ eye_cx - eye_zone / 2, y, eye_zone, h };
    }

    /* ── Vertical separator before eye icon ──────────────────── */
    draw_rect_alpha(scr, x + w - 36, y + 6, 1, h - 12,
                    rgba(0x40, 0x70, 0xA0, 0x30));

    /* ── Content area (right of separator, left of eye zone) ── */
    int tx = sep_x + 10;
    int ty = y + (h - FONT_H) / 2;
    int max_content_w = w - 36 - (sep_x - x) - 10; /* text area width */
    int max_chars = max_content_w / 8;                /* ~8px per char */
    if (max_chars > 24) max_chars = 24;

    if (f->len == 0 && !focused) {
        draw_string(scr, tx, ty, "Enter password", C_TEXT_DIM, rgba(0,0,0,0));
    } else if (f->len > 0) {
        if (g_show_password) {
            /* Show actual password characters */
            char vis[65];
            int show = f->len < max_chars ? f->len : max_chars;
            for (int i = 0; i < show; i++)
                vis[i] = f->buf[i];
            vis[show] = '\0';
            draw_string(scr, tx, ty, vis, C_TEXT, rgba(0,0,0,0));
        } else {
            /* Bullet dots – evenly spaced filled circles */
            for (int i = 0; i < f->len && i < max_chars; i++) {
                int dot_cx = tx + i * 12 + 5;
                int dot_cy = y + h / 2;
                draw_circle_filled(scr, dot_cx, dot_cy, 3, C_TEXT);
            }
        }
    }

    /* Blinking cursor */
    if (focused && g_cursor_vis) {
        int cw;
        if (g_show_password && f->len > 0) {
            /* Measure visible text width */
            char vis[65];
            int show = f->len < max_chars ? f->len : max_chars;
            for (int i = 0; i < show; i++) vis[i] = f->buf[i];
            vis[show] = '\0';
            cw = draw_string_width(vis);
        } else {
            cw = f->len > 0 ? f->len * 12 + 5 : 0;
        }
        draw_rect(scr, tx + cw + 2, ty + 1, 2, FONT_H - 2, C_CURSOR_COL);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * AUTHENTICATE BUTTON (gradient)
 * ═══════════════════════════════════════════════════════════════════════ */

static void draw_auth_button(canvas_t* scr, int x, int y, int w, int h)
{
    /* Gradient fill using rounded rect */
    draw_gradient_v(scr, x + BTN_RADIUS, y, w - 2 * BTN_RADIUS, h,
                    C_BTN_TOP, C_BTN_BOT);
    draw_gradient_v(scr, x, y + BTN_RADIUS, BTN_RADIUS, h - 2 * BTN_RADIUS,
                    C_BTN_TOP, C_BTN_BOT);
    draw_gradient_v(scr, x + w - BTN_RADIUS, y + BTN_RADIUS,
                    BTN_RADIUS, h - 2 * BTN_RADIUS, C_BTN_TOP, C_BTN_BOT);

    /* Rounded corners */
    for (int row = 0; row < BTN_RADIUS; row++) {
        /* Interpolate color */
        int r0 = 0x28, g0 = 0x78, b0 = 0xC0;
        int r1 = 0x1C, g1 = 0x5C, b1 = 0x98;

        int t_top = row;
        int cr_t = r0 + (r1 - r0) * t_top / h;
        int cg_t = g0 + (g1 - g0) * t_top / h;
        int cb_t = b0 + (b1 - b0) * t_top / h;
        uint32_t c_top = rgb((uint8_t)cr_t, (uint8_t)cg_t, (uint8_t)cb_t);

        int t_bot = h - 1 - row;
        int cr_b = r0 + (r1 - r0) * t_bot / h;
        int cg_b = g0 + (g1 - g0) * t_bot / h;
        int cb_b = b0 + (b1 - b0) * t_bot / h;
        uint32_t c_bot = rgb((uint8_t)cr_b, (uint8_t)cg_b, (uint8_t)cb_b);

        for (int col = 0; col < BTN_RADIUS; col++) {
            int dx = BTN_RADIUS - col - 1, dy = BTN_RADIUS - row - 1;
            if (dx * dx + dy * dy > BTN_RADIUS * BTN_RADIUS) continue;

            /* Top corners */
            int px = x + col, py = y + row;
            if ((unsigned)px < (unsigned)scr->width &&
                (unsigned)py < (unsigned)scr->height)
                scr->pixels[py * scr->stride + px] = c_top;
            px = x + w - 1 - col;
            if ((unsigned)px < (unsigned)scr->width &&
                (unsigned)py < (unsigned)scr->height)
                scr->pixels[py * scr->stride + px] = c_top;

            /* Bottom corners */
            py = y + h - 1 - row;
            px = x + col;
            if ((unsigned)px < (unsigned)scr->width &&
                (unsigned)py < (unsigned)scr->height)
                scr->pixels[py * scr->stride + px] = c_bot;
            px = x + w - 1 - col;
            if ((unsigned)px < (unsigned)scr->width &&
                (unsigned)py < (unsigned)scr->height)
                scr->pixels[py * scr->stride + px] = c_bot;
        }
    }

    /* Subtle top highlight */
    draw_rect_alpha(scr, x + BTN_RADIUS, y + 1, w - 2 * BTN_RADIUS, 1,
                    rgba(0xFF, 0xFF, 0xFF, 0x18));

    /* Border */
    draw_rect_rounded_outline(scr, x, y, w, h, BTN_RADIUS, 1,
                               rgba(0x50, 0xA0, 0xD0, 0x40));

    /* Label */
    const char* lbl = "AUTHENTICATE";
    int tw = draw_string_width(lbl);
    draw_string(scr, x + (w - tw) / 2, y + (h - FONT_H) / 2,
                lbl, C_BTN_TEXT, rgba(0,0,0,0));
}

/* ═══════════════════════════════════════════════════════════════════════
 * STATUS BAR ICONS
 *
 * Reference layout (left to right):
 *   WiFi | Signal Bars | separator | Drive | Arrows | Sync | sep | Shield
 * ═══════════════════════════════════════════════════════════════════════ */

/* 1) WiFi icon – center dot + 3 concentric arcs (upper half only) */
static void draw_icon_wifi(canvas_t* scr, int cx, int cy, uint32_t color)
{
    /* Base dot */
    draw_circle_filled(scr, cx, cy + 6, 2, color);

    /* Draw arcs as partial circles (upper 180°) using Bresenham-ish */
    int radii[3] = { 6, 10, 14 };
    for (int a = 0; a < 3; a++) {
        int r = radii[a];
        /* Walk the arc only for the upper-half, roughly -135° to -45° */
        for (int dx = -r; dx <= r; dx++) {
            int dy2 = r * r - dx * dx;
            if (dy2 < 0) continue;
            int dy = isqrt(dy2);
            /* Only keep the top arc portion: |dx| < dy (upper 90° cone) */
            if ((dx < 0 ? -dx : dx) > dy) continue;
            /* Draw 2-pixel thick arc */
            px_blend(scr, cx + dx, cy + 6 - dy, color);
            px_blend(scr, cx + dx, cy + 6 - dy + 1, color);
        }
    }
}

/* 2) Signal bars – 5 ascending bars */
static void draw_icon_signal(canvas_t* scr, int x, int y, uint32_t color)
{
    int total_h = 16;
    int n_bars = 5;
    for (int i = 0; i < n_bars; i++) {
        int bar_h = 4 + i * 3;
        int bx = x + i * 5;
        int by = y + total_h - bar_h;
        draw_rect_rounded(scr, bx, by, 3, bar_h, 1, color);
    }
}

/* 3) Vertical separator bar */
static void draw_icon_sep(canvas_t* scr, int x, int y, int h)
{
    draw_rect_alpha(scr, x, y, 1, h, rgba(0x40, 0x70, 0xA0, 0x50));
}

/* 4) Drive / storage icon – rounded rectangle with horizontal lines */
static void draw_icon_drive(canvas_t* scr, int x, int y, uint32_t color)
{
    /* Drive body */
    draw_rect_rounded(scr, x, y + 2, 20, 14, 3, color);
    /* Inner cutout (dark) to make it look like an enclosure */
    draw_rect(scr, x + 2, y + 5, 16, 8, C_BG_TOP);
    /* Horizontal platter lines inside */
    draw_hline(scr, x + 4, y + 7, 12, color);
    draw_hline(scr, x + 4, y + 10, 12, color);
    /* LED dot (bottom-right) */
    draw_rect(scr, x + 16, y + 12, 2, 2, C_CLOCK);
}

/* 5) Data transfer arrows (up + down) */
static void draw_icon_arrows(canvas_t* scr, int x, int y, uint32_t color)
{
    /* Up arrow */
    int ux = x + 4, uy = y + 1;
    /* Arrowhead */
    for (int i = 0; i < 4; i++) {
        draw_hline(scr, ux - i, uy + i, 2 * i + 1, color);
    }
    /* Shaft */
    draw_rect(scr, ux - 1, uy + 4, 3, 5, color);

    /* Down arrow */
    int dx = x + 14, dy = y + 7;
    /* Shaft */
    draw_rect(scr, dx - 1, dy, 3, 5, color);
    /* Arrowhead */
    for (int i = 0; i < 4; i++) {
        draw_hline(scr, dx - i, dy + 5 + i, 2 * i + 1, color);
    }
}

/* 6) Sync / refresh circle (circular arrow) */
static void draw_icon_sync(canvas_t* scr, int cx, int cy, uint32_t color)
{
    int r = 7;
    /* Draw circle outline but leave a gap for the arrowhead */
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            /* Ring: between (r-1)^2 and r^2 */
            if (d2 > r * r || d2 < (r - 2) * (r - 2)) continue;

            /* Leave gap in bottom-right quadrant for arrow */
            if (dx > 2 && dy > 0) continue;

            px_set(scr, cx + dx, cy + dy, color);
        }
    }
    /* Small arrowhead at gap end (pointing clockwise, i.e. right) */
    int ax = cx + r - 1, ay = cy + 1;
    for (int i = 0; i < 3; i++) {
        px_set(scr, ax + i, ay, color);
        if (i > 0) {
            px_set(scr, ax, ay + i, color);
            px_set(scr, ax, ay - i, color);
        }
    }
}

/* 7) Shield with checkmark */
static void draw_icon_shield(canvas_t* scr, int x, int y, uint32_t color)
{
    int w = 18, h = 20;
    int cx = x + w / 2;

    /* Shield shape: rounded top, pointed bottom */
    for (int row = 0; row < h; row++) {
        int half_w;
        if (row < 4) {
            /* Top: slightly rounded */
            half_w = w / 2 - (row == 0 ? 2 : (row == 1 ? 1 : 0));
        } else if (row < h - 4) {
            /* Middle: full width tapering slightly */
            half_w = w / 2 - (row - h / 2 + 4) * 1 / 6;
            if (half_w > w / 2) half_w = w / 2;
        } else {
            /* Bottom: taper to point */
            int remaining = h - row;
            half_w = remaining * (w / 2) / 5;
        }
        if (half_w < 0) half_w = 0;

        int left = cx - half_w;
        int right = cx + half_w;

        /* Draw outline only (2px thick at sides) */
        int py = y + row;
        for (int px = left; px <= right; px++) {
            bool is_edge = (px <= left + 1 || px >= right - 1 ||
                            row <= 1 || row >= h - 2);
            /* Fill the border band */
            if (is_edge) {
                px_set(scr, px, py, color);
            }
        }
    }

    /* Checkmark inside the shield */
    /* Short stroke: going down-right */
    draw_line(scr, cx - 4, y + 9, cx - 1, y + 12, color);
    draw_line(scr, cx - 4, y + 10, cx - 1, y + 13, color);
    /* Long stroke: going up-right */
    draw_line(scr, cx - 1, y + 12, cx + 4, y + 7, color);
    draw_line(scr, cx - 1, y + 13, cx + 4, y + 8, color);
}

/* Master function: draw all status indicators in a row */
static void draw_status_indicators(canvas_t* scr, int x, int y,
                                    uint32_t color)
{
    int cursor = x;
    int icon_h = 18;

    /* 1. WiFi */
    draw_icon_wifi(scr, cursor + 10, y + 2, color);
    cursor += 28;

    /* 2. Signal bars */
    draw_icon_signal(scr, cursor, y + 1, color);
    cursor += 28;

    /* separator */
    draw_icon_sep(scr, cursor, y, icon_h);
    cursor += 8;

    /* 3. Drive */
    draw_icon_drive(scr, cursor, y, color);
    cursor += 26;

    /* 4. Data transfer arrows */
    draw_icon_arrows(scr, cursor, y, color);
    cursor += 24;

    /* 5. Sync circle */
    draw_icon_sync(scr, cursor + 7, y + 9, color);
    cursor += 22;

    /* separator */
    draw_icon_sep(scr, cursor, y, icon_h);
    cursor += 8;

    /* 6. Shield with checkmark */
    draw_icon_shield(scr, cursor, y - 1, color);
}

/* ═══════════════════════════════════════════════════════════════════════
 * TIMEZONE SETUP WIZARD OVERLAY
 *
 * macOS/Windows-style first-boot experience:
 *   Step 1: "Select your region"   -> list of continents
 *   Step 2: "Select your city"     -> list of cities with UTC offsets
 *   Navigate with arrow keys, Enter to confirm, Esc to go back.
 * ═══════════════════════════════════════════════════════════════════════ */

#define WIZ_W    420
#define WIZ_H    380
#define WIZ_RADIUS 16
#define WIZ_ITEM_H  22
#define WIZ_PAD     24

static void draw_tz_wizard(canvas_t* scr)
{
    int W = scr->width, H = scr->height;

    /* Clock step uses a smaller card */
    int wiz_h = (g_tz_wizard == TZ_WIZARD_CLOCK) ? 220 : WIZ_H;
    int wx = (W - WIZ_W) / 2;
    int wy = (H - wiz_h) / 2;

    /* Dim background */
    draw_rect_alpha(scr, 0, 0, W, H, rgba(0, 0, 0, 0x80));

    /* Wizard card */
    draw_glass_card(scr, wx, wy, WIZ_W, wiz_h, WIZ_RADIUS);

    /* ── Step 0: Clock source ──────────────────────────────────── */
    if (g_tz_wizard == TZ_WIZARD_CLOCK) {
        const char* title = "Hardware Clock Setting";
        int tw_val = draw_string_width(title);
        draw_string(scr, wx + (WIZ_W - tw_val) / 2, wy + WIZ_PAD,
                    title, C_NAME, rgba(0,0,0,0));

        const char* sub = "How is your computer's clock configured?";
        int sw_val = draw_string_width(sub);
        draw_string(scr, wx + (WIZ_W - sw_val) / 2, wy + WIZ_PAD + 20,
                    sub, C_SUBTEXT, rgba(0,0,0,0));

        draw_rect_alpha(scr, wx + WIZ_PAD, wy + WIZ_PAD + 42,
                        WIZ_W - 2 * WIZ_PAD, 1,
                        rgba(0x40, 0x60, 0x80, 0x30));

        /* Two options */
        static const char* clk_opts[] = {
            "Local time (Windows / VirtualBox default)",
            "UTC (Linux / manual setup)",
        };
        int ly = wy + WIZ_PAD + 56;
        int lx = wx + WIZ_PAD;
        int lw = WIZ_W - 2 * WIZ_PAD;

        for (int i = 0; i < 2; i++) {
            int iy = ly + i * 36;
            bool is_sel = (i == g_tz_clock_sel);
            if (is_sel) {
                draw_rect_alpha(scr, lx, iy, lw, 28,
                                rgba(0x30, 0x70, 0xB0, 0x60));
                draw_rect_rounded_outline(scr, lx, iy, lw, 28,
                                           4, 1, C_FIELD_FOCUS);
            }
            /* Radio circle */
            draw_circle(scr, lx + 14, iy + 14, 5, is_sel ? C_NAME : C_TEXT);
            if (is_sel)
                draw_circle_filled(scr, lx + 14, iy + 14, 2, C_FIELD_FOCUS);

            draw_string(scr, lx + 28, iy + 6, clk_opts[i],
                        is_sel ? C_NAME : C_TEXT, rgba(0,0,0,0));
        }

        const char* hint = "Use Up/Down + Enter. Esc = skip (local)";
        int hw = draw_string_width(hint);
        draw_string(scr, wx + (WIZ_W - hw) / 2, wy + wiz_h - WIZ_PAD - 14,
                    hint, C_INFO, rgba(0,0,0,0));
        return;
    }

    /* ── Steps 1 & 2: Region / City list ───────────────────────── */
    const char* title;
    const char* subtitle;
    if (g_tz_wizard == TZ_WIZARD_REGION) {
        title = "Select Your Region";
        subtitle = "Use Up/Down arrows and Enter to select";
    } else {
        title = "Select Your City";
        subtitle = "Press Esc to go back";
    }

    int tw_val = draw_string_width(title);
    draw_string(scr, wx + (WIZ_W - tw_val) / 2, wy + WIZ_PAD,
                title, C_NAME, rgba(0,0,0,0));
    int sw_val = draw_string_width(subtitle);
    draw_string(scr, wx + (WIZ_W - sw_val) / 2, wy + WIZ_PAD + 20,
                subtitle, C_SUBTEXT, rgba(0,0,0,0));

    /* Separator */
    draw_rect_alpha(scr, wx + WIZ_PAD, wy + WIZ_PAD + 42,
                    WIZ_W - 2 * WIZ_PAD, 1,
                    rgba(0x40, 0x60, 0x80, 0x30));

    /* List items */
    int list_y = wy + WIZ_PAD + 50;
    int list_x = wx + WIZ_PAD;
    int list_w = WIZ_W - 2 * WIZ_PAD;

    int count, sel;
    if (g_tz_wizard == TZ_WIZARD_REGION) {
        count = TZ_REGION_COUNT;
        sel = g_tz_region_sel;
    } else {
        count = tz_regions[g_tz_region_sel].count;
        sel = g_tz_city_sel;
    }

    /* Calculate scroll offset to keep selection visible */
    int max_visible = (wiz_h - WIZ_PAD - 50 - WIZ_PAD) / WIZ_ITEM_H;
    int scroll = 0;
    if (sel >= max_visible) scroll = sel - max_visible + 1;
    if (scroll > count - max_visible) scroll = count - max_visible;
    if (scroll < 0) scroll = 0;

    for (int i = scroll; i < count && (i - scroll) < max_visible; i++) {
        int iy = list_y + (i - scroll) * WIZ_ITEM_H;
        bool is_sel = (i == sel);

        if (is_sel) {
            draw_rect_alpha(scr, list_x, iy, list_w, WIZ_ITEM_H - 2,
                            rgba(0x30, 0x70, 0xB0, 0x60));
            draw_rect_rounded_outline(scr, list_x, iy, list_w, WIZ_ITEM_H - 2,
                                       4, 1, C_FIELD_FOCUS);
        }

        const char* item_name;
        if (g_tz_wizard == TZ_WIZARD_REGION) {
            item_name = tz_regions[i].name;
        } else {
            item_name = tz_regions[g_tz_region_sel].cities[i].name;
        }

        draw_string(scr, list_x + 10, iy + 2, item_name,
                    is_sel ? C_NAME : C_TEXT, rgba(0,0,0,0));

        /* Show UTC offset for city list */
        if (g_tz_wizard == TZ_WIZARD_CITY) {
            int16_t off = tz_regions[g_tz_region_sel].cities[i].offset;
            char offstr[12];
            int oi = 0;
            offstr[oi++] = 'U'; offstr[oi++] = 'T'; offstr[oi++] = 'C';
            if (off >= 0) offstr[oi++] = '+';
            else { offstr[oi++] = '-'; off = -off; }
            int oh = off / 60, om = off % 60;
            if (oh >= 10) offstr[oi++] = '0' + (char)(oh / 10);
            offstr[oi++] = '0' + (char)(oh % 10);
            if (om > 0) {
                offstr[oi++] = ':';
                offstr[oi++] = '0' + (char)(om / 10);
                offstr[oi++] = '0' + (char)(om % 10);
            }
            offstr[oi] = '\0';
            int ow = draw_string_width(offstr);
            draw_string(scr, list_x + list_w - ow - 10, iy + 2,
                        offstr, C_INFO, rgba(0,0,0,0));
        }
    }

    /* Scroll indicators */
    if (scroll > 0) {
        draw_string(scr, wx + WIZ_W / 2 - 4, list_y - 12,
                    "^", C_INFO, rgba(0,0,0,0));
    }
    if (scroll + max_visible < count) {
        int bot = list_y + max_visible * WIZ_ITEM_H;
        draw_string(scr, wx + WIZ_W / 2 - 4, bot,
                    "v", C_INFO, rgba(0,0,0,0));
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN LOGIN SCREEN RENDERER
 * ═══════════════════════════════════════════════════════════════════════ */

static void draw_login_screen(canvas_t* scr)
{
    int W = scr->width;
    int H = scr->height;

    /* ── Background ─────────────────────────────────────────────────── */
    draw_gradient_v(scr, 0, 0, W, H, C_BG_TOP, C_BG_BOT);
    draw_grid(scr, W, H);

    /* Radial glow behind clock area */
    draw_glow(scr, W * 30 / 100, H * 38 / 100, H * 70 / 100,
              0x15, 0x30, 0x60, 22);
    /* Smaller glow near card */
    draw_glow(scr, W * 68 / 100, H * 42 / 100, H * 35 / 100,
              0x20, 0x50, 0x80, 12);

    /* ── Real-time clock from CMOS RTC ─────────────────────────────── */
    rtc_time_t now_time;
    rtc_get_time(&now_time);
    int hours   = now_time.hour;
    int minutes = now_time.minute;
    int seconds = now_time.second;

    /* ── LEFT SIDE: Clock ───────────────────────────────────────────── */
    int left_margin = W * 8 / 100;
    int clock_y = H * 22 / 100;

    draw_clock(scr, left_margin, clock_y, hours, minutes,
               (seconds % 2) == 0);

    /* ── Date line (from RTC) ───────────────────────────────────────── */
    int date_y = clock_y + DIGIT_H + 20;
    static const char* day_names[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    /* Format: YYYY.MM.DD  DAY */
    char date_str[20];
    date_str[0]  = '0' + (char)(now_time.year / 1000);
    date_str[1]  = '0' + (char)((now_time.year / 100) % 10);
    date_str[2]  = '0' + (char)((now_time.year / 10) % 10);
    date_str[3]  = '0' + (char)(now_time.year % 10);
    date_str[4]  = '.';
    date_str[5]  = '0' + (char)(now_time.month / 10);
    date_str[6]  = '0' + (char)(now_time.month % 10);
    date_str[7]  = '.';
    date_str[8]  = '0' + (char)(now_time.day / 10);
    date_str[9]  = '0' + (char)(now_time.day % 10);
    date_str[10] = ' ';
    date_str[11] = ' ';
    const char* dn = day_names[now_time.weekday % 7];
    date_str[12] = dn[0]; date_str[13] = dn[1]; date_str[14] = dn[2];
    date_str[15] = '\0';

    draw_string_scaled_aa(scr, left_margin, date_y, date_str, 2, C_DATE);

    /* ── System info ────────────────────────────────────────────────── */
    int info_y = date_y + 16 * 2 + 20;
    draw_string(scr, left_margin, info_y,
                OS_NAME " | Kernel v" OS_VERSION " | System Secure | Ready",
                C_INFO, rgba(0,0,0,0));
    draw_string(scr, left_margin, info_y + 20,
                "Device: AETHER-WS-01 | " OS_RELEASE " | Encrypted",
                C_INFO_DIM, rgba(0,0,0,0));

    /* ── Status indicators ──────────────────────────────────────────── */
    int icon_y = info_y + 50;
    draw_status_indicators(scr, left_margin, icon_y, C_INFO);

    /* ── RIGHT SIDE: Login card ─────────────────────────────────────── */
    int card_x = W - CARD_W - W * 10 / 100;
    int card_y = H * 18 / 100;
    if (card_x < W / 2) card_x = W / 2;

    draw_glass_card(scr, card_x, card_y, CARD_W, CARD_H, CARD_RADIUS);

    /* ── Avatar ─────────────────────────────────────────────────────── */
    int avatar_cx = card_x + 68;
    int avatar_cy = card_y + 68;
    draw_sphere_avatar(scr, avatar_cx, avatar_cy, AVATAR_R);

    /* ── Username and info (next to avatar) ─────────────────────────── */
    int name_x = avatar_cx + AVATAR_R + 18;
    int name_y = avatar_cy - 14;
    draw_string_scaled_aa(scr, name_x, name_y, login_username, 2, C_NAME);

    char login_info[64];
    /* Build "Last Login: YYYY.MM.DD HH:MM TZ" */
    int li = 0;
    const char* prefix = "Last Login: ";
    while (*prefix) login_info[li++] = *prefix++;
    /* Date */
    login_info[li++] = date_str[0]; login_info[li++] = date_str[1];
    login_info[li++] = date_str[2]; login_info[li++] = date_str[3];
    login_info[li++] = '.';
    login_info[li++] = date_str[5]; login_info[li++] = date_str[6];
    login_info[li++] = '.';
    login_info[li++] = date_str[8]; login_info[li++] = date_str[9];
    login_info[li++] = ' ';
    login_info[li++] = '0' + (char)(hours / 10);
    login_info[li++] = '0' + (char)(hours % 10);
    login_info[li++] = ':';
    login_info[li++] = '0' + (char)(minutes / 10);
    login_info[li++] = '0' + (char)(minutes % 10);
    login_info[li++] = ' ';
    /* Timezone label */
    int16_t tz = rtc_get_tz_offset();
    if (tz == 0) {
        login_info[li++] = 'U'; login_info[li++] = 'T'; login_info[li++] = 'C';
    } else {
        login_info[li++] = 'U'; login_info[li++] = 'T'; login_info[li++] = 'C';
        login_info[li++] = (tz > 0) ? '+' : '-';
        int abs_h = (tz < 0 ? -tz : tz) / 60;
        int abs_m = (tz < 0 ? -tz : tz) % 60;
        if (abs_h >= 10) login_info[li++] = '0' + (char)(abs_h / 10);
        login_info[li++] = '0' + (char)(abs_h % 10);
        if (abs_m > 0) {
            login_info[li++] = ':';
            login_info[li++] = '0' + (char)(abs_m / 10);
            login_info[li++] = '0' + (char)(abs_m % 10);
        }
    }
    login_info[li] = '\0';
    draw_string(scr, name_x, name_y + 36, login_info, C_SUBTEXT, rgba(0,0,0,0));

    /* Separator */
    int sep_y = card_y + 140;
    draw_rect_alpha(scr, card_x + 20, sep_y, CARD_W - 40, 1,
                    rgba(0x40, 0x60, 0x80, 0x30));

    /* ── Password field ─────────────────────────────────────────────── */
    int field_x = card_x + (CARD_W - FIELD_W) / 2;
    int field_y = sep_y + 20;
    draw_password_field(scr, field_x, field_y, FIELD_W, FIELD_H,
                        &g_fields[FIELD_PASS], true);
    g_field_x = field_x;
    g_field_y = field_y;

    /* ── Error message ──────────────────────────────────────────────── */
    int err_space = 0;
    if (g_error) {
        const char* err = "Authentication failed. Try again.";
        int ew = draw_string_width(err);
        int card_cx = card_x + CARD_W / 2;
        draw_string(scr, card_cx - ew / 2, field_y + FIELD_H + 6,
                    err, C_ERROR_COL, rgba(0,0,0,0));
        err_space = FONT_H + 6;
    }

    /* ── AUTHENTICATE button ────────────────────────────────────────── */
    int card_cx = card_x + CARD_W / 2;
    int btn_x = card_cx - BTN_W / 2;
    int btn_y = field_y + FIELD_H + 14 + err_space;
    draw_auth_button(scr, btn_x, btn_y, BTN_W, BTN_H);
    g_btn_x = btn_x;
    g_btn_y = btn_y;

    /* ── Footer: Pre-login system menu ─────────────────────────────── */
    /*
     * Reference layout (below the card):
     *   Row 1:  👤 Switch User
     *   ─────────────────────────────────────────
     *   Row 2:  ⏻ Shutdown / Restart / Sleep  (i) Accessibility  ≋ Network
     *   Row 3:  ⌨ Keyboard: EN-US
     */
    int footer_y = card_y + CARD_H + 16;
    int fl = card_x + 16;             /* footer left margin */
    int fr = card_x + CARD_W - 16;    /* footer right edge */

    /* --- Row 1: Switch User (person icon + text) --- */
    {
        int ry = footer_y;
        /* Person icon (head circle + body arc) */
        int ix = fl + 6, iy = ry + 6;
        draw_circle_filled(scr, ix, iy - 2, 4, C_FOOTER_HI); /* head */
        /* Shoulders/torso arc */
        for (int dy = 3; dy <= 8; dy++) {
            int half = 5 - (8 - dy) / 2;
            if (half < 2) half = 2;
            draw_hline(scr, ix - half, iy + dy, half * 2 + 1, C_FOOTER_HI);
            if (dy == 3) break; /* just top of shoulders visible */
        }
        /* Body (small trapezoid) */
        for (int dy = 4; dy <= 7; dy++) {
            int half = 3 + (dy - 4) / 2;
            draw_hline(scr, ix - half, iy + dy, half * 2 + 1, C_FOOTER_HI);
        }

        draw_string(scr, fl + 20, ry, "Switch User", C_FOOTER_HI, rgba(0,0,0,0));
    }

    /* --- Separator line --- */
    int sep_row = footer_y + 18;
    draw_rect_alpha(scr, fl, sep_row, fr - fl, 1,
                    rgba(0x40, 0x60, 0x80, 0x30));

    /* --- Row 2: Shutdown / Restart / Sleep | Accessibility | Network --- */
    {
        int ry = sep_row + 6;
        int cx_pos = fl;

        /* Power icon (circle with line) */
        draw_circle(scr, cx_pos + 6, ry + 7, 5, C_FOOTER);
        draw_vline(scr, cx_pos + 6, ry + 1, 5, C_FOOTER);
        cx_pos += 16;

        /* Draw "Shutdown", " / ", "Restart", " / ", "Sleep" separately
         * and record hitboxes for each clickable word */
        int tw;

        tw = draw_string_width("Shutdown");
        draw_string(scr, cx_pos, ry + 1, "Shutdown", C_FOOTER_HI, rgba(0,0,0,0));
        g_hit_shutdown = (hitbox_t){ cx_pos, ry, tw, 16 };
        cx_pos += tw;

        draw_string(scr, cx_pos, ry + 1, " / ", C_FOOTER, rgba(0,0,0,0));
        cx_pos += draw_string_width(" / ");

        tw = draw_string_width("Restart");
        draw_string(scr, cx_pos, ry + 1, "Restart", C_FOOTER_HI, rgba(0,0,0,0));
        g_hit_restart = (hitbox_t){ cx_pos, ry, tw, 16 };
        cx_pos += tw;

        draw_string(scr, cx_pos, ry + 1, " / ", C_FOOTER, rgba(0,0,0,0));
        cx_pos += draw_string_width(" / ");

        tw = draw_string_width("Sleep");
        draw_string(scr, cx_pos, ry + 1, "Sleep", C_FOOTER_HI, rgba(0,0,0,0));
        g_hit_sleep = (hitbox_t){ cx_pos, ry, tw, 16 };
        cx_pos += tw + 12;

        /* Accessibility icon: (i) in a circle */
        draw_circle(scr, cx_pos + 6, ry + 7, 6, C_FOOTER);
        /* letter i: dot + vertical bar */
        px_set(scr, cx_pos + 6, ry + 4, C_FOOTER);
        draw_vline(scr, cx_pos + 6, ry + 6, 5, C_FOOTER);
        cx_pos += 16;

        draw_string(scr, cx_pos, ry + 1, "Accessibility",
                    C_FOOTER_HI, rgba(0,0,0,0));
        cx_pos += draw_string_width("Accessibility") + 12;

        /* Network/WiFi mini-icon */
        {
            int wcx = cx_pos + 6, wcy = ry + 8;
            draw_circle_filled(scr, wcx, wcy + 3, 1, C_FOOTER);
            /* Two small arcs */
            for (int dx = -4; dx <= 4; dx++) {
                int d2 = 16 - dx * dx;
                if (d2 < 0) continue;
                int dy = isqrt(d2);
                if ((dx < 0 ? -dx : dx) > dy) continue;
                px_set(scr, wcx + dx, wcy + 3 - dy, C_FOOTER);
            }
            for (int dx = -7; dx <= 7; dx++) {
                int d2 = 49 - dx * dx;
                if (d2 < 0) continue;
                int dy = isqrt(d2);
                if ((dx < 0 ? -dx : dx) > dy) continue;
                px_set(scr, wcx + dx, wcy + 3 - dy, C_FOOTER);
            }
        }
        cx_pos += 16;

        draw_string(scr, cx_pos, ry + 1, "Network",
                    C_FOOTER_HI, rgba(0,0,0,0));
    }

    /* --- Row 3: Keyboard layout --- */
    {
        int ry = sep_row + 24;

        /* Keyboard icon (small rectangle with dots) */
        draw_rect_rounded_outline(scr, fl, ry + 1, 14, 10, 2, 1, C_FOOTER);
        /* Key dots inside */
        draw_rect(scr, fl + 3, ry + 3, 2, 2, C_FOOTER);
        draw_rect(scr, fl + 6, ry + 3, 2, 2, C_FOOTER);
        draw_rect(scr, fl + 9, ry + 3, 2, 2, C_FOOTER);
        /* Space bar */
        draw_hline(scr, fl + 4, ry + 7, 6, C_FOOTER);

        draw_string(scr, fl + 18, ry, "Keyboard: EN-US",
                    C_FOOTER_HI, rgba(0,0,0,0));
    }

    /* ── Decorative sparkles ────────────────────────────────────────── */
    draw_sparkle(scr, W - W * 7 / 100, H - H * 10 / 100, 22);
    draw_sparkle(scr, W - W * 12 / 100, H - H * 16 / 100, 10);

    /* ── OS branding (bottom-left) ──────────────────────────────────── */
    draw_string(scr, left_margin, H - 28, OS_BANNER_SHORT,
                C_INFO_DIM, rgba(0,0,0,0));

    /* ── Real-time clock (bottom-right) ─────────────────────────────── */
    char timestr[16];
    timestr[0] = '0' + (char)(hours / 10);   timestr[1] = '0' + (char)(hours % 10);
    timestr[2] = ':';
    timestr[3] = '0' + (char)(minutes / 10); timestr[4] = '0' + (char)(minutes % 10);
    timestr[5] = ':';
    timestr[6] = '0' + (char)(seconds / 10); timestr[7] = '0' + (char)(seconds % 10);
    timestr[8] = '\0';
    int uw = draw_string_width(timestr);
    draw_string(scr, W - uw - 20, H - 28, timestr,
                C_INFO, rgba(0,0,0,0));

    /* ── Timezone wizard overlay (if active) ─────────────────────────── */
    if (g_tz_wizard >= TZ_WIZARD_CLOCK && g_tz_wizard <= TZ_WIZARD_CITY)
        draw_tz_wizard(scr);
}

/* ═══════════════════════════════════════════════════════════════════════
 * HITBOX HELPER
 * ═══════════════════════════════════════════════════════════════════════ */
static bool hitbox_test(const hitbox_t* h, int mx, int my)
{
    return mx >= h->x && mx < h->x + h->w &&
           my >= h->y && my < h->y + h->h;
}

/* ── Authentication ───────────────────────────────────────────────────── */
static bool try_login(void)
{
    if (strlen(login_username) < 1) return false;
    int uid = users_authenticate(login_username, g_fields[FIELD_PASS].buf);
    if (uid < 0) return false;
    return true;
}

/* ── Main login loop ──────────────────────────────────────────────────── */
void login_run(void)
{
    if (!fb_ready()) return;
    canvas_t screen = draw_main_canvas();

    /* Initialize RTC */
    rtc_init();

    g_fields[FIELD_PASS].buf[0] = '\0';
    g_fields[FIELD_PASS].len    = 0;
    g_fields[FIELD_PASS].active = true;
    g_error        = false;
    g_show_password = false;
    g_blink_tick   = timer_get_ticks();
    g_cursor_vis   = true;

    /* If timezone not yet configured, launch the setup wizard */
    if (!rtc_tz_configured()) {
        g_tz_wizard = TZ_WIZARD_CLOCK;
        g_tz_clock_sel = 0;
        g_tz_region_sel = 0;
        g_tz_city_sel = 0;
    } else {
        g_tz_wizard = TZ_WIZARD_NONE;
    }

    bool    prev_enter   = false;

    while (1) {
        /* ── Cursor blink ────────────────────────────────────────── */
        uint32_t now = timer_get_ticks();
        if (now - g_blink_tick >= (uint32_t)(TIMER_FREQ / 2)) {
            g_cursor_vis = !g_cursor_vis;
            g_blink_tick = now;
        }

        /* ── Keyboard input ──────────────────────────────────────── */
        /* Use raw poll for arrow key support in the wizard */
        int kc = 0; uint8_t mods = 0; char ch = 0; bool kdown = false;
        bool got_key = keyboard_poll(&kc, &mods, &ch, &kdown);

        if (got_key && kdown) {

            /* --- Timezone wizard keyboard handling --- */
            if (g_tz_wizard == TZ_WIZARD_CLOCK) {
                /* Clock source step: local vs UTC */
                if (kc == KEY_UP_ARROW || ch == 'k' || ch == 'K') {
                    g_tz_clock_sel = 0;
                } else if (kc == KEY_DOWN_ARROW || ch == 'j' || ch == 'J') {
                    g_tz_clock_sel = 1;
                } else if (ch == '\n' || ch == '\r') {
                    rtc_set_utc_hwclock(g_tz_clock_sel == 1);
                    g_tz_wizard = TZ_WIZARD_REGION;
                    g_tz_region_sel = 0;
                } else if (kc == 0x1B) {
                    /* Esc = skip, default to local time */
                    rtc_set_utc_hwclock(false);
                    rtc_set_tz_offset(0);
                    rtc_set_tz_configured(true);
                    g_tz_wizard = TZ_WIZARD_NONE;
                }

            } else if (g_tz_wizard == TZ_WIZARD_REGION ||
                       g_tz_wizard == TZ_WIZARD_CITY) {

                int count;
                int* sel;
                if (g_tz_wizard == TZ_WIZARD_REGION) {
                    count = TZ_REGION_COUNT;
                    sel = &g_tz_region_sel;
                } else {
                    count = tz_regions[g_tz_region_sel].count;
                    sel = &g_tz_city_sel;
                }

                if (kc == 0x1B) { /* Escape */
                    if (g_tz_wizard == TZ_WIZARD_CITY) {
                        g_tz_wizard = TZ_WIZARD_REGION;
                        g_tz_city_sel = 0;
                    } else {
                        g_tz_wizard = TZ_WIZARD_CLOCK;
                    }
                } else if (kc == KEY_UP_ARROW || ch == 'k' || ch == 'K') {
                    if (*sel > 0) (*sel)--;
                } else if (kc == KEY_DOWN_ARROW || ch == 'j' || ch == 'J') {
                    if (*sel < count - 1) (*sel)++;
                } else if (ch == '\n' || ch == '\r') {
                    if (g_tz_wizard == TZ_WIZARD_REGION) {
                        g_tz_wizard = TZ_WIZARD_CITY;
                        g_tz_city_sel = 0;
                    } else {
                        /* City selected - apply timezone */
                        int16_t offset = tz_regions[g_tz_region_sel]
                                            .cities[g_tz_city_sel].offset;
                        rtc_set_tz_offset(offset);
                        rtc_set_tz_configured(true);
                        g_tz_wizard = TZ_WIZARD_NONE;
                    }
                }

            } else {
                /* --- Normal password field handling --- */
                field_t* f = &g_fields[FIELD_PASS];
                if (ch == '\n' || ch == '\r') {
                    if (!prev_enter) {
                        prev_enter = true;
                        if (try_login()) return;
                        g_error = true;
                        f->len = 0;
                        f->buf[0] = '\0';
                    }
                } else {
                    prev_enter = false;
                    if (ch == '\b') {
                        if (f->len > 0) {
                            f->buf[--f->len] = '\0';
                            g_error = false;
                        }
                    } else if (ch >= 0x20 && ch < 0x7F && f->len < 63) {
                        f->buf[f->len++] = ch;
                        f->buf[f->len]   = '\0';
                        g_error = false;
                    }
                }
            }
        } else if (!got_key) {
            prev_enter = false;
        }

        /* ── Mouse input ─────────────────────────────────────────── */
        {
            mouse_event_t mev;
            mouse_get_event(&mev);
            if (mev.left_clicked && g_tz_wizard == TZ_WIZARD_NONE) {
                int mx = mev.x, my = mev.y;

                /* Authenticate button */
                if (mx >= g_btn_x && mx < g_btn_x + BTN_W &&
                    my >= g_btn_y && my < g_btn_y + BTN_H) {
                    if (try_login()) return;
                    g_error = true;
                    g_fields[FIELD_PASS].len = 0;
                    g_fields[FIELD_PASS].buf[0] = '\0';
                }

                /* Footer: Shutdown */
                if (hitbox_test(&g_hit_shutdown, mx, my)) {
                    power_shutdown();
                }

                /* Footer: Restart */
                if (hitbox_test(&g_hit_restart, mx, my)) {
                    power_restart();
                }

                /* Footer: Sleep */
                if (hitbox_test(&g_hit_sleep, mx, my)) {
                    power_sleep();
                }

                /* Password show/hide eye toggle */
                if (hitbox_test(&g_hit_eye, mx, my)) {
                    g_show_password = !g_show_password;
                }
            }
        }

        /* ── Render ──────────────────────────────────────────────── */
        cursor_erase();
        draw_login_screen(&screen);
        cursor_render();
        fb_flip();
        scheduler_yield();
    }
}
