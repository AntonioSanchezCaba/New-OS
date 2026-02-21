/*
 * include/services/login.h - Aether OS login screen
 *
 * Displays a graphical login screen before the desktop loads.
 * Returns when the user has successfully authenticated.
 */
#ifndef SERVICES_LOGIN_H
#define SERVICES_LOGIN_H

#include <types.h>

/* Returned username (valid after login_run returns) */
extern char login_username[64];

/*
 * Show the login screen. Blocks until the user enters valid credentials.
 * For v0.1, accepts any non-empty username/password.
 */
void login_run(void);

#endif /* SERVICES_LOGIN_H */
