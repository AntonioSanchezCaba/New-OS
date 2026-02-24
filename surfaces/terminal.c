/*
 * surfaces/terminal.c — Aether Terminal Surface
 *
 * 80×24 text grid, VT-100-like escape handling.
 * Renders via draw_* into the ARE surface pixel buffer.
 * Input: keystrokes forwarded by ARE dispatcher.
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <aether/ui.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <gui/event.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <kernel/version.h>
#include <kernel/users.h>
#include <kernel/apkg.h>
#include <process.h>
#include <scheduler.h>
#include <drivers/timer.h>
#include <net/ip.h>
#include <net/net.h>
#include <net/dns.h>

/* =========================================================
 * Terminal geometry
 * ========================================================= */
#define TERM_COLS      80
#define TERM_ROWS      24
#define TERM_TITLE_H   32
#define TERM_PAD       8
#define TERM_TAB_W     8
#define TERM_HISTORY   200   /* scrollback rows */

/* Colours (ARGB32) */
#define TC_BG          ACOLOR(0x0D, 0x0D, 0x0D, 0xFF)
#define TC_FG          ACOLOR(0xD4, 0xD4, 0xD4, 0xFF)
#define TC_TITLE_BG    ACOLOR(0x14, 0x14, 0x14, 0xFF)
#define TC_TITLE_FG    ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define TC_CURSOR      ACOLOR(0x58, 0xA6, 0xFF, 0xFF)
#define TC_SEL         ACOLOR(0x26, 0x4F, 0x78, 0xFF)

/* =========================================================
 * Cell / state
 * ========================================================= */
typedef struct {
    char     ch;
    acolor_t fg;
    acolor_t bg;
} term_cell_t;

typedef struct {
    /* Ring buffer for scrollback */
    term_cell_t  buf[TERM_HISTORY][TERM_COLS];
    int          head;        /* top visible row index */
    int          write_row;   /* current write row */
    int          cx, cy;      /* cursor (col, row) within visible window */

    /* Scroll offset (0 = bottom-most) */
    int          scroll_off;

    /* Input line buffer */
    char         line_buf[256];
    int          line_len;
    int          line_cursor;

    /* Current colors */
    acolor_t     cur_fg;
    acolor_t     cur_bg;

    /* Blink counter */
    uint32_t     blink_frame;

    /* Current working directory */
    char         cwd[VFS_NAME_MAX + 1];

    /* Surface dimensions */
    uint32_t     surf_w;
    uint32_t     surf_h;

    sid_t        sid;
} term_state_t;

static term_state_t g_term;

/* =========================================================
 * Helpers
 * ========================================================= */
static void term_clear_row(int row)
{
    int real = (g_term.head + row) % TERM_HISTORY;
    for (int c = 0; c < TERM_COLS; c++) {
        g_term.buf[real][c].ch = ' ';
        g_term.buf[real][c].fg = g_term.cur_fg;
        g_term.buf[real][c].bg = g_term.cur_bg;
    }
}

static void term_scroll_up(void)
{
    g_term.write_row++;
    if (g_term.write_row >= TERM_HISTORY)
        g_term.write_row = 0;
    /* The newly freed row becomes the head */
    g_term.head = (g_term.head + 1) % TERM_HISTORY;
    /* Clear new bottom row */
    term_clear_row(TERM_ROWS - 1);
}

static void term_putchar_raw(char c)
{
    if (c == '\n') {
        g_term.cx = 0;
        g_term.cy++;
        if (g_term.cy >= TERM_ROWS) {
            g_term.cy = TERM_ROWS - 1;
            term_scroll_up();
        }
    } else if (c == '\r') {
        g_term.cx = 0;
    } else if (c == '\t') {
        g_term.cx = (g_term.cx / TERM_TAB_W + 1) * TERM_TAB_W;
        if (g_term.cx >= TERM_COLS) {
            g_term.cx = 0;
            g_term.cy++;
            if (g_term.cy >= TERM_ROWS) {
                g_term.cy = TERM_ROWS - 1;
                term_scroll_up();
            }
        }
    } else if (c == '\b') {
        if (g_term.cx > 0) g_term.cx--;
    } else {
        if (g_term.cx >= TERM_COLS) {
            g_term.cx = 0;
            g_term.cy++;
            if (g_term.cy >= TERM_ROWS) {
                g_term.cy = TERM_ROWS - 1;
                term_scroll_up();
            }
        }
        int real = (g_term.head + g_term.cy) % TERM_HISTORY;
        g_term.buf[real][g_term.cx].ch = c;
        g_term.buf[real][g_term.cx].fg = g_term.cur_fg;
        g_term.buf[real][g_term.cx].bg = g_term.cur_bg;
        g_term.cx++;
    }
}

