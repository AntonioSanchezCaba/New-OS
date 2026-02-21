/*
 * apps/stress_test.c - Aether OS Scheduler & Memory Stress Tester
 *
 * Spawns N worker kernel processes that perform CPU-bound loops,
 * memory allocation/free cycles, and voluntary yields to verify:
 *   1. Scheduler fairness: each worker accumulates ticks proportionally.
 *   2. No starvation: all workers make forward progress.
 *   3. Memory safety: rapid alloc/free does not corrupt the heap.
 *   4. Context switching: hundreds of switches per second without crash.
 *
 * The stress test renders a live status window showing per-worker
 * tick counts, heap usage, and a progress bar for each worker.
 *
 * Usage: gui_launch_stress_test() from the desktop/launcher.
 */
#include <gui/gui.h>
#include <gui/widgets.h>
#include <process.h>
#include <scheduler.h>
#include <memory.h>
#include <drivers/timer.h>
#include <string.h>

/* Number of worker processes */
#define STRESS_WORKERS    6

/* Allocations per worker per cycle */
#define STRESS_ALLOCS     16
#define STRESS_ALLOC_SIZE 256

/* Window dimensions */
#define STRESS_W  460
#define STRESS_H  380

/* =========================================================
 * Shared state (written by workers, read by GUI)
 * ========================================================= */

typedef struct {
    uint64_t  tick_count;     /* Total scheduler ticks given to this worker */
    uint64_t  alloc_count;    /* Total successful kmalloc calls */
    uint64_t  free_count;     /* Total kfree calls */
    uint64_t  cpu_iters;      /* CPU burn iterations */
    pid_t     pid;
    bool      running;
    char      name[16];
} stress_worker_t;

static stress_worker_t g_workers[STRESS_WORKERS];
static bool            g_test_running = false;
static uint64_t        g_start_tick   = 0;

/* =========================================================
 * Worker process entry points
 * ========================================================= */

/*
 * Each worker is parameterized by its index.  Since process_create()
 * takes a void(*)(void) we use separate entry stubs.
 */
static void worker_body(int idx)
{
    stress_worker_t* w = &g_workers[idx];
    w->running = true;

    /* Allocation scratch buffer */
    void* ptrs[STRESS_ALLOCS];
    for (int i = 0; i < STRESS_ALLOCS; i++) ptrs[i] = NULL;

    while (g_test_running) {
        /* CPU burn */
        volatile uint64_t sum = 0;
        for (int i = 0; i < 10000; i++) sum += (uint64_t)i * (uint64_t)(i + 1);
        (void)sum;
        w->cpu_iters++;

        /* Memory alloc / free cycle */
        for (int i = 0; i < STRESS_ALLOCS; i++) {
            if (ptrs[i]) {
                kfree(ptrs[i]);
                ptrs[i] = NULL;
                w->free_count++;
            }
            ptrs[i] = kmalloc(STRESS_ALLOC_SIZE);
            if (ptrs[i]) {
                /* Write a pattern to catch corruption */
                memset(ptrs[i], (uint8_t)(idx + 1), STRESS_ALLOC_SIZE);
                w->alloc_count++;
            }
        }

        /* Track scheduler ticks */
        process_t* self = current_process;
        if (self) w->tick_count = self->total_ticks;

        scheduler_yield();
    }

    /* Clean up allocations */
    for (int i = 0; i < STRESS_ALLOCS; i++) {
        if (ptrs[i]) { kfree(ptrs[i]); ptrs[i] = NULL; }
    }
    w->running = false;
}

/* Separate entry stubs for each worker (no closures in C89/C11) */
static void worker0(void) { worker_body(0); }
static void worker1(void) { worker_body(1); }
static void worker2(void) { worker_body(2); }
static void worker3(void) { worker_body(3); }
static void worker4(void) { worker_body(4); }
static void worker5(void) { worker_body(5); }

static void (*worker_entries[STRESS_WORKERS])(void) = {
    worker0, worker1, worker2, worker3, worker4, worker5
};

/* =========================================================
 * GUI State
 * ========================================================= */

typedef struct {
    wid_t          wid;
    widget_group_t wg;
    bool           started;
    uint32_t       last_refresh;
} stress_app_t;

static stress_app_t g_stress;

/* Widget IDs */
#define WID_START_BTN   100
#define WID_STOP_BTN    101
#define WID_STATUS_LBL  102
/* Worker progress bars: 110 + idx */
/* Worker label: 120 + idx */

/* =========================================================
 * GUI rendering
 * ========================================================= */

