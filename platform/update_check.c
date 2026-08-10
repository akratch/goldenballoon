/* update_check.c — see platform/update_check.h for the grammar and the why.
 *
 * Everything below reads only its parameters and writes only its outputs:
 * no clock, no environment, no global state, no heap. Neither input string
 * is mutated, so a caller can pass the same buffer it will log or re-parse
 * afterward.
 */
#include "update_check.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MDKR_UPDATE_RELEASE_PARTS 3

typedef struct ParsedVersion {
    int  parts[MDKR_UPDATE_RELEASE_PARTS]; /* release core, zero-padded */
    int  has_prerelease;
    char prerelease[MDKR_UPDATE_VERSION_MAX_LEN + 1];
} ParsedVersion;

static void clear_reason(char *reason_out, size_t reason_out_size) {
    if (reason_out == NULL || reason_out_size == 0) return;
    reason_out[0] = '\0';
}

static void set_reason(char *reason_out, size_t reason_out_size, const char *text) {
    size_t length;

    if (reason_out == NULL || reason_out_size == 0) return;
    length = strlen(text);
    if (length >= reason_out_size) length = reason_out_size - 1;
    memcpy(reason_out, text, length);
    reason_out[length] = '\0';
}

/* Parses a run of decimal digits (no sign, no whitespace) into `*out`.
 * Refuses an empty run and a value that would not fit in an int. The
 * accumulator is checked after every digit, so it can never itself run past
 * a few billion before the function returns -- there is no path by which the
 * multiply-add below overflows `long long`. */
static int parse_uint_component(const char *digits, size_t len, int *out) {
    long long accumulator = 0;
    size_t i;

    if (len == 0) return 0;
    for (i = 0; i < len; i++) {
        char c = digits[i];
        if (c < '0' || c > '9') return 0;
        accumulator = accumulator * 10 + (long long) (c - '0');
        if (accumulator > INT_MAX) return 0;
    }
    *out = (int) accumulator;
    return 1;
}

/* Parses one version string under the grammar in update_check.h. `label` is
 * "running" or "feed", used only to make a refusal reason say which side was
 * at fault. Returns 1 and fills `out` on success; returns 0 and (if a buffer
 * was supplied) writes a one-line reason on failure. */
static int parse_version(const char *raw, const char *label, ParsedVersion *out,
                          char *reason_out, size_t reason_out_size) {
    char reason[96];
    size_t total_len;
    const char *s;
    size_t core_len;
    const char *dash;
    size_t release_len;
    const char *release_end;
    const char *cursor;
    int part_index;

    memset(out, 0, sizeof *out);

    if (raw == NULL) {
        snprintf(reason, sizeof reason, "%s version is null", label);
        set_reason(reason_out, reason_out_size, reason);
        return 0;
    }

    /* Bounded scan: a hostile or truncated body cannot make this loop any
     * longer than MDKR_UPDATE_VERSION_MAX_LEN + 1 characters. */
    total_len = strnlen(raw, MDKR_UPDATE_VERSION_MAX_LEN + 1);
    if (total_len == 0) {
        snprintf(reason, sizeof reason, "%s version is empty", label);
        set_reason(reason_out, reason_out_size, reason);
        return 0;
    }
    if (total_len > MDKR_UPDATE_VERSION_MAX_LEN) {
        snprintf(reason, sizeof reason, "%s version exceeds %d characters", label,
                 MDKR_UPDATE_VERSION_MAX_LEN);
        set_reason(reason_out, reason_out_size, reason);
        return 0;
    }

    s = raw;
    if (s[0] == 'v' || s[0] == 'V') {
        s++;
    }
    core_len = total_len - (size_t) (s - raw);

    dash = (const char *) memchr(s, '-', core_len);
    release_len = dash ? (size_t) (dash - s) : core_len;
    if (release_len == 0) {
        snprintf(reason, sizeof reason, "%s version has no release core", label);
        set_reason(reason_out, reason_out_size, reason);
        return 0;
    }

    release_end = s + release_len;
    cursor = s;
    part_index = 0;
    for (;;) {
        const char *dot;
        const char *part_end;
        size_t part_len;
        int value;

        if (part_index >= MDKR_UPDATE_RELEASE_PARTS) {
            snprintf(reason, sizeof reason,
                     "%s version has more than %d release components", label,
                     MDKR_UPDATE_RELEASE_PARTS);
            set_reason(reason_out, reason_out_size, reason);
            return 0;
        }

        dot = (const char *) memchr(cursor, '.', (size_t) (release_end - cursor));
        part_end = dot ? dot : release_end;
        part_len = (size_t) (part_end - cursor);

        if (!parse_uint_component(cursor, part_len, &value)) {
            snprintf(reason, sizeof reason, "%s version has a malformed release component",
                     label);
            set_reason(reason_out, reason_out_size, reason);
            return 0;
        }
        out->parts[part_index++] = value;

        if (dot == NULL) break;
        cursor = dot + 1;
    }
    while (part_index < MDKR_UPDATE_RELEASE_PARTS) {
        out->parts[part_index++] = 0;
    }

    if (dash != NULL) {
        size_t prerelease_len = core_len - release_len - 1; /* -1 for the dash itself */
        if (prerelease_len == 0) {
            snprintf(reason, sizeof reason, "%s version has an empty prerelease tag", label);
            set_reason(reason_out, reason_out_size, reason);
            return 0;
        }
        /* prerelease_len < core_len <= MDKR_UPDATE_VERSION_MAX_LEN, so this
         * always fits in ParsedVersion::prerelease. */
        memcpy(out->prerelease, dash + 1, prerelease_len);
        out->prerelease[prerelease_len] = '\0';
        out->has_prerelease = 1;
    }

    return 1;
}

