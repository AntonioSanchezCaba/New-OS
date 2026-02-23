/*
 * surfaces/sysmon.c — Aether System Monitor Surface
 *
 * Live CPU / memory graphs and process list.
 * Self-invalidates every frame by marking dirty in the render tick.
 */
#include <aether/are.h>
#include <gui/event.h>
#include <aether/surface.h>
#include <aether/input.h>
#include <aether/ui.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>
#include <process.h>

/* =========================================================
 * Layout
 * ========================================================= */
#define SM_TITLE_H      40
#define SM_GRAPH_W      280
#define SM_GRAPH_H      80
#define SM_GRAPH_HIST   SM_GRAPH_W
#define SM_PAD          16
#define SM_SECTION_H    24
#define SM_ROW_H        18
#define SM_MAX_PROCS    32

/* Colours */
#define SM_BG           ACOLOR(0x0A, 0x0E, 0x14, 0xFF)
#define SM_TITLE_BG     ACOLOR(0x10, 0x14, 0x1E, 0xFF)
#define SM_TITLE_FG     ACOLOR(0x80, 0xC0, 0xFF, 0xFF)
#define SM_SECTION_BG   ACOLOR(0x14, 0x1C, 0x28, 0xFF)
#define SM_SECTION_FG   ACOLOR(0x60, 0x90, 0xC0, 0xFF)
#define SM_TEXT_FG      ACOLOR(0xCC, 0xCC, 0xCC, 0xFF)
#define SM_DIM_FG       ACOLOR(0x55, 0x66, 0x77, 0xFF)
#define SM_CPU_COLOR    ACOLOR(0x40, 0xA0, 0xFF, 0xFF)
#define SM_MEM_COLOR    ACOLOR(0x40, 0xFF, 0x90, 0xFF)
#define SM_GRAPH_BG     ACOLOR(0x08, 0x0C, 0x12, 0xFF)
#define SM_GRAPH_GRID   ACOLOR(0x20, 0x28, 0x38, 0xFF)
#define SM_BORDER       ACOLOR(0x20, 0x28, 0x38, 0xFF)

/* =========================================================
 * State
 * ========================================================= */
typedef struct {
    char     name[32];
    uint32_t pid;
    uint32_t cpu_pct;
    uint32_t mem_kb;
    uint64_t prev_ticks;  /* for delta-based CPU% */
} sm_proc_t;

typedef struct {
    /* Rolling history */
    uint8_t  cpu_hist[SM_GRAPH_HIST];
    uint8_t  mem_hist[SM_GRAPH_HIST];
    int      hist_pos;

    /* Current readings */
    uint32_t cpu_pct;
    uint32_t mem_used_kb;
    uint32_t mem_total_kb;

    /* Process list */
    sm_proc_t procs[SM_MAX_PROCS];
    int       proc_count;
    int       selected_proc;
    int       scroll_y;

    /* Surface */
    uint32_t  surf_w;
    uint32_t  surf_h;
    sid_t     sid;
    uint32_t  tick;
} sm_state_t;

static sm_state_t g_sm;

/* =========================================================
 * Data collection (uses scheduler/kernel internals)
 * ========================================================= */
static void sm_collect(void)
{
    g_sm.tick++;

    /* Memory — use kheap_used() / kheap_free() from memory.h */
    size_t used = kheap_used();
    size_t free_mem = kheap_free();
    g_sm.mem_used_kb  = (uint32_t)(used / 1024);
    g_sm.mem_total_kb = (uint32_t)((used + free_mem) / 1024);

    /* CPU — delta scheduler ticks vs previous sample */
    static uint64_t prev_sched = 0;
    uint64_t now_sched = scheduler_ticks();
    uint64_t sched_delta = now_sched - prev_sched;
    prev_sched = now_sched;

    g_sm.cpu_pct = (sched_delta > 0 && sched_delta < 100)
                 ? (uint32_t)(sched_delta * 2) : 0;
    if (g_sm.cpu_pct > 100) g_sm.cpu_pct = 100;

    /* Record history */
    g_sm.cpu_hist[g_sm.hist_pos] = (uint8_t)g_sm.cpu_pct;
    g_sm.mem_hist[g_sm.hist_pos] =
        (g_sm.mem_total_kb > 0)
            ? (uint8_t)((uint64_t)g_sm.mem_used_kb * 100 / g_sm.mem_total_kb)
            : 0;
    g_sm.hist_pos = (g_sm.hist_pos + 1) % SM_GRAPH_HIST;

    /* Snapshot previous per-process ticks for delta CPU% */
    uint64_t old_ticks[SM_MAX_PROCS];
    uint32_t old_pids [SM_MAX_PROCS];
    int old_count = g_sm.proc_count;
    for (int i = 0; i < old_count; i++) {
        old_ticks[i] = g_sm.procs[i].prev_ticks;
        old_pids [i] = g_sm.procs[i].pid;
    }

    /* Walk process list */
    g_sm.proc_count = 0;
    process_t* p = process_list;
    while (p && g_sm.proc_count < SM_MAX_PROCS) {
        if (p->state != PROC_STATE_UNUSED && p->state != PROC_STATE_DEAD) {
            int idx = g_sm.proc_count;
            strncpy(g_sm.procs[idx].name, p->name, 31);
            g_sm.procs[idx].name[31] = '\0';
            g_sm.procs[idx].pid = (uint32_t)p->pid;

            /* Find saved ticks for this PID to compute delta CPU% */
            uint64_t prev_proc = 0;
            for (int j = 0; j < old_count; j++) {
                if (old_pids[j] == (uint32_t)p->pid) {
                    prev_proc = old_ticks[j];
                    break;
                }
            }
            uint64_t proc_delta = p->total_ticks - prev_proc;
            g_sm.procs[idx].prev_ticks = p->total_ticks;
            g_sm.procs[idx].cpu_pct = (sched_delta > 0)
                ? (uint32_t)((proc_delta * 100) / sched_delta)
                : 0;
            if (g_sm.procs[idx].cpu_pct > 100)
                g_sm.procs[idx].cpu_pct = 100;

            /* Memory: estimate from process heap size */
            uint64_t heap_bytes = (p->heap_end > p->heap_start)
                                ? (p->heap_end - p->heap_start) : 0;
            g_sm.procs[idx].mem_kb = (uint32_t)(heap_bytes / 1024);

            g_sm.proc_count++;
        }
        p = p->next;
    }
}