static void stress_build_ui(stress_app_t* app)
{
    widget_group_init(&app->wg);

    /* Header label */
    widget_add_label(&app->wg, 8, 4, 360, 20,
                     "Scheduler & Memory Stress Test", WID_STATUS_LBL);

    /* Start/Stop buttons */
    widget_add_button(&app->wg, 8,   28, 100, 28, "Start Test", WID_START_BTN);
    widget_add_button(&app->wg, 116, 28, 100, 28, "Stop Test",  WID_STOP_BTN);

    /* Per-worker rows */
    for (int i = 0; i < STRESS_WORKERS; i++) {
        int row_y = 70 + i * 48;
        char nm[16];
        strncpy(nm, g_workers[i].name, sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = '\0';
        widget_add_label(&app->wg, 8, row_y, 80, 18, nm, 120 + i);
        widget_add_progressbar(&app->wg, 90, row_y + 2, 340, 14,
                               0, 1000, 110 + i);
    }
}

static void stress_update_ui(stress_app_t* app)
{
    /* Find max tick count for normalization */
    uint64_t max_ticks = 1;
    for (int i = 0; i < STRESS_WORKERS; i++) {
        if (g_workers[i].tick_count > max_ticks)
            max_ticks = g_workers[i].tick_count;
    }

    for (int i = 0; i < STRESS_WORKERS; i++) {
        /* Progress bar: proportion of ticks relative to max */
        int pct = (int)(g_workers[i].tick_count * 1000 / max_ticks);
        widget_set_progress(&app->wg, 110 + i, pct);
    }

    /* Status label */
    uint32_t elapsed = (timer_get_ticks() - g_start_tick) / TIMER_FREQ;
    char status[64];
    const char* state = g_test_running ? "RUNNING" : "IDLE";
    /* Format: "Status: RUNNING  Elapsed: 12s" */
    int si = 0;
    const char* s1 = "Status: ";
    while (*s1 && si < 60) status[si++] = *s1++;
    const char* s2 = state;
    while (*s2 && si < 60) status[si++] = *s2++;
    const char* s3 = "  Elapsed: ";
    while (*s3 && si < 60) status[si++] = *s3++;
    status[si++] = '0' + (char)(elapsed / 10);
    status[si++] = '0' + (char)(elapsed % 10);
    status[si++] = 's';
    status[si]   = '\0';
    widget_set_text(&app->wg, WID_STATUS_LBL, status);
}

static void stress_draw(wid_t wid)
{
    stress_app_t* app = &g_stress;
    canvas_t cv = wm_client_canvas(wid);
    const theme_t* th = theme_current();

    draw_rect(&cv, 0, 0, STRESS_W, STRESS_H, th->win_bg);

    /* Separator under buttons */
    draw_hline(&cv, 0, 62, STRESS_W, th->panel_border);

    /* Worker detail rows */
    for (int i = 0; i < STRESS_WORKERS; i++) {
        int row_y = 70 + i * 48;
        stress_worker_t* w = &g_workers[i];

        /* Row background */
        uint32_t row_bg = (i & 1) ? th->row_alt : th->win_bg;
        draw_rect(&cv, 0, row_y - 2, STRESS_W, 44, row_bg);

        /* Tick count */
        char tbuf[24];
        uint64_t tc = w->tick_count;
        int ti = 0;
        if (tc == 0) {
            tbuf[ti++] = '0';
        } else {
            char tmp[20]; int tl = 0;
            while (tc > 0 && tl < 18) { tmp[tl++] = '0' + (char)(tc % 10); tc /= 10; }
            for (int j = tl - 1; j >= 0; j--) tbuf[ti++] = tmp[j];
        }
        tbuf[ti++] = 't'; tbuf[ti++] = 'k'; tbuf[ti] = '\0';
        draw_string(&cv, 8, row_y + 22, tbuf,
                    w->running ? th->ok : th->text_disabled, rgba(0, 0, 0, 0));

        /* Alloc count */
        uint64_t ac = w->alloc_count;
        char abuf[20]; int ai = 0;
        if (ac == 0) { abuf[ai++] = '0'; }
        else { char tmp[18]; int al = 0;
               while (ac > 0 && al < 16) { tmp[al++] = '0' + (char)(ac % 10); ac /= 10; }
               for (int j = al - 1; j >= 0; j--) abuf[ai++] = tmp[j]; }
        abuf[ai++] = 'A'; abuf[ai] = '\0';
        draw_string(&cv, 90, row_y + 22, abuf, th->text_secondary, rgba(0, 0, 0, 0));
    }

    /* Heap usage */
    int hy = 70 + STRESS_WORKERS * 48 + 8;
    size_t used = kheap_used();
    size_t free_sz = kheap_free();
    size_t total = used + free_sz;
    if (total == 0) total = 1;

    draw_string(&cv, 8, hy, "Kernel Heap:", th->text_secondary, rgba(0, 0, 0, 0));
    draw_rect(&cv, 8, hy + 20, STRESS_W - 16, 14, th->panel_border);
    int fill = (int)((STRESS_W - 18) * used / total);
    if (fill > 0)
        draw_rect(&cv, 9, hy + 21, fill, 12, th->warn);

    char hbuf[48];
    int hi = 0;
    /* Format: "used / total KB" */
    uint64_t uk = used / 1024;
    uint64_t tk2 = total / 1024;
    /* Simple itoa */
    {
        char tmp[12]; int tl = 0;
        if (uk == 0) { hbuf[hi++] = '0'; }
        else { uint64_t v = uk;
               while (v > 0 && tl < 10) { tmp[tl++] = '0' + (char)(v % 10); v /= 10; }
               for (int j = tl - 1; j >= 0; j--) hbuf[hi++] = tmp[j]; }
    }
    hbuf[hi++] = 'K'; hbuf[hi++] = ' '; hbuf[hi++] = '/'; hbuf[hi++] = ' ';
    {
        char tmp[12]; int tl = 0;
        if (tk2 == 0) { hbuf[hi++] = '0'; }
        else { uint64_t v = tk2;
               while (v > 0 && tl < 10) { tmp[tl++] = '0' + (char)(v % 10); v /= 10; }
               for (int j = tl - 1; j >= 0; j--) hbuf[hi++] = tmp[j]; }
    }
    hbuf[hi++] = 'K'; hbuf[hi] = '\0';
    draw_string(&cv, 8, hy + 38, hbuf, th->text_primary, rgba(0, 0, 0, 0));

    widget_group_draw(&cv, &app->wg);
}

/* =========================================================
 * Test control
 * ========================================================= */

static void stress_start(void)
{
    if (g_test_running) return;

    for (int i = 0; i < STRESS_WORKERS; i++) {
        memset(&g_workers[i], 0, sizeof(g_workers[i]));
        g_workers[i].name[0] = 'W';
        g_workers[i].name[1] = 'k';
        g_workers[i].name[2] = 'r';
        g_workers[i].name[3] = '0' + (char)i;
        g_workers[i].name[4] = '\0';
    }

    g_test_running = true;
    g_start_tick   = timer_get_ticks();

    for (int i = 0; i < STRESS_WORKERS; i++) {
        char pname[24];
        pname[0] = 's'; pname[1] = 't'; pname[2] = 'r'; pname[3] = 'e';
        pname[4] = 's'; pname[5] = 's'; pname[6] = '_';
        pname[7] = '0' + (char)i; pname[8] = '\0';
        process_t* p = process_create(pname, worker_entries[i], true);
        if (p) {
            g_workers[i].pid = p->pid;
            scheduler_add(p);
        }
    }
}

static void stress_stop(void)
{
    g_test_running = false;
    /* Workers will detect g_test_running == false and exit */
}

/* =========================================================
 * Window event handler
 * ========================================================= */

static void stress_on_event(wid_t wid, gui_event_t* evt, void* ud)
{
    (void)ud;
    stress_app_t* app = &g_stress;

    if (evt->type == GUI_EVENT_PAINT) {
        stress_update_ui(app);
        stress_draw(wid);
        return;
    }

    if (evt->type == GUI_EVENT_CLOSE) {
        stress_stop();
        app->wid = -1;
        return;
    }

    uint32_t triggered = 0;
    if (widget_group_handle_event(&app->wg, evt, &triggered)) {
        if (triggered == WID_START_BTN) stress_start();
        if (triggered == WID_STOP_BTN)  stress_stop();
        wm_invalidate(wid);
    }
}

/* =========================================================
 * Public launch
 * ========================================================= */

void gui_launch_stress_test(void)
{
    stress_app_t* app = &g_stress;
    if (app->wid > 0 && wm_get_window(app->wid)) {
        wm_raise(app->wid);
        return;
    }

    stress_build_ui(app);

    app->wid = wm_create_window("Stress Test", 80, 80, STRESS_W, STRESS_H,
                                stress_on_event, NULL);
    if (app->wid > 0) {
        taskbar_add(app->wid, "Stress Test");
    }
}

/* Called each desktop frame to refresh display */
void stress_tick(void)
{
    stress_app_t* app = &g_stress;
    if (app->wid <= 0) return;
    if (!wm_get_window(app->wid)) { app->wid = -1; return; }
    uint32_t now = timer_get_ticks();
    if (now - app->last_refresh >= (uint32_t)(TIMER_FREQ / 4)) {
        app->last_refresh = now;
        wm_invalidate(app->wid);
    }
}
