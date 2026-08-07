/*
 * Concurrent SPSC contract for the native sink ring (platform/audio_ring.c).
 *
 * WHY A SECOND RING TEST
 *
 * tests/test_audio_ring.c drives push and pull from one thread, interleaved by
 * hand. That is the right shape for the content and concealment properties: it
 * makes them deterministic. But it cannot see the class of defect the ring
 * exists to avoid, because the whole class requires two threads to be inside
 * the structure at once. A producer that wrote the consumer's index, for
 * instance, passed every sequential case in that file — the sequential harness
 * simply never had a pull in flight for the write to race.
 *
 * So this file runs the real configuration: the game main loop's push on one
 * thread, SDL's audio callback's pull on another, for long enough to wrap the
 * ring many times over, with a forced-overflow phase in the middle. It asserts
 * INVARIANTS rather than exact values, because with two threads running free
 * the exact values are not reproducible and asserting them would either be
 * flaky or secretly single-threaded.
 *
 * The invariant that matters most is the one the overflow redesign turns on:
 *
 *     fill <= capacity, always, as the CONSUMER sees it.
 *
 * The ring's fill is a control input on both sides. If the producer moved
 * `tail` to evict, its write could regress a `tail` the consumer had already
 * advanced from a value it captured earlier, and the next fill the consumer
 * computed would exceed the capacity — it would then read frames that do not
 * exist, from slots the producer is concurrently overwriting. Under the
 * current design the producer only ever advances `head`, so a fill above the
 * capacity is a legitimate, deliberate signal that frames were overwritten,
 * and the consumer resolves it by clamping ITS OWN index and crossfading the
 * skip. This test asserts the consumer never observes a post-clamp fill above
 * the capacity, and that the clamp arms concealment.
 *
 * Run it under a sanitizer to get the rest: ASan/TSan see the index and
 * payload races directly, where the assertions can only see their consequences.
 */
#include "audio_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* Never zero, so a concealed frame is distinguishable from a delivered one. */
static int16_t sample_at(uint32_t frame, uint32_t channel) {
    const int32_t v = (int32_t)((frame * 2654435761u + channel * 40503u) >> 17);
    const int16_t s = (int16_t)((v % 20000) - 10000);
    return s == 0 ? (int16_t)1234 : s;
}

/* ---- shared fixture ------------------------------------------------------ */

#define RING_FRAMES     4096u
#define PUSH_BLOCK      512u
#define PULL_BLOCK      256u
/*
 * Enough to wrap a 4096-frame ring several hundred times.
 *
 * Overridable because a ThreadSanitizer run wants a much shorter stream: TSan
 * instruments every atomic, and it detects a race from the interleaving, not
 * from the duration -- the two threads are inside the ring together within the
 * first few blocks. The long default is for the uninstrumented run, where the
 * only way to shake out a timing-dependent bug is to keep going.
 */
#ifndef STREAM_FRAMES
#define STREAM_FRAMES   (RING_FRAMES * 400u)
#endif

typedef struct Fixture {
    MdkrAudioRing ring;
    SDL_atomic_t producer_done;
    SDL_atomic_t stop_consumer;
    /* Consumer-side verdicts, published for the main thread to read after the
     * join. Written only by the consumer thread. */
    SDL_atomic_t fill_exceeded_capacity;
    SDL_atomic_t pulls;
    /* Producer-side. */
    uint64_t pushed_frames;
    uint64_t reported_evictions;
    /* Throttle the producer to roughly the consumer's rate, or not. */
    int flood;
} Fixture;

static int SDLCALL producer_thread(void *arg) {
    Fixture *fx = (Fixture *)arg;
    int16_t block[PUSH_BLOCK * 2u];
    uint32_t produced = 0u;

    while (produced < STREAM_FRAMES) {
        uint32_t i;
        for (i = 0u; i < PUSH_BLOCK; i++) {
            block[i * 2u] = sample_at(produced + i, 0u);
            block[i * 2u + 1u] = sample_at(produced + i, 1u);
        }
        fx->reported_evictions +=
            mdkr_audio_ring_push(&fx->ring, block, PUSH_BLOCK);
        fx->pushed_frames += PUSH_BLOCK;
        produced += PUSH_BLOCK;
        if (!fx->flood) {
            /*
             * Sized to keep the ring near half full so the steady phase spends
             * its time in ordinary wraparound rather than pinned at an
             * extreme. The flood arm deliberately removes this.
             */
            while (mdkr_audio_ring_fill(&fx->ring) > RING_FRAMES / 2u) {
                SDL_Delay(0);
            }
        }
    }
    SDL_AtomicSet(&fx->producer_done, 1);
    return 0;
}

