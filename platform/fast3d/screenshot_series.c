/*
 * screenshot_series.c — screenshot-series cadence + P6 write logic.
 *
 * The decision and write path is ROM-free and GPU-free so it stays unit-testable
 * with a mocked readback: capture takes a readback function pointer rather than
 * calling gfx_read_framebuffer_rgb() directly, and the wrapper in gfx_pc.c
 * supplies the real backend adapter. The env var names and semantics, the
 * "<dir>/<prefix>_f%06d.ppm" filename, the P6 header, the bottom-to-top row
 * flip, the no-partial-file failure handling, the written counter and every
 * stderr diagnostic line are this module's contract.
 */
#include "screenshot_series.h"
#include "fs_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * u64-list membership matcher — copied from gfx_pc.c's static
 * gfx_diag_u64_matches_list so this TU links no engine code. Same grammar:
 * comma/space/tab separated tokens, each a strtoull value or "first:last" /
 * "first-last" inclusive range, "*" matches anything, malformed tokens skipped.
 */
static bool screenshot_series_u64_matches_list(const char *spec, uint64_t value)
{
    const char *p;

    if (spec == NULL || spec[0] == '\0') {
        return false;
    }

    p = spec;
    while (*p != '\0') {
        char *end;
        unsigned long long first;
        unsigned long long last;

        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '*') {
            return true;
        }

        first = strtoull(p, &end, 0);
        if (end == p) {
            while (*p != '\0' && *p != ',') {
                p++;
            }
            continue;
        }

        last = first;
        if (*end == ':' || *end == '-') {
            char *range_end;
            unsigned long long range_last = strtoull(end + 1, &range_end, 0);
            if (range_end != end + 1) {
                last = range_last;
                end = range_end;
            }
        }

        if (first > last) {
            unsigned long long tmp = first;
            first = last;
            last = tmp;
        }
        if (value >= (uint64_t)first && value <= (uint64_t)last) {
            return true;
        }

        p = end;
        while (*p != '\0' && *p != ',') {
            p++;
        }
    }

    return false;
}

void screenshot_series_init(screenshot_series_state *st)
{
    const char *env;

    if (st->initialized) {
        return;
    }

    st->initialized = 1;
    st->dir = getenv("GE007_SCREENSHOT_SERIES_DIR");
    st->enabled = (st->dir != NULL && st->dir[0] != '\0' && strcmp(st->dir, "0") != 0);
    st->prefix = getenv("GE007_SCREENSHOT_SERIES_PREFIX");
    if (st->prefix == NULL || st->prefix[0] == '\0') {
        st->prefix = "capture";
    }
    st->room_spec = getenv("GE007_SCREENSHOT_SERIES_ROOM");
    if (st->room_spec != NULL && (st->room_spec[0] == '\0' ||
                                  (st->room_spec[0] == '0' && st->room_spec[1] == '\0'))) {
        st->room_spec = NULL;
    }

    env = getenv("GE007_SCREENSHOT_SERIES_AFTER_FRAME");
    st->after_frame = env ? atoi(env) : 0;
    if (st->after_frame < 0) {
        st->after_frame = 0;
    }

    env = getenv("GE007_SCREENSHOT_SERIES_EVERY");
    st->every = env ? atoi(env) : 30;
    if (st->every < 1) {
        st->every = 1;
    }

    env = getenv("GE007_SCREENSHOT_SERIES_LIMIT");
    st->limit = env ? atoi(env) : 240;
    if (st->limit < 0) {
        st->limit = 0;
    }

    if (st->enabled) {
        fprintf(stderr,
                "[SCREENSHOT-SERIES] enabled dir=\"%s\" prefix=\"%s\" "
                "after=%d every=%d limit=%d room=\"%s\"\n",
                st->dir, st->prefix, st->after_frame, st->every, st->limit,
                st->room_spec != NULL ? st->room_spec : "*");
        fflush(stderr);
    }
}

bool screenshot_series_capture_if_due(screenshot_series_state *st,
                                      int frame, uint64_t room,
                                      int width, int height,
                                      screenshot_series_readback_fn readback,
                                      void *ctx)
{
    screenshot_series_init(st);

    if (!st->enabled ||
        frame < st->after_frame ||
        (st->room_spec != NULL &&
         !screenshot_series_u64_matches_list(st->room_spec, room)) ||
        (st->limit > 0 && st->written >= st->limit) ||
        ((frame - st->after_frame) % st->every) != 0) {
        return false;
    }

    {
        int sw = width;
        int sh = height;
        size_t pixel_bytes;
        uint8_t *pixels;
        char path[1024];
        int path_len;
        FILE *sf;

        if (sw <= 0 || sh <= 0) {
            return false;
        }

        pixel_bytes = (size_t)sw * (size_t)sh * 3;
        pixels = (uint8_t *)malloc(pixel_bytes);
        if (pixels == NULL) {
            fprintf(stderr,
                    "[SCREENSHOT-SERIES] frame=%d failed=malloc bytes=%zu\n",
                    frame, pixel_bytes);
            fflush(stderr);
            return false;
        }

        /* Backend-neutral readback: raw glReadPixels crashes under Metal, which
         * has no GL context. The wrapper routes GL->glReadPixels, Metal->blit,
         * WebGPU->scene-tex copy. A readback failure writes no (partial) file. */
        if (readback == NULL || !readback(0, 0, sw, sh, pixels, ctx)) {
            fprintf(stderr, "[SCREENSHOT-SERIES] frame=%d failed=readback\n", frame);
            fflush(stderr);
            free(pixels);
            return false;
        }

        path_len = snprintf(path, sizeof(path), "%s/%s_f%06d.ppm",
                            st->dir, st->prefix, frame);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            fprintf(stderr,
                    "[SCREENSHOT-SERIES] frame=%d failed=path_too_long\n",
                    frame);
            fflush(stderr);
            free(pixels);
            return false;
        }

        sf = mdkr_fopen_utf8(path, "wb");
        if (sf == NULL) {
            fprintf(stderr,
                    "[SCREENSHOT-SERIES] frame=%d failed=fopen path=\"%s\"\n",
                    frame, path);
            fflush(stderr);
            free(pixels);
            return false;
        }

        fprintf(sf, "P6\n%d %d\n255\n", sw, sh);
        for (int row = sh - 1; row >= 0; row--) {
            fwrite(pixels + (size_t)row * (size_t)sw * 3, 1, (size_t)sw * 3, sf);
        }
        fclose(sf);
        free(pixels);

        st->written++;
        fprintf(stderr,
                "[SCREENSHOT-SERIES] frame=%d room=%llu path=\"%s\" size=[%d,%d] index=%d\n",
                frame, (unsigned long long)room, path, sw, sh, st->written);
        fflush(stderr);
    }

    return true;
}
