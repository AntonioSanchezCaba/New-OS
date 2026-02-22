/*
 * gui/clipboard.c — System-wide text clipboard
 */
#include <gui/clipboard.h>
#include <memory.h>
#include <string.h>
#include <types.h>

static struct {
    char*    buf;
    int      len;
    bool     is_text;
    uint32_t gen;
} g_clip = { NULL, 0, false, 0 };

int clipboard_set_text(const char* text)
{
    if (!text) { clipboard_clear(); return 0; }
    int n = (int)strlen(text);
    if (n >= CLIPBOARD_MAX_TEXT) n = CLIPBOARD_MAX_TEXT - 1;

    if (g_clip.buf) kfree(g_clip.buf);
    g_clip.buf = (char*)kmalloc((size_t)(n + 1));
    if (!g_clip.buf) return -1;

    memcpy(g_clip.buf, text, (size_t)n);
    g_clip.buf[n] = '\0';
    g_clip.len     = n;
    g_clip.is_text = true;
    g_clip.gen++;
    return 0;
}

int clipboard_set_data(const void* data, int len)
{
    if (!data || len <= 0) { clipboard_clear(); return 0; }
    if (len > CLIPBOARD_MAX_TEXT) len = CLIPBOARD_MAX_TEXT;

    if (g_clip.buf) kfree(g_clip.buf);
    g_clip.buf = (char*)kmalloc((size_t)len);
    if (!g_clip.buf) return -1;

    memcpy(g_clip.buf, data, (size_t)len);
    g_clip.len     = len;
    g_clip.is_text = false;
    g_clip.gen++;
    return 0;
}

const char* clipboard_get_text(void)
{
    if (!g_clip.is_text || !g_clip.buf) return NULL;
    return g_clip.buf;
}

int clipboard_get_length(void) { return g_clip.len; }

void clipboard_clear(void)
{
    if (g_clip.buf) { kfree(g_clip.buf); g_clip.buf = NULL; }
    g_clip.len     = 0;
    g_clip.is_text = false;
    g_clip.gen++;
}

bool     clipboard_has_text(void)  { return g_clip.is_text && g_clip.buf != NULL; }
uint32_t clipboard_generation(void){ return g_clip.gen; }
