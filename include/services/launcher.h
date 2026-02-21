/*
 * include/services/launcher.h — Aether OS Launcher Service
 */
#ifndef SERVICES_LAUNCHER_H
#define SERVICES_LAUNCHER_H

#include <types.h>
#include <gui/window.h>

void  launcher_init(void);
void  launcher_run(void);

/* Track an existing window in the taskbar */
void  launcher_track_window(wid_t wid, const char* label);

#endif /* SERVICES_LAUNCHER_H */
