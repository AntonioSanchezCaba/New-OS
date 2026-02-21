/*
 * include/services/splash.h - Boot splash screen
 *
 * Displays a branded splash screen while the OS finishes initializing.
 * The progress bar can be advanced from kernel init code.
 */
#ifndef SERVICES_SPLASH_H
#define SERVICES_SPLASH_H

#include <types.h>

/* Run the boot splash (blocks until complete or timeout) */
void splash_run(void);

/* Advance progress bar (0-100) — can be called from init phases */
void splash_set_progress(int pct, const char* status_msg);

#endif /* SERVICES_SPLASH_H */
