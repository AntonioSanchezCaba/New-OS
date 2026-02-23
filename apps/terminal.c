/*
 * apps/terminal.c - Aether OS Graphical Terminal Emulator
 *
 * Full-featured terminal with:
 *   - 200-line scrollback history
 *   - Blinking block cursor
 *   - Complete built-in command set (help, clear, ls, cd, cat, ps, kill,
 *     echo, pwd, mem, uptime, date, uname, whoami, hostname, env, mkdir,
 *     rm, touch, head, wc)
 *   - ANSI-like color output per command
 *   - Scrolling with keyboard (PgUp/PgDn) [cosmetic]
 */
#include <gui/window.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <process.h>
#include <scheduler.h>
#include <stdarg.h>

#define TERM_W      680
#define TERM_H      420
#define TERM_PAD    4
#define TERM_COLS   ((TERM_W - TERM_PAD * 2) / FONT_W)    /* ~84 */
#define TERM_ROWS   ((TERM_H - FONT_H) / FONT_H)           /* ~25 visible */
#define TERM_HIST   300                                      /* Scroll-back lines */

#define TERM_BG        rgb(0x0D, 0x11, 0x17)
#define TERM_FG        rgb(0xCC, 0xE8, 0xCC)
#define TERM_CURSOR    rgb(0x00, 0xFF, 0x80)
#define TERM_PROMPT    rgb(0x40, 0xC8, 0xFF)
#define TERM_PROMPT2   rgb(0xFF, 0xAA, 0x00)   /* hostname color */
#define TERM_ERR       rgb(0xFF, 0x60, 0x60)
#define TERM_HDR       rgb(0x80, 0xD0, 0xFF)
#define TERM_DIR       rgb(0x60, 0xB0, 0xFF)
#define TERM_OK        rgb(0x60, 0xD0, 0x60)
#define TERM_DIM       rgb(0x50, 0x70, 0x60)
#define TERM_STATUSBAR rgb(0x10, 0x18, 0x22)
#define TERM_STATUS_FG rgb(0x60, 0x90, 0xA0)

typedef struct {
    wid_t     wid;
    char      cells[TERM_HIST][TERM_COLS + 1];
    uint32_t  colors[TERM_HIST];
    int       top_row;
    int       buf_row;
    int       cur_col;

    /* Input line */
    char      input[256];
    int       input_len;

    /* History navigation */
    char      hist[32][256];
    int       hist_count;
    int       hist_nav;   /* -1 = not navigating */

    /* Current working directory */
    char      cwd[256];

    /* Blink */
    uint32_t  last_blink;
    bool      cursor_visible;
} term_t;

static term_t* g_term = NULL;

/* =========================================================
 * Output helpers
 * ========================================================= */

static void term_newline(term_t* t)
{
    t->buf_row = (t->buf_row + 1) % TERM_HIST;
    t->cur_col = 0;
    memset(t->cells[t->buf_row], ' ', TERM_COLS);
    t->cells[t->buf_row][TERM_COLS] = '\0';
    t->colors[t->buf_row] = TERM_FG;

    int visible_start = (t->buf_row - TERM_ROWS + 2 + TERM_HIST) % TERM_HIST;
    t->top_row = visible_start;
}

