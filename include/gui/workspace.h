/*
 * include/gui/workspace.h — Virtual workspace (desktop) manager
 *
 * AetherOS supports 4 virtual workspaces (desktops).  Each workspace
 * owns a list of window IDs.  Switching workspaces hides all windows
 * on the departing workspace and shows all windows on the arriving one.
 *
 * Windows on workspace 0 are visible by default at boot.
 */
#pragma once
#include <types.h>
#include <gui/window.h>

#define WS_COUNT       4    /* Number of virtual workspaces */
#define WS_MAX_WINDOWS 32   /* Max windows per workspace    */

/* Initialize workspace manager (all windows start on workspace 0) */
void workspace_init(void);

/* Switch to workspace n (0 … WS_COUNT-1).
 * Hides all windows on current workspace, shows all on new workspace.
 * Returns false if n is out of range. */
bool workspace_switch(int n);

/* Return the currently active workspace index */
int  workspace_current(void);

/* Add a window to the current workspace (called after wm_create_window).
 * If wid is already tracked, does nothing. */
void workspace_add_window(wid_t wid);

/* Remove a window from all workspaces (called when window is closed) */
void workspace_remove_window(wid_t wid);

/* Move a window to a specific workspace.
 * Hides it if target != current, shows it if target == current. */
void workspace_move_window(wid_t wid, int target_ws);

/* Returns how many windows are on workspace n */
int  workspace_window_count(int ws);

/* Draw workspace indicator dots (called by desktop taskbar renderer).
 * Draws 4 small dots at (x, y) with w width and h height. */
void workspace_draw_indicator(void* canvas, int x, int y, int w, int h);
