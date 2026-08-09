/*
 * test_update_check.c — offline, pure tests for the update-check policy.
 *
 * No network, no clock, no file I/O: every case below drives
 * platform/update_check.c with string literals and injected integers. See
 * platform/update_check.h for the version grammar and the interval rule.
 */
#include "update_check.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect_int(const char *name, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-46s actual=%d expected=%d\n", name, actual, expected);
        s_failures++;
    }
}

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* Runs one compare and returns the verdict, leaving `reason` filled in
 * (possibly empty) for the caller to inspect. */
static MdkrUpdateVerdict compare(const char *running, const char *feed, char *reason,
                                  size_t reason_size) {
    reason[0] = '\1'; /* poison: a function that never touches reason_out is a bug too */
    return mdkr_update_compare_versions(running, feed, reason, reason_size);
}

/* 1. 1.1.0 vs 1.1.1 -> newer available. */
static void test_newer_patch(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1.0", "1.1.1", reason, sizeof reason);
    expect_int("1.1.0 vs 1.1.1 verdict", (int) v, (int) MDKR_UPDATE_AVAILABLE);
    expect_true("1.1.0 vs 1.1.1 reason empty", reason[0] == '\0');
}

/* 2. 1.1.0 vs 1.1.0 -> none (equal). */
static void test_equal(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1.0", "1.1.0", reason, sizeof reason);
    expect_int("1.1.0 vs 1.1.0 verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("1.1.0 vs 1.1.0 reason empty", reason[0] == '\0');
}

/* 3. 1.2.0 vs 1.1.9 -> none. A running build ahead of the feed is never
 * downgraded. */
static void test_running_ahead(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.2.0", "1.1.9", reason, sizeof reason);
    expect_int("1.2.0 vs 1.1.9 verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("1.2.0 vs 1.1.9 reason empty", reason[0] == '\0');
}

/* 4. 1.1.0 vs 1.1.0-nightly.abc1234 -> none. A nightly is not an upgrade from
 * a release. */
static void test_nightly_not_an_upgrade(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1.0", "1.1.0-nightly.abc1234", reason, sizeof reason);
    expect_int("release vs nightly verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("release vs nightly reason empty", reason[0] == '\0');
}

/* 5. 1.1.0-nightly.abc1234 vs 1.1.0 -> newer. A release supersedes the
 * nightly it came from. */
static void test_release_supersedes_nightly(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1.0-nightly.abc1234", "1.1.0", reason, sizeof reason);
    expect_int("nightly vs release verdict", (int) v, (int) MDKR_UPDATE_AVAILABLE);
    expect_true("nightly vs release reason empty", reason[0] == '\0');
}

/* 6. A malformed feed version -> none, plus a reason. Never a crash. */
static void test_malformed_feed(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1.0", "not-a-version", reason, sizeof reason);
    expect_int("malformed feed verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("malformed feed reason non-empty", reason[0] != '\0');
}

/* 8. 1.10.0 vs 1.9.0 -> none. Numeric comparison, not lexicographic — the
 * classic version-compare bug. */
static void test_numeric_not_lexicographic(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.10.0", "1.9.0", reason, sizeof reason);
    expect_int("1.10.0 vs 1.9.0 verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("1.10.0 vs 1.9.0 reason empty", reason[0] == '\0');

    /* And the mirror: 1.9.0 running, 1.10.0 feed, IS newer. If a lexicographic
     * compare snuck in, both directions read "none" or both read "newer". */
    v = compare("1.9.0", "1.10.0", reason, sizeof reason);
    expect_int("1.9.0 vs 1.10.0 verdict", (int) v, (int) MDKR_UPDATE_AVAILABLE);
}

/* 9. A version with fewer components compares equal: missing trailing
 * components are treated as zero. See platform/update_check.h. */
static void test_short_form_equal(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("1.1", "1.1.0", reason, sizeof reason);
    expect_int("1.1 vs 1.1.0 verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("1.1 vs 1.1.0 reason empty", reason[0] == '\0');

    v = compare("1.1.0", "1.1", reason, sizeof reason);
    expect_int("1.1.0 vs 1.1 verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("1.1.0 vs 1.1 reason empty", reason[0] == '\0');

    /* A short form that is genuinely newer still reads as an upgrade. */
    v = compare("1.1", "1.2", reason, sizeof reason);
    expect_int("1.1 vs 1.2 verdict", (int) v, (int) MDKR_UPDATE_AVAILABLE);
}

/* 10. A leading "v" is accepted on either side, matching this repo's release
 * tags (v1.1.0). See platform/update_check.h. */
static void test_leading_v_accepted(void) {
    char reason[128];
    MdkrUpdateVerdict v = compare("v1.1.0", "v1.1.1", reason, sizeof reason);
    expect_int("v1.1.0 vs v1.1.1 verdict", (int) v, (int) MDKR_UPDATE_AVAILABLE);
    expect_true("v1.1.0 vs v1.1.1 reason empty", reason[0] == '\0');

    v = compare("1.1.0", "v1.1.1", reason, sizeof reason);
    expect_int("1.1.0 vs v1.1.1 verdict (mixed)", (int) v, (int) MDKR_UPDATE_AVAILABLE);

    v = compare("v1.1.0", "1.1.0", reason, sizeof reason);
    expect_int("v1.1.0 vs 1.1.0 verdict (mixed, equal)", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("v1.1.0 vs 1.1.0 reason empty", reason[0] == '\0');
}

/* 11. Absurd input is refused with a reason, never a crash or UB. Run this
 * file under ASan/UBSan and mean it. */
static void test_absurd_input(void) {
    char reason[128];
    char many_dots[2048];
    size_t i;

    /* 500 dots. */
    for (i = 0; i < 500 && i + 1 < sizeof many_dots; i++) {
        many_dots[i] = '.';
    }
    many_dots[i] = '\0';
    MdkrUpdateVerdict v = compare("1.1.0", many_dots, reason, sizeof reason);
    expect_int("500 dots verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("500 dots reason non-empty", reason[0] != '\0');

    /* A component that overflows int. */
    v = compare("1.1.0", "1.99999999999999999999.0", reason, sizeof reason);
    expect_int("overflow component verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("overflow component reason non-empty", reason[0] != '\0');

    /* Empty string. */
    v = compare("1.1.0", "", reason, sizeof reason);
    expect_int("empty feed verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("empty feed reason non-empty", reason[0] != '\0');

    v = compare("", "1.1.0", reason, sizeof reason);
    expect_int("empty running verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("empty running reason non-empty", reason[0] != '\0');

    /* NULL pointer, on both sides in turn, and both at once. Must not crash
     * under ASan/UBSan. */
    v = mdkr_update_compare_versions(NULL, "1.1.0", reason, sizeof reason);
    expect_int("null running verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("null running reason non-empty", reason[0] != '\0');

    v = mdkr_update_compare_versions("1.1.0", NULL, reason, sizeof reason);
    expect_int("null feed verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("null feed reason non-empty", reason[0] != '\0');

    v = mdkr_update_compare_versions(NULL, NULL, reason, sizeof reason);
    expect_int("null both verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("null both reason non-empty", reason[0] != '\0');

    /* A reason buffer of size zero, and a null reason pointer, must also
     * survive — the caller-supplied buffer is not guaranteed generous. */
    v = mdkr_update_compare_versions("1.1.0", "not-a-version", reason, 0);
    expect_int("zero-size reason buffer verdict", (int) v, (int) MDKR_UPDATE_NONE);
    v = mdkr_update_compare_versions("1.1.0", "not-a-version", NULL, sizeof reason);
    expect_int("null reason buffer verdict", (int) v, (int) MDKR_UPDATE_NONE);

    /* Too many numeric components is malformed, not silently truncated. */
    v = compare("1.1.0", "1.1.0.1", reason, sizeof reason);
    expect_int("four components verdict", (int) v, (int) MDKR_UPDATE_NONE);
    expect_true("four components reason non-empty", reason[0] != '\0');
}

/* 7. The interval policy: "do not check" under 24h, "check" at exactly 24h.
 * The clock is a parameter — no real time, no sleeping. */
static void test_interval_policy(void) {
    const int64_t day = 86400;
    const int64_t last_check = 1000000000; /* arbitrary fixed epoch second */

    expect_int("just checked: not due",
               mdkr_update_check_due(last_check, last_check), 0);
    expect_int("23h59m59s later: not due",
               mdkr_update_check_due(last_check, last_check + day - 1), 0);
    expect_int("exactly 24h later: due",
               mdkr_update_check_due(last_check, last_check + day), 1);
    expect_int("well past 24h later: due",
               mdkr_update_check_due(last_check, last_check + day * 10), 1);

    /* Clock stepped backward (e.g. NTP correction): must not crash or wedge
     * on a huge unsigned wraparound; not asserting a specific verdict beyond
     * "not due" since it did just check moments ago from its own view. */
    expect_int("clock stepped backward: not due",
               mdkr_update_check_due(last_check, last_check - day), 0);
}

/* 12. A first-ever check (no recorded last-check time) must return "check". */
static void test_first_ever_check(void) {
    expect_int("never checked (0): due", mdkr_update_check_due(0, 1000000000), 1);
    expect_int("never checked (negative sentinel): due",
               mdkr_update_check_due(-1, 1000000000), 1);
    /* Even if "now" is also epoch zero, a never-checked build is still due. */
    expect_int("never checked, now also zero: due", mdkr_update_check_due(0, 0), 1);
}

int main(void) {
    test_newer_patch();
    test_equal();
    test_running_ahead();
    test_nightly_not_an_upgrade();
    test_release_supersedes_nightly();
    test_malformed_feed();
    test_interval_policy();
    test_numeric_not_lexicographic();
    test_short_form_equal();
    test_leading_v_accepted();
    test_absurd_input();
    test_first_ever_check();

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("all update_check tests passed\n");
    return 0;
}