static void term_put_char(term_t* t, char c, uint32_t color)
{
    if (c == '\n') { term_newline(t); return; }
    if (c == '\r') { t->cur_col = 0; return; }
    if (c == '\t') {
        int next = (t->cur_col + 8) & ~7;
        while (t->cur_col < next && t->cur_col < TERM_COLS)
            term_put_char(t, ' ', color);
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

static void term_printf(term_t* t, uint32_t color, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(t, buf, color);
}

/* =========================================================
 * Redraw
 * ========================================================= */

static void term_redraw(wid_t wid)
{
    term_t* t = g_term;
    if (!t || t->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    draw_rect(&c, 0, 0, c.width, c.height, TERM_BG);

    /* History rows */
    for (int row = 0; row < TERM_ROWS - 1; row++) {
        int idx = (t->top_row + row) % TERM_HIST;
        int y   = TERM_PAD + row * FONT_H;
        draw_string(&c, TERM_PAD, y, t->cells[idx], t->colors[idx], TERM_BG);
    }

    /* Input row at bottom */
    int iy = TERM_PAD + (TERM_ROWS - 1) * FONT_H;

    /* user@hostname:/cwd$ */
    const char* user = "user";
    const char* host = OS_HOSTNAME;
    int px = TERM_PAD;
    draw_string(&c, px, iy, user, TERM_OK, TERM_BG);
    px += (int)strlen(user) * FONT_W;
    draw_string(&c, px, iy, "@", TERM_FG, TERM_BG);
    px += FONT_W;
    draw_string(&c, px, iy, host, TERM_PROMPT2, TERM_BG);
    px += (int)strlen(host) * FONT_W;
    draw_string(&c, px, iy, ":", TERM_FG, TERM_BG);
    px += FONT_W;
    draw_string(&c, px, iy, t->cwd, TERM_DIR, TERM_BG);
    px += (int)strlen(t->cwd) * FONT_W;
    draw_string(&c, px, iy, "$ ", TERM_PROMPT, TERM_BG);
    px += 2 * FONT_W;

    draw_string(&c, px, iy, t->input, TERM_FG, TERM_BG);
    px += t->input_len * FONT_W;

    if (t->cursor_visible)
        draw_rect(&c, px, iy, FONT_W, FONT_H, TERM_CURSOR);

    /* Status bar */
    int sb_y = c.height - FONT_H - 2;
    draw_rect(&c, 0, sb_y, c.width, FONT_H + 2, TERM_STATUSBAR);
    draw_hline(&c, 0, sb_y, c.width, rgb(0x20, 0x30, 0x40));

    uint32_t secs  = timer_get_ticks() / TIMER_FREQ;
    uint32_t m2    = (secs / 60) % 60;
    uint32_t h2    = (secs / 3600) % 24;
    uint32_t s2    =  secs % 60;
    char time_str[12];
    time_str[0] = '0' + (char)(h2/10); time_str[1] = '0' + (char)(h2%10);
    time_str[2] = ':';
    time_str[3] = '0' + (char)(m2/10); time_str[4] = '0' + (char)(m2%10);
    time_str[5] = ':';
    time_str[6] = '0' + (char)(s2/10); time_str[7] = '0' + (char)(s2%10);
    time_str[8] = '\0';

    draw_string(&c, 4, sb_y + 1,
                OS_NAME " Terminal  |  Type 'help' for commands",
                TERM_STATUS_FG, rgba(0,0,0,0));
    draw_string(&c, c.width - 9 * FONT_W - 4, sb_y + 1,
                time_str, TERM_STATUS_FG, rgba(0,0,0,0));
}

/* =========================================================
 * Built-in commands
 * ========================================================= */

/* Tokenize input: split by spaces into argc/argv */
static int tokenize(char* line, char** argv, int max_args)
{
    int argc = 0;
    char* p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void cmd_help(term_t* t)
{
    term_puts(t, OS_NAME " Terminal — Built-in commands:\n", TERM_HDR);
    const char* cmds[] = {
        "  help          - Show this help",
        "  clear         - Clear screen",
        "  ls [path]     - List directory",
        "  cd [path]     - Change directory",
        "  pwd           - Print working directory",
        "  cat <file>    - Print file contents",
        "  touch <file>  - Create empty file",
        "  mkdir <dir>   - Create directory",
        "  rm <file>     - Remove file",
        "  echo [text]   - Print text",
        "  head <file>   - Print first 10 lines",
        "  wc <file>     - Count lines/words/chars",
        "  ps            - List processes",
        "  kill <pid>    - Kill a process",
        "  mem           - Memory statistics",
        "  uptime        - System uptime",
        "  date          - Current time",
        "  uname         - OS information",
        "  whoami        - Current user",
        "  hostname      - System hostname",
        "  env           - Environment variables",
        NULL
    };
    for (int i = 0; cmds[i]; i++) {
        term_puts(t, cmds[i], TERM_FG);
        term_puts(t, "\n", TERM_FG);
    }
}

static void cmd_clear(term_t* t)
{
    for (int i = 0; i < TERM_HIST; i++) {
        memset(t->cells[i], ' ', TERM_COLS);
        t->cells[i][TERM_COLS] = '\0';
        t->colors[i] = TERM_FG;
    }
    t->buf_row = 0; t->top_row = 0; t->cur_col = 0;
}

static void cmd_ls(term_t* t, const char* path)
{
    const char* target = (path && path[0]) ? path : t->cwd;
    vfs_node_t* dir = vfs_resolve_path(target);
    if (!dir || !(dir->flags & VFS_DIRECTORY)) {
        term_printf(t, TERM_ERR, "ls: cannot access '%s': No such directory\n", target);
        return;
    }
    if (!dir->ops || !dir->ops->readdir) {
        term_puts(t, "(empty directory)\n", TERM_DIM);
        return;
    }

    int col = 0;
    for (int i = 0; ; i++) {
        vfs_dirent_t* child = dir->ops->readdir(dir, i);
        if (!child) break;
        bool is_dir = (child->type == VFS_TYPE_DIR);
        uint32_t col_c = is_dir ? TERM_DIR : TERM_FG;
        char entry[32];
        strncpy(entry, child->name, 28);
        if (is_dir) strncat(entry, "/", 29);
        /* Pad to 20 chars */
        int l = (int)strlen(entry);
        while (l < 20) { entry[l++] = ' '; }
        entry[l] = '\0';
        term_puts(t, entry, col_c);
        col++;
        if (col >= 4) { term_puts(t, "\n", TERM_FG); col = 0; }
    }
    if (col > 0) term_puts(t, "\n", TERM_FG);
}

static void cmd_cd(term_t* t, const char* path)
{
    if (!path || !path[0]) { strncpy(t->cwd, "/", 255); return; }

    char new_path[256];
    if (path[0] == '/') {
        strncpy(new_path, path, 255);
    } else {
        strncpy(new_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0)
            strncat(new_path, "/", sizeof(new_path) - strlen(new_path) - 1);
        strncat(new_path, path, sizeof(new_path) - strlen(new_path) - 1);
    }

    /* Handle ".." */
    if (strcmp(path, "..") == 0) {
        char* slash = strrchr(new_path, '/');
        if (slash && slash != new_path) *slash = '\0';
        else strcpy(new_path, "/");
        strncpy(t->cwd, new_path, 255);
        return;
    }

    vfs_node_t* node = vfs_resolve_path(new_path);
    if (!node || !(node->flags & VFS_DIRECTORY)) {
        term_printf(t, TERM_ERR, "cd: no such directory: %s\n", new_path);
        return;
    }
    strncpy(t->cwd, new_path, 255);
    t->cwd[255] = '\0';
}

static void cmd_cat(term_t* t, const char* path)
{
    if (!path || !path[0]) {
        term_puts(t, "cat: missing filename\n", TERM_ERR);
        return;
    }

    char full_path[256];
    if (path[0] == '/') {
        strncpy(full_path, path, 255);
    } else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0)
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    }

    vfs_node_t* node = vfs_resolve_path(full_path);
    if (!node) {
        term_printf(t, TERM_ERR, "cat: %s: No such file\n", path);
        return;
    }
    if (node->flags & VFS_DIRECTORY) {
        term_printf(t, TERM_ERR, "cat: %s: Is a directory\n", path);
        return;
    }

    char buf[4096];
    if (!node->ops || !node->ops->read) {
        term_puts(t, "(unreadable)\n", TERM_DIM);
        return;
    }
    int n = node->ops->read(node, 0, sizeof(buf) - 1, buf);
    if (n < 0) n = 0;
    buf[n] = '\0';
    term_puts(t, buf, TERM_FG);
    if (n > 0 && buf[n-1] != '\n') term_puts(t, "\n", TERM_FG);
}

static void cmd_ps(term_t* t)
{
    term_printf(t, TERM_HDR, "%-6s %-20s %-10s\n", "PID", "NAME", "STATE");
    
    for (process_t* p = process_list; p; p = p->next) {
        const char* state = "?";
        switch (p->state) {
        case PROC_STATE_RUNNING:  state = "RUNNING";  break;
        case PROC_STATE_READY:    state = "READY";    break;
        case PROC_STATE_SLEEPING: state = "SLEEP";    break;
        case PROC_STATE_ZOMBIE:   state = "ZOMBIE";   break;
        case PROC_STATE_DEAD:     state = "DEAD";     break;
        default:                  state = "IDLE";     break;
        }
        uint32_t col = (p->state == PROC_STATE_RUNNING) ? TERM_OK  :
                       (p->state == PROC_STATE_ZOMBIE)  ? TERM_ERR : TERM_FG;
        term_printf(t, col, "%-6d %-20s %-10s\n",
                    (int)p->pid, p->name, state);
    }
}

static void cmd_mem(term_t* t)
{
    uint32_t free_frames = (uint32_t)pmm_free_frames_count();
    uint32_t total_kb    = 256 * 1024;
    uint32_t free_kb     = free_frames * 4;
    uint32_t used_kb     = (total_kb > free_kb) ? total_kb - free_kb : 0;

    term_puts(t, "Memory Statistics:\n", TERM_HDR);
    term_printf(t, TERM_FG,  "  Total:  %u MB\n", total_kb / 1024);
    term_printf(t, TERM_OK,  "  Free:   %u MB (%u KB)\n",
                free_kb / 1024, free_kb);
    term_printf(t, TERM_ERR, "  Used:   %u MB (%u KB)\n",
                used_kb / 1024, used_kb);
    term_printf(t, TERM_FG,  "  Usage:  %u%%\n", used_kb * 100 / total_kb);
    term_printf(t, TERM_DIM, "  Frames: %u free (%u total)\n",
                free_frames, (uint32_t)total_kb / 4);
}

static void cmd_uptime(term_t* t)
{
    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    uint32_t h2 = secs / 3600;
    uint32_t m2 = (secs / 60) % 60;
    uint32_t s2 = secs % 60;
    uint32_t days = h2 / 24; h2 %= 24;
    if (days > 0)
        term_printf(t, TERM_FG, "Uptime: %ud %uh %um %us\n", days, h2, m2, s2);
    else
        term_printf(t, TERM_FG, "Uptime: %uh %um %us\n", h2, m2, s2);
}

static void cmd_date(term_t* t)
{
    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    uint32_t h2 = (secs / 3600) % 24;
    uint32_t m2 = (secs / 60)   % 60;
    uint32_t s2 =  secs         % 60;
    term_printf(t, TERM_FG, "System time (since boot): %02u:%02u:%02u\n",
                h2, m2, s2);
    term_puts(t, "Date: " OS_YEAR " (RTC not yet implemented)\n", TERM_DIM);
}

static void cmd_uname(term_t* t)
{
    term_printf(t, TERM_FG,
                OS_NAME " " OS_VERSION " " OS_HOSTNAME " " OS_ARCH " " OS_KERNEL_TYPE "\n");
}

static void cmd_whoami(term_t* t)
{
    term_puts(t, "user\n", TERM_FG);
}

static void cmd_hostname(term_t* t)
{
    term_puts(t, OS_HOSTNAME "\n", TERM_FG);
}

static void cmd_env(term_t* t)
{
    term_puts(t, "HOME=/home/user\n", TERM_FG);
    term_puts(t, "USER=user\n", TERM_FG);
    term_puts(t, "HOSTNAME=" OS_HOSTNAME "\n", TERM_FG);
    term_printf(t, TERM_FG, "PWD=%s\n", t->cwd);
    term_puts(t, "SHELL=/bin/ash\n", TERM_FG);
    term_puts(t, "OS=" OS_NAME "\n", TERM_FG);
    term_puts(t, "TERM=" OS_TERM_NAME "\n", TERM_FG);
}

static void cmd_kill(term_t* t, const char* pid_s)
{
    if (!pid_s || !pid_s[0]) {
        term_puts(t, "kill: missing PID\n", TERM_ERR);
        return;
    }
    int pid = 0;
    for (const char* p = pid_s; *p; p++) {
        if (*p < '0' || *p > '9') { term_puts(t, "kill: invalid PID\n", TERM_ERR); return; }
        pid = pid * 10 + (*p - '0');
    }
    for (process_t* p = process_list; p; p = p->next) {
        if (p->pid == (pid_t)pid) {
            p->state = PROC_STATE_ZOMBIE;
            term_printf(t, TERM_OK, "Sent kill to PID %d (%s)\n", pid, p->name);
            return;
        }
    }
    term_printf(t, TERM_ERR, "kill: no process with PID %d\n", pid);
}

static void cmd_touch(term_t* t, const char* name)
{
    if (!name || !name[0]) { term_puts(t, "touch: missing filename\n", TERM_ERR); return; }
    char full_path[256];
    if (name[0] == '/') { strncpy(full_path, name, 255); }
    else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0) strncat(full_path, "/", 2);
        strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);
    }
    int _cr = vfs_create(full_path, 0);
    if (_cr == 0) term_printf(t, TERM_OK, "Created: %s\n", name);
    else          term_printf(t, TERM_ERR, "touch: cannot create '%s'\n", name);
}

