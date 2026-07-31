/*
 * screenshot_series.h — diagnostic screenshot-series cadence and write logic.
 *
 * The screenshot-series diagnostic decides, once per frame, whether the current
 * frame is "due" (AFTER_FRAME / EVERY / LIMIT / room-filter accounting) and, if
 * so, reads back the composited framebuffer and writes a P6 PPM. The cadence,
 * env-config parse, filename pattern, bottom-left->top-left vertical flip, the
 * write-only-on-durable-file contract, and the written-count accounting are all
 * ROM-free / GPU-free — they were locked inside a static function in the 25k-line
 * gfx_pc.c. They live here so they are unit-testable with a mock readback.
 *
 * gfx_pc.c keeps a thin wrapper that gathers width/height/frame/room and passes
 * gfx_read_framebuffer_rgb as the readback pointer. The env var names and
 * semantics, the "<dir>/<prefix>_f%06d.ppm" filename, the P6 header, the row
 * flip, the no-partial-file failure handling, the counters and the stderr logs
 * are all part of this module's contract.
 */
#ifndef SCREENSHOT_SERIES_H
#define SCREENSHOT_SERIES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Readback function pointer. Fills rgb_out (width*height*3 bytes, row-major,
 * bottom-left origin — same convention glReadPixels produces) and returns true
 * on success, false on failure. ctx is passed through opaque.
 */
typedef bool (*screenshot_series_readback_fn)(int x, int y, int width, int height,
                                              uint8_t *rgb_out, void *ctx);

/*
 * Per-instance state. In gfx_pc.c this is a single function-local static, so the
 * env parse runs once per process (matching the original). Tests own their own
 * instance so each case is independent. Zero-initialize before first use.
 */
typedef struct {
    int initialized;
    int enabled;
    int after_frame;
    int every;
    int limit;
    int written;
    const char *dir;
    const char *prefix;
    const char *room_spec;
} screenshot_series_state;

/*
 * Parse configuration from the environment into st (idempotent: does nothing if
 * st->initialized is already set). Reads GE007_SCREENSHOT_SERIES_DIR / _PREFIX /
 * _ROOM / _AFTER_FRAME / _EVERY / _LIMIT with the original defaults and clamps,
 * and emits the "[SCREENSHOT-SERIES] enabled ..." line to stderr when enabled.
 */
void screenshot_series_init(screenshot_series_state *st);

/*
 * If the given frame/room is due (per st's cadence), read back a width*height
 * RGB image via readback(ctx) and write it as a P6 PPM to
 * "<dir>/<prefix>_f%06d.ppm" with the rows flipped bottom-to-top. On a durable
 * full write, increments st->written and returns true. Any failure (not due,
 * bad dimensions, malloc, readback, path-too-long, fopen) writes no partial file,
 * leaves st->written unchanged, and returns false. Calls screenshot_series_init
 * first, so callers may pass a freshly zeroed state.
 */
bool screenshot_series_capture_if_due(screenshot_series_state *st,
                                      int frame, uint64_t room,
                                      int width, int height,
                                      screenshot_series_readback_fn readback,
                                      void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_SERIES_H */
