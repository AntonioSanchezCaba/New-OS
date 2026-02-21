/*
 * apps/calculator.c - Aether OS Graphical Calculator
 *
 * A four-function calculator with a 4x5 button grid.
 * Operations: 0-9, +, -, *, /, =, C (clear), CE (clear entry),
 *             +/- (negate), . (decimal point), % (modulo).
 *
 * The calculator maintains a simple operator-value state machine:
 *   operand1 [op] operand2 => result
 */
#include <gui/gui.h>
#include <gui/widgets.h>
#include <drivers/timer.h>
#include <string.h>
#include <memory.h>

/* Window geometry */
#define CALC_W   280
#define CALC_H   370

/* Display area */
#define DISP_X     8
#define DISP_Y     8
#define DISP_W   (CALC_W - 16)
#define DISP_H    52

/* Button grid */
#define BTN_COLS   4
#define BTN_ROWS   5
#define BTN_PAD    5
#define BTN_X      8
#define BTN_Y    (DISP_Y + DISP_H + BTN_PAD)
#define BTN_W   ((CALC_W - BTN_X * 2 - (BTN_COLS - 1) * BTN_PAD) / BTN_COLS)
#define BTN_H    ((CALC_H - BTN_Y - BTN_PAD - (BTN_ROWS - 1) * BTN_PAD) / BTN_ROWS)

/* Button IDs */
#define BID_0     0
#define BID_1     1
#define BID_2     2
#define BID_3     3
#define BID_4     4
#define BID_5     5
#define BID_6     6
#define BID_7     7
#define BID_8     8
#define BID_9     9
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

/* Calculator state */
typedef struct {
    wid_t          wid;
    widget_group_t wg;

    double operand1;
    double operand2;
    int    pending_op;   /* BID_PLUS/MINUS/MUL/DIV, or 0 */
    bool   entering_op2; /* True when user started entering second operand */
    bool   has_decimal;  /* Decimal point pressed */
    int    decimal_pos;  /* Position after decimal (1 = first digit after .) */
    bool   need_reset;   /* Next digit press clears display */
    bool   error;

    char   display[32];
} calc_t;

static calc_t g_calc;

/* =========================================================
 * Simple double -> string (no libc)
 * ========================================================= */

static void fmt_double(char* buf, int bufsz, double v)
{
    if (bufsz <= 0) return;

    /* Handle sign */
    bool neg = (v < 0.0);
    if (neg) v = -v;

    /* Separate integer and fractional parts */
    long long ipart = (long long)v;
    double    fpart = v - (double)ipart;

    /* Format integer part */
    char ibuf[24];
    int  ilen = 0;
    if (ipart == 0) {
        ibuf[ilen++] = '0';
    } else {
        long long tmp = ipart;
        while (tmp > 0 && ilen < 20) {
            ibuf[ilen++] = '0' + (char)(tmp % 10);
            tmp /= 10;
        }
        /* Reverse */
        for (int i = 0; i < ilen / 2; i++) {
            char c = ibuf[i];
            ibuf[i] = ibuf[ilen - 1 - i];
            ibuf[ilen - 1 - i] = c;
        }
    }
    ibuf[ilen] = '\0';

    /* Format fractional part (up to 8 sig digits) */
    char fbuf[12];
    int  flen = 0;
    fpart += 0.000000005; /* Round */
    for (int i = 0; i < 8 && fpart > 0.0000000001; i++) {
        fpart *= 10.0;
        int digit = (int)fpart;
        fbuf[flen++] = '0' + (char)digit;
        fpart -= digit;
    }
    /* Strip trailing zeros */
    while (flen > 0 && fbuf[flen - 1] == '0') flen--;
    fbuf[flen] = '\0';

    int out = 0;
    if (neg && out < bufsz - 1) buf[out++] = '-';
    for (int i = 0; i < ilen && out < bufsz - 1; i++) buf[out++] = ibuf[i];
    if (flen > 0 && out < bufsz - 1) {
        buf[out++] = '.';
        for (int i = 0; i < flen && out < bufsz - 1; i++) buf[out++] = fbuf[i];
    }
    buf[out] = '\0';
}

/* =========================================================
 * Calculator logic
 * ========================================================= */

static double apply_op(double a, double b, int op)
{
    if (op == BID_PLUS)  return a + b;
    if (op == BID_MINUS) return a - b;
    if (op == BID_MUL)   return a * b;
    if (op == BID_DIV) {
        if (b == 0.0) return 0.0; /* Division by zero -> 0 (error set by caller) */
        return a / b;
    }
    if (op == BID_PCT)   return (double)((long long)a % (long long)b);
    return b;
}

