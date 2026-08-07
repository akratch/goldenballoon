#include "audio_queue_controller.h"

#include <stdint.h>
#include <stdio.h>

#define OUTPUT_RATE 22050u
#define COUNTER_RATE 46875000u
#define FRAME_SIZE 736u
#define MAX_SAMPLES 2048u

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_headless_determinism(void) {
    MdkrAudioQueueController controller;
    uint32_t first;
    uint32_t second;

    mdkr_audio_queue_controller_init(&controller);
    first = mdkr_audio_queue_controller_choose(
        &controller, false, 0u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, 0u);
    second = mdkr_audio_queue_controller_choose(
        &controller, false, UINT32_MAX, 1u, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, UINT32_MAX);
    expect("headless blocks ignore host time and occupancy",
           first == 736u && second == first);
    expect("headless blocks do not contaminate live history",
           !controller.have_last_counter &&
               controller.stats.live_decisions == 0u);
    expect("zero frame size has a 30 Hz fallback",
           mdkr_audio_queue_controller_choose(
               &controller, false, 0u, COUNTER_RATE, OUTPUT_RATE,
               0u, MAX_SAMPLES, 0u) == 720u);
}

static void test_clamps_alignment_and_wrap(void) {
    MdkrAudioQueueController controller;
    uint32_t value;

    mdkr_audio_queue_controller_init(&controller);
    value = mdkr_audio_queue_controller_choose(
        &controller, true, UINT32_MAX - 100u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, 1025u, 0u);
    expect("first live refill is capacity-clamped and 16-aligned",
           value == 1024u);
    value = mdkr_audio_queue_controller_choose(
        &controller, true, 100u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE * 2u);
    expect("counter wrap and an overfull queue retain the minimum block",
           value == 16u);
    expect("counter wrap is not misclassified as a stall",
           controller.stats.stall_guards == 0u);
    value = mdkr_audio_queue_controller_choose(
        &controller, true, 101u, 0u, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    expect("invalid counter rate uses the bounded fallback",
           value == 368u);
    expect("invalid counter rate is observable",
           controller.stats.stall_guards == 1u);
    expect("sub-16 synth capacity fails closed",
           mdkr_audio_queue_controller_choose(
               &controller, true, 0u, COUNTER_RATE, OUTPUT_RATE,
               FRAME_SIZE, 15u, 0u) == 0u);
}

static void run_schedule(const char *name, uint32_t service_rate) {
    MdkrAudioQueueController controller;
    uint64_t counter_numerator = 0u;
    uint64_t drain_numerator = 0u;
    uint64_t total_drained = 0u;
    uint32_t now = UINT32_MAX - COUNTER_RATE / 2u;
    uint32_t queued = 0u;
    uint32_t max_queued = 0u;
    uint32_t expected_queued = FRAME_SIZE + OUTPUT_RATE / service_rate;
    uint32_t step;

    mdkr_audio_queue_controller_init(&controller);
    for (step = 0u; step < service_rate * 20u; step++) {
        uint64_t due;
        uint32_t drained;
        uint32_t produced;

        counter_numerator += COUNTER_RATE;
        due = counter_numerator / service_rate;
        counter_numerator %= service_rate;
        now += (uint32_t)due;

        drain_numerator += OUTPUT_RATE;
        drained = (uint32_t)(drain_numerator / service_rate);
        drain_numerator %= service_rate;
        if (drained > queued) {
            drained = queued;
        }
        queued -= drained;
        total_drained += drained;

        produced = mdkr_audio_queue_controller_choose(
            &controller, true, now, COUNTER_RATE, OUTPUT_RATE,
            FRAME_SIZE, MAX_SAMPLES, queued);
        queued += produced;
        if (queued > max_queued) {
            max_queued = queued;
        }
    }

    /* SDL's application queue is sampled before enqueue. The controller keeps
     * that pre-enqueue occupancy near FRAME_SIZE while adding the interval's
     * estimated drain, so post-enqueue occupancy is one interval higher. */
    expect(name, queued + 32u >= expected_queued &&
                     queued <= expected_queued + 32u);
    expect("schedule queue stays below three target blocks",
           max_queued < FRAME_SIZE * 3u);
    expect("schedule has no stall recovery", controller.stats.stall_guards == 0u);
    expect("schedule accounts every production decision",
           controller.stats.live_decisions == (uint64_t)service_rate * 20u);
    expect("production conservation is exact",
           controller.stats.produced_frames == total_drained + queued);
}

static void test_rate_independence(void) {
    run_schedule("30 Hz controller converges", 30u);
    run_schedule("60 Hz controller converges", 60u);
    run_schedule("120 Hz controller converges", 120u);
    run_schedule("144 Hz controller converges", 144u);
    run_schedule("240 Hz controller converges", 240u);
    run_schedule("1000 Hz controller converges", 1000u);
}

/*
 * A 50 Hz source presented on a 60 Hz display. The authored 2-field tick is
 * 40 ms, which is not a whole number of 16.7 ms refreshes, so a blocking
 * present quantises consecutive ticks onto the refresh grid: the audio service
 * is therefore refilled on a repeating 50/33.3/50/33.3/33.3 ms beat whose mean
 * is the authored 40 ms. The queue has to survive the 50 ms half of that beat.
 *
 * This is the shape reported as PAL audio crackling; before the latency target
 * tracked the worst refill gap it emptied the sink on every long interval.
 */
#define PAL_FRAME_SIZE 896u
static const uint32_t kPalBeatMicros[] = { 50000u, 33333u, 50000u, 33333u,
                                           33334u };

static void test_coarse_refill_beat(void) {
    MdkrAudioQueueController controller;
    /* The host sink drains in whole device periods, not continuously: SDL's
     * queue backend hands the device one buffer at a time and pads whatever it
     * is short. 1,024 frames is the size this port opens (audi_port_dkr.c). */
    const uint32_t device_period = 1024u;
    uint64_t next_device_counter = 0u;
    uint64_t elapsed_micros = 0u;
    uint32_t queued = 0u;
    uint32_t min_queued_after_warmup = UINT32_MAX;
    uint32_t short_pulls = 0u;
    uint32_t step;

    mdkr_audio_queue_controller_init(&controller);
    for (step = 0u; step < 600u; step++) {
        const uint32_t interval =
            kPalBeatMicros[step % (sizeof(kPalBeatMicros) /
                                   sizeof(kPalBeatMicros[0]))];
        uint64_t interval_end;
        uint32_t produced;

        elapsed_micros += interval;
        interval_end = (uint64_t)elapsed_micros * COUNTER_RATE / 1000000u;
        while (next_device_counter <= interval_end) {
            uint32_t take = queued < device_period ? queued : device_period;
            if (take < device_period && step > 20u) {
                short_pulls++;
            }
            queued -= take;
            if (step > 20u && queued < min_queued_after_warmup) {
                min_queued_after_warmup = queued;
            }
            next_device_counter +=
                (uint64_t)device_period * COUNTER_RATE / OUTPUT_RATE;
        }
        produced = mdkr_audio_queue_controller_choose(
            &controller, true, (uint32_t)interval_end, COUNTER_RATE,
            OUTPUT_RATE, PAL_FRAME_SIZE, MAX_SAMPLES, queued);
        queued += produced;
    }

    expect("a coarse refill beat never starves the sink", short_pulls == 0u);
    expect("the beat keeps a real cushion rather than skimming empty",
           min_queued_after_warmup != UINT32_MAX &&
               min_queued_after_warmup > 0u);
    expect("the worst refill gap is observable",
           controller.stats.max_gap_frames > PAL_FRAME_SIZE);
    expect("the latency target grew to cover that gap",
           controller.stats.max_target_frames > PAL_FRAME_SIZE);
    expect("the cushion stays bounded at three blocks",
           controller.stats.max_target_frames <= PAL_FRAME_SIZE * 3u);
    expect("a coarse beat is not misread as a stall",
           controller.stats.stall_guards == 0u);
    /* The whole point of the adaptive target: the PAL-on-60Hz beat is exactly
     * the cadence that used to crackle, and it must now report a clean sink.
     * This is the counter the pacing-quality gate asserts is zero. */
    expect("a covered coarse beat reports no underrun",
           controller.stats.underruns == 0u);
}

static void test_even_refill_target_is_one_block(void) {
    MdkrAudioQueueController controller;
    uint32_t step;

    /* The authored NTSC cadence refills once per block. Nothing about the
     * adaptive target may move that case: same target, same latency. */
    mdkr_audio_queue_controller_init(&controller);
    for (step = 0u; step < 200u; step++) {
        (void)mdkr_audio_queue_controller_choose(
            &controller, true, (uint32_t)(step * (COUNTER_RATE / 30u)),
            COUNTER_RATE, OUTPUT_RATE, FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    }
    expect("an even refill cadence keeps the one-block target",
           controller.stats.max_target_frames == FRAME_SIZE);
    expect("an even refill cadence records no excess gap",
           controller.stats.max_gap_frames == FRAME_SIZE);
    expect("an even refill cadence never underruns",
           controller.stats.underruns == 0u);
    expect("a queue held at one block never brushes the floor",
           controller.stats.floor_breaches == 0u);
}

static void test_stall_recovery(void) {
    MdkrAudioQueueController controller;
    uint32_t first;
    uint32_t after_stall;

    mdkr_audio_queue_controller_init(&controller);
    first = mdkr_audio_queue_controller_choose(
        &controller, true, 10u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, 0u);
    after_stall = mdkr_audio_queue_controller_choose(
        &controller, true, 10u + COUNTER_RATE, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, 0u);
    expect("first empty refill is target plus half a block",
           first == 1104u);
    expect("one-second stall cannot request one second of PCM",
           after_stall == 1104u);
    expect("stalled host interval is observable",
           controller.stats.stall_guards == 1u);
    expect("empty queue observations are telemetry, not inferred underruns",
           controller.stats.empty_queue_observations == 2u);
    /*
     * Both calls saw an empty queue, but only the second is starvation: the
     * first is the boot prime, where the sink is empty because nothing has ever
     * been enqueued. Separating them is what makes underruns==0 assertable by a
     * gate -- counting the prime would make every healthy run fail.
     */
    expect("the boot prime is not counted as an underrun",
           controller.stats.underruns == 1u);
}

/*
 * The host asks for a whole device period at a time and is served short if the
 * sink holds less. A target expressed only in synthesis blocks cannot see that:
 * the shortfall happens inside the host, between refills, so the refill-gap
 * signal never observes it. WASAPI, PulseAudio/PipeWire and CoreAudio all
 * negotiate different periods, which is why this is the per-platform knob.
 */
static void test_device_period_floors_the_target(void) {
    MdkrAudioQueueController controller;
    uint32_t step;

    /* A period FINER than one block must change nothing at all — this is the
     * common case (the port asks for 256 frames) and it must not cost latency. */
    mdkr_audio_queue_controller_init(&controller);
    mdkr_audio_queue_controller_set_device_period(&controller, 256u);
    for (step = 0u; step < 200u; step++) {
        (void)mdkr_audio_queue_controller_choose(
            &controller, true, (uint32_t)(step * (COUNTER_RATE / 30u)),
            COUNTER_RATE, OUTPUT_RATE, FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    }
    expect("a fine device period leaves the one-block target untouched",
           controller.stats.max_target_frames == FRAME_SIZE);

    /* A period COARSER than one block must raise the floor to it, or the
     * device is served short on every callback no matter how even the refill
     * cadence looks from the producer's side. */
    mdkr_audio_queue_controller_init(&controller);
    mdkr_audio_queue_controller_set_device_period(&controller, 2048u);
    for (step = 0u; step < 200u; step++) {
        (void)mdkr_audio_queue_controller_choose(
            &controller, true, (uint32_t)(step * (COUNTER_RATE / 30u)),
            COUNTER_RATE, OUTPUT_RATE, FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    }
    expect("a coarse device period raises the target to one period",
           controller.stats.max_target_frames == 2048u);
    expect("the device-period floor is not misread as a refill gap",
           controller.stats.max_gap_frames == FRAME_SIZE);
}

/*
 * A long main-loop hitch, with and without ground truth.
 *
 * Inferring the drain from elapsed host time forces a defensive stall guard:
 * an interval implying more than four blocks is assumed to be a suspended
 * process and replaced with half a block. That is right about suspension and
 * wrong about a frametime hitch -- and when it fires it throws away the gap
 * measurement, so the adaptive target collapses to one block at exactly the
 * moment it needed to grow. A callback sink can measure the drain instead of
 * guessing, and then there is nothing to defend against.
 */
static void test_measured_drain_survives_a_hitch(void) {
    MdkrAudioQueueController estimated;
    MdkrAudioQueueController measured;
    /* 150 ms of drain at 22050 Hz: well past the four-block guard threshold. */
    const uint32_t hitch_frames = 3307u;
    const uint32_t hitch_counter = COUNTER_RATE / 1000u * 150u;

    mdkr_audio_queue_controller_init(&estimated);
    (void)mdkr_audio_queue_controller_choose(
        &estimated, true, 0u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    (void)mdkr_audio_queue_controller_choose(
        &estimated, true, hitch_counter, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, 0u);
    expect("an inferred drain trips the stall guard on a real hitch",
           estimated.stats.stall_guards == 1u);
    expect("the tripped guard hides the gap the sink must cover",
           estimated.stats.max_gap_frames == FRAME_SIZE);

    mdkr_audio_queue_controller_init(&measured);
    (void)mdkr_audio_queue_controller_choose(
        &measured, true, 0u, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, FRAME_SIZE);
    mdkr_audio_queue_controller_note_drain(&measured, hitch_frames);
    (void)mdkr_audio_queue_controller_choose(
        &measured, true, hitch_counter, COUNTER_RATE, OUTPUT_RATE,
        FRAME_SIZE, MAX_SAMPLES, 0u);
    expect("a measured drain never trips the stall guard",
           measured.stats.stall_guards == 0u);
    expect("a measured drain reports the real gap",
           measured.stats.max_gap_frames == hitch_frames);
    expect("the real gap raises the latency target",
           measured.stats.max_target_frames > FRAME_SIZE);
    expect("the raised target still respects the cushion ceiling",
           measured.stats.max_target_frames <= FRAME_SIZE * 3u);
    /* The measurement is single-shot: the next decision must fall back to the
     * estimate rather than reusing a stale drain forever. */
    expect("a measured drain is consumed by the decision it informs",
           !measured.have_measured_drain);
}

int main(void) {
    test_headless_determinism();
    test_device_period_floors_the_target();
    test_measured_drain_survives_a_hitch();
    test_clamps_alignment_and_wrap();
    test_rate_independence();
    test_even_refill_target_is_one_block();
    test_coarse_refill_beat();
    test_stall_recovery();
    if (s_failures != 0) {
        fprintf(stderr, "%d audio-queue-controller test(s) failed\n", s_failures);
        return 1;
    }
    puts("audio queue controller: PASS");
    return 0;
}