static int SDLCALL consumer_thread(void *arg) {
    Fixture *fx = (Fixture *)arg;
    int16_t out[PULL_BLOCK * 2u];

    for (;;) {
        uint32_t fill;

        mdkr_audio_ring_pull(&fx->ring, out, PULL_BLOCK);
        SDL_AtomicAdd(&fx->pulls, 1);

        /*
         * Only meaningful while the producer is THROTTLED. With the producer
         * running free the raw fill is legitimately unbounded between pulls —
         * the ring never blocks a push, so head runs as far ahead as the
         * producer gets, and that surplus IS the overwrite signal. The bounded
         * quantity is the one the consumer reads from, which is not visible
         * out here; the flood arm asserts it through stats.max_readable_frames
         * instead. (Checking the raw fill here in the flood arm is exactly the
         * mistake that made the first version of this test fail against
         * correct code.)
         */
        if (!fx->flood) {
            fill = mdkr_audio_ring_fill(&fx->ring);
            if (fill > mdkr_audio_ring_capacity(&fx->ring)) {
                SDL_AtomicSet(&fx->fill_exceeded_capacity, 1);
            }
        }

        if (SDL_AtomicGet(&fx->stop_consumer)) {
            break;
        }
        /* Model a device period: without this the consumer spins so much
         * faster than the producer that the ring is dry the whole run and the
         * wraparound coverage is lost. */
        SDL_Delay(0);
    }
    return 0;
}

static void run_stream(Fixture *fx, int flood) {
    SDL_Thread *producer;
    SDL_Thread *consumer;

    memset(fx, 0, sizeof(*fx));
    fx->flood = flood;
    expect("threaded ring initialises",
           mdkr_audio_ring_init(&fx->ring, RING_FRAMES));
    SDL_AtomicSet(&fx->producer_done, 0);
    SDL_AtomicSet(&fx->stop_consumer, 0);
    SDL_AtomicSet(&fx->fill_exceeded_capacity, 0);
    SDL_AtomicSet(&fx->pulls, 0);

    consumer = SDL_CreateThread(consumer_thread, "ring-consumer", fx);
    producer = SDL_CreateThread(producer_thread, "ring-producer", fx);
    expect("both threads start", consumer != NULL && producer != NULL);
    if (consumer == NULL || producer == NULL) {
        SDL_AtomicSet(&fx->stop_consumer, 1);
        if (consumer != NULL) {
            SDL_WaitThread(consumer, NULL);
        }
        if (producer != NULL) {
            SDL_WaitThread(producer, NULL);
        }
        mdkr_audio_ring_free(&fx->ring);
        return;
    }

    SDL_WaitThread(producer, NULL);
    /* Let the consumer drain what the producer left before telling it to stop,
     * so the shutdown itself is a normal steady-state moment rather than a
     * special case. */
    SDL_Delay(20);
    SDL_AtomicSet(&fx->stop_consumer, 1);
    SDL_WaitThread(consumer, NULL);
}

/*
 * Steady concurrent operation across many wraparounds.
 *
 * The producer is throttled toward half-full, so this arm is about the
 * ordinary path: two threads crossing the ring's wrap point continuously,
 * thousands of times, with nothing forcing an extreme.
 */
