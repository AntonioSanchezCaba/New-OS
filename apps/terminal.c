/*
 * apps/terminal.c - Graphical terminal emulator window
 *
 * A simple VT-style terminal that reads from a pipe/keyboard
 * and renders text output in a scrollable canvas.
 * Commands are run inline (no forking to shell process yet —
 * we execute built-in commands directly from the GUI thread).
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <drivers/framebuffer.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <drivers/timer.h>

#define TERM_W      640
#define TERM_H      400
#define TERM_COLS   (TERM_W / FONT_W)    /* ~80 */
#define TERM_ROWS   (TERM_H / FONT_H)    /* ~25 */
#define TERM_HIST   200                  /* Scroll-back lines */

#define TERM_BG     rgb(0x0C, 0x0C, 0x0C)
#define TERM_FG     rgb(0xCC, 0xFF, 0xCC)
#define TERM_CURSOR rgb(0x00, 0xFF, 0x40)
#define TERM_PROMPT rgb(0x40, 0xFF, 0xFF)

typedef struct {
    wid_t wid;

    /* Text cell grid */
    char   cells[TERM_HIST][TERM_COLS + 1]; /* null-terminated rows */
    uint32_t colors[TERM_HIST];              /* per-row fg color */
    int    top_row;          /* Index of first displayed row */
    int    buf_row;          /* Next row to write into (history) */

    /* Current cursor position (within the visible area) */
    int    cur_col;

    /* Input line buffer */
    char   input[256];
    int    input_len;

    /* Blink state */
    uint32_t last_blink;
    bool   cursor_visible;
} term_t;

static term_t* g_term = NULL;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static void term_newline(term_t* t)
{
    t->buf_row = (t->buf_row + 1) % TERM_HIST;
    t->cur_col = 0;
    /* Clear new row */
    memset(t->cells[t->buf_row], ' ', TERM_COLS);
    t->cells[t->buf_row][TERM_COLS] = '\0';
    t->colors[t->buf_row] = TERM_FG;

    /* Scroll view if needed */
    int visible_start = (t->buf_row - TERM_ROWS + 1 + TERM_HIST) % TERM_HIST;
    t->top_row = visible_start;
}

static void term_put_char(term_t* t, char c, uint32_t color)
{
    if (c == '\n') {
        term_newline(t);
        return;
    }
    if (c == '\r') {
        t->cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (t->cur_col > 0) {
            t->cur_col--;
            t->cells[t->buf_row][t->cur_col] = ' ';
        }
        return;
    }
    if (t->cur_col >= TERM_COLS) term_newline(t);

    t->cells[t->buf_row][t->cur_col] = c;
    t->colors[t->buf_row] = color;
    t->cur_col++;
}

static void term_puts(term_t* t, const char* s, uint32_t color)
{
    while (*s) term_put_char(t, *s++, color);
}

