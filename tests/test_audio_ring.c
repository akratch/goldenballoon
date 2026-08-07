/*
 * ROM-free contract for the native sink ring (platform/audio_ring.c).
 *
 * The ring is what replaced SDL_QueueAudio as the native delivery mechanism
 * (issue #23). Two of its properties are load-bearing and neither was
 * expressible while SDL owned the buffer:
 *
 *   - CONTENT IDENTITY. PCM that goes in comes out byte-identical. The fix
 *     changes delivery timing only; if the ring altered a single sample the
 *     audio gates downstream would be measuring something the synthesiser did
 *     not produce.
 *   - UNDERFLOW CONCEALMENT. When the ring runs dry the output ramps to
 *     silence and back rather than stepping. SDL queue mode padded shortfalls
 *     with hard silence inside SDL, so every dropout arrived as a pair of step
 *     discontinuities — the clicking that issue #23 reports. The step-size
 *     assertions below fail by construction against that behaviour.
 */
#include "audio_ring.h"

#include <stdio.h>
#include <stdlib.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* A deterministic, non-trivial signal: never zero, so a concealed frame is
 * always distinguishable from a real one. */
static int16_t sample_at(uint32_t frame, uint32_t channel) {
    const int32_t v = (int32_t)((frame * 2654435761u + channel * 40503u) >> 17);
    const int16_t s = (int16_t)((v % 20000) - 10000);
    return s == 0 ? (int16_t)1234 : s;
}

static void fill_block(int16_t *out, uint32_t first, uint32_t frames) {
    uint32_t i;
    for (i = 0u; i < frames; i++) {
        out[i * 2u] = sample_at(first + i, 0u);
        out[i * 2u + 1u] = sample_at(first + i, 1u);
    }
}

static void test_roundtrip_is_byte_identical(void) {
    MdkrAudioRing ring;
    int16_t in[512 * 2];
    int16_t out[512 * 2];
    uint32_t i;
    int identical = 1;

    expect("ring initialises", mdkr_audio_ring_init(&ring, 2048u));
    expect("capacity is rounded up to a power of two",
           mdkr_audio_ring_capacity(&ring) == 2048u);

    fill_block(in, 0u, 512u);
    expect("a push into an empty ring evicts nothing",
           mdkr_audio_ring_push(&ring, in, 512u) == 0u);
    expect("fill reports the pushed frames",
           mdkr_audio_ring_fill(&ring) == 512u);

    mdkr_audio_ring_pull(&ring, out, 512u);
    for (i = 0u; i < 512u * 2u; i++) {
        if (in[i] != out[i]) {
            identical = 0;
        }
    }
    expect("PCM crosses the ring byte-identically", identical);
    expect("a full pull drains the ring", mdkr_audio_ring_fill(&ring) == 0u);
    expect("a clean roundtrip conceals nothing",
           ring.stats.underruns == 0u && ring.stats.silence_frames == 0u);
    mdkr_audio_ring_free(&ring);
}

static void test_wraparound_is_byte_identical(void) {
    MdkrAudioRing ring;
    int16_t in[300 * 2];
    int16_t out[300 * 2];
    uint32_t round;
    uint32_t produced = 0u;
    uint32_t consumed = 0u;
    int identical = 1;

    /* 1024-frame ring, 300-frame traffic: the read and write indices wrap at
     * different points, which is where an index bug shows up. */
    expect("wrap ring initialises", mdkr_audio_ring_init(&ring, 1024u));
    for (round = 0u; round < 200u; round++) {
        uint32_t i;
        fill_block(in, produced, 300u);
        (void)mdkr_audio_ring_push(&ring, in, 300u);
        produced += 300u;
        mdkr_audio_ring_pull(&ring, out, 300u);
        for (i = 0u; i < 300u; i++) {
            if (out[i * 2u] != sample_at(consumed + i, 0u) ||
                out[i * 2u + 1u] != sample_at(consumed + i, 1u)) {
                identical = 0;
            }
        }
        consumed += 300u;
    }
    expect("index wraparound preserves every sample", identical);
    expect("balanced traffic never underruns", ring.stats.underruns == 0u);
    expect("balanced traffic never overflows", ring.stats.overflows == 0u);
    mdkr_audio_ring_free(&ring);
}

