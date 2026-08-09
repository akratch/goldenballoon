/* Unit test for the bounds-checked asset sub-entry accessor (S5 M1).
 *
 * No ROM, no window, no engine. The whole point of platform/asset_subentry.c is
 * that an out-of-range entry index ABORTS with a named diagnostic instead of
 * returning plausible wrong data, so the interesting half of this test is a
 * process that must die: each aborting case runs in a forked child whose stderr
 * is captured, and the assertion is on both the signal and the message text.
 * Asserting only "it did not return" would pass for a segfault, which is the
 * outcome the accessor exists to prevent.
 */
#include "asset_subentry.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_contains(const char *name, const char *haystack,
                            const char *needle) {
    if (strstr(haystack, needle) == NULL) {
        fprintf(stderr, "FAIL %-46s missing '%s' in:\n%s\n", name, needle,
                haystack);
        s_failures++;
    }
}

static void expect_u32(const char *name, uint32_t actual, uint32_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-46s actual=%u expected=%u\n", name,
                (unsigned) actual, (unsigned) expected);
        s_failures++;
    }
}

/* ------------------------------------------------------------------ */
/* fixtures                                                            */
/* ------------------------------------------------------------------ */

/* A one-entry byte-offset section: entry 0 spans [0, 4). */
static const uint32_t kOneEntryOffsets[2] = {0u, 4u};
static const uint8_t kOneEntryPayload[8] = {0xDEu, 0xADu, 0xBEu, 0xEFu,
                                            0x00u, 0x00u, 0x00u, 0x00u};
static const MdkrAssetSection kOneEntry = {
    "GAME_TEXT_UNIT", kOneEntryPayload, 1u, kOneEntryOffsets, 1u};

/* A zero-entry section. Only the terminator is present, so index 0 is already
 * out of range -- the 1.0-ROM shape, one section shorter than the build wants. */
static const uint32_t kEmptyOffsets[1] = {0u};
static const MdkrAssetSection kEmpty = {
    "EMPTY_SECTION_UNIT", kOneEntryPayload, 0u, kEmptyOffsets, 1u};

/* Two entries whose offsets are s32 WORD indices, as gAssetsMiscTable stores
 * them, terminated by the -1 the tree's tables end with. */
static const uint32_t kWordOffsets[3] = {1u, 3u, 0xFFFFFFFFu};
static const uint8_t kWordPayload[16] = {0};
static const MdkrAssetSection kWordScaled = {
    "ASSET_MISC_UNIT", kWordPayload, 2u, kWordOffsets, 4u};

/* ------------------------------------------------------------------ */
/* child-process harness                                               */
/* ------------------------------------------------------------------ */

typedef void (*SubentryCase)(void);

/* Runs `body` in a forked child with stdout+stderr captured into `out`.
 * Returns the terminating signal, or 0 when the child exited normally. */
static int run_case(SubentryCase body, char *out, size_t cap) {
    int fds[2];
    pid_t pid;
    int status = 0;
    size_t used = 0;

    out[0] = '\0';
    if (pipe(fds) != 0) {
        perror("pipe");
        exit(2);
    }
    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) {
            _exit(3);
        }
        close(fds[1]);
        body();
        _exit(0);
    }
    close(fds[1]);
    for (;;) {
        char scratch[512];
        ssize_t n = read(fds[0], scratch, sizeof(scratch));

        if (n <= 0) {
            break;
        }
        /* Keep draining after the buffer fills, so a chatty child (a sanitizer
         * epilogue, say) can never deadlock on a full pipe. */
        if (used + (size_t) n < cap) {
            memcpy(out + used, scratch, (size_t) n);
            used += (size_t) n;
        }
    }
    out[used] = '\0';
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0) {
        /* retry on EINTR */
    }
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status);
    }
    return 0;
}

static void case_index_one_of_one(void) {
    (void) mdkr_asset_subentry(&kOneEntry, 1u, NULL);
}

static void case_huge_unsigned(void) {
    (void) mdkr_asset_subentry(&kOneEntry, 0xFFFFFFFFu, NULL);
}

static void case_signed_negative(void) {
    /* The real signature is unsigned, so -1 arrives as UINT32_MAX. Driven
     * through the signed value a caller would actually hold, because that
     * conversion is the thing that must not read backwards off the base. */
    int32_t index = -1;

    (void) mdkr_asset_subentry(&kOneEntry, (uint32_t) index, NULL);
}

static void case_zero_entry_section(void) {
    (void) mdkr_asset_subentry(&kEmpty, 0u, NULL);
}

