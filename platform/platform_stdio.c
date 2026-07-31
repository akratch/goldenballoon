#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <platform_stdio.h>

int ge007_sprintf(char *dst, const char *fmt, ...) {
    va_list ap;
    int written;

    va_start(ap, fmt);
    /*
     * Preserve sprintf-style behavior for the port without calling the
     * deprecated libc entrypoint directly. INT_MAX keeps the call effectively
     * unbounded for the original game code's expectations.
     */
    written = vsnprintf(dst, INT_MAX, fmt, ap);
    va_end(ap);

    return written;
}
