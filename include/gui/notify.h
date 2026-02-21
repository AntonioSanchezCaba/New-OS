/*
 * include/gui/notify.h - System notification popup system
 *
 * Non-blocking toast notifications that appear in the top-right corner
 * and auto-dismiss after a configurable duration.
 */
#ifndef GUI_NOTIFY_H
#define GUI_NOTIFY_H

#include <types.h>
#include <gui/draw.h>

/* Notification types */
typedef enum {
    NOTIFY_INFO  = 0,
    NOTIFY_WARN  = 1,
    NOTIFY_ERROR = 2,
} notify_type_t;

/* Display duration */
#define NOTIFY_DURATION_SHORT   150   /* ~1.5s at 100Hz */
#define NOTIFY_DURATION_NORMAL  300   /* ~3s */
#define NOTIFY_DURATION_LONG    600   /* ~6s */
#define NOTIFY_MAX              5

/* Initialize the notification system */
void notify_init(void);

/* Post a notification (fire-and-forget) */
void notify_post(notify_type_t type, const char* title, const char* body);

/* Short helpers */
static inline void notify_info(const char* title, const char* body)
    { notify_post(NOTIFY_INFO, title, body); }
static inline void notify_warn(const char* title, const char* body)
    { notify_post(NOTIFY_WARN, title, body); }
static inline void notify_error(const char* title, const char* body)
    { notify_post(NOTIFY_ERROR, title, body); }

/* Called every GUI frame — ages and draws active notifications */
void notify_tick(canvas_t* screen);

/* Dismiss all notifications */
void notify_clear(void);

#endif /* GUI_NOTIFY_H */
