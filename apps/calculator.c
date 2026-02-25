/*
 * apps/calculator.c — ARE Floating Calculator
 *
 * Four-function integer calculator rendered as a SURF_FLOAT draggable window.
 * Launched via surface_calculator_open(); singleton — re-focuses if already open.
 * Close via the window's X button in the ARE title bar.
 *
 * Uses draw_rect/draw_string directly with the same color vocabulary as
 * aether/ui.c buttons (CB_BTN_OP == ui_button default blue, etc.).
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <gui/event.h>
#include <memory.h>
#include <string.h>

/* =========================================================
 * Geometry (body only; ARE FLOAT_TITLE_H=26 is added above)
 * ========================================================= */
#define CALC_W        260
#define CALC_H        340
#define CALC_PAD        8
#define CALC_DISP_H    48
#define CALC_BTN_COLS   4
#define CALC_BTN_ROWS   5
#define CALC_BTN_PAD    4

/* Derived */
#define CALC_BTN_Y0   (CALC_PAD + CALC_DISP_H + CALC_BTN_PAD)
#define CALC_BTN_W    ((CALC_W - CALC_PAD*2 - CALC_BTN_PAD*(CALC_BTN_COLS-1)) / CALC_BTN_COLS)
#define CALC_BTN_H    ((CALC_H - CALC_BTN_Y0 - CALC_PAD - CALC_BTN_PAD*(CALC_BTN_ROWS-1)) / CALC_BTN_ROWS)

/* =========================================================
 * Colors (match aether/ui.c visual language)
 * ========================================================= */
#define CB_BG         ACOLOR(0x0E, 0x12, 0x1C, 0xFF)
#define CB_DISP_BG    ACOLOR(0x06, 0x0A, 0x12, 0xFF)
#define CB_DISP_FG    ACOLOR(0xE0, 0xF0, 0xFF, 0xFF)
#define CB_DISP_ERR   ACOLOR(0xFF, 0x60, 0x60, 0xFF)
#define CB_BTN        ACOLOR(0x28, 0x38, 0x58, 0xFF)   /* digit */
#define CB_BTN_HOV    ACOLOR(0x38, 0x52, 0x78, 0xFF)
#define CB_BTN_PRE    ACOLOR(0x18, 0x24, 0x40, 0xFF)
#define CB_BTN_OP     ACOLOR(0x30, 0x50, 0x90, 0xFF)   /* operator — ui_button blue */
#define CB_BTN_OP_H   ACOLOR(0x44, 0x68, 0xB4, 0xFF)
#define CB_BTN_EQ     ACOLOR(0x1C, 0x68, 0x38, 0xFF)   /* equals */
#define CB_BTN_EQ_H   ACOLOR(0x28, 0x88, 0x50, 0xFF)
#define CB_BTN_CLR    ACOLOR(0x70, 0x20, 0x20, 0xFF)   /* clear */
#define CB_BTN_CLR_H  ACOLOR(0x90, 0x30, 0x30, 0xFF)
#define CB_BTN_FG     ACOLOR(0xDD, 0xDD, 0xFF, 0xFF)
#define CB_BORDER     ACOLOR(0x30, 0x40, 0x60, 0xFF)

/* =========================================================
 * Button IDs
 * ========================================================= */
#define BID_0   0
#define BID_1   1
#define BID_2   2
#define BID_3   3
#define BID_4   4
#define BID_5   5
#define BID_6   6
#define BID_7   7
#define BID_8   8
#define BID_9   9
#define BID_DOT   10
#define BID_NEG   11
#define BID_PLUS  12
#define BID_MINUS 13
#define BID_MUL   14
#define BID_DIV   15
#define BID_EQ    16
#define BID_C     17
#define BID_CE    18
#define BID_PCT   19
#define BID_NONE  255

/* cat: 0=digit, 1=operator, 2=clear, 3=equals */
typedef struct { const char* lbl; uint8_t id; uint8_t cat; } btn_def_t;