/* =========================================================
 * Print a string to terminal
 * ========================================================= */
static void term_puts(const char* s)
{
    while (*s) term_putchar_raw(*s++);
    surface_invalidate(g_term.sid);
}

/* =========================================================
 * Resolve a path argument against CWD
 * ========================================================= */
static void term_make_path(const char* arg, char* out, int out_len)
{
    if (!arg || !arg[0]) {
        strncpy(out, g_term.cwd, out_len - 1);
        out[out_len - 1] = '\0';
    } else if (arg[0] == '~') {
        /* ~ expands to /home/user */
        const char* rest = arg + 1;
        if (*rest == '/' || *rest == '\0') {
            int n = 0;
            const char* base = "/home/user";
            while (*base && n < out_len - 1) out[n++] = *base++;
            while (*rest && n < out_len - 1) out[n++] = *rest++;
            out[n] = '\0';
        } else {
            strncpy(out, arg, out_len - 1);
            out[out_len - 1] = '\0';
        }
    } else if (arg[0] == '/') {
        strncpy(out, arg, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        /* Relative: join CWD + "/" + arg */
        int n = 0;
        const char* c = g_term.cwd;
        while (*c && n < out_len - 1) out[n++] = *c++;
        if (n > 1 && out[n-1] != '/' && n < out_len - 1) out[n++] = '/';
        const char* a = arg;
        while (*a && n < out_len - 1) out[n++] = *a++;
        out[n] = '\0';
    }
}

/* =========================================================
 * Simple built-in shell
 * ========================================================= */
/* Parse dotted-decimal IPv4; returns 0 on failure */
static uint32_t parse_ip4(const char* s)
{
    uint32_t a = 0, b = 0, c = 0, d = 0, n = 0;
    for (; *s >= '0' && *s <= '9'; s++) a = a * 10 + (uint32_t)(*s - '0');
    if (*s++ != '.') return 0;
    for (; *s >= '0' && *s <= '9'; s++) b = b * 10 + (uint32_t)(*s - '0');
    if (*s++ != '.') return 0;
    for (; *s >= '0' && *s <= '9'; s++) c = c * 10 + (uint32_t)(*s - '0');
    if (*s++ != '.') return 0;
    for (; *s >= '0' && *s <= '9'; s++) d = d * 10 + (uint32_t)(*s - '0');
    (void)n;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return a | (b << 8) | (c << 16) | (d << 24);
}

static void shell_exec(const char* cmd)
{
    /* Skip leading whitespace */
    while (*cmd == ' ') cmd++;

    if (!*cmd) {
        /* empty */
    } else if (strncmp(cmd, "help", 4) == 0) {
        term_puts("Aether Shell — built-in commands:\r\n");
        term_puts("  help      — this message\r\n");
        term_puts("  clear     — clear screen\r\n");
        term_puts("  version   — OS version\r\n");
        term_puts("  uname     — system info\r\n");
        term_puts("  echo ...  — echo text\r\n");
        term_puts("  ls [path] — list directory (default: cwd)\r\n");
        term_puts("  cat path  — print file contents\r\n");
        term_puts("  cd path   — change directory\r\n");
        term_puts("  pwd       — print working directory\r\n");
        term_puts("  mkdir dir — create directory\r\n");
        term_puts("  touch f   — create empty file\r\n");
        term_puts("  rm f      — remove file\r\n");
        term_puts("  cp s d    — copy file\r\n");
        term_puts("  uptime    — show system uptime\r\n");
        term_puts("  mem       — show heap memory usage\r\n");
        term_puts("  ps        — list running processes\r\n");
        term_puts("  kill pid  — send SIGKILL to process\r\n");
        term_puts("  whoami    — print current user\r\n");
        term_puts("  mv src dst — move/rename a file\r\n");
        term_puts("  chmod oct path — change file mode\r\n");
        term_puts("  date      — show uptime clock\r\n");
        term_puts("  logout    — end session\r\n");
        term_puts("  ifconfig  — show network interface\r\n");
        term_puts("  ping host — ICMP ping a host/IP\r\n");
        term_puts("  dns host  — resolve hostname to IP\r\n");
        term_puts("  run pkg   — execute installed package\r\n");
        term_puts("  reboot    — reboot system\r\n");
        term_puts("  halt      — halt system\r\n");
    } else if (strncmp(cmd, "clear", 5) == 0) {
        for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);
        g_term.cx = 0; g_term.cy = 0;
    } else if (strncmp(cmd, "version", 7) == 0) {
        term_puts(OS_BANNER);
        term_putchar_raw('\n');
    } else if (strncmp(cmd, "uname", 5) == 0) {
        term_puts("Aether x86_64 ARE/1.0 #NewParadigm\r\n");
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        term_puts(cmd + 5);
        term_putchar_raw('\n');
    } else if (strncmp(cmd, "ls", 2) == 0) {
        /* Optional path argument: "ls [path]" — default: CWD */
        char lspath[VFS_NAME_MAX + 1];
        term_make_path((cmd[2] == ' ' && cmd[3]) ? cmd + 3 : NULL,
                       lspath, sizeof(lspath));
        vfs_node_t* dir = vfs_resolve_path(lspath);
        if (!dir || !(dir->flags & VFS_DIRECTORY)) {
            term_puts("ls: no such directory\r\n");
        } else {
            vfs_dirent_t ent;
            uint32_t idx = 0;
            int col = 0;
            while (vfs_readdir(dir, idx, &ent) == 0) {
                term_puts(ent.name);
                if (ent.type == VFS_TYPE_DIR) term_putchar_raw('/');
                term_puts("  ");
                col++;
                if (col >= 6) { term_puts("\r\n"); col = 0; }
                idx++;
            }
            if (col > 0) term_puts("\r\n");
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        const char* path = cmd + 4;
        vfs_node_t* f = vfs_resolve_path(path);
        if (!f || (f->flags & VFS_DIRECTORY)) {
            term_puts("cat: cannot read: ");
            term_puts(path);
            term_putchar_raw('\n');
        } else {
            char fbuf[256];
            off_t off = 0;
            ssize_t n;
            while ((n = vfs_read(f, off, sizeof(fbuf) - 1, fbuf)) > 0) {
                fbuf[n] = '\0';
                term_puts(fbuf);
                off += n;
            }
            term_putchar_raw('\n');
        }
    } else if (strncmp(cmd, "pwd", 3) == 0) {
        term_puts(g_term.cwd);
        term_puts("\r\n");
    } else if (strncmp(cmd, "cd", 2) == 0) {
        const char* arg = (cmd[2] == ' ' && cmd[3]) ? cmd + 3 : "/";
        if (strcmp(arg, "..") == 0) {
            /* Navigate up one directory level */
            int len = (int)strlen(g_term.cwd);
            if (len > 1) {
                if (g_term.cwd[len - 1] == '/') len--;
                while (len > 1 && g_term.cwd[len - 1] != '/') len--;
                g_term.cwd[len > 1 ? len - 1 : 1] = '\0';
            }
        } else {
            char newpath[VFS_NAME_MAX + 1];
            term_make_path(arg, newpath, sizeof(newpath));
            vfs_node_t* d = vfs_resolve_path(newpath);
            if (!d || !(d->flags & VFS_DIRECTORY)) {
                term_puts("cd: no such directory: ");
                term_puts(arg);
                term_puts("\r\n");
            } else {
                strncpy(g_term.cwd, newpath, sizeof(g_term.cwd) - 1);
                g_term.cwd[sizeof(g_term.cwd) - 1] = '\0';
            }
        }
    } else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char newdir[VFS_NAME_MAX + 1];
        term_make_path(cmd + 6, newdir, sizeof(newdir));
        if (vfs_mkdir(newdir) == 0) {
            term_puts("mkdir: created ");
            term_puts(newdir);
            term_puts("\r\n");
        } else {
            term_puts("mkdir: failed to create ");
            term_puts(newdir);
            term_puts("\r\n");
        }
    } else if (strncmp(cmd, "touch ", 6) == 0) {
        char touchpath[VFS_NAME_MAX + 1];
        term_make_path(cmd + 6, touchpath, sizeof(touchpath));
        if (vfs_create(touchpath, 0644) == 0) {
            term_puts("touch: created ");
            term_puts(touchpath);
            term_puts("\r\n");
        } else {
            term_puts("touch: failed: ");
            term_puts(touchpath);
            term_puts("\r\n");
        }
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        char rmpath[VFS_NAME_MAX + 1];
        term_make_path(cmd + 3, rmpath, sizeof(rmpath));
        if (vfs_unlink(rmpath) == 0) {
            term_puts("rm: removed ");
            term_puts(rmpath);
            term_puts("\r\n");
        } else {
            term_puts("rm: failed: ");
            term_puts(rmpath);
            term_puts("\r\n");
        }
    } else if (strcmp(cmd, "uptime") == 0) {
        uint64_t secs = timer_get_ticks() / TIMER_FREQ;
        uint64_t mins = secs / 60;
        uint64_t hrs  = mins / 60;
        secs %= 60; mins %= 60;
        char ubuf[48];
        snprintf(ubuf, sizeof(ubuf), "up %llu:%02llu:%02llu\r\n",
                 (unsigned long long)hrs, (unsigned long long)mins,
                 (unsigned long long)secs);
        term_puts(ubuf);
    } else if (strcmp(cmd, "mem") == 0) {
        size_t used = kheap_used() / 1024;
        size_t free_mem = kheap_free() / 1024;
        char mbuf[64];
        snprintf(mbuf, sizeof(mbuf), "heap: %u KB used, %u KB free\r\n",
                 (unsigned)used, (unsigned)free_mem);
        term_puts(mbuf);
    } else if (strncmp(cmd, "cp ", 3) == 0) {
        /* cp <src> <dst> */
        const char* sp = cmd + 3;
        const char* ep = sp;
        while (*ep && *ep != ' ') ep++;
        if (!*ep) {
            term_puts("usage: cp <src> <dst>\r\n");
        } else {
            char src[VFS_NAME_MAX + 1], dst[VFS_NAME_MAX + 1];
            int sl = (int)(ep - sp);
            if (sl >= VFS_NAME_MAX) sl = VFS_NAME_MAX - 1;
            memcpy(src, sp, (size_t)sl); src[sl] = '\0';
            term_make_path(src, src, sizeof(src));
            term_make_path(ep + 1, dst, sizeof(dst));
            vfs_node_t* sf = vfs_resolve_path(src);
            if (!sf || (sf->flags & VFS_DIRECTORY)) {
                term_puts("cp: source not found or is directory\r\n");
            } else {
                vfs_node_t* df = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
                if (!df) {
                    term_puts("cp: cannot create destination\r\n");
                } else {
                    char cpbuf[512]; off_t off = 0; ssize_t n;
                    while ((n = vfs_read(sf, off, sizeof(cpbuf), cpbuf)) > 0) {
                        vfs_write(df, off, (size_t)n, cpbuf);
                        off += n;
                    }
                    vfs_close(df);
                    term_puts("cp: done\r\n");
                }
            }
        }
    } else if (strcmp(cmd, "ps") == 0) {
        term_puts("  PID  PPID STATE    NAME\r\n");
        char psbuf[80];
        for (process_t* p = process_list; p; p = p->next) {
            if (p->state == PROC_STATE_UNUSED || p->state == PROC_STATE_DEAD) continue;
            const char* st = "?      ";
            switch (p->state) {
            case PROC_STATE_RUNNING:  st = "RUNNING"; break;
            case PROC_STATE_READY:    st = "READY  "; break;
            case PROC_STATE_SLEEPING: st = "SLEEP  "; break;
            case PROC_STATE_WAITING:  st = "WAIT   "; break;
            case PROC_STATE_ZOMBIE:   st = "ZOMBIE "; break;
            default: break;
            }
            snprintf(psbuf, sizeof(psbuf), "%5d %5d %s %s\r\n",
                     (int)p->pid, (int)p->ppid, st, p->name);
            term_puts(psbuf);
        }
    } else if (strncmp(cmd, "kill ", 5) == 0) {
        int kpid = 0;
        for (const char* kp = cmd + 5; *kp >= '0' && *kp <= '9'; kp++)
            kpid = kpid * 10 + (*kp - '0');
        if (kpid > 0) {
            process_kill((pid_t)kpid, 9);  /* SIGKILL */
            char kb[40];
            snprintf(kb, sizeof(kb), "kill: SIGKILL -> pid %d\r\n", kpid);
            term_puts(kb);
        } else {
            term_puts("usage: kill <pid>\r\n");
        }
    } else if (strcmp(cmd, "whoami") == 0) {
        if (current_process) {
            const user_t* u = users_get_by_uid(current_process->uid);
            if (u) {
                term_puts(u->name);
            } else {
                char ub[24];
                snprintf(ub, sizeof(ub), "uid=%u", current_process->uid);
                term_puts(ub);
            }
            term_puts("\r\n");
        }
    } else if (strcmp(cmd, "logout") == 0 || strcmp(cmd, "exit") == 0) {
        term_puts("Logging out...\r\n");
        are_logout();
    } else if (strcmp(cmd, "ifconfig") == 0) {
        char ib[256];
        uint32_t ip = net_iface.ip;
        uint32_t gw = net_iface.gateway;
        uint32_t nm = net_iface.netmask;
        snprintf(ib, sizeof(ib),
            "eth0: %s\r\n"
            "  inet %u.%u.%u.%u  netmask %u.%u.%u.%u\r\n"
            "  gateway %u.%u.%u.%u\r\n"
            "  ether %02x:%02x:%02x:%02x:%02x:%02x\r\n",
            net_iface.up ? "UP RUNNING" : "DOWN",
            ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF,
            nm&0xFF,(nm>>8)&0xFF,(nm>>16)&0xFF,(nm>>24)&0xFF,
            gw&0xFF,(gw>>8)&0xFF,(gw>>16)&0xFF,(gw>>24)&0xFF,
            net_iface.mac.b[0], net_iface.mac.b[1], net_iface.mac.b[2],
            net_iface.mac.b[3], net_iface.mac.b[4], net_iface.mac.b[5]);
        term_puts(ib);
    } else if (strncmp(cmd, "ping ", 5) == 0) {
        const char* host = cmd + 5;
        uint32_t target = parse_ip4(host);
        if (!target) target = dns_resolve(host);
        if (!target) {
            term_puts("ping: cannot resolve: ");
            term_puts(host);
            term_puts("\r\n");
        } else {
            char pb[80];
            snprintf(pb, sizeof(pb), "PING %s (%u.%u.%u.%u)\r\n", host,
                     target&0xFF,(target>>8)&0xFF,(target>>16)&0xFF,(target>>24)&0xFF);
            term_puts(pb);
            for (uint16_t seq = 1; seq <= 4; seq++) {
                uint32_t t0 = (uint32_t)timer_get_ticks();
                icmp_ping(target, seq);
                uint32_t deadline = t0 + TIMER_FREQ * 2;
                while ((uint32_t)timer_get_ticks() < deadline) {
                    if (net_iface.poll) net_iface.poll();
                    if (g_icmp_reply_seq == seq) break;
                    scheduler_yield();
                }
                uint32_t ms = ((uint32_t)timer_get_ticks() - t0) * 1000 / TIMER_FREQ;
                if (g_icmp_reply_seq == seq)
                    snprintf(pb, sizeof(pb), "  seq=%u time=%ums\r\n", seq, ms);
                else
                    snprintf(pb, sizeof(pb), "  seq=%u timeout\r\n", seq);
                term_puts(pb);
            }
        }
    } else if (strncmp(cmd, "dns ", 4) == 0) {
        const char* host = cmd + 4;
        uint32_t ip = dns_resolve(host);
        if (!ip) {
            term_puts("dns: failed to resolve: ");
            term_puts(host);
            term_puts("\r\n");
        } else {
            char db[80];
            snprintf(db, sizeof(db), "%s -> %u.%u.%u.%u\r\n", host,
                     ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
            term_puts(db);
        }
    } else if (strncmp(cmd, "run ", 4) == 0) {
        const char* pkg = cmd + 4;
        pid_t rpid = apkg_exec(pkg);
        if ((int)rpid < 0) {
            term_puts("run: not found: ");
            term_puts(pkg);
            term_puts("\r\n");
        } else {
            char rb[64];
            snprintf(rb, sizeof(rb), "run: '%s' launched as pid %d\r\n",
                     pkg, (int)rpid);
            term_puts(rb);
        }
    } else if (strncmp(cmd, "mv ", 3) == 0) {
        /* mv <src> <dst> — copy then unlink source */
        const char* sp = cmd + 3;
        const char* ep = sp;
        while (*ep && *ep != ' ') ep++;
        if (!*ep) {
            term_puts("usage: mv <src> <dst>\r\n");
        } else {
            char src[VFS_NAME_MAX+1], dst[VFS_NAME_MAX+1];
            int sl = (int)(ep - sp); if (sl >= VFS_NAME_MAX) sl = VFS_NAME_MAX-1;
            memcpy(src, sp, (size_t)sl); src[sl] = '\0';
            term_make_path(src, src, sizeof(src));
            term_make_path(ep + 1, dst, sizeof(dst));
            vfs_node_t* sf = vfs_resolve_path(src);
            if (!sf || (sf->flags & VFS_DIRECTORY)) {
                term_puts("mv: source not found or is directory\r\n");
            } else {
                vfs_node_t* df = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
                if (!df) { term_puts("mv: cannot create destination\r\n"); }
                else {
                    char mvbuf[512]; off_t off = 0; ssize_t n;
                    while ((n = vfs_read(sf, off, sizeof(mvbuf), mvbuf)) > 0) {
                        vfs_write(df, off, (size_t)n, mvbuf); off += n;
                    }
                    vfs_close(df);
                    vfs_unlink(src);
                    term_puts("mv: done\r\n");
                }
            }
        }
    } else if (strcmp(cmd, "date") == 0) {
        /* Show uptime as a stand-in for wall-clock time */
        uint32_t t = timer_get_ticks();
        uint32_t ss = t / 1000, mm = ss / 60, hh = mm / 60;
        ss %= 60; mm %= 60; hh %= 24;
        char dbuf[48];
        snprintf(dbuf, sizeof(dbuf), "Uptime: %02u:%02u:%02u\r\n", hh, mm, ss);
        term_puts(dbuf);
    } else if (strncmp(cmd, "chmod ", 6) == 0) {
        /* chmod <octal> <path> */
        const char* sp = cmd + 6;
        uint32_t mode = 0;
        while (*sp >= '0' && *sp <= '7') mode = mode * 8 + (uint32_t)(*sp++ - '0');
        while (*sp == ' ') sp++;
        if (!*sp) { term_puts("usage: chmod <octal> <path>\r\n"); }
        else {
            char cpath[VFS_NAME_MAX+1];
            term_make_path(sp, cpath, sizeof(cpath));
            vfs_node_t* cn = vfs_resolve_path(cpath);
            if (!cn) { term_puts("chmod: not found\r\n"); }
            else { cn->mode = (uint16_t)mode & 0777; term_puts("chmod: done\r\n"); }
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        term_puts("Rebooting...\r\n");
        uint8_t good = 0x02;
        while (good & 0x02) good = inb(0x64);
        outb(0x64, 0xFE);
        cpu_halt();
    } else if (strcmp(cmd, "halt") == 0 || strcmp(cmd, "poweroff") == 0) {
        term_puts("System halting.\r\n");
        cpu_cli();
        cpu_halt();
    } else {
        term_puts("unknown command: ");
        term_puts(cmd);
        term_putchar_raw('\n');
    }
}

static void shell_prompt(void)
{
    term_puts("\r\nare:");
    term_puts(g_term.cwd);
    term_puts(" $ ");
}

/* =========================================================
 * Render callback
 * ========================================================= */
static void term_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                        void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = w, .height = h };

    /* Background */
    draw_rect(&c, 0, 0, (int)w, (int)h, TC_BG);

    /* Title bar */
    draw_rect(&c, 0, 0, (int)w, TERM_TITLE_H, TC_TITLE_BG);
    draw_string(&c, TERM_PAD, (TERM_TITLE_H - FONT_H) / 2,
                "Terminal", TC_TITLE_FG, ACOLOR(0,0,0,0));

    /* Text grid */
    int cell_x0 = TERM_PAD;
    int cell_y0 = TERM_TITLE_H + 4;
    g_term.blink_frame++;
    bool cursor_vis = (g_term.blink_frame / 30) % 2 == 0;

    for (int row = 0; row < TERM_ROWS; row++) {
        int real = (g_term.head + row) % TERM_HISTORY;
        for (int col = 0; col < TERM_COLS; col++) {
            term_cell_t* cell = &g_term.buf[real][col];
            int px = cell_x0 + col * FONT_W;
            int py = cell_y0 + row * FONT_H;
            if (px + FONT_W > (int)w || py + FONT_H > (int)h) continue;

            acolor_t bg = cell->bg;
            /* Cursor highlight */
            if (cursor_vis && row == g_term.cy && col == g_term.cx)
                bg = TC_CURSOR;

            if (ACOLOR_A(bg) > 0)
                draw_rect(&c, px, py, FONT_W, FONT_H, bg);

            if (cell->ch > ' ') {
                char s[2] = { cell->ch, 0 };
                draw_string(&c, px, py, s, cell->fg, ACOLOR(0,0,0,0));
            }
        }
    }

    /* Render line-buffer echo at cursor row */
    {
        int real = (g_term.head + g_term.cy) % TERM_HISTORY;
        /* cursor cell is already painted by grid loop */
        (void)real;
    }
}

