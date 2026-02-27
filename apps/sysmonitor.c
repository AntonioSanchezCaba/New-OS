/*
 * apps/sysmonitor.c - System monitor application
 *
 * Displays real-time system statistics:
 *   - CPU usage estimate (idle ticks vs total)
 *   - Memory usage (PMM free frames)
 *   - Running processes list
 *   - Uptime
 */
#include <gui/window.h>
#include <gui/draw.h>
#include <gui/event.h>
#include <memory.h>
#include <process.h>
#include <scheduler.h>
#include <drivers/timer.h>
#include <string.h>
#include <kernel.h>

#define SM_W        480
#define SM_H        360
#define SM_BAR_H    18
#define SM_SECTION_H 20
#define SM_REFRESH_TICKS 50   /* Refresh every 50 timer ticks (~0.5s) */

#define SM_BG       COLOR_WIN_BG
#define SM_HDR_BG   rgb(0x20, 0x40, 0x80)
#define SM_BAR_FG   COLOR_ACCENT
#define SM_BAR_BG   rgb(0xCC, 0xCC, 0xDD)
#define SM_FG       COLOR_TEXT_DARK
#define SM_GRID     rgb(0xCC, 0xCC, 0xE0)

/* CPU usage history (percentage 0-100) */
#define CPU_HIST_LEN 60
static uint8_t cpu_hist[CPU_HIST_LEN];
static int cpu_hist_pos = 0;
static uint32_t last_idle_ticks = 0;
static uint32_t last_total_ticks = 0;

typedef struct {
    wid_t    wid;
    uint32_t last_refresh;
    int      tab;            /* 0=overview 1=processes */
} sm_t;

static sm_t* g_sm = NULL;

/* =========================================================
 * Formatting helpers
 * ========================================================= */