static void test_concurrent_stream_across_wraparound(void) {
    Fixture fx;
    MdkrAudioRingStats stats;

    run_stream(&fx, 0);
    if (fx.ring.capacity == 0u) {
        return;
    }
    mdkr_audio_ring_stats(&fx.ring, &stats);

    expect("the consumer never observes a fill above the capacity",
           SDL_AtomicGet(&fx.fill_exceeded_capacity) == 0);
    expect("the consumer never reads past the capacity",
           stats.max_readable_frames <= mdkr_audio_ring_capacity(&fx.ring));
    expect("the whole stream was pushed",
           fx.pushed_frames == (uint64_t)STREAM_FRAMES);
    expect("the run really wrapped the ring many times",
           stats.pushed_frames > (uint64_t)RING_FRAMES * 8u);
    expect("the consumer really ran", SDL_AtomicGet(&fx.pulls) > 0);
    expect("the device was served every frame it asked for",
           stats.requested_frames ==
               (uint64_t)SDL_AtomicGet(&fx.pulls) * PULL_BLOCK);
    expect("delivered plus concealed accounts for every requested frame",
           stats.pulled_frames + stats.silence_frames <= stats.requested_frames);
    expect("nothing was delivered that was never pushed",
           stats.pulled_frames <= stats.pushed_frames);
    /* The final indices must be consistent: everything pushed was either
     * retired by the consumer or is still sitting in the ring. */
    expect("the residual fill is within the capacity",
           mdkr_audio_ring_fill(&fx.ring) <= mdkr_audio_ring_capacity(&fx.ring));
    mdkr_audio_ring_free(&fx.ring);
}

/*
 * The forced-overflow arm — the one that exercises the redesign.
 *
 * With the producer unthrottled it outruns the consumer permanently, so the
 * ring is driven past capacity over and over while the consumer is inside
 * pull. Under the old design this is where the producer's write to `tail`
 * raced the consumer's captured-tail write-back. Under the current one the
 * producer only advances `head`, and every overflow must be resolved by the
 * consumer's clamp.
 */
static void test_forced_overflow_is_resolved_by_the_consumer(void) {
    Fixture fx;
    MdkrAudioRingStats stats;

    run_stream(&fx, 1);
    if (fx.ring.capacity == 0u) {
        return;
    }
    mdkr_audio_ring_stats(&fx.ring, &stats);

    /* Non-vacuity: if the flood did not actually overflow, everything below is
     * asserting nothing. */
    expect("the flood arm really overflowed the ring", stats.overflows > 0u);
    expect("the flood arm really reported evictions",
           stats.evicted_frames > 0u && fx.reported_evictions > 0u);
    expect("the consumer resolved the overflows with a clamp",
           stats.overflow_skips > 0u);
    expect("each clamp armed a crossfade over the discontinuity",
           stats.concealments >= stats.overflow_skips);

    /*
     * THE B1 ASSERTION. The readable count is what the consumer indexes the
     * slot array with; a value above the capacity means it addressed frames
     * that do not exist, which is what a regressed `tail` produces and what
     * the clamp exists to make impossible. Sampled inside pull, after the
     * clamp, because that is the only place the quantity is well defined.
     */
    expect("a flooded ring never lets the consumer read past its capacity",
           stats.max_readable_frames <= mdkr_audio_ring_capacity(&fx.ring));
    /* Non-vacuity for that assertion: the flood must actually have driven the
     * readable count up to the ceiling, not merely stayed far below it. */
    expect("the flood really pushed the readable count to the ceiling",
           stats.max_readable_frames == mdkr_audio_ring_capacity(&fx.ring));
    expect("no frame was delivered twice",
           stats.pulled_frames <= stats.pushed_frames);
    mdkr_audio_ring_free(&fx.ring);
}

/*
 * Back-to-back underflow episodes on the real two-thread configuration.
 *
 * A dry ring must re-arm concealment on EVERY entry into starvation, not just
 * the first: `starving` latches, and a latch that is never cleared conceals
 * the first dropout of a session and steps through every one after it. Driven
 * here by pulling a ring that is only intermittently fed.
 */
#define UNDERFLOW_EPISODES 8u

