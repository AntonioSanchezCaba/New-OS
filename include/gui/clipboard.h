/*
 * include/gui/clipboard.h — System-wide clipboard
 *
 * Provides a single shared text clipboard with optional binary data.
 * Thread-safe via a simple spin-lock (kernel single-threaded GUI context).
 */
#pragma once
#include <types.h>

#define CLIPBOARD_MAX_TEXT  65536   /* 64 KB text limit */

/* Copy text into clipboard (NUL-terminated). Returns 0 on success. */
int  clipboard_set_text(const char* text);

/* Set raw binary data. Returns 0 on success. */
int  clipboard_set_data(const void* data, int len);

/* Get pointer to clipboard text. Returns NULL if empty or not text.
 * Caller must NOT free the returned pointer. */
const char* clipboard_get_text(void);

/* Get clipboard data length. Returns 0 if empty. */
int  clipboard_get_length(void);

/* Clear clipboard */
void clipboard_clear(void);

/* Returns true if clipboard has text content */
bool clipboard_has_text(void);

/* Returns the clipboard generation counter (increments on each set/clear).
 * Widgets can poll this to detect changes. */
uint32_t clipboard_generation(void);