static void calc_reset(calc_t* c)
{
    c->operand1    = 0.0;
    c->operand2    = 0.0;
    c->pending_op  = 0;
    c->entering_op2= false;
    c->has_decimal = false;
    c->decimal_pos = 0;
    c->need_reset  = false;
    c->error       = false;
    strncpy(c->display, "0", sizeof(c->display) - 1);
}

static void calc_press(calc_t* c, uint32_t bid)
{
    if (bid == BID_C) {
        calc_reset(c);
        return;
    }
    if (bid == BID_CE) {
        strncpy(c->display, "0", sizeof(c->display) - 1);
        if (c->entering_op2) { c->operand2 = 0.0; }
        else                 { c->operand1 = 0.0; }
        c->has_decimal = false;
        c->decimal_pos = 0;
        c->error       = false;
        return;
    }

    if (c->error) return;

    /* Digits and decimal */
    if ((bid >= BID_0 && bid <= BID_9) || bid == BID_DOT) {
        if (c->need_reset) {
            /* Start fresh after = or operator */
            strncpy(c->display, "0", sizeof(c->display) - 1);
            c->has_decimal = false;
            c->decimal_pos = 0;
            c->need_reset  = false;
        }
        if (bid == BID_DOT) {
            if (!c->has_decimal) {
                c->has_decimal = true;
                c->decimal_pos = 1;
                size_t dl = strlen(c->display);
                if (dl < sizeof(c->display) - 2) {
                    c->display[dl]     = '.';
                    c->display[dl + 1] = '\0';
                }
            }
        } else {
            char digit = '0' + (char)bid;
            size_t dl = strlen(c->display);
            bool is_zero = (dl == 1 && c->display[0] == '0');
            /* Replace leading zero with digit */
            if (is_zero && !c->has_decimal) {
                c->display[0] = digit;
            } else if (dl < sizeof(c->display) - 2) {
                c->display[dl]     = digit;
                c->display[dl + 1] = '\0';
            }
        }
        return;
    }

    /* Negate */
    if (bid == BID_NEG) {
        if (c->display[0] == '-') {
            /* Remove leading minus */
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

    /* Operators: +, -, *, / */
    if (bid == BID_PLUS || bid == BID_MINUS ||
        bid == BID_MUL  || bid == BID_DIV)
    {
        /* Parse current display into operand */
        double cur = 0.0;
        bool neg2 = (c->display[0] == '-');
        const char* p = c->display + (neg2 ? 1 : 0);
        while (*p && *p != '.') { cur = cur * 10.0 + (*p - '0'); p++; }
        if (*p == '.') {
            p++;
            double frac = 0.1;
            while (*p) { cur += (*p - '0') * frac; frac *= 0.1; p++; }
        }
        if (neg2) cur = -cur;

        if (c->pending_op && c->entering_op2) {
            /* Chain: eval previous op */
            c->operand2 = cur;
            if (c->pending_op == BID_DIV && c->operand2 == 0.0) {
                strncpy(c->display, "Error", sizeof(c->display) - 1);
                c->error = true;
                return;
            }
            c->operand1 = apply_op(c->operand1, c->operand2, c->pending_op);
            fmt_double(c->display, (int)sizeof(c->display), c->operand1);
        } else {
            c->operand1 = cur;
        }

        c->pending_op   = (int)bid;
        c->entering_op2 = true;
        c->need_reset   = true;
        c->has_decimal  = false;
        c->decimal_pos  = 0;
        return;
    }

    /* Equals */
    if (bid == BID_EQ) {
        if (!c->pending_op) return;

        /* Parse second operand */
        double cur = 0.0;
        bool neg2 = (c->display[0] == '-');
        const char* p = c->display + (neg2 ? 1 : 0);
        while (*p && *p != '.') { cur = cur * 10.0 + (*p - '0'); p++; }
        if (*p == '.') {
            p++;
            double frac = 0.1;
            while (*p) { cur += (*p - '0') * frac; frac *= 0.1; p++; }
        }
        if (neg2) cur = -cur;

        c->operand2 = c->entering_op2 ? cur : c->operand2;

        if (c->pending_op == BID_DIV && c->operand2 == 0.0) {
            strncpy(c->display, "Error", sizeof(c->display) - 1);
            c->error      = true;
            c->pending_op = 0;
            return;
        }

        double result = apply_op(c->operand1, c->operand2, c->pending_op);
        fmt_double(c->display, (int)sizeof(c->display), result);
        c->operand1    = result;
        c->entering_op2 = false;
        c->need_reset  = true;
        c->has_decimal = false;
        c->decimal_pos = 0;
        /* Keep pending_op so repeated = applies same op */
        return;
    }
}

/* =========================================================
 * Rendering
 * ========================================================= */

static void calc_draw(wid_t wid)
{
    calc_t* c = &g_calc;
    canvas_t cv = wm_client_canvas(wid);
    const theme_t* th = theme_current();

    /* Background */
    draw_rect(&cv, 0, 0, CALC_W, CALC_H, th->win_bg);

    /* Display area */
    draw_rect(&cv, DISP_X, DISP_Y, DISP_W, DISP_H, th->panel_bg);
    draw_rect_outline(&cv, DISP_X, DISP_Y, DISP_W, DISP_H, 1, th->win_border);

    /* Display text (right-aligned) */
    int tw = (int)strlen(c->display) * FONT_W;
    int tx = DISP_X + DISP_W - tw - 8;
    if (tx < DISP_X + 4) tx = DISP_X + 4;
    int ty = DISP_Y + (DISP_H - FONT_H) / 2;
    uint32_t disp_fg = c->error ? th->error : th->text_primary;
    draw_string(&cv, tx, ty, c->display, disp_fg, rgba(0, 0, 0, 0));

    /* Draw widgets (buttons) */
    widget_group_draw(&cv, &c->wg);
}

/* =========================================================
 * Button layout (4 columns × 5 rows)
 *
 *  Row 0:  CE   C   +/-   /
 *  Row 1:   7   8     9   *
 *  Row 2:   4   5     6   -
 *  Row 3:   1   2     3   +
 *  Row 4:   0   .     %   =
 * ========================================================= */

typedef struct { const char* label; uint32_t id; } btn_def_t;

static const btn_def_t btn_layout[BTN_ROWS][BTN_COLS] = {
    { {"CE", BID_CE}, {"C", BID_C}, {"+/-", BID_NEG}, {"/", BID_DIV} },
    { {"7",  BID_7},  {"8", BID_8}, {"9",   BID_9},   {"*", BID_MUL} },
    { {"4",  BID_4},  {"5", BID_5}, {"6",   BID_6},   {"-", BID_MINUS} },
    { {"1",  BID_1},  {"2", BID_2}, {"3",   BID_3},   {"+", BID_PLUS} },
    { {"0",  BID_0},  {".", BID_DOT},  {"%", BID_PCT},  {"=", BID_EQ} },
};

static void calc_build_buttons(calc_t* c)
{
    widget_group_init(&c->wg);
    for (int row = 0; row < BTN_ROWS; row++) {
        for (int col = 0; col < BTN_COLS; col++) {
            int bx = BTN_X + col * (BTN_W + BTN_PAD);
            int by = BTN_Y + row * (BTN_H + BTN_PAD);
            widget_add_button(&c->wg, bx, by, BTN_W, BTN_H,
                              btn_layout[row][col].label,
                              btn_layout[row][col].id);
        }
    }
}

/* =========================================================
 * Window event handler
 * ========================================================= */

static void calc_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    calc_t* c = &g_calc;

    if (evt->type == GUI_EVENT_PAINT) {
        calc_draw(wid);
        return;
    }

    if (evt->type == GUI_EVENT_CLOSE) {
        c->wid = -1;
        return;
    }

    uint32_t triggered = 0;
    if (widget_group_handle_event(&c->wg, evt, &triggered)) {
        calc_press(c, triggered);
        wm_invalidate(wid);
        return;
    }

    if (evt->type == GUI_EVENT_KEY_DOWN) {
        int kc = evt->key.keycode;
        char ch = evt->key.ch;
        if (ch >= '0' && ch <= '9') { calc_press(c, (uint32_t)(ch - '0')); wm_invalidate(wid); }
        else if (ch == '+') { calc_press(c, BID_PLUS);  wm_invalidate(wid); }
        else if (ch == '-') { calc_press(c, BID_MINUS); wm_invalidate(wid); }
        else if (ch == '*') { calc_press(c, BID_MUL);   wm_invalidate(wid); }
        else if (ch == '/') { calc_press(c, BID_DIV);   wm_invalidate(wid); }
        else if (ch == '=') { calc_press(c, BID_EQ);    wm_invalidate(wid); }
        else if (ch == '.') { calc_press(c, BID_DOT);   wm_invalidate(wid); }
        else if (kc == KEY_ENTER) { calc_press(c, BID_EQ); wm_invalidate(wid); }
        else if (kc == KEY_BACKSPACE || kc == KEY_ESCAPE) {
            calc_press(c, BID_C); wm_invalidate(wid);
        }
    }
}

/* =========================================================
 * Public launch function
 * ========================================================= */

void gui_launch_calculator(void)
{
    calc_t* c = &g_calc;
    if (c->wid > 0 && wm_get_window(c->wid)) {
        wm_raise(c->wid);
        return;
    }
    calc_reset(c);
    calc_build_buttons(c);

    c->wid = wm_create_window("Calculator", 300, 150, CALC_W, CALC_H,
                               calc_on_event, NULL);
    if (c->wid > 0) {
        taskbar_add(c->wid, "Calculator");
    }
}