static void fmt_uint(char* buf, size_t bufsz, uint32_t n)
{
    char tmp[16];
    int i = 0;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    while (n > 0) { tmp[i++] = '0' + (n%10); n /= 10; }
    int j = 0;
    if ((size_t)i >= bufsz) i = (int)bufsz - 1;
    for (int k = i-1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

static void fmt_kb(char* buf, size_t bufsz, uint32_t kb)
{
    if (kb >= 1024) {
        char tmp[16];
        fmt_uint(tmp, sizeof(tmp), kb / 1024);
        strncpy(buf, tmp, bufsz);
        strncat(buf, " MB", bufsz - strlen(buf) - 1);
    } else {
        char tmp[16];
        fmt_uint(tmp, sizeof(tmp), kb);
        strncpy(buf, tmp, bufsz);
        strncat(buf, " KB", bufsz - strlen(buf) - 1);
    }
}

/* =========================================================
 * Draw a labeled progress bar
 * ========================================================= */

static void draw_bar(canvas_t* c, int x, int y, int w, int h,
                     const char* label, uint32_t pct,
                     const char* value_str)
{
    /* Label */
    draw_string(c, x, y, label, SM_FG, rgba(0,0,0,0));
    int lw = (int)strlen(label) * FONT_W + 4;

    /* Bar background */
    int bx = x + lw;
    int bw = w - lw - (int)strlen(value_str) * FONT_W - 8;
    draw_rect(c, bx, y + 1, bw, h - 2, SM_BAR_BG);
    draw_rect_outline(c, bx, y + 1, bw, h - 2, 1, COLOR_MID_GREY);

    /* Bar fill */
    int fill = (int)((uint64_t)pct * (uint32_t)(bw - 2) / 100);
    uint32_t bar_col = (pct > 80) ? COLOR_RED :
                       (pct > 50) ? COLOR_YELLOW : SM_BAR_FG;
    if (fill > 0)
        draw_rect(c, bx + 1, y + 2, fill, h - 4, bar_col);

    /* Percentage text inside bar */
    char pct_str[8];
    fmt_uint(pct_str, sizeof(pct_str), pct);
    strncat(pct_str, "%", 2);
    draw_string_centered(c, bx, y, bw, h, pct_str, COLOR_TEXT_DARK, rgba(0,0,0,0));

    /* Value */
    draw_string(c, x + w - (int)strlen(value_str) * FONT_W, y,
                value_str, SM_FG, rgba(0,0,0,0));
}

/* =========================================================
 * Draw CPU history graph
 * ========================================================= */

static void draw_cpu_graph(canvas_t* c, int x, int y, int w, int h)
{
    /* Border */
    draw_rect(c, x, y, w, h, rgb(0xF0,0xF0,0xF8));
    draw_rect_outline(c, x, y, w, h, 1, COLOR_MID_GREY);

    /* Grid lines at 25%, 50%, 75% */
    for (int g = 1; g <= 3; g++) {
        int gy = y + h - (h * g / 4);
        draw_hline(c, x + 1, gy, w - 2, SM_GRID);
    }

    /* Plot history */
    int bar_w = (w - 2) / CPU_HIST_LEN;
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < CPU_HIST_LEN; i++) {
        int idx = (cpu_hist_pos + i) % CPU_HIST_LEN;
        int val = cpu_hist[idx];
        int bar_h = (h - 2) * val / 100;
        if (bar_h > 0) {
            uint32_t col = (val > 80) ? COLOR_RED :
                           (val > 50) ? COLOR_YELLOW : SM_BAR_FG;
            draw_rect(c, x + 1 + i * bar_w,
                      y + h - 1 - bar_h, bar_w, bar_h, col);
        }
    }

    draw_string(c, x + 2, y + 2, "CPU %", SM_FG, rgba(0,0,0,0));
}

/* =========================================================
 * Overview tab
 * ========================================================= */

static void draw_overview(canvas_t* c, sm_t* s)
{
    (void)s;
    int y = 10;

    /* Section: CPU */
    draw_rect(c, 4, y, c->width - 8, SM_SECTION_H, SM_HDR_BG);
    draw_string(c, 8, y + 2, "CPU", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    y += SM_SECTION_H + 4;

    /* Estimate CPU usage from scheduler ticks (total ticks used) */
    uint32_t total = timer_get_ticks();
    uint32_t sched_total = (uint32_t)scheduler_ticks();
    uint32_t dt_total = total - last_total_ticks;
    uint32_t dt_sched = sched_total - last_idle_ticks;
    uint32_t cpu_pct = (dt_total > 0)
                       ? (dt_sched * 100 / dt_total) : 0;
    if (cpu_pct > 100) cpu_pct = 100;
    last_total_ticks = total;
    last_idle_ticks  = sched_total;

    cpu_hist[cpu_hist_pos] = (uint8_t)cpu_pct;
    cpu_hist_pos = (cpu_hist_pos + 1) % CPU_HIST_LEN;

    char pct_s[8]; fmt_uint(pct_s, sizeof(pct_s), cpu_pct); strncat(pct_s,"%",2);
    draw_bar(c, 8, y, c->width - 16, SM_BAR_H, "Usage: ", cpu_pct, pct_s);
    y += SM_BAR_H + 4;

    /* Mini graph */
    draw_cpu_graph(c, 8, y, c->width - 16, 60);
    y += 64 + 8;

    /* Section: Memory */
    draw_rect(c, 4, y, c->width - 8, SM_SECTION_H, SM_HDR_BG);
    draw_string(c, 8, y + 2, "Memory", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    y += SM_SECTION_H + 4;

    uint32_t total_kb = 256 * 1024;  /* Assume 256MB total (from QEMU default) */
    uint32_t free_kb  = (uint32_t)(pmm_free_frames_count() * 4);
    uint32_t used_kb  = (total_kb > free_kb) ? total_kb - free_kb : 0;
    uint32_t mem_pct  = used_kb * 100 / total_kb;

    char mem_val[32];
    char used_s[16], total_s[16];
    fmt_kb(used_s, sizeof(used_s), used_kb);
    fmt_kb(total_s, sizeof(total_s), total_kb);
    strncpy(mem_val, used_s, sizeof(mem_val));
    strncat(mem_val, " / ", sizeof(mem_val) - strlen(mem_val) - 1);
    strncat(mem_val, total_s, sizeof(mem_val) - strlen(mem_val) - 1);

    draw_bar(c, 8, y, c->width - 16, SM_BAR_H, "Used:  ", mem_pct, mem_val);
    y += SM_BAR_H + 4;

    char free_buf[32];
    fmt_kb(free_buf, sizeof(free_buf), free_kb);
    draw_string(c, 8, y, "Free: ", SM_FG, rgba(0,0,0,0));
    draw_string(c, 8 + 6 * FONT_W, y, free_buf, COLOR_GREEN, rgba(0,0,0,0));
    y += FONT_H + 8;

    /* Section: System */
    draw_rect(c, 4, y, c->width - 8, SM_SECTION_H, SM_HDR_BG);
    draw_string(c, 8, y + 2, "System", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    y += SM_SECTION_H + 4;

    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    uint32_t h = secs/3600, m=(secs/60)%60, sec=secs%60;
    char up[32];
    char hb[8],mb[8],sb[8];
    fmt_uint(hb,sizeof(hb),h); fmt_uint(mb,sizeof(mb),m); fmt_uint(sb,sizeof(sb),sec);
    strncpy(up, "Uptime: ", sizeof(up));
    strncat(up, hb, sizeof(up)-strlen(up)-1);
    strncat(up, "h ", sizeof(up)-strlen(up)-1);
    strncat(up, mb, sizeof(up)-strlen(up)-1);
    strncat(up, "m ", sizeof(up)-strlen(up)-1);
    strncat(up, sb, sizeof(up)-strlen(up)-1);
    strncat(up, "s", sizeof(up)-strlen(up)-1);
    draw_string(c, 8, y, up, SM_FG, rgba(0,0,0,0));
    y += FONT_H + 4;

    /* Count processes by walking the linked list */
    int nproc = 0;
    for (process_t* p = process_list; p; p = p->next) nproc++;
    char proc_s[32];
    strncpy(proc_s, "Processes: ", sizeof(proc_s));
    char nb[8]; fmt_uint(nb,sizeof(nb),(uint32_t)nproc);
    strncat(proc_s, nb, sizeof(proc_s)-strlen(proc_s)-1);
    draw_string(c, 8, y, proc_s, SM_FG, rgba(0,0,0,0));
}

/* =========================================================
 * Process list tab
 * ========================================================= */

static void draw_proc_list(canvas_t* c)
{
    /* Header */
    draw_rect(c, 0, 0, c->width, SM_SECTION_H, SM_HDR_BG);
    draw_string(c, 4,   2, "PID",   COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    draw_string(c, 50,  2, "Name",  COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    draw_string(c, 200, 2, "State", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    draw_string(c, 300, 2, "PRI",   COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    int y = SM_SECTION_H;
    int row_num = 0;
    for (process_t* p = process_list; p && y + FONT_H < c->height; p = p->next) {
        uint32_t row_bg = (row_num++ % 2 == 0) ? SM_BG : rgb(0xF0,0xF0,0xF8);
        draw_rect(c, 0, y, c->width, FONT_H + 2, row_bg);

        char pid_s[8];
        fmt_uint(pid_s, sizeof(pid_s), (uint32_t)p->pid);
        draw_string(c, 4,   y + 1, pid_s,    SM_FG, rgba(0,0,0,0));
        draw_string(c, 50,  y + 1, p->name,  SM_FG, rgba(0,0,0,0));

        const char* state_str = "?";
        switch (p->state) {
        case PROC_STATE_UNUSED:  state_str = "UNUSED";  break;
        case PROC_STATE_CREATED: state_str = "CREATED"; break;
        case PROC_STATE_RUNNING: state_str = "RUNNING"; break;
        case PROC_STATE_READY:   state_str = "READY";   break;
        case PROC_STATE_SLEEPING:state_str = "SLEEP";   break;
        case PROC_STATE_WAITING: state_str = "WAIT";    break;
        case PROC_STATE_ZOMBIE:  state_str = "ZOMBIE";  break;
        case PROC_STATE_DEAD:    state_str = "DEAD";    break;
        }
        uint32_t state_col = (p->state == PROC_STATE_RUNNING) ? COLOR_GREEN :
                             (p->state == PROC_STATE_ZOMBIE)  ? COLOR_RED : SM_FG;
        draw_string(c, 200, y + 1, state_str, state_col, rgba(0,0,0,0));
        draw_string(c, 300, y + 1, "0", SM_FG, rgba(0,0,0,0));

        draw_hline(c, 0, y + FONT_H + 1, c->width, SM_GRID);
        y += FONT_H + 2;
    }
}

/* =========================================================
 * Window event handler
 * ========================================================= */

static void sm_redraw(wid_t wid)
{
    sm_t* s = g_sm;
    if (!s || s->wid != wid) return;

    canvas_t c = wm_client_canvas(wid);
    if (!c.pixels) return;

    draw_rect(&c, 0, 0, c.width, c.height, SM_BG);

    /* Tab bar */
    draw_rect(&c, 0, 0, c.width, SM_SECTION_H, rgb(0xD8,0xD8,0xE8));
    const char* tabs[] = { " Overview ", " Processes " };
    int tx = 0;
    for (int i = 0; i < 2; i++) {
        int tw = (int)strlen(tabs[i]) * FONT_W + 4;
        uint32_t bg = (s->tab == i) ? COLOR_WIN_BG : rgb(0xC0,0xC0,0xD0);
        draw_rect(&c, tx, 0, tw, SM_SECTION_H, bg);
        draw_rect_outline(&c, tx, 0, tw, SM_SECTION_H, 1, COLOR_MID_GREY);
        draw_string(&c, tx + 2, 2, tabs[i], SM_FG, rgba(0,0,0,0));
        tx += tw;
    }
    draw_hline(&c, 0, SM_SECTION_H, c.width, COLOR_MID_GREY);

    /* Tab content (sub-canvas below tab bar) */
    canvas_t content = draw_sub_canvas(&c, 0, SM_SECTION_H + 1,
                                       c.width, c.height - SM_SECTION_H - 1);

    if (s->tab == 0) draw_overview(&content, s);
    else             draw_proc_list(&content);
}

static void sm_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    sm_t* s = g_sm;
    if (!s || s->wid != wid) return;

    switch (evt->type) {
    case GUI_EVENT_PAINT:
        sm_redraw(wid);
        break;

    case GUI_EVENT_MOUSE_DOWN: {
        /* Tab switching */
        const char* tabs[] = { " Overview ", " Processes " };
        int tx = 0;
        for (int i = 0; i < 2; i++) {
            int tw = (int)strlen(tabs[i]) * FONT_W + 4;
            if (evt->mouse.x >= tx && evt->mouse.x < tx + tw &&
                evt->mouse.y < SM_SECTION_H) {
                s->tab = i;
                sm_redraw(wid);
                break;
            }
            tx += tw;
        }
        break;
    }

    case GUI_EVENT_CLOSE:
        kfree(s);
        g_sm = NULL;
        break;

    default: break;
    }

    /* Auto-refresh */
    uint32_t now = timer_get_ticks();
    if (now - s->last_refresh >= SM_REFRESH_TICKS) {
        s->last_refresh = now;
        sm_redraw(wid);
    }
}

wid_t app_sysmonitor_create(void)
{
    if (g_sm) return g_sm->wid;

    sm_t* s = (sm_t*)kmalloc(sizeof(sm_t));
    if (!s) return -1;
    memset(s, 0, sizeof(sm_t));
    s->last_refresh = timer_get_ticks();

    wid_t wid = wm_create_window("System Monitor",
                                  200, 120, SM_W, SM_H,
                                  sm_on_event, NULL);
    if (wid < 0) { kfree(s); return -1; }

    s->wid = wid;
    g_sm = s;

    sm_redraw(wid);
    return wid;
}
