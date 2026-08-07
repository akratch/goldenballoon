/*
 * audio_ring.h — single-producer/single-consumer PCM ring with underflow
 * concealment. This is the native analogue of the web AudioWorklet's ring
 * (platform/web_audio_worklet.c): the game's main loop is the producer, SDL's
 * own audio thread is the consumer, and the consumer never blocks on the
 * producer.
 *
 * WHY THIS EXISTS
 *
 * The port previously delivered PCM with SDL_QueueAudio. That is also drained
 * by SDL's audio thread, so the *drain* was already off the main loop; what it
 * did not give us was the UNDERFLOW PATH. When the application queue is short,
 * SDL pads the device buffer with hard digital silence, inside SDL, invisibly.
 * A 20 ms shortfall therefore reaches the speaker as silence bracketed by two
 * step discontinuities — two clicks — which is what issue #23 reports as
 * "stutter/jitter". The browser build does not have this problem because the
 * worklet owns its own underflow and crossfades it.
 *
 * Owning the ring gives three things SDL_QueueAudio cannot:
 *   1. Concealment. A dry ring ramps to silence and back over
 *      MDKR_AUDIO_RING_FADE_FRAMES instead of stepping, exactly as the worklet
 *      does. A residual dropout becomes a soft dip rather than a click pair.
 *   2. Ground-truth drain. The consumer counts the frames it really took, so
 *      the latency controller no longer has to infer the drain from elapsed
 *      osGetCount() time and no longer needs a "stall guard" heuristic that
 *      mis-sizes recovery after precisely the hitch it is meant to absorb.
 *   3. Device-period independence. Ring depth is chosen by us, not implied by
 *      the SDL buffer size the host happened to negotiate.
 *
 * The ring carries the SAME PCM the queue path carried. Content is untouched;
 * only timing robustness changes.
 *
 * CONCURRENCY CONTRACT
 *
 * Exactly one producer thread (the game main loop) may call _push. Exactly one
 * consumer thread (SDL's audio callback) may call _pull. Everything else is
 * read-only telemetry. Indices are published with SDL's acquire/release
 * barriers; no lock is taken on either side, so the audio thread can never be
 * blocked by, or priority-inverted against, the main loop.
 *
 * Overflow policy matches the worklet: drop the OLDEST frames. After a stall
 * the freshest audio is the correct audio, and dropping the head keeps output
 * latency bounded by the ring capacity in every failure mode.
 */
#ifndef MDKR_AUDIO_RING_H
#define MDKR_AUDIO_RING_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stereo s16 only, matching the port's single output format. */
#define MDKR_AUDIO_RING_CHANNELS 2

/*
 * Length of the equal-power-free linear crossfade applied at every
 * discontinuity (entering and leaving an underflow, and after an overflow
 * eviction). 128 frames is 5.8 ms at 22050 Hz — long enough to remove the step
 * that makes a dropout click, short enough that concealment cannot be mistaken
 * for the signal. The worklet uses the same 128.
 */
#define MDKR_AUDIO_RING_FADE_FRAMES 128

typedef struct MdkrAudioRingStats {
    /* Consumer-side, published for telemetry. Written only by the audio
     * thread; read by the main loop for the [AUDIO-SINK] summary. Torn reads
     * are acceptable here: these are diagnostics, never control inputs. */
    /*
     * Frames the device ASKED for. This, not `pulled_frames`, is the rate the
     * producer has to match: the host advances its timeline by the full
     * request whether or not the ring could fill it. Using the delivered count
     * as the drain signal would under-report during precisely the starvation
     * it is supposed to help the controller escape.
     */
    uint64_t requested_frames;
    uint64_t pulled_frames;      /* frames actually delivered to the device   */
    uint64_t silence_frames;     /* frames the device got that the ring could
                                  * not supply (concealed, but still lost)    */
    uint32_t underruns;          /* distinct dry-ring episodes                */
    uint32_t concealments;       /* crossfades armed (underflow + overflow)   */
    uint32_t callbacks;          /* device callbacks serviced                 */
    uint32_t max_callback_frames;/* largest single device request seen        */
    /* Producer-side. */
    uint64_t pushed_frames;
    uint64_t evicted_frames;     /* oldest-first drops on overflow            */
    uint32_t overflows;          /* distinct overflow episodes                */
    uint32_t min_fill_frames;    /* shallowest occupancy the consumer observed
                                  * at callback entry, after first fill       */
} MdkrAudioRingStats;