static void test_overflow_drops_the_oldest(void) {
    MdkrAudioRing ring;
    int16_t in[1024 * 2];
    int16_t out[1024 * 2];
    uint32_t i;
    int kept_newest = 1;

    expect("overflow ring initialises", mdkr_audio_ring_init(&ring, 1024u));
    fill_block(in, 0u, 1024u);
    (void)mdkr_audio_ring_push(&ring, in, 1024u);
    /* Ring is now exactly full. Push 256 more: the oldest 256 must go, so the
     * ring holds frames 256..1279. Keeping the FRESHEST audio is the
     * realtime-correct policy and matches the web worklet. */
    fill_block(in, 1024u, 256u);
    expect("overflow evicts exactly the surplus",
           mdkr_audio_ring_push(&ring, in, 256u) == 256u);
    /*
     * The producer signals the overwrite by letting head lead tail by more
     * than the capacity; it may NOT retire the frames itself, because `tail`
     * belongs to the consumer. So the surplus is visible in the fill until the
     * consumer resolves it.
     */
    expect("overflow leaves the surplus visible to the consumer",
           mdkr_audio_ring_fill(&ring) == 1024u + 256u);

    mdkr_audio_ring_pull(&ring, out, 1024u);
    expect("the consumer clamps the overflow away",
           ring.stats.overflow_skips == 1u);
    expect("the clamp leaves the ring empty after a full-capacity pull",
           mdkr_audio_ring_fill(&ring) == 0u);
    for (i = 0u; i < 1024u; i++) {
        if (out[i * 2u] != sample_at(256u + i, 0u)) {
            kept_newest = 0;
        }
    }
    expect("overflow drops the OLDEST frames, not the newest", kept_newest);
    expect("overflow is counted", ring.stats.overflows == 1u &&
                                      ring.stats.evicted_frames == 256u);
    mdkr_audio_ring_free(&ring);
}

/*
 * The property SDL_QueueAudio could not provide.
 *
 * Starve the ring mid-stream and measure the largest sample-to-sample step in
 * the delivered output. Hard silence padding produces a step equal to the last
 * sample's magnitude (up to full scale); a concealed dropout ramps, so the
 * step is bounded by roughly magnitude/FADE_FRAMES. The bound asserted here is
 * far below what an unconcealed cut can produce, so this test cannot pass
 * against the old delivery path.
 */
#define CONCEAL_LEVEL 10000