/* =========================================================
 * Draw a mini graph
 * ========================================================= */
static void draw_graph(canvas_t* c, int x, int y, int w, int h,
                       const uint8_t* hist, int hist_pos, int hist_len,
                       acolor_t line_color)
{
    /* Background */
    draw_rect(c, x, y, w, h, SM_GRAPH_BG);
    draw_rect_outline(c, x, y, w, h, 1, SM_BORDER);

    /* Grid lines at 25%, 50%, 75% */
    for (int g = 1; g <= 3; g++) {
        int gy = y + h - (g * h / 4);
        draw_rect(c, x+1, gy, w-2, 1, SM_GRAPH_GRID);
    }

    /* Plot */
    for (int i = 0; i < w - 2; i++) {
        int idx = (hist_pos + i - (w - 2) + hist_len) % hist_len;
        if (idx < 0) idx += hist_len;
        uint8_t val = hist[idx];
        int bar_h = (val * (h - 2)) / 100;
        if (bar_h > 0)
            draw_rect(c, x + 1 + i, y + h - 1 - bar_h, 1, bar_h, line_color);
    }
}

/* =========================================================
 * Render
 * ========================================================= */
static void sm_render(sid_t id, uint32_t* pixels, uint32_t w, uint32_t h,
                      void* ud)
{
    (void)id; (void)ud;
    canvas_t c = { .pixels = pixels, .width = w, .height = h };

    /* Collect data every render (are_run ticks us via surface_tick) */
    sm_collect();

    /* Background */
    draw_rect(&c, 0, 0, (int)w, (int)h, SM_BG);

    /* Title bar */
    draw_rect(&c, 0, 0, (int)w, SM_TITLE_H, SM_TITLE_BG);
    draw_string(&c, SM_PAD, (SM_TITLE_H - FONT_H) / 2,
                "System Monitor", SM_TITLE_FG, ACOLOR(0,0,0,0));

    int y = SM_TITLE_H + SM_PAD;

    /* ---- CPU section ---- */
    draw_rect(&c, 0, y, (int)w, SM_SECTION_H, SM_SECTION_BG);
    draw_string(&c, SM_PAD, y + (SM_SECTION_H - FONT_H)/2,
                "CPU", SM_SECTION_FG, ACOLOR(0,0,0,0));

    char cpu_str[32];
    snprintf(cpu_str, sizeof(cpu_str), "%u%%", g_sm.cpu_pct);
    draw_string(&c, SM_PAD + 6*FONT_W,
                y + (SM_SECTION_H - FONT_H)/2,
                cpu_str, SM_TEXT_FG, ACOLOR(0,0,0,0));
    y += SM_SECTION_H + 4;

    draw_graph(&c, SM_PAD, y, SM_GRAPH_W, SM_GRAPH_H,
               g_sm.cpu_hist, g_sm.hist_pos, SM_GRAPH_HIST, SM_CPU_COLOR);
    y += SM_GRAPH_H + SM_PAD;

    /* ---- Memory section ---- */
    draw_rect(&c, 0, y, (int)w, SM_SECTION_H, SM_SECTION_BG);
    draw_string(&c, SM_PAD, y + (SM_SECTION_H - FONT_H)/2,
                "Memory", SM_SECTION_FG, ACOLOR(0,0,0,0));

    char mem_str[48];
    snprintf(mem_str, sizeof(mem_str), "%u / %u MB",
             g_sm.mem_used_kb / 1024, g_sm.mem_total_kb / 1024);
    draw_string(&c, SM_PAD + 9*FONT_W,
                y + (SM_SECTION_H - FONT_H)/2,
                mem_str, SM_TEXT_FG, ACOLOR(0,0,0,0));
    y += SM_SECTION_H + 4;

    draw_graph(&c, SM_PAD, y, SM_GRAPH_W, SM_GRAPH_H,
               g_sm.mem_hist, g_sm.hist_pos, SM_GRAPH_HIST, SM_MEM_COLOR);
    y += SM_GRAPH_H + SM_PAD;

    /* ---- Process list ---- */
    draw_rect(&c, 0, y, (int)w, SM_SECTION_H, SM_SECTION_BG);
    draw_string(&c, SM_PAD, y + (SM_SECTION_H - FONT_H)/2,
                "Processes", SM_SECTION_FG, ACOLOR(0,0,0,0));
    y += SM_SECTION_H;

    /* Header */
    draw_string(&c, SM_PAD,           y + 2, "PID",   SM_DIM_FG, ACOLOR(0,0,0,0));
    draw_string(&c, SM_PAD + 5*FONT_W, y + 2, "Name",  SM_DIM_FG, ACOLOR(0,0,0,0));
    draw_string(&c, SM_PAD + 22*FONT_W,y + 2, "CPU%",  SM_DIM_FG, ACOLOR(0,0,0,0));
    draw_string(&c, SM_PAD + 28*FONT_W,y + 2, "Mem KB",SM_DIM_FG, ACOLOR(0,0,0,0));
    y += SM_ROW_H;
    draw_rect(&c, SM_PAD, y, (int)w - 2*SM_PAD, 1, SM_BORDER);
    y += 2;

    for (int i = 0; i < g_sm.proc_count; i++) {
        int ry = y + i * SM_ROW_H - g_sm.scroll_y;
        if (ry + SM_ROW_H < SM_TITLE_H || ry > (int)h) continue;

        if (i == g_sm.selected_proc)
            draw_rect(&c, SM_PAD, ry, (int)w - 2*SM_PAD, SM_ROW_H,
                           ACOLOR(0x18, 0x30, 0x50, 0xFF));

        char pid_s[8];
        snprintf(pid_s, sizeof(pid_s), "%u", g_sm.procs[i].pid);
        draw_string(&c, SM_PAD,             ry+2, pid_s,              SM_TEXT_FG, ACOLOR(0,0,0,0));
        draw_string(&c, SM_PAD + 5*FONT_W,  ry+2, g_sm.procs[i].name, SM_TEXT_FG, ACOLOR(0,0,0,0));

        char cpu_s[8], mem_s[12];
        snprintf(cpu_s, sizeof(cpu_s),  "%u",   g_sm.procs[i].cpu_pct);
        snprintf(mem_s, sizeof(mem_s),  "%u",   g_sm.procs[i].mem_kb);
        draw_string(&c, SM_PAD + 22*FONT_W, ry+2, cpu_s, SM_DIM_FG, ACOLOR(0,0,0,0));
        draw_string(&c, SM_PAD + 28*FONT_W, ry+2, mem_s, SM_DIM_FG, ACOLOR(0,0,0,0));
    }

    /* Always stay dirty — sysmon re-renders every tick */
    surface_invalidate(id);
}