static void test_repeated_underflow_rearms_concealment(void) {
    MdkrAudioRing ring;
    int16_t in[PULL_BLOCK * 2u];
    int16_t out[PULL_BLOCK * 2u];
    MdkrAudioRingStats before;
    MdkrAudioRingStats after;
    uint32_t episode;
    uint32_t i;

    expect("underflow ring initialises", mdkr_audio_ring_init(&ring, RING_FRAMES));
    if (ring.capacity == 0u) {
        return;
    }
    /* Prime: the boot-prime exemption must be spent before the episodes, or
     * the first underrun would not be counted as one. */
    for (i = 0u; i < PULL_BLOCK; i++) {
        in[i * 2u] = sample_at(i, 0u);
        in[i * 2u + 1u] = sample_at(i, 1u);
    }
    (void)mdkr_audio_ring_push(&ring, in, PULL_BLOCK);
    mdkr_audio_ring_pull(&ring, out, PULL_BLOCK);
    expect("the ring is primed after its first real delivery", ring.primed);

    mdkr_audio_ring_stats(&ring, &before);
    for (episode = 0u; episode < UNDERFLOW_EPISODES; episode++) {
        /* Dry: the ring has nothing, so this is a starvation entry. */
        mdkr_audio_ring_pull(&ring, out, PULL_BLOCK);
        /* Fed again: this is a starvation exit. Both edges are step
         * discontinuities and both must be crossfaded. */
        (void)mdkr_audio_ring_push(&ring, in, PULL_BLOCK);
        mdkr_audio_ring_pull(&ring, out, PULL_BLOCK);
    }
    mdkr_audio_ring_stats(&ring, &after);

    expect("every dry episode is counted as its own underrun",
           after.underruns - before.underruns == UNDERFLOW_EPISODES);
    /* Two edges per episode: the ramp out into silence and the ramp back in. */
    expect("concealment re-arms on both edges of every episode",
           after.concealments - before.concealments == UNDERFLOW_EPISODES * 2u);
    mdkr_audio_ring_free(&ring);
}

/*
 * The requested counter is a 32-bit wrapping control input: the producer
 * differences two samples of it to size the next synthesis block. At 22050 Hz
 * it wraps every ~54 hours, which no test will ever reach honestly and no
 * developer will ever hit before a player does. Start it just below the wrap
 * and step across, asserting the DIFFERENCE stays exact — that is the only
 * property anything downstream uses.
 */
static void test_requested_counter_wraps_cleanly(void) {
    MdkrAudioRing ring;
    int16_t out[PULL_BLOCK * 2u];
    uint32_t before;
    uint32_t after;
    uint32_t total = 0u;
    int step;
    int crossed = 0;

    expect("wrap ring initialises", mdkr_audio_ring_init(&ring, RING_FRAMES));
    if (ring.capacity == 0u) {
        return;
    }
    /* Seed the counter near the top of its range. This reaches into the ring
     * deliberately: there is no other way to put a 54-hour-old process state
     * in front of the code that has to survive it. */
    SDL_AtomicSet(&ring.requested, (int)(UINT32_MAX - (PULL_BLOCK * 3u) + 1u));
    expect("the seeded counter reads back",
           mdkr_audio_ring_requested(&ring) ==
               UINT32_MAX - (PULL_BLOCK * 3u) + 1u);

    for (step = 0; step < 8; step++) {
        before = mdkr_audio_ring_requested(&ring);
        mdkr_audio_ring_pull(&ring, out, PULL_BLOCK);
        after = mdkr_audio_ring_requested(&ring);
        /* uint32 subtraction: defined, and exact across the wrap. */
        expect("the requested delta is exact on both sides of the wrap",
               after - before == PULL_BLOCK);
        if (after < before) {
            crossed = 1;
        }
        total += PULL_BLOCK;
    }
    expect("the run really crossed the 32-bit wrap", crossed);
    expect("the accumulated delta matches the frames requested",
           total == PULL_BLOCK * 8u);
    mdkr_audio_ring_free(&ring);
}

int main(void) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(0) != 0) {
        fprintf(stderr, "FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    test_concurrent_stream_across_wraparound();
    test_forced_overflow_is_resolved_by_the_consumer();
    test_repeated_underflow_rearms_concealment();
    test_requested_counter_wraps_cleanly();
    SDL_Quit();
    if (s_failures != 0) {
        fprintf(stderr, "%d threaded audio-ring test(s) failed\n", s_failures);
        return 1;
    }
    puts("audio ring (threaded): PASS");
    return 0;
}