static void test_underflow_is_concealed(void) {
    MdkrAudioRing ring;
    int16_t in[256 * 2];
    int16_t out[1024 * 2];
    int32_t max_step = 0;
    int32_t previous;
    uint32_t i;

    /*
     * A CONSTANT signal, deliberately. The quantity under test is the
     * concealment envelope, and any step the source itself contains would be
     * indistinguishable from a step the sink introduced. Against a flat
     * source every non-zero step in the output is the sink's doing, so the
     * bound below measures exactly one thing.
     */
    for (i = 0u; i < 256u; i++) {
        in[i * 2u] = (int16_t)CONCEAL_LEVEL;
        in[i * 2u + 1u] = (int16_t)CONCEAL_LEVEL;
    }

    expect("conceal ring initialises", mdkr_audio_ring_init(&ring, 4096u));

    /* Prime with real audio so the ring is past its boot state. */
    (void)mdkr_audio_ring_push(&ring, in, 256u);
    mdkr_audio_ring_pull(&ring, out, 256u);
    previous = out[255u * 2u];

    /* Now ask for 512 frames with an EMPTY ring: a total dropout. */
    mdkr_audio_ring_pull(&ring, out, 512u);
    for (i = 0u; i < 512u; i++) {
        const int32_t step = out[i * 2u] - previous;
        const int32_t magnitude = step < 0 ? -step : step;
        if (magnitude > max_step) {
            max_step = magnitude;
        }
        previous = out[i * 2u];
    }
    /*
     * A concealed edge moves at about level/FADE_FRAMES per sample (10000/128
     * ~= 78). An unconcealed cut -- SDL queue mode padding the shortfall with
     * hard silence -- steps the full 10000 in one sample. Any bound between
     * the two proves concealment; 512 leaves room for rounding while staying
     * an order of magnitude below the unconcealed case.
     */
    expect("entering a dropout ramps instead of stepping to silence",
           max_step <= 512);
    expect("a dropout eventually reaches true silence",
           out[511u * 2u] == 0 && out[511u * 2u + 1u] == 0);
    expect("the dropout is counted once, not per frame",
           ring.stats.underruns == 1u);
    expect("the dropout's lost frames are accounted",
           ring.stats.silence_frames == 512u);

    /* Resume: real audio returns after the gap and must ramp back IN. */
    (void)mdkr_audio_ring_push(&ring, in, 256u);
    max_step = 0;
    previous = 0;
    mdkr_audio_ring_pull(&ring, out, 256u);
    for (i = 0u; i < 256u; i++) {
        const int32_t step = out[i * 2u] - previous;
        const int32_t magnitude = step < 0 ? -step : step;
        if (magnitude > max_step) {
            max_step = magnitude;
        }
        previous = out[i * 2u];
    }
    expect("leaving a dropout ramps instead of stepping back in",
           max_step <= 512);
    expect("both edges of the dropout were concealed",
           ring.stats.concealments >= 2u);
    /* Concealment must be transient: once the ramp is done the signal is the
     * synthesiser's, untouched. */
    expect("audio past the ramp is the exact pushed signal",
           out[200u * 2u] == (int16_t)CONCEAL_LEVEL &&
               out[200u * 2u + 1u] == (int16_t)CONCEAL_LEVEL);
    mdkr_audio_ring_free(&ring);
}

/*
 * The boot prime is not starvation. The device is unpaused before the first
 * synthesis block exists, so the opening callbacks legitimately find nothing.
 * Counting those would make `underruns == 0` unassertable by any gate.
 */
static void test_boot_prime_is_not_an_underrun(void) {
    MdkrAudioRing ring;
    int16_t in[256 * 2];
    int16_t out[256 * 2];

    expect("prime ring initialises", mdkr_audio_ring_init(&ring, 1024u));
    mdkr_audio_ring_pull(&ring, out, 256u);
    mdkr_audio_ring_pull(&ring, out, 256u);
    expect("callbacks before the first push are not underruns",
           ring.stats.underruns == 0u && ring.stats.silence_frames == 0u);
    expect("an unprimed ring still emits defined silence",
           out[0] == 0 && out[511] == 0);

    fill_block(in, 0u, 256u);
    (void)mdkr_audio_ring_push(&ring, in, 256u);
    mdkr_audio_ring_pull(&ring, out, 256u);
    mdkr_audio_ring_pull(&ring, out, 256u);
    expect("starvation after priming IS an underrun",
           ring.stats.underruns == 1u);
    mdkr_audio_ring_free(&ring);
}

static void test_partial_pull_keeps_the_remainder(void) {
    MdkrAudioRing ring;
    int16_t in[512 * 2];
    int16_t out[512 * 2];
    uint32_t i;
    int identical = 1;

    expect("partial ring initialises", mdkr_audio_ring_init(&ring, 2048u));
    fill_block(in, 0u, 512u);
    (void)mdkr_audio_ring_push(&ring, in, 512u);
    /* Ask for more than is present: the ring must deliver everything it has
     * and conceal only the shortfall, never discard the good frames. */
    mdkr_audio_ring_pull(&ring, out, 512u);
    for (i = 0u; i < 512u; i++) {
        if (out[i * 2u] != sample_at(i, 0u)) {
            identical = 0;
        }
    }
    expect("a pull served exactly to the end loses nothing", identical);
    expect("a pull that exactly empties the ring is not an underrun",
           ring.stats.underruns == 0u);
    mdkr_audio_ring_free(&ring);
}