static void case_null_descriptor(void) {
    (void) mdkr_asset_subentry(NULL, 0u, NULL);
}

static void case_unloaded_section(void) {
    MdkrAssetSection unloaded = kOneEntry;

    unloaded.base = NULL;
    (void) mdkr_asset_subentry(&unloaded, 0u, NULL);
}

static void case_explicit_guard(void) {
    mdkr_asset_subentry_out_of_range("ASSET_PARTICLES_TABLE", 40, 27,
                                     "emitter_init_with_pos");
}

static void expect_abort(const char *name, SubentryCase body, char *out,
                         size_t cap) {
    int signo = run_case(body, out, cap);

    if (signo == SIGABRT) {
        return;
    }
    if (signo == 0) {
        fprintf(stderr, "FAIL %-46s returned instead of aborting; output:\n%s\n",
                name, out);
    } else {
        fprintf(stderr, "FAIL %-46s died with signal %d, not SIGABRT; output:\n%s\n",
                name, signo, out);
    }
    s_failures++;
}

int main(void) {
    static char out[8192];
    const uint8_t *entry;
    uint32_t size = 0xA5A5A5A5u;

    /* --- index 0 of a one-entry section resolves --------------------- */
    entry = mdkr_asset_subentry(&kOneEntry, 0u, &size);
    expect_true("index 0 of a 1-entry section resolves",
                entry == kOneEntryPayload);
    expect_u32("index 0 reports its byte length", size, 4u);

    /* out_size is optional. */
    entry = mdkr_asset_subentry(&kOneEntry, 0u, NULL);
    expect_true("index 0 resolves without an out_size", entry == kOneEntryPayload);

    /* --- word-scaled offsets, as ASSET_MISC stores them -------------- */
    size = 0xA5A5A5A5u;
    entry = mdkr_asset_subentry(&kWordScaled, 0u, &size);
    expect_true("word-scaled entry 0 lands at base + 1 word",
                entry == kWordPayload + 4);
    expect_u32("word-scaled entry 0 spans two words", size, 8u);

    /* The last usable entry of a terminated table has no successor offset, so
     * its length is unknown -- reported as 0, exactly as get_misc_asset_size()
     * has always reported it. Not an error, and it must not abort. */
    size = 0xA5A5A5A5u;
    entry = mdkr_asset_subentry(&kWordScaled, 1u, &size);
    expect_true("entry before the terminator still resolves",
                entry == kWordPayload + 12);
    expect_u32("entry before the terminator has unknown length", size, 0u);

    /* --- the aborting half ------------------------------------------- */
    expect_abort("index 1 of a 1-entry section aborts", case_index_one_of_one,
                 out, sizeof(out));
    expect_contains("abort names the section", out, "GAME_TEXT_UNIT");
    expect_contains("abort names the requested index", out, "index=1");
    expect_contains("abort names the actual count", out, "count=1");
    expect_contains("abort is greppable", out,
                    "[FATAL] asset sub-entry index out of range");

    expect_abort("a huge unsigned index aborts", case_huge_unsigned, out,
                 sizeof(out));
    expect_contains("huge index is reported verbatim", out, "index=4294967295");
    expect_contains("huge index reports the count", out, "count=1");

    expect_abort("a signed -1 index aborts", case_signed_negative, out,
                 sizeof(out));
    expect_contains("signed -1 is reported as UINT32_MAX", out,
                    "index=4294967295");

    expect_abort("index 0 of a zero-entry section aborts",
                 case_zero_entry_section, out, sizeof(out));
    expect_contains("zero-entry abort names the section", out,
                    "EMPTY_SECTION_UNIT");
    expect_contains("zero-entry abort names the index", out, "index=0");
    expect_contains("zero-entry abort names the count", out, "count=0");

    expect_abort("a NULL descriptor aborts", case_null_descriptor, out,
                 sizeof(out));
    expect_abort("an unloaded section aborts", case_unloaded_section, out,
                 sizeof(out));
    expect_contains("unloaded section says so", out, "not loaded");

    expect_abort("the explicit guard aborts", case_explicit_guard, out,
                 sizeof(out));
    expect_contains("explicit guard names the section", out,
                    "ASSET_PARTICLES_TABLE");
    expect_contains("explicit guard names the index", out, "index=40");
    expect_contains("explicit guard names the count", out, "count=27");
    expect_contains("explicit guard names the call site", out,
                    "at=emitter_init_with_pos");

    if (s_failures != 0) {
        fprintf(stderr, "test_asset_subentry: FAIL (%d)\n", s_failures);
        return 1;
    }
    printf("test_asset_subentry: PASS\n");
    return 0;
}
