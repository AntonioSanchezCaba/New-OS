/*
 * gui/theme.c - Aether OS UI Theme Engine
 *
 * Two built-in themes: Dark (default) and Light.
 * Accent colors can be switched independently.
 */
#include <gui/theme.h>
#include <string.h>

/* ---- Dark theme palette ---- */
static const theme_t theme_dark = {
    .name = "Aether Dark",

    /* Desktop */
    .desktop_bg        = 0xFF1A2336,
    .desktop_bg2       = 0xFF0D1520,
    .desktop_grid      = 0x14FFFFFF,   /* very translucent white */

    /* Taskbar */
    .taskbar_bg        = 0xFF0E1625,
    .taskbar_bg2       = 0xFF080E18,
    .taskbar_border    = 0xFF2A6AAE,
    .taskbar_text      = 0xFFD8E8F8,
    .taskbar_clock     = 0xFFAAD4F8,

    /* Windows */
    .win_bg            = 0xFFF2F4F8,
    .win_title_bg      = 0xFF1E4A82,
    .win_title_bg2     = 0xFF163870,
    .win_title_inactive= 0xFF2C3A52,
    .win_title_text    = 0xFFF0F4FF,
    .win_border        = 0xFF1A3A6A,
    .win_close_btn     = 0xFFC03838,

    /* Buttons */
    .btn_normal        = 0xFF2A6AAE,
    .btn_hover         = 0xFF3A82CC,
    .btn_active        = 0xFF1A5090,
    .btn_text          = 0xFFF0F4FF,

    /* Text */
    .text_primary      = 0xFF0E1020,
    .text_secondary    = 0xFF505870,
    .text_disabled     = 0xFF909098,
    .text_on_accent    = 0xFFF0F4FF,

    /* Panels */
    .panel_bg          = 0xFFE4E8F4,
    .panel_header      = 0xFF1A4A84,
    .panel_border      = 0xFFC8D0E0,
    .row_alt           = 0xFFF8FAFF,
    .selection         = 0xFF1E5AAA,
    .selection_text    = 0xFFF0F4FF,

    /* Accent */
    .accent            = 0xFF007ACC,
    .accent2           = 0xFF005FA0,
    .ok                = 0xFF3AAA50,
    .warn              = 0xFFCCA020,
    .error             = 0xFFCC3030,

    /* Notifications */
    .notif_bg          = 0xFF252E42,
    .notif_border      = 0xFF3A6AAE,
    .notif_text        = 0xFFD8E4F4,
    .notif_info        = 0xFF2A82CC,
    .notif_warn        = 0xFFCCA020,
    .notif_error       = 0xFFCC3030,

    /* Splash */
    .splash_bg         = 0xFF0A1020,
    .splash_logo       = 0xFF60C0FF,
    .splash_bar_bg     = 0xFF1A2840,
    .splash_bar_fill   = 0xFF007ACC,
    .splash_text       = 0xFF809AB8,

    /* Login */
    .login_bg          = 0xFF0E1828,
    .login_box         = 0xFF182438,
    .login_box_border  = 0xFF2A5A9E,
    .login_field_bg    = 0xFF0E1828,
    .login_field_border= 0xFF3A6AAE,
    .login_text        = 0xFFB0C8E8,
    .login_cursor      = 0xFF60C0FF,
};