/* -1 if a's release core is lower, 0 if equal, 1 if higher. Purely numeric:
 * this is the whole fix for the "1.9.0" vs "1.10.0" lexicographic trap named
 * in update_check.h. */
static int compare_release(const ParsedVersion *a, const ParsedVersion *b) {
    int i;
    for (i = 0; i < MDKR_UPDATE_RELEASE_PARTS; i++) {
        if (a->parts[i] != b->parts[i]) {
            return a->parts[i] < b->parts[i] ? -1 : 1;
        }
    }
    return 0;
}

/* -1 if a is older than b, 0 if equal precedence, 1 if a is newer. */
static int compare_parsed(const ParsedVersion *a, const ParsedVersion *b) {
    int release_cmp = compare_release(a, b);
    int tag_cmp;

    if (release_cmp != 0) return release_cmp;

    if (a->has_prerelease && !b->has_prerelease) return -1; /* prerelease < release */
    if (!a->has_prerelease && b->has_prerelease) return 1;
    if (!a->has_prerelease && !b->has_prerelease) return 0;

    tag_cmp = strcmp(a->prerelease, b->prerelease);
    if (tag_cmp < 0) return -1;
    if (tag_cmp > 0) return 1;
    return 0;
}

MdkrUpdateVerdict mdkr_update_compare_versions(const char *running_version,
                                                const char *feed_version,
                                                char *reason_out,
                                                size_t reason_out_size) {
    ParsedVersion running;
    ParsedVersion feed;

    clear_reason(reason_out, reason_out_size);

    if (!parse_version(running_version, "running", &running, reason_out, reason_out_size)) {
        return MDKR_UPDATE_NONE;
    }
    if (!parse_version(feed_version, "feed", &feed, reason_out, reason_out_size)) {
        return MDKR_UPDATE_NONE;
    }

    return (compare_parsed(&running, &feed) < 0) ? MDKR_UPDATE_AVAILABLE : MDKR_UPDATE_NONE;
}

int mdkr_update_check_due(int64_t last_check_epoch_s, int64_t now_epoch_s) {
    int64_t elapsed;

    if (last_check_epoch_s <= 0) return 1; /* never checked before */
    if (now_epoch_s <= last_check_epoch_s) return 0; /* steady, or clock stepped back */

    elapsed = now_epoch_s - last_check_epoch_s;
    return elapsed >= MDKR_UPDATE_CHECK_INTERVAL_SECONDS;
}