static const btn_def_t btn_grid[CALC_BTN_ROWS][CALC_BTN_COLS] = {
    { {"CE",  BID_CE,  2}, {"C",   BID_C,    2}, {"+/-", BID_NEG, 1}, {"/", BID_DIV,   1} },
    { {"7",   BID_7,   0}, {"8",   BID_8,    0}, {"9",   BID_9,   0}, {"*", BID_MUL,   1} },
    { {"4",   BID_4,   0}, {"5",   BID_5,    0}, {"6",   BID_6,   0}, {"-", BID_MINUS, 1} },
    { {"1",   BID_1,   0}, {"2",   BID_2,    0}, {"3",   BID_3,   0}, {"+", BID_PLUS,  1} },
    { {"0",   BID_0,   0}, {".",   BID_DOT,  0}, {"%",   BID_PCT, 1}, {"=", BID_EQ,    3} },
};

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    long long op1, op2;
    int       pending_op;
    bool      entering_op2, has_decimal, need_reset, error;
    int       decimal_pos;
    char      display[32];
    int       hover_btn;    /* -1 = none */
    bool      btn_down;
    sid_t     sid;
    bool      open;
} calc_t;

static calc_t g_calc;

/* =========================================================
 * Integer → string (no FP, no SSE required)
 * ========================================================= */
static void fmt_ll(char* buf, int sz, long long v)
{
    bool neg = (v < 0);
    if (neg) v = -v;
    char tmp[24];
    int len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    else { while (v > 0 && len < 22) { tmp[len++] = (char)('0' + v % 10); v /= 10; } }
    int o = 0;
    if (neg && o < sz - 1) buf[o++] = '-';
    for (int i = len - 1; i >= 0 && o < sz - 1; i--) buf[o++] = tmp[i];
    buf[o] = '\0';
}

/* =========================================================
 * Calculator logic
 * ========================================================= */
static long long do_op(long long a, long long b, int op)
{
    if (op == BID_PLUS)  return a + b;
    if (op == BID_MINUS) return a - b;
    if (op == BID_MUL)   return a * b;
    if (op == BID_DIV)   return b ? a / b : 0;
    if (op == BID_PCT)   return b ? a % b : 0;
    return b;
}

static void calc_clear(calc_t* c)
{
    c->op1 = c->op2 = 0;
    c->pending_op = 0;
    c->entering_op2 = c->has_decimal = c->need_reset = c->error = false;
    c->decimal_pos = 0;
    strncpy(c->display, "0", sizeof(c->display) - 1);
}

static void calc_press(calc_t* c, uint8_t bid)
{
    if (bid == BID_C)  { calc_clear(c); return; }
    if (bid == BID_CE) {
        strncpy(c->display, "0", sizeof(c->display) - 1);
        if (c->entering_op2) c->op2 = 0; else c->op1 = 0;
        c->has_decimal = false; c->decimal_pos = 0; c->error = false;
        return;
    }
    if (c->error) return;

    /* Digits and decimal point */
    if ((bid >= BID_0 && bid <= BID_9) || bid == BID_DOT) {
        if (c->need_reset) {
            strncpy(c->display, "0", sizeof(c->display) - 1);
            c->has_decimal = false; c->decimal_pos = 0; c->need_reset = false;
        }
        if (bid == BID_DOT) {
            if (!c->has_decimal) {
                c->has_decimal = true;
                size_t dl = strlen(c->display);
                if (dl < sizeof(c->display) - 2) {
                    c->display[dl] = '.'; c->display[dl + 1] = '\0';
                }
            }
        } else {
            char digit = (char)('0' + bid);
            size_t dl = strlen(c->display);
            bool is_zero = (dl == 1 && c->display[0] == '0');
            if (is_zero && !c->has_decimal) c->display[0] = digit;
            else if (dl < sizeof(c->display) - 2) {
                c->display[dl] = digit; c->display[dl + 1] = '\0';
            }
        }
        return;
    }

    /* Negate */
    if (bid == BID_NEG) {
        if (c->display[0] == '-') {
            size_t dl = strlen(c->display);
            for (size_t i = 0; i < dl; i++) c->display[i] = c->display[i + 1];
        } else {
            size_t dl = strlen(c->display);
            if (dl < sizeof(c->display) - 2) {
                for (size_t i = dl + 1; i > 0; i--) c->display[i] = c->display[i - 1];
                c->display[0] = '-';
            }
        }
        return;
    }

    /* Binary operators */
    if (bid == BID_PLUS || bid == BID_MINUS || bid == BID_MUL || bid == BID_DIV) {
        bool neg = (c->display[0] == '-');
        const char* p = c->display + (neg ? 1 : 0);
        long long cur = 0;
        while (*p && *p != '.') { cur = cur * 10 + (*p - '0'); p++; }
        if (neg) cur = -cur;
        if (c->pending_op && c->entering_op2) {
            c->op2 = cur;
            if (c->pending_op == BID_DIV && c->op2 == 0) {
                strncpy(c->display, "Error", sizeof(c->display) - 1);
                c->error = true; return;
            }
            c->op1 = do_op(c->op1, c->op2, c->pending_op);
            fmt_ll(c->display, (int)sizeof(c->display), c->op1);
        } else {
            c->op1 = cur;
        }
        c->pending_op = bid; c->entering_op2 = true;
        c->need_reset = true; c->has_decimal = false; c->decimal_pos = 0;
        return;
    }

    /* Equals / percent */
    if (bid == BID_EQ || bid == BID_PCT) {
        if (!c->pending_op && bid == BID_EQ) return;
        bool neg = (c->display[0] == '-');
        const char* p = c->display + (neg ? 1 : 0);
        long long cur = 0;
        while (*p && *p != '.') { cur = cur * 10 + (*p - '0'); p++; }
        if (neg) cur = -cur;
        c->op2 = c->entering_op2 ? cur : c->op2;
        int op = (bid == BID_PCT) ? BID_PCT : c->pending_op;
        if (!c->pending_op) { op = BID_PCT; c->op1 = cur; c->op2 = 100; }
        if ((op == BID_DIV || op == BID_PCT) && c->op2 == 0) {
            strncpy(c->display, "Error", sizeof(c->display) - 1);
            c->error = true; c->pending_op = 0; return;
        }
        long long res = do_op(c->op1, c->op2, op);
        fmt_ll(c->display, (int)sizeof(c->display), res);
        c->op1 = res; c->entering_op2 = false;
        c->need_reset = true; c->has_decimal = false; c->decimal_pos = 0;
        if (bid == BID_EQ) c->pending_op = 0;
    }
}