/* ---- Light theme palette ---- */
static const theme_t theme_light = {
    .name = "Aether Light",

    /* Desktop */
    .desktop_bg        = 0xFF5B8AC8,
    .desktop_bg2       = 0xFF3A6AA8,
    .desktop_grid      = 0x12000050,

    /* Taskbar */
    .taskbar_bg        = 0xFFE8EEF8,
    .taskbar_bg2       = 0xFFD8E2F0,
    .taskbar_border    = 0xFF5A8AC8,
    .taskbar_text      = 0xFF182848,
    .taskbar_clock     = 0xFF204880,

    /* Windows */
    .win_bg            = 0xFFFFFFFF,
    .win_title_bg      = 0xFF3A78C0,
    .win_title_bg2     = 0xFF2A60A8,
    .win_title_inactive= 0xFFAEB8C8,
    .win_title_text    = 0xFFF8FBFF,
    .win_border        = 0xFF5888C0,
    .win_close_btn     = 0xFFD02828,

    /* Buttons */
    .btn_normal        = 0xFF3A78C0,
    .btn_hover         = 0xFF4A88D0,
    .btn_active        = 0xFF2A60A8,
    .btn_text          = 0xFFF8FBFF,

    /* Text */
    .text_primary      = 0xFF101820,
    .text_secondary    = 0xFF485868,
    .text_disabled     = 0xFF909898,
    .text_on_accent    = 0xFFF8FBFF,

    /* Panels */
    .panel_bg          = 0xFFF0F4FA,
    .panel_header      = 0xFF3A78C0,
    .panel_border      = 0xFFD0D8E8,
    .row_alt           = 0xFFF8FAFF,
    .selection         = 0xFF2A72C0,
    .selection_text    = 0xFFF8FBFF,

    /* Accent */
    .accent            = 0xFF0070BB,
    .accent2           = 0xFF005898,
    .ok                = 0xFF289A40,
    .warn              = 0xFFB88010,
    .error             = 0xFFB82020,

    /* Notifications */
    .notif_bg          = 0xFFF8FAFF,
    .notif_border      = 0xFF3A78C0,
    .notif_text        = 0xFF101820,
    .notif_info        = 0xFF2070BB,
    .notif_warn        = 0xFFB88010,
    .notif_error       = 0xFFB82020,

    /* Splash */
    .splash_bg         = 0xFF2A4A7C,
    .splash_logo       = 0xFFF0F8FF,
    .splash_bar_bg     = 0xFF3A5A8E,
    .splash_bar_fill   = 0xFFF0F8FF,
    .splash_text       = 0xFFB0C8E0,

    /* Login */
    .login_bg          = 0xFF3A5A8E,
    .login_box         = 0xFFEEF4FF,
    .login_box_border  = 0xFF4A78C0,
    .login_field_bg    = 0xFFFFFFFF,
    .login_field_border= 0xFF3A78C0,
    .login_text        = 0xFF182848,
    .login_cursor      = 0xFF0060CC,
};

/* ---- Runtime state ---- */
static theme_id_t  g_theme_id  = THEME_DARK;
static accent_id_t g_accent_id = ACCENT_BLUE;
static theme_t     g_current;

/* Accent override palettes (only the accent-related fields) */
static const uint32_t accent_colors[][2] = {
    /* [ACCENT_BLUE]   accent, accent2 */
    { 0xFF007ACC, 0xFF005FA0 },
    /* [ACCENT_PURPLE] */
    { 0xFF8040C0, 0xFF6030A0 },
    /* [ACCENT_GREEN]  */
    { 0xFF28A050, 0xFF1A8040 },
    /* [ACCENT_RED]    */
    { 0xFFC03030, 0xFF9A2020 },
};

static void apply_accent(void)
{
    g_current.accent  = accent_colors[g_accent_id][0];
    g_current.accent2 = accent_colors[g_accent_id][1];
    /* Update dependent colors */
    g_current.btn_normal   = g_current.accent;
    g_current.btn_hover    = accent_colors[g_accent_id][0] + 0x00101010u;
    g_current.btn_active   = g_current.accent2;
    g_current.win_title_bg = (g_theme_id == THEME_DARK)
                             ? 0xFF1E3A72 : 0xFF2A68B8;
    g_current.splash_bar_fill = g_current.accent;
    g_current.notif_info      = g_current.accent;
    g_current.selection       = g_current.accent;
}

void theme_init(void)
{
    g_theme_id  = THEME_LIGHT;
    g_accent_id = ACCENT_BLUE;
    g_current   = theme_light;
    apply_accent();
}

void theme_set(theme_id_t id)
{
    g_theme_id = id;
    if (id == THEME_DARK)  g_current = theme_dark;
    else                   g_current = theme_light;
    apply_accent();
}

void theme_set_accent(accent_id_t accent)
{
    if ((int)accent < 0 || accent > ACCENT_RED) return;
    g_accent_id = accent;
    apply_accent();
}

const theme_t* theme_current(void)
{
    return &g_current;
}

theme_id_t theme_get_id(void)
{
    return g_theme_id;
}

accent_id_t theme_get_accent(void)
{
    return g_accent_id;
}