static void cmd_mkdir(term_t* t, const char* name)
{
    if (!name || !name[0]) { term_puts(t, "mkdir: missing name\n", TERM_ERR); return; }
    char full_path[256];
    if (name[0] == '/') { strncpy(full_path, name, 255); }
    else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0) strncat(full_path, "/", 2);
        strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);
    }
    int _mr = vfs_mkdir(full_path);
    if (_mr == 0) term_printf(t, TERM_OK, "Directory created: %s\n", name);
    else          term_printf(t, TERM_ERR, "mkdir: cannot create '%s'\n", name);
}

static void cmd_rm(term_t* t, const char* name)
{
    if (!name || !name[0]) { term_puts(t, "rm: missing filename\n", TERM_ERR); return; }
    char full_path[256];
    if (name[0] == '/') { strncpy(full_path, name, 255); }
    else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0) strncat(full_path, "/", 2);
        strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);
    }
    int r = vfs_unlink(full_path);
    if (r == 0) term_printf(t, TERM_OK, "Removed: %s\n", name);
    else        term_printf(t, TERM_ERR, "rm: cannot remove '%s'\n", name);
}

static void cmd_head(term_t* t, const char* path)
{
    if (!path || !path[0]) { term_puts(t, "head: missing filename\n", TERM_ERR); return; }
    char full_path[256];
    if (path[0] == '/') strncpy(full_path, path, 255);
    else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0) strncat(full_path, "/", 2);
        strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    }
    vfs_node_t* node = vfs_resolve_path(full_path);
    if (!node || !node->ops || !node->ops->read) {
        term_printf(t, TERM_ERR, "head: %s: No such file\n", path);
        return;
    }
    char buf[4096];
    int n = node->ops->read(node, 0, sizeof(buf)-1, buf);
    if (n < 0) n = 0;
    buf[n] = '\0';
    int lines = 0;
    for (int i = 0; buf[i] && lines < 10; i++) {
        term_put_char(t, buf[i], TERM_FG);
        if (buf[i] == '\n') lines++;
    }
    if (n > 0 && buf[n-1] != '\n') term_puts(t, "\n", TERM_FG);
}