typedef struct MdkrAudioRing {
    int16_t *frames;             /* capacity * CHANNELS interleaved samples   */
    uint32_t capacity;           /* frames; always a power of two             */
    uint32_t mask;               /* capacity - 1                              */

    /* Published indices in FRAMES. Monotonic; wrap is handled by `mask`, and
     * unsigned wraparound of the counters themselves is well defined and
     * intentional. head is written by the producer, tail by the consumer. */
    SDL_atomic_t head;
    SDL_atomic_t tail;

    /*
     * Frames the device has asked for, published atomically by the consumer.
     *
     * The `stats` block below is diagnostics: it is written by both sides and
     * read without synchronisation, which is fine for numbers that only ever
     * reach a log line. This counter is different in kind -- the producer
     * differences it to size the next synthesis block, so it is a CONTROL
     * input, and a torn or stale read would mis-size real audio rather than
     * misprint a summary. It gets its own properly published 32-bit atomic;
     * wrapping is handled by taking the difference in uint32, exactly as the
     * head/tail indices do.
     */
    SDL_atomic_t requested;

    /* Consumer-private concealment state. */
    int32_t fade_remaining;
    int16_t fade_from[MDKR_AUDIO_RING_CHANNELS];
    int16_t last_emitted[MDKR_AUDIO_RING_CHANNELS];
    bool starving;               /* inside a dry-ring episode                 */
    bool primed;                 /* the consumer has delivered real PCM once  */

    MdkrAudioRingStats stats;
} MdkrAudioRing;

/*
 * Allocate a ring holding at least `min_frames`, rounded up to a power of two.
 * Returns false (and leaves the ring zeroed) on allocation failure or a
 * nonsensical request. The capacity is the hard ceiling on output latency.
 */
bool mdkr_audio_ring_init(MdkrAudioRing *ring, uint32_t min_frames);

void mdkr_audio_ring_free(MdkrAudioRing *ring);

/* Frames currently readable. Safe to call from either side. */
uint32_t mdkr_audio_ring_fill(const MdkrAudioRing *ring);

uint32_t mdkr_audio_ring_capacity(const MdkrAudioRing *ring);

/*
 * PRODUCER ONLY. Append `frames` interleaved stereo frames. Never blocks and
 * never fails: if the ring is full the OLDEST frames are evicted to make room
 * (see the overflow policy above) and the eviction is counted. Returns the
 * number of frames evicted, which is zero on the healthy path.
 */
uint32_t mdkr_audio_ring_push(MdkrAudioRing *ring, const int16_t *interleaved,
                              uint32_t frames);

/*
 * CONSUMER ONLY. Fill `frames` interleaved stereo frames for the device.
 * Always writes exactly `frames` frames: whatever the ring cannot supply is
 * concealed (ramped toward, and held at, silence) rather than left undefined.
 */
void mdkr_audio_ring_pull(MdkrAudioRing *ring, int16_t *interleaved,
                          uint32_t frames);

/*
 * Snapshot the telemetry counters. Safe from the producer side while the
 * consumer runs; individual fields are consistent, the set is not atomic as a
 * whole, which is what a diagnostic summary needs.
 */
void mdkr_audio_ring_stats(const MdkrAudioRing *ring, MdkrAudioRingStats *out);

/*
 * Frames the device has asked for since the ring was created, as a wrapping
 * 32-bit counter. Safe to call from the producer while the consumer runs.
 * Difference two samples in uint32 to get the drain across an interval; that
 * is the ground truth the latency controller sizes against.
 */
uint32_t mdkr_audio_ring_requested(const MdkrAudioRing *ring);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDIO_RING_H */
