/*
 * libc/printf.c - Minimal printf/sprintf implementation
 *
 * Provides vsnprintf, snprintf, and sprintf for use throughout the kernel.
 * Supports: %d, %i, %u, %x, %X, %o, %c, %s, %p, %%, %llu, %lld, etc.
 * Supports: width, zero-padding, left-justify, +/- sign, 0x prefix.
 */
#include <types.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================
 * Integer -> string conversion
 * ============================================================ */

static int uint_to_str(uint64_t value, char* buf, int base, bool upper)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char* digits = upper ? digits_upper : digits_lower;

    char tmp[66]; /* max 64 binary digits + sign + null */
    int  len = 0;

    if (value == 0) {
        tmp[len++] = '0';
    } else {
        while (value > 0) {
            tmp[len++] = digits[value % (uint64_t)base];
            value /= (uint64_t)base;
        }
    }

    /* Reverse into buf */
    for (int i = 0; i < len; i++) {
        buf[i] = tmp[len - 1 - i];
    }
    buf[len] = '\0';
    return len;
}

/* ============================================================
 * vsnprintf - the core implementation
 * ============================================================ */

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap)
{
    size_t pos = 0;

#define PUT(c) do { if (pos + 1 < size) { buf[pos++] = (c); } } while(0)
#define PUTS(s, n) do { \
    for (size_t _i = 0; _i < (size_t)(n); _i++) { PUT((s)[_i]); } \
} while(0)

    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        bool flag_left    = false;
        bool flag_zero    = false;
        bool flag_plus    = false;
        bool flag_space   = false;
        bool flag_hash    = false;

        while (1) {
            if (*fmt == '-')      { flag_left  = true; fmt++; }
            else if (*fmt == '0') { flag_zero  = true; fmt++; }
            else if (*fmt == '+') { flag_plus  = true; fmt++; }
            else if (*fmt == ' ') { flag_space = true; fmt++; }
            else if (*fmt == '#') { flag_hash  = true; fmt++; }
            else break;
        }

        /* Parse width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flag_left = true; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }

        /* Parse length modifier */
        int length = 0; /* 0=int, 1=long, 2=long long, 3=short, 4=char */
        if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') { length = 4; fmt++; }
            else length = 3;
        } else if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { length = 2; fmt++; }
            else length = 1;
        } else if (*fmt == 'z') {
            length = 2; /* size_t = uint64_t */
            fmt++;
        }

        /* Conversion specifier */
        char spec = *fmt++;
        char tmp[128];
        const char* str_val = NULL;
        int str_len = 0;
        bool is_negative = false;
        char prefix[3] = {0};
        int prefix_len = 0;

        switch (spec) {
            case 'd': case 'i': {
                int64_t val;
                if (length == 2)      val = va_arg(ap, long long);
                else if (length == 1) val = va_arg(ap, long);
                else                  val = va_arg(ap, int);

                if (val < 0) { is_negative = true; val = -val; }

                str_len = uint_to_str((uint64_t)val, tmp, 10, false);
                str_val = tmp;

                if (is_negative)         { prefix[prefix_len++] = '-'; }
                else if (flag_plus)      { prefix[prefix_len++] = '+'; }
                else if (flag_space)     { prefix[prefix_len++] = ' '; }
                break;
            }

            case 'u': {
                uint64_t val;
                if (length == 2)      val = va_arg(ap, unsigned long long);
                else if (length == 1) val = va_arg(ap, unsigned long);
                else                  val = va_arg(ap, unsigned int);
                str_len = uint_to_str(val, tmp, 10, false);
                str_val = tmp;
                break;
            }

            case 'x': case 'X': {
                uint64_t val;
                if (length == 2)      val = va_arg(ap, unsigned long long);
                else if (length == 1) val = va_arg(ap, unsigned long);
                else                  val = va_arg(ap, unsigned int);
                str_len = uint_to_str(val, tmp, 16, spec == 'X');
                str_val = tmp;
                if (flag_hash && val != 0) {
                    prefix[prefix_len++] = '0';
                    prefix[prefix_len++] = (spec == 'X') ? 'X' : 'x';
                }
                break;
            }

            case 'o': {
                uint64_t val = va_arg(ap, unsigned int);
                str_len = uint_to_str(val, tmp, 8, false);
                str_val = tmp;
                if (flag_hash) { prefix[prefix_len++] = '0'; }
                break;
            }

            case 'b': {
                uint64_t val = va_arg(ap, unsigned int);
                str_len = uint_to_str(val, tmp, 2, false);
                str_val = tmp;
                break;
            }

            case 'p': {
                uint64_t val = (uint64_t)(uintptr_t)va_arg(ap, void*);
                str_len = uint_to_str(val, tmp, 16, false);
                str_val = tmp;
                prefix[prefix_len++] = '0';
                prefix[prefix_len++] = 'x';
                break;
            }

            case 'c': {
                tmp[0] = (char)va_arg(ap, int);
                tmp[1] = '\0';
                str_val = tmp;
                str_len = 1;
                break;
            }

            case 's': {
                str_val = va_arg(ap, const char*);
                if (!str_val) str_val = "(null)";
                str_len = (int)strlen(str_val);
                if (precision >= 0 && str_len > precision) str_len = precision;
                break;
            }

            case '%': {
                PUT('%');
                continue;
            }

            case '\0':
                goto done;

            default:
                PUT('%');
                PUT(spec);
                continue;
        }

        /* Apply width padding */
        int total_len = prefix_len + str_len;
        int pad = (width > total_len) ? (width - total_len) : 0;
        char pad_char = (flag_zero && !flag_left) ? '0' : ' ';

        if (!flag_left) {
            /* Right-align: print spaces/zeros first */
            if (pad_char == '0') {
                /* Zero-pad after sign/prefix */
                PUTS(prefix, prefix_len);
                prefix_len = 0;
                for (int i = 0; i < pad; i++) PUT('0');
            } else {
                for (int i = 0; i < pad; i++) PUT(' ');
            }
        }

        PUTS(prefix, prefix_len);
        PUTS(str_val, str_len);

        if (flag_left) {
            for (int i = 0; i < pad; i++) PUT(' ');
        }
    }

done:
    if (size > 0) buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;

#undef PUT
#undef PUTS
}

int snprintf(char* buf, size_t n, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char* buf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, 65536, fmt, ap);
    va_end(ap);
    return ret;
}