/* =========================================================
 * Hit test: which button index is at (x,y)?
 * ========================================================= */
static int calc_btn_at(int x, int y)
{
    for (int r = 0; r < CALC_BTN_ROWS; r++) {
        int by = CALC_BTN_Y0 + r * (CALC_BTN_H + CALC_BTN_PAD);
        for (int col = 0; col < CALC_BTN_COLS; col++) {
            int bx = CALC_PAD + col * (CALC_BTN_W + CALC_BTN_PAD);
            if (x >= bx && x < bx + CALC_BTN_W &&
                y >= by && y < by + CALC_BTN_H)
                return r * CALC_BTN_COLS + col;
        }
    }
    return -1;
}

/* =========================================================
 * Render callback
 * ========================================================= */
static void calc_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                        void* ud)
{
    (void)id; (void)ud;
    calc_t* c = &g_calc;
    canvas_t cv = { .pixels = pixels, .width = w, .height = h };

    /* Background */
    draw_rect(&cv, 0, 0, CALC_W, CALC_H, CB_BG);

    /* Display area */
    draw_rect(&cv, CALC_PAD, CALC_PAD,
              CALC_W - CALC_PAD * 2, CALC_DISP_H, CB_DISP_BG);
    draw_rect_outline(&cv, CALC_PAD, CALC_PAD,
                      CALC_W - CALC_PAD * 2, CALC_DISP_H, 1, CB_BORDER);

    /* Display text — right-aligned */
    int tw = (int)strlen(c->display) * FONT_W;
    int tx = CALC_PAD + (CALC_W - CALC_PAD * 2) - tw - 8;
    if (tx < CALC_PAD + 4) tx = CALC_PAD + 4;
    int ty = CALC_PAD + (CALC_DISP_H - FONT_H) / 2;
    acolor_t dfg = c->error ? CB_DISP_ERR : CB_DISP_FG;
    draw_string(&cv, tx, ty, c->display, dfg, ACOLOR(0, 0, 0, 0));

    /* Pending operator indicator (dim, top-left of display) */
    if (c->pending_op) {
        const char* op_sym =
            c->pending_op == BID_PLUS  ? "+" :
            c->pending_op == BID_MINUS ? "-" :
            c->pending_op == BID_MUL   ? "*" :
            c->pending_op == BID_DIV   ? "/" : "";
        draw_string(&cv, CALC_PAD + 4, CALC_PAD + 4, op_sym,
                    ACOLOR(0x60, 0x80, 0xC0, 0xFF), ACOLOR(0, 0, 0, 0));
    }

    /* Button grid */
    for (int r = 0; r < CALC_BTN_ROWS; r++) {
        int by = CALC_BTN_Y0 + r * (CALC_BTN_H + CALC_BTN_PAD);
        for (int col = 0; col < CALC_BTN_COLS; col++) {
            int bx = CALC_PAD + col * (CALC_BTN_W + CALC_BTN_PAD);
            const btn_def_t* b = &btn_grid[r][col];
            int  idx = r * CALC_BTN_COLS + col;
            bool hov = (idx == c->hover_btn);
            bool pre = (hov && c->btn_down);

            acolor_t bg;
            if      (b->cat == 2) bg = pre ? ACOLOR(0x58,0x18,0x18,0xFF) : hov ? CB_BTN_CLR_H : CB_BTN_CLR;
            else if (b->cat == 3) bg = pre ? ACOLOR(0x14,0x50,0x28,0xFF) : hov ? CB_BTN_EQ_H  : CB_BTN_EQ;
            else if (b->cat == 1) bg = pre ? ACOLOR(0x20,0x38,0x70,0xFF) : hov ? CB_BTN_OP_H  : CB_BTN_OP;
            else                  bg = pre ? CB_BTN_PRE                  : hov ? CB_BTN_HOV   : CB_BTN;

            draw_rect(&cv, bx, by, CALC_BTN_W, CALC_BTN_H, bg);
            draw_rect_outline(&cv, bx, by, CALC_BTN_W, CALC_BTN_H, 1, CB_BORDER);

            int ltx = bx + (CALC_BTN_W - (int)strlen(b->lbl) * FONT_W) / 2;
            int lty = by + (CALC_BTN_H - FONT_H) / 2;
            draw_string(&cv, ltx, lty, b->lbl, CB_BTN_FG, ACOLOR(0, 0, 0, 0));
        }
    }
}