/*
 * Every other case here asks for a capacity that is ALREADY a power of two, so
 * round_up_pow2 has never actually had to round anything: a broken
 * implementation that returned its argument unchanged would pass the whole
 * file. The mask arithmetic every read and write depends on is only valid for
 * a power-of-two capacity, so feed it a value that is not one.
 */
static void test_capacity_rounds_a_non_power_of_two_up(void) {
    MdkrAudioRing ring;
    int16_t in[64 * 2];
    uint32_t capacity;

    expect("odd-capacity ring initialises", mdkr_audio_ring_init(&ring, 3000u));
    capacity = mdkr_audio_ring_capacity(&ring);
    expect("a non-power-of-two request rounds UP to the next power of two",
           capacity == 4096u);
    expect("capacity is at least the request", capacity >= 3000u);
    expect("the mask is capacity-1", ring.mask == capacity - 1u);
    expect("capacity is a power of two", (capacity & (capacity - 1u)) == 0u);
    /* And the rounded ring is functional, not just correctly sized. */
    fill_block(in, 0u, 64u);
    expect("the rounded ring accepts a push",
           mdkr_audio_ring_push(&ring, in, 64u) == 0u);
    expect("the rounded ring reports its fill",
           mdkr_audio_ring_fill(&ring) == 64u);
    mdkr_audio_ring_free(&ring);

    /* A request below the floor is raised to it, still a power of two. */
    expect("undersized ring initialises", mdkr_audio_ring_init(&ring, 1u));
    expect("a sub-minimum request is raised to the 1024-frame floor",
           mdkr_audio_ring_capacity(&ring) == 1024u);
    mdkr_audio_ring_free(&ring);
}

static void test_oversized_push_keeps_the_tail(void) {
    MdkrAudioRing ring;
    int16_t *in;
    int16_t out[1024 * 2];
    uint32_t i;
    int kept_tail = 1;

    expect("oversize ring initialises", mdkr_audio_ring_init(&ring, 1024u));
    in = (int16_t *)malloc(sizeof(int16_t) * 3000u * 2u);
    expect("oversize fixture allocates", in != NULL);
    if (in == NULL) {
        mdkr_audio_ring_free(&ring);
        return;
    }
    fill_block(in, 0u, 3000u);
    /* A block bigger than the whole ring can only keep its newest capacity
     * frames, and must report exactly the frames it could not keep. */
    expect("an oversized push reports the exact overflow",
           mdkr_audio_ring_push(&ring, in, 3000u) == 3000u - 1024u);
    mdkr_audio_ring_pull(&ring, out, 1024u);
    for (i = 0u; i < 1024u; i++) {
        if (out[i * 2u] != sample_at(3000u - 1024u + i, 0u)) {
            kept_tail = 0;
        }
    }
    expect("an oversized push keeps its newest frames", kept_tail);
    free(in);
    mdkr_audio_ring_free(&ring);
}

int main(void) {
    test_roundtrip_is_byte_identical();
    test_wraparound_is_byte_identical();
    test_overflow_drops_the_oldest();
    test_underflow_is_concealed();
    test_boot_prime_is_not_an_underrun();
    test_partial_pull_keeps_the_remainder();
    test_oversized_push_keeps_the_tail();
    test_capacity_rounds_a_non_power_of_two_up();
    if (s_failures != 0) {
        fprintf(stderr, "%d audio-ring test(s) failed\n", s_failures);
        return 1;
    }
    puts("audio ring: PASS");
    return 0;
}
