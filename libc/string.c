/*
 * libc/string.c - String and memory manipulation functions
 *
 * These implementations are used by both the kernel and userland.
 * They are intentionally simple and do not depend on any external libraries.
 */
#include <string.h>
#include <types.h>

/* ============================================================
 * Memory functions
 * ============================================================ */

void* memset(void* dest, int c, size_t n)
{
    uint8_t* d = (uint8_t*)dest;
    uint8_t  v = (uint8_t)c;

    /* Fast word-aligned fill for large regions */
    while (n > 0 && ((uintptr_t)d & 7)) {
        *d++ = v;
        n--;
    }

    uint64_t fill64 = (uint64_t)v | ((uint64_t)v << 8) | ((uint64_t)v << 16) |
                      ((uint64_t)v << 24) | ((uint64_t)v << 32) |
                      ((uint64_t)v << 40) | ((uint64_t)v << 48) |
                      ((uint64_t)v << 56);

    uint64_t* d64 = (uint64_t*)d;
    while (n >= 8) {
        *d64++ = fill64;
        n -= 8;
    }

    d = (uint8_t*)d64;
    while (n > 0) {
        *d++ = v;
        n--;
    }

    return dest;
}

void* memcpy(void* dest, const void* src, size_t n)
{
    /* Handle overlap via memmove if needed */
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    /* Word-aligned fast path */
    if (((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0) {
        uint64_t* d64       = (uint64_t*)d;
        const uint64_t* s64 = (const uint64_t*)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        d = (uint8_t*)d64;
        s = (const uint8_t*)s64;
    }

    while (n > 0) {
        *d++ = *s++;
        n--;
    }

    return dest;
}

void* memmove(void* dest, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (d == s || n == 0) return dest;

    if (d < s || d >= s + n) {
        /* No overlap or dest is before src: forward copy */
        return memcpy(dest, src, n);
    } else {
        /* Overlap: backward copy */
        d += n;
        s += n;
        while (n > 0) {
            *--d = *--s;
            n--;
        }
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n > 0) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++; n--;
    }
    return 0;
}

/* ============================================================
 * String functions
 * ============================================================ */

size_t strlen(const char* str)
{
    const char* p = str;
    while (*p) p++;
    return (size_t)(p - str);
}

char* strcpy(char* dest, const char* src)
{
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n)
{
    char* d = dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

char* strcat(char* dest, const char* src)
{
    char* d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char* dest, const char* src, size_t n)
{
    char* d = dest + strlen(dest);
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dest;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    while (n > 0 && *a && *a == *b) {
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strchr(const char* str, int c)
{
    while (*str) {
        if (*str == (char)c) return (char*)str;
        str++;
    }
    return (c == '\0') ? (char*)str : NULL;
}

char* strrchr(const char* str, int c)
{
    const char* last = NULL;
    while (*str) {
        if (*str == (char)c) last = str;
        str++;
    }
    return (c == '\0') ? (char*)str : (char*)last;
}

char* strstr(const char* haystack, const char* needle)
{
    if (!*needle) return (char*)haystack;
    size_t nlen = strlen(needle);

    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
            return (char*)haystack;
        }
        haystack++;
    }
    return NULL;
}

/* strtok_r - reentrant tokenizer */
char* strtok_r(char* str, const char* delim, char** saveptr)
{
    char* token_start;

    if (str) {
        *saveptr = str;
    }

    if (!*saveptr) return NULL;

    /* Skip leading delimiters */
    *saveptr += strspn(*saveptr, delim);

    if (!**saveptr) {
        *saveptr = NULL;
        return NULL;
    }

    token_start = *saveptr;

    /* Find end of token */
    *saveptr += strcspn(*saveptr, delim);

    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }

    return token_start;
}

/* Non-reentrant strtok (uses static saveptr) */
static char* strtok_saved = NULL;

char* strtok(char* str, const char* delim)
{
    return strtok_r(str, delim, &strtok_saved);
}

/* Helper: span count */
size_t strspn(const char* s, const char* accept)
{
    size_t n = 0;
    while (*s && strchr(accept, *s)) { s++; n++; }
    return n;
}

size_t strcspn(const char* s, const char* reject)
{
    size_t n = 0;
    while (*s && !strchr(reject, *s)) { s++; n++; }
    return n;
}

/* ============================================================
 * Number conversion
 * ============================================================ */

long strtol(const char* str, char** endptr, int base)
{
    while (*str == ' ' || *str == '\t') str++;

    int neg = 0;
    if (*str == '-') { neg = 1; str++; }
    else if (*str == '+') str++;

    if (base == 0) {
        if (*str == '0' && (str[1] == 'x' || str[1] == 'X')) {
            base = 16; str += 2;
        } else if (*str == '0') {
            base = 8; str++;
        } else {
            base = 10;
        }
    } else if (base == 16 && *str == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }

    long result = 0;
    const char* start = str;

    while (*str) {
        int digit;
        char c = *str;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;

        if (digit >= base) break;
        result = result * base + digit;
        str++;
    }

    if (endptr) {
        *endptr = (str == start) ? (char*)start : (char*)str;
    }

    return neg ? -result : result;
}

unsigned long strtoul(const char* str, char** endptr, int base)
{
    return (unsigned long)strtol(str, endptr, base);
}

int atoi(const char* str)
{
    return (int)strtol(str, NULL, 10);
}

long atol(const char* str)
{
    return strtol(str, NULL, 10);
}