static void term_redraw(wid_t wid)
{
    term_t* t = g_term;
    if (!t || t->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    draw_rect(&c, 0, 0, c.width, c.height, TERM_BG);

    /* Draw text rows */
    for (int row = 0; row < TERM_ROWS; row++) {
        int hist_idx = (t->top_row + row) % TERM_HIST;
        int y = row * FONT_H;
        draw_string(&c, 0, y, t->cells[hist_idx],
                    t->colors[hist_idx], TERM_BG);
    }

    /* Draw input prompt on last row */
    int prompt_row = TERM_ROWS - 1;
    int py = prompt_row * FONT_H;

    /* Prompt */
    const char* prompt = "$ ";
    draw_string(&c, 0, py, prompt, TERM_PROMPT, TERM_BG);
    int px = (int)strlen(prompt) * FONT_W;

    /* Input text */
    draw_string(&c, px, py, t->input, TERM_FG, TERM_BG);
    px += t->input_len * FONT_W;

    /* Blinking cursor */
    if (t->cursor_visible) {
        draw_rect(&c, px, py, FONT_W, FONT_H, TERM_CURSOR);
    }
}

/* Execute a simple built-in command */
static void term_exec(term_t* t, const char* line)
{
    term_newline(t);

    if (strlen(line) == 0) return;

    if (strcmp(line, "clear") == 0) {
        for (int i = 0; i < TERM_HIST; i++) {
            memset(t->cells[i], ' ', TERM_COLS);
            t->cells[i][TERM_COLS] = '\0';
            t->colors[i] = TERM_FG;
        }
        t->buf_row = 0;
        t->top_row = 0;
        t->cur_col = 0;
        return;
    }
    if (strcmp(line, "help") == 0) {
        term_puts(t, "Built-in commands: help, clear, ls, uptime, mem\n", TERM_FG);
        return;
    }
    if (strcmp(line, "uptime") == 0) {
        uint32_t secs = timer_get_ticks() / TIMER_FREQ;
        char buf[64];
        uint32_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
        /* Manual snprintf */
        buf[0] = '0' + (h/10); buf[1] = '0' + (h%10);
        buf[2] = ':';
        buf[3] = '0' + (m/10); buf[4] = '0' + (m%10);
        buf[5] = ':';
        buf[6] = '0' + (s/10); buf[7] = '0' + (s%10);
        buf[8] = '\n'; buf[9] = '\0';
        term_puts(t, "Uptime: ", TERM_FG);
        term_puts(t, buf, TERM_FG);
        return;
    }
    if (strcmp(line, "ls") == 0) {
        vfs_node_t* dir = vfs_resolve_path("/");
        if (dir && dir->ops && dir->ops->readdir) {
            for (int i = 0; ; i++) {
                vfs_node_t* child = dir->ops->readdir(dir, i);
                if (!child) break;
                term_puts(t, child->name, COLOR_CYAN);
                term_puts(t, "  ", TERM_FG);
            }
            term_puts(t, "\n", TERM_FG);
        }
        return;
    }
    if (strncmp(line, "echo ", 5) == 0) {
        term_puts(t, line + 5, TERM_FG);
        term_puts(t, "\n", TERM_FG);
        return;
    }
    if (strcmp(line, "mem") == 0) {
        uint32_t free_kb = (uint32_t)(pmm_free_frames_count() * PAGE_SIZE / 1024);
        char buf[64];
        /* Write "Free memory: XXXXXX KB\n" */
        uint32_t n = free_kb;
        int pos = 20;
        buf[pos] = '\n'; buf[pos+1] = '\0';
        if (n == 0) { buf[--pos] = '0'; } else {
            while (n > 0) { buf[--pos] = '0' + (n % 10); n /= 10; }
        }
        memcpy(buf, "Free memory: ", 13);
        for (int i = 13; i < pos; i++) buf[i] = ' ';
        buf[13] = '\0';
        term_puts(t, "Free memory: ", TERM_FG);
        term_puts(t, buf + pos, COLOR_GREEN);
        term_puts(t, " KB\n", TERM_FG);
        return;
    }

    /* Unknown */
    term_puts(t, "Unknown command: ", COLOR_RED);
    term_puts(t, line, COLOR_RED);
    term_puts(t, "\n", TERM_FG);
}

/* =========================================================
 * Window event handler
 * ========================================================= */

static void term_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    term_t* t = g_term;
    if (!t || t->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT:
        term_redraw(wid);
        break;

    case GUI_EVENT_KEY_DOWN:
        if (evt->key.ch >= 0x20 && evt->key.ch < 0x7F) {
            if (t->input_len < 255) {
                t->input[t->input_len++] = evt->key.ch;
                t->input[t->input_len]   = '\0';
            }
        } else if (evt->key.ch == '\b' || evt->key.keycode == KEY_BACKSPACE) {
            if (t->input_len > 0)
                t->input[--t->input_len] = '\0';
        } else if (evt->key.ch == '\n' || evt->key.ch == '\r'
                   || evt->key.keycode == KEY_ENTER) {
            /* Echo input to history */
            term_puts(t, "$ ", TERM_PROMPT);
            term_puts(t, t->input, TERM_FG);
            term_exec(t, t->input);
            t->input_len = 0;
            t->input[0]  = '\0';
        }
        term_redraw(wid);
        break;

    case GUI_EVENT_CLOSE:
        kfree(t);
        g_term = NULL;
        break;

    default:
        break;
    }

    /* Cursor blink */
    uint32_t now = timer_get_ticks();
    if (now - t->last_blink >= TIMER_FREQ / 2) {
        t->cursor_visible = !t->cursor_visible;
        t->last_blink = now;
        term_redraw(wid);
    }
}

/* =========================================================
 * Public: create terminal window
 * ========================================================= */

wid_t app_terminal_create(void)
{
    if (g_term) return g_term->wid;  /* Only one terminal for now */

    term_t* t = (term_t*)kmalloc(sizeof(term_t));
    if (!t) return -1;
    memset(t, 0, sizeof(term_t));

    /* Initialize history buffer */
    for (int i = 0; i < TERM_HIST; i++) {
        memset(t->cells[i], ' ', TERM_COLS);
        t->cells[i][TERM_COLS] = '\0';
        t->colors[i] = TERM_FG;
    }
    t->cursor_visible = true;
    t->last_blink = timer_get_ticks();

    wid_t wid = wm_create_window("Terminal",
                                  60, 60, TERM_W, TERM_H,
                                  term_on_event, NULL);
    if (wid < 0) { kfree(t); return -1; }

    t->wid = wid;
    g_term = t;

    /* Welcome banner */
    term_puts(t, "NovOS Terminal v1.0\n", TERM_PROMPT);
    term_puts(t, "Type 'help' for commands\n\n", TERM_FG);
    term_redraw(wid);

    return wid;
}