static void cmd_wc(term_t* t, const char* path)
{
    if (!path || !path[0]) { term_puts(t, "wc: missing filename\n", TERM_ERR); return; }
    char full_path[256];
    if (path[0] == '/') strncpy(full_path, path, 255);
    else {
        strncpy(full_path, t->cwd, 200);
        if (strcmp(t->cwd, "/") != 0) strncat(full_path, "/", 2);
        strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    }
    vfs_node_t* node = vfs_resolve_path(full_path);
    if (!node || !node->ops || !node->ops->read) {
        term_printf(t, TERM_ERR, "wc: %s: No such file\n", path);
        return;
    }
    char buf[4096];
    int n = node->ops->read(node, 0, sizeof(buf)-1, buf);
    if (n < 0) n = 0;
    buf[n] = '\0';

    int lines = 0, words = 0, chars = n;
    bool in_word = false;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') in_word = false;
        else if (!in_word) { in_word = true; words++; }
    }
    term_printf(t, TERM_FG, "%4d %4d %4d %s\n", lines, words, chars, path);
}

/* =========================================================
 * Command dispatch
 * ========================================================= */

static void term_exec(term_t* t, char* line)
{
    /* Add to history */
    if (strlen(line) > 0) {
        if (t->hist_count < 32) {
            strncpy(t->hist[t->hist_count++], line, 255);
        } else {
            memmove(t->hist[0], t->hist[1], 31 * 256);
            strncpy(t->hist[31], line, 255);
        }
        t->hist_nav = -1;
    }

    term_newline(t);
    if (strlen(line) == 0) return;

    char* argv[16];
    char line_copy[256];
    strncpy(line_copy, line, 255);
    line_copy[255] = '\0';
    int argc = tokenize(line_copy, argv, 16);
    if (argc == 0) return;

    const char* cmd = argv[0];

    if (strcmp(cmd, "help")     == 0) { cmd_help(t); return; }
    if (strcmp(cmd, "clear")    == 0) { cmd_clear(t); return; }
    if (strcmp(cmd, "pwd")      == 0) { term_printf(t, TERM_FG, "%s\n", t->cwd); return; }
    if (strcmp(cmd, "whoami")   == 0) { cmd_whoami(t); return; }
    if (strcmp(cmd, "hostname") == 0) { cmd_hostname(t); return; }
    if (strcmp(cmd, "uname")    == 0) { cmd_uname(t); return; }
    if (strcmp(cmd, "date")     == 0) { cmd_date(t); return; }
    if (strcmp(cmd, "uptime")   == 0) { cmd_uptime(t); return; }
    if (strcmp(cmd, "mem")      == 0) { cmd_mem(t); return; }
    if (strcmp(cmd, "ps")       == 0) { cmd_ps(t); return; }
    if (strcmp(cmd, "env")      == 0) { cmd_env(t); return; }

    if (strcmp(cmd, "ls")       == 0) { cmd_ls(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "cd")       == 0) { cmd_cd(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "cat")      == 0) { cmd_cat(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "head")     == 0) { cmd_head(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "wc")       == 0) { cmd_wc(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "touch")    == 0) { cmd_touch(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "mkdir")    == 0) { cmd_mkdir(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "rm")       == 0) { cmd_rm(t, argc > 1 ? argv[1] : NULL); return; }
    if (strcmp(cmd, "kill")     == 0) { cmd_kill(t, argc > 1 ? argv[1] : NULL); return; }

    if (strcmp(cmd, "echo")     == 0) {
        for (int i = 1; i < argc; i++) {
            term_puts(t, argv[i], TERM_FG);
            if (i < argc - 1) term_puts(t, " ", TERM_FG);
        }
        term_puts(t, "\n", TERM_FG);
        return;
    }

    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        wm_close(t->wid);
        return;
    }

    term_printf(t, TERM_ERR,
                "%s: command not found. Type 'help' for a list.\n", cmd);
}

