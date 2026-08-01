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
    uint64_t target;
    int rebased;
    MdkrPresentPolicy present;
    MdkrPresentPolicy same;
    MdkrPresentDeadlineClock present_clock;

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

    expect("original present policy parses",
           mdkr_present_policy_parse("original", &present) &&
               present.kind == MDKR_PRESENT_ORIGINAL && present.rate == 0u);
    expect("numeric present policy parses",
           mdkr_present_policy_parse("144", &present) &&
               present.kind == MDKR_PRESENT_CAPPED && present.rate == 144u);
    same = present;
    expect("present policy equality includes rate",
           mdkr_present_policy_equal(&present, &same));
    expect("numeric cap disables backend vsync",
           !mdkr_present_policy_uses_vsync(&present));
    expect("numeric cap above tick rate needs the subloop",
           mdkr_present_policy_needs_subloop(&present, 30u));
    expect("tick-rate cap does not need replay",
           mdkr_present_policy_parse("30", &present) &&
               !mdkr_present_policy_needs_subloop(&present, 30u));
    expect("display policy parses and uses backend sync",
           mdkr_present_policy_parse("display", &present) &&
               present.kind == MDKR_PRESENT_DISPLAY &&
               mdkr_present_policy_uses_vsync(&present) &&
               mdkr_present_policy_needs_subloop(&present, 30u));
    expect("uncapped policy parses without backend sync",
           mdkr_present_policy_parse("uncapped", &present) &&
               present.kind == MDKR_PRESENT_UNCAPPED &&
               !mdkr_present_policy_uses_vsync(&present));
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
