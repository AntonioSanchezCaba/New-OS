/*
 * include/gui/theme.h - Aether OS UI Theme Engine
 *
 * Provides a unified color palette that can be switched between
 * dark and light modes at runtime. All GUI components should use
 * theme colors rather than hard-coded values.
 */
#ifndef GUI_THEME_H
#define GUI_THEME_H

#include <types.h>
#include <drivers/framebuffer.h>

/* Available themes */
typedef enum {
    THEME_DARK  = 0,
    THEME_LIGHT = 1,
} theme_id_t;

/* Accent palette */
typedef enum {
    ACCENT_BLUE   = 0,
    ACCENT_PURPLE = 1,
    ACCENT_GREEN  = 2,
    ACCENT_RED    = 3,
} accent_id_t;

/* Theme color structure — a complete palette */
typedef struct {
    /* Desktop */
    uint32_t desktop_bg;       /* Desktop background (top) */
    uint32_t desktop_bg2;      /* Desktop background (bottom gradient) */
    uint32_t desktop_grid;     /* Desktop grid lines */

    /* Taskbar */
    uint32_t taskbar_bg;       /* Taskbar background (top) */
    uint32_t taskbar_bg2;      /* Taskbar background (bottom gradient) */
    uint32_t taskbar_border;   /* Taskbar top border line */
    uint32_t taskbar_text;     /* Taskbar label text */
    uint32_t taskbar_clock;    /* Clock text color */

    /* Windows */
    uint32_t win_bg;           /* Window client background */
    uint32_t win_title_bg;     /* Focused titlebar background (top) */
    uint32_t win_title_bg2;    /* Focused titlebar background (bottom) */
    uint32_t win_title_inactive;/* Unfocused titlebar */
    uint32_t win_title_text;   /* Titlebar text */
    uint32_t win_border;       /* Window border */
    uint32_t win_close_btn;    /* Close button */

    /* Buttons */
    uint32_t btn_normal;       /* Normal button */
    uint32_t btn_hover;        /* Hovered button */
    uint32_t btn_active;       /* Pressed button */
    uint32_t btn_text;         /* Button label */

    /* Text */
    uint32_t text_primary;     /* Primary content text */
    uint32_t text_secondary;   /* Dimmer/secondary text */
    uint32_t text_disabled;    /* Disabled/placeholder text */
    uint32_t text_on_accent;   /* Text drawn on accent color */

    /* Panels & surfaces */
    uint32_t panel_bg;         /* Side/toolbar panel background */
    uint32_t panel_header;     /* Panel section header */
    uint32_t panel_border;     /* Panel separator */
    uint32_t row_alt;          /* Alternating table row */
    uint32_t selection;        /* Selected item background */
    uint32_t selection_text;   /* Selected item text */

    /* Accent / status */
    uint32_t accent;           /* Primary accent color */
    uint32_t accent2;          /* Secondary accent (darker) */
    uint32_t ok;               /* Success/ok color */
    uint32_t warn;             /* Warning color */
    uint32_t error;            /* Error color */

    /* Notifications */
    uint32_t notif_bg;         /* Notification popup background */
    uint32_t notif_border;     /* Notification border */
    uint32_t notif_text;       /* Notification text */
    uint32_t notif_info;       /* Info notification accent */
    uint32_t notif_warn;       /* Warning notification accent */
    uint32_t notif_error;      /* Error notification accent */

    /* Splash / Login */
    uint32_t splash_bg;        /* Splash screen background */
    uint32_t splash_logo;      /* Logo text color */
    uint32_t splash_bar_bg;    /* Progress bar track */
    uint32_t splash_bar_fill;  /* Progress bar fill */
    uint32_t splash_text;      /* Splash subtitle text */

    uint32_t login_bg;         /* Login screen background */
    uint32_t login_box;        /* Login panel background */
    uint32_t login_box_border; /* Login panel border */
    uint32_t login_field_bg;   /* Input field background */
    uint32_t login_field_border;/* Input field border */
    uint32_t login_text;       /* Login label text */
    uint32_t login_cursor;     /* Text cursor in field */

    /* Name */
    const char* name;
} theme_t;

/* ---- API ---- */

/* Initialize theme engine with default (dark) theme */
void theme_init(void);

/* Switch to a different theme */
void theme_set(theme_id_t id);
void theme_set_accent(accent_id_t accent);

/* Get the current theme palette (read-only) */
const theme_t* theme_current(void);

/* Query current theme id */
theme_id_t theme_get_id(void);
accent_id_t theme_get_accent(void);

/* Get a named color from the current theme */
#define THEME_COLOR(field) (theme_current()->field)

/* Convenience macros */
#define TC_DESKTOP_BG        THEME_COLOR(desktop_bg)
#define TC_TASKBAR_BG        THEME_COLOR(taskbar_bg)
#define TC_WIN_BG            THEME_COLOR(win_bg)
#define TC_WIN_TITLE         THEME_COLOR(win_title_bg)
#define TC_WIN_TITLE2        THEME_COLOR(win_title_bg2)
#define TC_WIN_BORDER        THEME_COLOR(win_border)
#define TC_TEXT              THEME_COLOR(text_primary)
#define TC_TEXT2             THEME_COLOR(text_secondary)
#define TC_ACCENT            THEME_COLOR(accent)
#define TC_PANEL             THEME_COLOR(panel_bg)
#define TC_SELECTION         THEME_COLOR(selection)
#define TC_SEL_TEXT          THEME_COLOR(selection_text)
#define TC_BTN               THEME_COLOR(btn_normal)
#define TC_BTN_HOVER         THEME_COLOR(btn_hover)
#define TC_OK                THEME_COLOR(ok)
#define TC_WARN              THEME_COLOR(warn)
#define TC_ERROR             THEME_COLOR(error)

#endif /* GUI_THEME_H */