/* =========================================================
 * Event handler
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

    case GUI_EVENT_KEY_DOWN: {
        uint8_t kc = evt->key.keycode;
        char ch    = evt->key.ch;

        if (kc == KEY_UP_ARROW) {
            /* History navigation */
            if (t->hist_count > 0) {
                if (t->hist_nav < 0) t->hist_nav = t->hist_count - 1;
                else if (t->hist_nav > 0) t->hist_nav--;
                strncpy(t->input, t->hist[t->hist_nav], 255);
                t->input_len = (int)strlen(t->input);
            }
        } else if (kc == KEY_DOWN_ARROW) {
            if (t->hist_nav >= 0) {
                t->hist_nav++;
                if (t->hist_nav >= t->hist_count) {
                    t->hist_nav = -1;
                    t->input[0] = '\0';
                    t->input_len = 0;
                } else {
                    strncpy(t->input, t->hist[t->hist_nav], 255);
                    t->input_len = (int)strlen(t->input);
                }
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            if (t->input_len < 254) {
                t->input[t->input_len++] = ch;
                t->input[t->input_len]   = '\0';
            }
            t->hist_nav = -1;
        } else if (kc == KEY_BACKSPACE || ch == '\b') {
            if (t->input_len > 0) t->input[--t->input_len] = '\0';
            t->hist_nav = -1;
        } else if (kc == KEY_ENTER || ch == '\n' || ch == '\r') {
            /* Echo input line */
            term_puts(t, "user@" OS_HOSTNAME ":", TERM_OK);
            term_puts(t, t->cwd, TERM_DIR);
            term_puts(t, "$ ", TERM_PROMPT);
            term_puts(t, t->input, TERM_FG);
            term_exec(t, t->input);
            t->input_len = 0;
            t->input[0]  = '\0';
            t->hist_nav  = -1;
        }
        term_redraw(wid);
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(t);
        g_term = NULL;
        break;

    default: break;
    }

    /* Cursor blink */
    uint32_t now = timer_get_ticks();
    if (now - t->last_blink >= (uint32_t)(TIMER_FREQ / 2)) {
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
    if (g_term) { wm_raise(g_term->wid); return g_term->wid; }

    term_t* t = (term_t*)kmalloc(sizeof(term_t));
    if (!t) return -1;
    memset(t, 0, sizeof(term_t));

    for (int i = 0; i < TERM_HIST; i++) {
        memset(t->cells[i], ' ', TERM_COLS);
        t->cells[i][TERM_COLS] = '\0';
        t->colors[i] = TERM_FG;
    }
    t->cursor_visible = true;
    t->last_blink     = timer_get_ticks();
    t->hist_nav       = -1;
    strncpy(t->cwd, "/", 2);

    wid_t wid = wm_create_window("Terminal — " OS_NAME,
                                  50, 40, TERM_W, TERM_H,
                                  term_on_event, NULL);
    if (wid < 0) { kfree(t); return -1; }

    t->wid  = wid;
    g_term  = t;

    /* Welcome banner */
    term_puts(t, OS_TERM_BANNER "\n", TERM_HDR);
    term_puts(t, OS_ARCH " — " OS_KERNEL_TYPE "\n", TERM_DIM);
    term_printf(t, TERM_FG,
                "Type 'help' to list built-in commands.\n\n");
    term_redraw(wid);
    return wid;
}