/* =========================================================
 * Input callback
 * ========================================================= */
static void calc_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    calc_t* c = &g_calc;

    if (ev->type == INPUT_POINTER) {
        c->hover_btn = calc_btn_at(ev->pointer.x, ev->pointer.y);
        bool down   = (ev->pointer.buttons & IBTN_LEFT) != 0;
        bool click  = down && !(ev->pointer.prev_buttons & IBTN_LEFT);
        c->btn_down = down && (c->hover_btn >= 0);
        if (click && c->hover_btn >= 0) {
            int r   = c->hover_btn / CALC_BTN_COLS;
            int col = c->hover_btn % CALC_BTN_COLS;
            calc_press(c, btn_grid[r][col].id);
        }
        surface_invalidate(id);
    }

    if (ev->type == INPUT_KEY && ev->key.down) {
        char ch = ev->key.ch;
        int  kc = ev->key.keycode;
        if (ch >= '0' && ch <= '9')             calc_press(c, (uint8_t)(ch - '0'));
        else if (ch == '+')                      calc_press(c, BID_PLUS);
        else if (ch == '-')                      calc_press(c, BID_MINUS);
        else if (ch == '*')                      calc_press(c, BID_MUL);
        else if (ch == '/')                      calc_press(c, BID_DIV);
        else if (ch == '%')                      calc_press(c, BID_PCT);
        else if (ch == '=' || kc == KEY_ENTER)   calc_press(c, BID_EQ);
        else if (ch == '.')                      calc_press(c, BID_DOT);
        else if (kc == KEY_BACKSPACE)            calc_press(c, BID_CE);
        else if (kc == KEY_ESCAPE)               calc_press(c, BID_C);
        surface_invalidate(id);
    }
}

/* =========================================================
 * Close callback (called by ARE when X button is clicked)
 * ========================================================= */
static void calc_on_close(sid_t id, void* ud)
{
    (void)id; (void)ud;
    g_calc.open = false;
    g_calc.sid  = SID_NONE;
}

/* =========================================================
 * Public: open calculator as a floating ARE window
 * ========================================================= */
sid_t surface_calculator_open(void)
{
    if (g_calc.open && g_calc.sid != SID_NONE)
        return g_calc.sid;   /* already open — bring to front */

    calc_clear(&g_calc);
    g_calc.hover_btn = -1;
    g_calc.btn_down  = false;
    g_calc.open      = true;

    g_calc.sid = are_add_surface(SURF_FLOAT, CALC_W, CALC_H,
                                  "Calculator", "C",
                                  calc_render, calc_input,
                                  calc_on_close, NULL);
    return g_calc.sid;
}

/* Legacy GUI API stubs — never called when ARE is active */
void gui_launch_calculator(void) { surface_calculator_open(); }
