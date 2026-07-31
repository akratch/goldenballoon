#ifndef GE007_PLATFORM_STDIO_H
#define GE007_PLATFORM_STDIO_H

#ifdef NATIVE_PORT
/*
 * Native-port formatting shim. The macOS libc sprintf symbol is deprecated
 * and floods strict/sanitizer logs, so native code should call this helper
 * explicitly instead of the libc entrypoint.
 */
int ge007_sprintf(char *dst, const char *fmt, ...);
#endif

#endif
