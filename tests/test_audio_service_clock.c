#include "audio_service_clock.h"

#include <stdio.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_no_minimum_per_call(void) {
    MdkrAudioServiceClock clock;
    mdkr_audio_service_clock_init(&clock, 2u);
    expect("one field is not a two-field audio quantum",
           mdkr_audio_service_clock_advance(&clock, 1u, false) == 0u);
    expect("presentation-only service cannot manufacture a block",
           !mdkr_audio_service_clock_take(&clock));
    expect("second field completes exactly one quantum",
           mdkr_audio_service_clock_advance(&clock, 1u, false) == 1u);
    expect("completed quantum can be serviced",
           mdkr_audio_service_clock_take(&clock));
    expect("quantum is consumed once", !mdkr_audio_service_clock_take(&clock));
}

static void test_grouping_invariance(void) {
    MdkrAudioServiceClock grouped;
    MdkrAudioServiceClock split;
    unsigned index;
    mdkr_audio_service_clock_init(&grouped, 2u);
    mdkr_audio_service_clock_init(&split, 2u);
    expect("six grouped fields make three quanta",
           mdkr_audio_service_clock_advance(&grouped, 6u, false) == 3u);
    for (index = 0; index < 6u; index++) {
        (void)mdkr_audio_service_clock_advance(&split, 1u, false);
    }
    expect("split and grouped time have equal debt",
           mdkr_audio_service_clock_pending(&grouped) ==
               mdkr_audio_service_clock_pending(&split));
    for (index = 0; index < 3u; index++) {
        expect("grouped quantum available",
               mdkr_audio_service_clock_take(&grouped));
        expect("split quantum available",
               mdkr_audio_service_clock_take(&split));
    }
    expect("both schedules drain exactly",
           mdkr_audio_service_clock_pending(&grouped) == 0u &&
           mdkr_audio_service_clock_pending(&split) == 0u);
}

static void test_subfield_schedule(void) {
    MdkrAudioServiceClock clock;
    unsigned present;

    mdkr_audio_service_clock_init(&clock, 2u);
    for (present = 0; present < 3u; present++) {
        expect("sub-field present cannot manufacture an audio quantum",
               mdkr_audio_service_clock_advance_units(
                   &clock, UINT64_C(500000000), false) == 0u);
    }
    expect("four half-field presents complete one two-field quantum",
           mdkr_audio_service_clock_advance_units(
               &clock, UINT64_C(500000000), false) == 1u);
    expect("sub-field accounting reports exactly two elapsed fields",
           clock.stats.elapsed_units == UINT64_C(2000000000) &&
               clock.stats.elapsed_fields == 2u);
    expect("sub-field quantum can be consumed exactly once",
           mdkr_audio_service_clock_take(&clock) &&
               !mdkr_audio_service_clock_take(&clock));
}

static void test_catchup_order_and_rebase(void) {
    MdkrAudioServiceClock clock;
    mdkr_audio_service_clock_init(&clock, 2u);
    (void)mdkr_audio_service_clock_advance(&clock, 6u, false);
    expect("catch-up retains three ordered quanta",
           mdkr_audio_service_clock_pending(&clock) == 3u);
    expect("one game pass consumes at most one quantum",
           mdkr_audio_service_clock_take(&clock) &&
           mdkr_audio_service_clock_pending(&clock) == 2u);
    expect("rebase starts a fresh interval",
           mdkr_audio_service_clock_advance(&clock, 2u, true) == 1u);
    expect("old catch-up debt was retired",
           mdkr_audio_service_clock_pending(&clock) == 1u &&
           clock.stats.retired_quanta == 2u);
    expect("rebase is observable", clock.stats.rebases == 1u);
}

static void test_enhanced_game_cadence(void) {
    MdkrAudioServiceClock clock;
    unsigned tick;
    unsigned serviced = 0u;
    mdkr_audio_service_clock_init(&clock, 2u);
    for (tick = 0; tick < 120u; tick++) {
        (void)mdkr_audio_service_clock_advance(&clock, 1u, false);
        if (mdkr_audio_service_clock_take(&clock)) {
            serviced++;
        }
    }
    expect("120 one-field game ticks still produce 60 audio quanta",
           serviced == 60u);
    expect("enhanced cadence leaves no audio debt",
           mdkr_audio_service_clock_pending(&clock) == 0u);
}

int main(void) {
    test_no_minimum_per_call();
    test_grouping_invariance();
    test_subfield_schedule();
    test_catchup_order_and_rebase();
    test_enhanced_game_cadence();
    if (s_failures != 0) {
        fprintf(stderr, "%d audio-service-clock test(s) failed\n", s_failures);
        return 1;
    }
    puts("audio service clock: PASS");
    return 0;
}
