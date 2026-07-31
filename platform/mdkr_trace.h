#ifndef MDKR_TRACE_H
#define MDKR_TRACE_H

/*
 * Trace plumbing used by native-only probes in otherwise game-owned sources.
 * This header intentionally includes no host libc headers, so it is safe beside
 * libultra's PR/os_libc.h declarations.
 */
int mdkr_trace_enabled(void);
int mdkr_trace_level(void);
/* Generation/resource telemetry without the per-frame pacing stream. */
int mdkr_resource_trace_enabled(void);
void mdkr_trace(const char *fmt, ...);

#define MDKR_TRACE(...) \
    do { \
        if (mdkr_trace_enabled()) { \
            mdkr_trace(__VA_ARGS__); \
        } \
    } while (0)

#endif
