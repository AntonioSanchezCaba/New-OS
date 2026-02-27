/*
 * services/splash.c - Aether OS boot splash screen
 *
 * Renders a full-screen branded splash with a progress bar.
 * Runs as a blocking call in gui_run() before the desktop loads.
 */
#include <services/splash.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/timer.h>
#include <scheduler.h>
#include <string.h>

#define SPLASH_DURATION_MS   2400   /* Total splash time in ~ticks */
#define SPLASH_BAR_W         320
#define SPLASH_BAR_H         6

/* Boot phases shown in the progress bar */
static const char* g_phases[] = {
    "Initializing memory...",
    "Loading drivers...",
    "Starting services...",
    "Mounting filesystems...",
    "Starting compositor...",
    "Launching desktop...",
    NULL
};

static int g_progress = 0;
static const char* g_status = "Booting...";

void splash_set_progress(int pct, const char* msg)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    g_progress = pct;
    if (msg) g_status = msg;
}

static void splash_draw_frame(canvas_t* screen, int progress_pct)
{
    const theme_t* th = theme_current();
    int W = screen->width;
    int H = screen->height;
    int cx = W / 2;
    int cy = H / 2;

    /* Full-screen gradient background */
    draw_gradient_v(screen, 0, 0, W, H, th->splash_bg,
                    th->splash_bg + 0x00080808u);

    /* Subtle radial glow effect — concentric circles at center */
    for (int r = 240; r >= 60; r -= 30) {
        uint32_t glow = rgba(30, 90, 160, (uint8_t)(10 + (240 - r) / 6));
        draw_circle(screen, cx, cy - 40, r, glow);
    }

    /* OS logo text — large ASCII art style */
    /* Line 1: stylized "A" */
    const char* logo[] = {
        " ___       _   _               ",
        "/ _ \\ ___ | |_| |__   ___ _ __ ",
        "| |_) / _ \\| __| '_ \\ / _ \\ '__|",
        "|  _ <  __/| |_| | | |  __/ |  ",
        "|_| \\_\\___|\\__|_| |_|\\___|_|  ",
        NULL
    };

    int logo_y = cy - 120;
    for (int i = 0; logo[i]; i++) {
        int lw = (int)strlen(logo[i]) * FONT_W;
        draw_string(screen, cx - lw / 2, logo_y + i * FONT_H,
                    logo[i], th->splash_logo, rgba(0,0,0,0));
    }

    /* OS version subtitle */
    const char* sub1 = OS_BANNER;
    const char* sub2 = OS_TAGLINE;
    int sub1_w = (int)strlen(sub1) * FONT_W;
    int sub2_w = (int)strlen(sub2) * FONT_W;

    int sub_y = logo_y + 5 * FONT_H + 16;
    draw_string(screen, cx - sub1_w / 2, sub_y, sub1,
                th->accent, rgba(0,0,0,0));
    draw_string(screen, cx - sub2_w / 2, sub_y + FONT_H + 4, sub2,
                th->splash_text, rgba(0,0,0,0));

    /* Separator line */
    draw_hline(screen, cx - 160, sub_y + FONT_H * 2 + 12, 320,
               rgba(0x60, 0x90, 0xC0, 0x50));

    /* Progress bar track */
    int bar_x = cx - SPLASH_BAR_W / 2;
    int bar_y = cy + 90;
    draw_rect(screen, bar_x, bar_y, SPLASH_BAR_W, SPLASH_BAR_H,
              th->splash_bar_bg);
    draw_rect_outline(screen, bar_x, bar_y, SPLASH_BAR_W, SPLASH_BAR_H, 1,
                      rgba(0x60, 0xA0, 0xD0, 0x60));

    /* Progress bar fill */
    int fill = (int)((uint64_t)progress_pct * (SPLASH_BAR_W - 2) / 100);
    if (fill > 0) {
        draw_rect(screen, bar_x + 1, bar_y + 1, fill, SPLASH_BAR_H - 2,
                  th->splash_bar_fill);
        /* Bright leading edge */
        if (fill > 3)
            draw_rect(screen, bar_x + fill - 2, bar_y + 1, 2, SPLASH_BAR_H - 2,
                      th->splash_logo);
    }

    /* Status message below bar */
    int sm_w = (int)strlen(g_status) * FONT_W;
    draw_string(screen, cx - sm_w / 2, bar_y + SPLASH_BAR_H + 8,
                g_status, th->splash_text, rgba(0,0,0,0));

    /* Copyright footer */
    const char* copy = OS_COPYRIGHT;
    int copy_w = (int)strlen(copy) * FONT_W;
    draw_string(screen, cx - copy_w / 2, H - FONT_H - 12,
                copy, rgba(0x30, 0x50, 0x80, 0x80), rgba(0,0,0,0));
}

void splash_run(void)
{
    if (!fb_ready()) return;

    canvas_t screen = draw_main_canvas();

    /* Animate through phases */
    uint32_t total_ticks = (uint32_t)(SPLASH_DURATION_MS * TIMER_FREQ / 1000);

    /* Count phases */
    int phase_count = 0;
    while (g_phases[phase_count]) phase_count++;

    for (int phase = 0; phase <= phase_count; phase++) {
        if (phase < phase_count)
            g_status = g_phases[phase];

        /* Animate progress smoothly within each phase segment */
        int pct_start = phase * 100 / (phase_count + 1);
        int pct_end   = (phase + 1) * 100 / (phase_count + 1);

        uint32_t phase_ticks = total_ticks / (uint32_t)(phase_count + 1);
        uint32_t phase_start = timer_get_ticks();

        while (1) {
            uint32_t elapsed = timer_get_ticks() - phase_start;
            if (elapsed >= phase_ticks) break;

            int pct = pct_start +
                      (int)((uint64_t)(pct_end - pct_start) * elapsed / phase_ticks);
            g_progress = pct;

            splash_draw_frame(&screen, pct);
            fb_flip();
            scheduler_yield();
        }
    }

    /* Final: full bar */
    g_progress = 100;
    g_status   = OS_BOOT_WELCOME;
    splash_draw_frame(&screen, 100);
    fb_flip();

    /* Hold briefly */
    uint32_t hold_start = timer_get_ticks();
    while (timer_get_ticks() - hold_start < (uint32_t)(TIMER_FREQ / 3)) {
        scheduler_yield();
    }
}
