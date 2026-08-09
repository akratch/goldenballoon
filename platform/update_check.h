/* update_check.h — is the feed's version newer than mine, and is it time to
 * ask?
 *
 * The trap this module exists to avoid is comparing version strings as
 * strings. "1.9.0" > "1.10.0" under strcmp, because '9' sorts after '1', so a
 * naive feed check either nags a player who is already current or — worse —
 * tells someone two releases behind that they are up to date. Every component
 * here is parsed to an integer and compared as one; nothing is ever compared
 * character by character.
 *
 * The second trap is a test suite that cannot run without a network or a real
 * day passing. This module touches neither: it takes both version strings and
 * the clock as plain parameters, so mdkr_update_compare_versions() and
 * mdkr_update_check_due() are exhaustively testable offline, in a tenth of a
 * second, with no ROM and no host reachable. It knows nothing about HTTP,
 * files, threads, or MDKR_APP_UPDATE_CHECK (platform/video_config.h) — Task 6
 * owns fetching the feed, honouring the opt-out, and showing the notice in the
 * launcher. This module only decides, from strings and integers already in
 * hand, and it does so with no heap allocation and without mutating either
 * caller-owned string.
 *
 * ── Version grammar ──────────────────────────────────────────────────────
 *
 * A version is:
 *
 *     version    := "v"? release ("-" prerelease)?
 *     release    := part ("." part){0,2}          -- 1, 2 or 3 parts
 *     part       := digit+                          -- decimal, no sign
 *     prerelease := any non-empty run of characters other than NUL
 *
 * Concretely: an optional leading 'v' or 'V', then one to three dot-separated
 * decimal integers (each fitting in an int; more than three parts, or a part
 * that overflows, is malformed), optionally followed by "-" and an arbitrary
 * prerelease tag such as "nightly.abc1234". Leading zeros in a part are
 * accepted ("01" is 1). Whitespace anywhere is malformed.
 *
 * Three grammar choices worth stating precisely, because a release workflow's
 * feed generator (Task 6) needs to know the answer without reading the .c:
 *
 *   - A release core with fewer than three parts is padded with zeros, not
 *     rejected: "1.1" and "1.1.0" compare equal. A feed may emit either form.
 *   - The leading 'v'/'V' is accepted and stripped before parsing, on either
 *     the running version or the feed version, independently. This repo's
 *     release tags are "v1.1.0"; a feed built straight from a tag and a
 *     compiled-in MDKR_APP_VERSION_STR without one both parse the same way.
 *   - A whole version string longer than MDKR_UPDATE_VERSION_MAX_LEN
 *     characters is refused outright, unparsed. Every version this project
 *     actually emits is under 20; the limit exists to make a malformed or
 *     hostile feed body a cheap, immediate refusal instead of a long scan.
 *
 * ── Precedence when the release cores are equal ──────────────────────────
 *
 * A version with a prerelease tag is older than the same release core without
 * one — that is the whole rule behind "a nightly is not an upgrade from the
 * release with the same numbers" and its mirror, "a release supersedes the
 * nightly it came from". If both sides carry a (non-identical) prerelease tag
 * at the same release core, they are ordered by a plain byte-wise comparison
 * of the tag text; this repo only ever emits "nightly.<short-sha>", so that
 * tiebreak exists for completeness rather than because it is load-bearing.
 *
 * A version that fails to parse is refused, not treated as any particular
 * ordering — see mdkr_update_compare_versions() below.
 */
#ifndef MDKR64_UPDATE_CHECK_H
#define MDKR64_UPDATE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One day, in seconds. The interval policy's own unit. */
#define MDKR_UPDATE_CHECK_INTERVAL_SECONDS ((int64_t) 86400)

/* Longest version string mdkr_update_compare_versions() will parse, not
 * counting the terminating NUL. See the grammar above. */
#define MDKR_UPDATE_VERSION_MAX_LEN 63

typedef enum MdkrUpdateVerdict {
    /* The feed is not a strict upgrade over the running version. Also
     * returned, together with a reason, when either string could not be
     * parsed at all — refusal reads as "no update" to a caller that only
     * checks the verdict, which is the safe default: never claim an update
     * exists on input this module could not make sense of. */
    MDKR_UPDATE_NONE = 0,
    /* The feed is a strict upgrade over the running version. */
    MDKR_UPDATE_AVAILABLE = 1
} MdkrUpdateVerdict;

/*
 * Compares two version strings under the grammar above and returns whether
 * `feed_version` is a strict upgrade over `running_version`.
 *
 * On a clean comparison (both strings parse, whatever the verdict),
 * `reason_out` is set to an empty string. On a refusal — either string is
 * NULL, empty, or does not match the grammar — the verdict is
 * MDKR_UPDATE_NONE and `reason_out` is set to a one-line, human-readable
 * explanation naming which side and why. A caller that wants to tell "no
 * update available" apart from "could not tell" checks whether
 * reason_out[0] is not '\0'.
 *
 * `reason_out`/`reason_out_size` may be NULL/0, in which case no reason is
 * written (the verdict is still correct). Neither input string is mutated,
 * and the function performs no I/O and no heap allocation — it is pure over
 * its arguments.
 */
MdkrUpdateVerdict mdkr_update_compare_versions(const char *running_version,
                                                const char *feed_version,
                                                char *reason_out,
                                                size_t reason_out_size);

/*
 * The interval policy: given the epoch-second timestamp of the last check and
 * the current epoch-second time — both supplied by the caller, never read
 * from the clock in here — returns 1 if a day or more has elapsed and a check
 * is due, 0 otherwise.
 *
 * `last_check_epoch_s <= 0` means "no recorded check" (a zero-initialised
 * field, or an explicit sentinel such as -1, both work) and is always due:
 * the first-ever check must not wait a day.
 *
 * If `now_epoch_s` is before `last_check_epoch_s` (the system clock moved
 * backward), elapsed time is treated as zero rather than underflowing or
 * producing a nonsensical negative-interval "due" — not due, and not stuck,
 * since the next call with a sane clock resumes the normal count.
 */
int mdkr_update_check_due(int64_t last_check_epoch_s, int64_t now_epoch_s);

#ifdef __cplusplus
}
#endif

#endif  /* MDKR64_UPDATE_CHECK_H */