/* =========================================================
 * Input
 * ========================================================= */
static void sm_input(sid_t id, const input_event_t* ev, void* ud)
{
    (void)ud;
    if (ev->type == INPUT_POINTER) {
        if (ev->pointer.scroll) {
            g_sm.scroll_y += ev->pointer.scroll * SM_ROW_H;
            if (g_sm.scroll_y < 0) g_sm.scroll_y = 0;
            surface_invalidate(id);
        }
    }
    if (ev->type == INPUT_KEY && ev->key.down) {
        switch (ev->key.keycode) {
        case KEY_DOWN_ARROW:
            if (g_sm.selected_proc < g_sm.proc_count - 1)
                g_sm.selected_proc++;
            break;
        case KEY_UP_ARROW:
            if (g_sm.selected_proc > 0)
                g_sm.selected_proc--;
            break;
        }
        surface_invalidate(id);
    }
}

/* =========================================================
 * Init
 * ========================================================= */
void surface_sysmon_init(uint32_t w, uint32_t h)
{
    memset(&g_sm, 0, sizeof(g_sm));
    g_sm.surf_w = w;
    g_sm.surf_h = h;
    g_sm.selected_proc = -1;

    g_sm.sid = are_add_surface(SURF_APP, w, h,
                               "System Monitor", "M",
                               sm_render, sm_input, NULL, NULL);
}