/* =========================================================
 * Input callback
 * ========================================================= */
static void term_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)id; (void)ud;
    if (ev->type != INPUT_KEY || !ev->key.down) return;

    char ch  = ev->key.ch;
    int  kc  = ev->key.keycode;

    if (kc == KEY_ENTER || kc == '\r' || kc == '\n' || ch == '\r' || ch == '\n') {
        /* Echo newline */
        g_term.line_buf[g_term.line_len] = '\0';
        term_putchar_raw('\n');
        /* Execute */
        shell_exec(g_term.line_buf);
        g_term.line_len    = 0;
        g_term.line_cursor = 0;
        memset(g_term.line_buf, 0, sizeof(g_term.line_buf));
        shell_prompt();
    } else if (kc == KEY_BACKSPACE || ch == '\b') {
        if (g_term.line_len > 0) {
            g_term.line_len--;
            g_term.line_cursor = g_term.line_len;
            /* Erase from terminal */
            term_putchar_raw('\b');
            term_putchar_raw(' ');
            term_putchar_raw('\b');
        }
    } else if (ch >= 0x20 && ch < 0x7F && g_term.line_len < 255) {
        g_term.line_buf[g_term.line_len++] = ch;
        g_term.line_cursor = g_term.line_len;
        term_putchar_raw(ch);
    }

    surface_invalidate(id);
}

/* =========================================================
 * Init — called by are_launch_core_surfaces
 * ========================================================= */
void surface_terminal_init(uint32_t w, uint32_t h)
{
    memset(&g_term, 0, sizeof(g_term));
    g_term.cur_fg  = TC_FG;
    g_term.cur_bg  = ACOLOR(0,0,0,0);
    g_term.cwd[0] = '/'; g_term.cwd[1] = '\0';
    g_term.surf_w  = w;
    g_term.surf_h  = h;

    /* Clear all rows */
    for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);

    g_term.sid = are_add_surface(SURF_APP, w, h,
                                 "Terminal", "T",
                                 term_render, term_input, NULL, NULL);

    /* Welcome message */
    term_puts("Aether Terminal  —  Spatial OS Shell\r\n");
    term_puts("Type 'help' for commands.\r\n");
    shell_prompt();
}
