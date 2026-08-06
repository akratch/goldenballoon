#include "pacing_policy.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

int main(void) {
    MdkrPacingClock ntsc;
    MdkrPacingClock pal;
    MdkrPacingClock enhanced;
    MdkrPacingClock stall_below;
    MdkrPacingClock stall_equal;
    uint64_t target;
    int rebased;
    MdkrPresentPolicy present;
    MdkrPresentPolicy same;
    MdkrPresentDeadlineClock present_clock;
    MdkrCounterGuard counter = {0};

    expect("original is valid", mdkr_pacing_cadence_valid("original"));
    expect("enhanced is valid", mdkr_pacing_cadence_valid("enhanced"));
    expect("unknown is invalid", !mdkr_pacing_cadence_valid("60"));
    expect("null is invalid", !mdkr_pacing_cadence_valid(NULL));
    expect("original is two VI fields", mdkr_pacing_min_fields("original") == 2);
    expect("enhanced is one VI field", mdkr_pacing_min_fields("enhanced") == 1);
    expect("invalid fails safe to original", mdkr_pacing_min_fields("bad") == 2);
    expect("null fails safe to original", mdkr_pacing_min_fields(NULL) == 2);

    expect("NTSC source field rate",
           mdkr_pacing_field_hz(60, NULL) == 60);
    expect("PAL source field rate",
           mdkr_pacing_field_hz(50, NULL) == 50);
    expect("invalid source fails safe to NTSC",
           mdkr_pacing_field_hz(0, NULL) == 60);
    expect("diagnostic field-rate override",
           mdkr_pacing_field_hz(50, "120") == 120);
    expect("low field-rate override rejected",
           mdkr_pacing_field_hz(50, "19") == 50);
    expect("high field-rate override rejected",
           mdkr_pacing_field_hz(60, "241") == 60);
    expect("malformed field-rate override rejected",
           mdkr_pacing_field_hz(50, "60junk") == 50);

    expect("implicit synthetic cadence follows original",
           mdkr_pacing_synthetic_fields(0, 2, 6) == 2);
    expect("synthetic request cannot undercut original",
           mdkr_pacing_synthetic_fields(1, 2, 6) == 2);
    expect("enhanced synthetic one-field remains explicit",
           mdkr_pacing_synthetic_fields(1, 1, 6) == 1);
    expect("synthetic maximum clamps",
           mdkr_pacing_synthetic_fields(99, 1, 6) == 6);
    expect("video queue receives both original fields",
           mdkr_pacing_queue_refill(0, 2, 8) == 2);
    expect("video queue preserves existing retraces",
           mdkr_pacing_queue_refill(1, 2, 8) == 3);
    expect("video queue caps an oversleep burst",
           mdkr_pacing_queue_refill(7, 6, 8) == 8);
    expect("video queue fails safe on a zero measurement",
           mdkr_pacing_queue_refill(0, 0, 8) == 1);

    expect("stall interval below boundary is retained",
           !mdkr_pacing_interval_requires_rebase(
               MDKR_PACING_STALL_REBASE_NS - UINT64_C(1)));
    expect("stall interval equality rebases",
           mdkr_pacing_interval_requires_rebase(
               MDKR_PACING_STALL_REBASE_NS));
    expect("stall interval above boundary rebases",
           mdkr_pacing_interval_requires_rebase(
               MDKR_PACING_STALL_REBASE_NS + UINT64_C(1)));
    expect("225 ms interval rebases",
           mdkr_pacing_interval_requires_rebase(UINT64_C(225000000)));
    expect("250 ms interval rebases",
           mdkr_pacing_interval_requires_rebase(UINT64_C(250000000)));

    /* Host uptime selects an arbitrary low 32-bit COUNTER phase. A first
     * sample in the upper half is valid, not an apparent backwards step from
     * an invented zero predecessor. */
    expect("counter accepts a high-bit first sample",
           mdkr_counter_guard_commit(&counter, UINT32_C(0xf0000000)) ==
               UINT32_C(0xf0000000));
    expect("counter preserves a genuine forward sample",
           mdkr_counter_guard_commit(&counter, UINT32_C(0xf0000100)) ==
               UINT32_C(0xf0000100));
    expect("counter nudges equal samples by one hardware tick",
           mdkr_counter_guard_commit(&counter, UINT32_C(0xf0000100)) ==
               UINT32_C(0xf0000101));
    expect("counter accepts a genuine 32-bit wrap",
           mdkr_counter_guard_commit(&counter, UINT32_C(0x00000100)) ==
               UINT32_C(0x00000100));
    expect("null counter guard is a transparent fallback",
           mdkr_counter_guard_commit(NULL, UINT32_C(0xdeadbeef)) ==
               UINT32_C(0xdeadbeef));

    expect("initialize below-boundary stall clock",
           mdkr_pacing_clock_init(&stall_below, 60, 2, 6));
    (void)mdkr_pacing_clock_target(
        &stall_below, UINT64_C(9000000000));
    expect("clock keeps the interval immediately below boundary",
           mdkr_pacing_clock_commit(
               &stall_below,
               UINT64_C(9000000000) + MDKR_PACING_STALL_REBASE_NS -
                   UINT64_C(1),
               &rebased) == 6 && rebased == 0);
    expect("initialize equality-boundary stall clock",
           mdkr_pacing_clock_init(&stall_equal, 60, 2, 6));
    (void)mdkr_pacing_clock_target(
        &stall_equal, UINT64_C(10000000000));
    expect("clock rebases exactly at shared boundary",
           mdkr_pacing_clock_commit(
               &stall_equal,
               UINT64_C(10000000000) + MDKR_PACING_STALL_REBASE_NS,
               &rebased) == 6 && rebased == 1);

    expect("original present policy parses",
           mdkr_present_policy_parse("original", &present) &&
               present.kind == MDKR_PRESENT_ORIGINAL && present.rate == 0u);
    expect("numeric present policy parses",
           mdkr_present_policy_parse("144", &present) &&
               present.kind == MDKR_PRESENT_CAPPED && present.rate == 144u);
    same = present;
    expect("present policy equality includes rate",
           mdkr_present_policy_equal(&present, &same));
    expect("a cap above the display asks for the latest-image queue",
           mdkr_present_policy_sync(&present, 60u) ==
               MDKR_PRESENT_SYNC_LATEST);
    expect("the same cap under a faster display keeps the blocking queue",
           mdkr_present_policy_sync(&present, 240u) ==
               MDKR_PRESENT_SYNC_BLOCKING);
    expect("an unknown display refresh keeps the blocking queue",
           mdkr_present_policy_sync(&present, 0u) ==
               MDKR_PRESENT_SYNC_BLOCKING);
    expect("numeric cap above tick rate needs the subloop",
           mdkr_present_policy_needs_subloop(&present, 30u));
    expect("tick-rate cap does not need replay",
           mdkr_present_policy_parse("30", &present) &&
               !mdkr_present_policy_needs_subloop(&present, 30u));
    expect("a cap below the display keeps the blocking queue",
           mdkr_present_policy_sync(&present, 60u) ==
               MDKR_PRESENT_SYNC_BLOCKING);
    expect("display policy parses and uses backend sync",
           mdkr_present_policy_parse("display", &present) &&
               present.kind == MDKR_PRESENT_DISPLAY &&
               mdkr_present_policy_sync(&present, 60u) ==
                   MDKR_PRESENT_SYNC_BLOCKING &&
               mdkr_present_policy_needs_subloop(&present, 30u));
    expect("held display frames receive a software deadline",
           mdkr_present_policy_needs_held_frame_deadline(&present, 0));
    expect("interpolated display frames retain backend vsync pacing",
           !mdkr_present_policy_needs_held_frame_deadline(&present, 1));
    expect("uncapped policy parses and asks for the latest-image queue",
           mdkr_present_policy_parse("uncapped", &present) &&
               present.kind == MDKR_PRESENT_UNCAPPED &&
               mdkr_present_policy_sync(&present, 60u) ==
                   MDKR_PRESENT_SYNC_LATEST);
    expect("original policy keeps the blocking queue",
           mdkr_present_policy_parse("original", &present) &&
               mdkr_present_policy_sync(&present, 60u) ==
                   MDKR_PRESENT_SYNC_BLOCKING);
    expect("a null policy fails closed onto the blocking queue",
           mdkr_present_policy_sync(NULL, 60u) ==
               MDKR_PRESENT_SYNC_BLOCKING);
    (void)mdkr_present_policy_parse("uncapped", &present);
    expect("held uncapped frames cannot busy-spin",
           mdkr_present_policy_needs_held_frame_deadline(&present, 0));
    expect("interpolated uncapped frames remain uncapped",
           !mdkr_present_policy_needs_held_frame_deadline(&present, 1));
    expect("numeric caps already own their deadline",
           mdkr_present_policy_parse("240", &present) &&
               !mdkr_present_policy_needs_held_frame_deadline(&present, 0));
    expect("null held-frame policy fails closed",
           !mdkr_present_policy_needs_held_frame_deadline(NULL, 0));
    expect("legacy keyword spelling remains case-insensitive",
           mdkr_present_policy_parse("DISPLAY", &present) &&
               present.kind == MDKR_PRESENT_DISPLAY);
    expect("low numeric rate is rejected",
           !mdkr_present_policy_parse("29", &present));
    expect("high numeric rate is rejected",
           !mdkr_present_policy_parse("1001", &present));
    expect("malformed numeric rate is rejected",
           !mdkr_present_policy_parse("120hz", &present));
    expect("signed numeric rate is rejected",
           !mdkr_present_policy_parse("+120", &present));
    expect("space-prefixed numeric rate is rejected",
           !mdkr_present_policy_parse(" 120", &present));

    expect("initialize 144 Hz present deadline",
           mdkr_present_deadline_init(&present_clock, 144u));
    target = mdkr_present_deadline_target(
        &present_clock, UINT64_C(1000000000));
    expect("144 Hz first deadline is exact rational floor",
           target == UINT64_C(1006944444));
    mdkr_present_deadline_commit(&present_clock, target);
    expect("144 Hz second deadline retains absolute phase",
           mdkr_present_deadline_target(&present_clock, target) ==
               UINT64_C(1013888888));
    mdkr_present_deadline_commit(
        &present_clock, UINT64_C(1050000000));
    expect("late present skips expired deadlines without drift",
           mdkr_present_deadline_target(
               &present_clock, UINT64_C(1050000000)) ==
               UINT64_C(1055555555));

    expect("initialize NTSC original clock",
           mdkr_pacing_clock_init(&ntsc, 60, 2, 6));
    target = mdkr_pacing_clock_target(&ntsc, UINT64_C(1000000000));
    expect("NTSC first target is exact two-field rational",
           target == UINT64_C(1033333333));
    expect("NTSC early browser wake still injects two",
           mdkr_pacing_clock_commit(
               &ntsc, target - UINT64_C(2000000), &rebased) == 2);
    expect("early wake does not rebase", rebased == 0);
    expect("early wake preserves the rational grid",
           mdkr_pacing_clock_target(&ntsc, target - UINT64_C(2000000)) ==
               UINT64_C(1066666666));
    expect("timestamp just above boundary injects two",
           mdkr_pacing_clock_commit(
               &ntsc, UINT64_C(1066666667), &rebased) == 2);
    expect("NTSC long-run grid has no integer-field drift",
           mdkr_pacing_clock_target(&ntsc, UINT64_C(1066666667)) ==
               UINT64_C(1100000000));
    expect("three-field oversleep stays three fixed fields",
           mdkr_pacing_clock_commit(
               &ntsc, UINT64_C(1118000000), &rebased) == 3);
    expect("ordinary oversleep does not rebase", rebased == 0);
    expect("long stall clamps to game maximum",
           mdkr_pacing_clock_commit(
               &ntsc, UINT64_C(3000000000), &rebased) == 6);
    expect("long stall rebases", rebased == 1);
    expect("post-stall target starts a fresh two-field interval",
           mdkr_pacing_clock_target(&ntsc, UINT64_C(3000000000)) ==
               UINT64_C(3033333333));

    expect("initialize PAL original clock",
           mdkr_pacing_clock_init(&pal, 50, 2, 6));
    target = mdkr_pacing_clock_target(&pal, UINT64_C(5000000000));
    expect("PAL first target is 40ms",
           target == UINT64_C(5040000000));
    expect("PAL original injects two fields",
           mdkr_pacing_clock_commit(&pal, target, &rebased) == 2);
    expect("PAL second target retains 25Hz tick grid",
           mdkr_pacing_clock_target(&pal, target) == UINT64_C(5080000000));

    expect("initialize enhanced clock",
           mdkr_pacing_clock_init(&enhanced, 60, 1, 6));
    target = mdkr_pacing_clock_target(&enhanced, UINT64_C(7000000000));
    expect("enhanced first target is one field",
           target == UINT64_C(7016666666));
    expect("enhanced commit injects one field",
           mdkr_pacing_clock_commit(&enhanced, target, &rebased) == 1);

    if (s_failures != 0) {
        return 1;
    }
    puts("pacing policy: PASS");
    return 0;
}
