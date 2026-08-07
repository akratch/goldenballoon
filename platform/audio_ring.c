/* See audio_ring.h for the architecture rationale and the SPSC contract. */
#include "audio_ring.h"

#include <stdlib.h>
#include <string.h>

/* A ring smaller than this cannot hold one device period on any host we
 * support, and a ring larger than this is a bug, not a latency policy. */
#define MDKR_AUDIO_RING_MIN_FRAMES 1024u
#define MDKR_AUDIO_RING_MAX_FRAMES 262144u

static uint32_t round_up_pow2(uint32_t v) {
    uint32_t p = 1u;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

bool mdkr_audio_ring_init(MdkrAudioRing *ring, uint32_t min_frames) {
    uint32_t capacity;

    if (ring == NULL) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    if (min_frames < MDKR_AUDIO_RING_MIN_FRAMES) {
        min_frames = MDKR_AUDIO_RING_MIN_FRAMES;
    }
    if (min_frames > MDKR_AUDIO_RING_MAX_FRAMES) {
        return false;
    }
    capacity = round_up_pow2(min_frames);
    ring->frames = (int16_t *)calloc(
        (size_t)capacity * MDKR_AUDIO_RING_CHANNELS, sizeof(int16_t));
    if (ring->frames == NULL) {
        return false;
    }
    ring->capacity = capacity;
    ring->mask = capacity - 1u;
    SDL_AtomicSet(&ring->head, 0);
    SDL_AtomicSet(&ring->tail, 0);
    ring->stats.min_fill_frames = UINT32_MAX;
    return true;
}

void mdkr_audio_ring_free(MdkrAudioRing *ring) {
    if (ring == NULL) {
        return;
    }
    free(ring->frames);
    ring->frames = NULL;
    ring->capacity = 0u;
    ring->mask = 0u;
}

uint32_t mdkr_audio_ring_capacity(const MdkrAudioRing *ring) {
    return ring != NULL ? ring->capacity : 0u;
}

uint32_t mdkr_audio_ring_requested(const MdkrAudioRing *ring) {
    MdkrAudioRing *m = (MdkrAudioRing *)ring;   /* SDL getters are non-const */
    if (ring == NULL) {
        return 0u;
    }
    return (uint32_t)SDL_AtomicGet(&m->requested);
}

/*
 * head and tail are stored as int32 inside SDL_atomic_t but are USED as
 * unsigned frame counters. The difference is taken in uint32 so the wrap at
 * 2^32 frames (54 hours of audio) is well-defined rather than UB.
 */
static uint32_t ring_fill_raw(const MdkrAudioRing *ring) {
    uint32_t head;
    uint32_t tail;
    /* Cast away const only to reach SDL's atomic getters, which are not
     * const-qualified; neither call mutates the object. */
    MdkrAudioRing *m = (MdkrAudioRing *)ring;
    head = (uint32_t)SDL_AtomicGet(&m->head);
    tail = (uint32_t)SDL_AtomicGet(&m->tail);
    return head - tail;
}

uint32_t mdkr_audio_ring_fill(const MdkrAudioRing *ring) {
    if (ring == NULL || ring->capacity == 0u) {
        return 0u;
    }
    return ring_fill_raw(ring);
}

uint32_t mdkr_audio_ring_push(MdkrAudioRing *ring, const int16_t *interleaved,
                              uint32_t frames) {
    uint32_t head;
    uint32_t fill;
    uint32_t evicted = 0u;
    uint32_t i;

    if (ring == NULL || ring->capacity == 0u || interleaved == NULL ||
        frames == 0u) {
        return 0u;
    }
    /*
     * A push larger than the ring can only keep its tail. Clamp first so the
     * eviction accounting below stays exact instead of counting a whole
     * capacity of phantom drops per oversized block.
     */
    if (frames > ring->capacity) {
        evicted += frames - ring->capacity;
        interleaved += (size_t)(frames - ring->capacity) *
                       MDKR_AUDIO_RING_CHANNELS;
        frames = ring->capacity;
    }

    fill = ring_fill_raw(ring);
    if (fill + frames > ring->capacity) {
        /* Overflow: drop the OLDEST frames by advancing the READ index. The
         * consumer owns `tail`, so this is the one place the producer touches
         * it. It is safe because eviction only happens when the ring is full,
         * which means the consumer is not keeping up and the frames being
         * dropped are ones it has not reached; a concurrent pull can at worst
         * make the eviction slightly larger than strictly necessary, never
         * corrupt an index. Latency, not correctness, is what is at stake. */
        uint32_t need = fill + frames - ring->capacity;
        uint32_t tail = (uint32_t)SDL_AtomicGet(&ring->tail);
        SDL_AtomicSet(&ring->tail, (int)(tail + need));
        evicted += need;
    }

    head = (uint32_t)SDL_AtomicGet(&ring->head);
    for (i = 0u; i < frames; i++) {
        const uint32_t slot = (head + i) & ring->mask;
        int16_t *dst = &ring->frames[(size_t)slot * MDKR_AUDIO_RING_CHANNELS];
        dst[0] = interleaved[(size_t)i * MDKR_AUDIO_RING_CHANNELS];
        dst[1] = interleaved[(size_t)i * MDKR_AUDIO_RING_CHANNELS + 1];
    }
    /* Publish the data before the index that makes it visible. */
    SDL_MemoryBarrierRelease();
    SDL_AtomicSet(&ring->head, (int)(head + frames));

    ring->stats.pushed_frames += frames;
    if (evicted != 0u) {
        ring->stats.evicted_frames += evicted;
        ring->stats.overflows++;
    }
    return evicted;
}

/* Arm the concealment ramp from whatever the device last actually heard. */
static void ring_arm_fade(MdkrAudioRing *ring) {
    ring->fade_from[0] = ring->last_emitted[0];
    ring->fade_from[1] = ring->last_emitted[1];
    ring->fade_remaining = MDKR_AUDIO_RING_FADE_FRAMES;
    ring->stats.concealments++;
}

void mdkr_audio_ring_pull(MdkrAudioRing *ring, int16_t *interleaved,
                          uint32_t frames) {
    uint32_t tail;
    uint32_t available;
    uint32_t i;

    if (interleaved == NULL || frames == 0u) {
        return;
    }
    if (ring == NULL || ring->capacity == 0u) {
        memset(interleaved, 0,
               (size_t)frames * MDKR_AUDIO_RING_CHANNELS * sizeof(int16_t));
        return;
    }

    available = ring_fill_raw(ring);
    /* Observe the data the published index promises. */
    SDL_MemoryBarrierAcquire();

    ring->stats.callbacks++;
    ring->stats.requested_frames += frames;
    /* Publish the control counter with a release barrier so the producer
     * cannot observe a request count that precedes the ring state it implies. */
    SDL_MemoryBarrierRelease();
    SDL_AtomicSet(&ring->requested,
                  (int)((uint32_t)SDL_AtomicGet(&ring->requested) + frames));
    if (frames > ring->stats.max_callback_frames) {
        ring->stats.max_callback_frames = frames;
    }
    if (ring->primed && available < ring->stats.min_fill_frames) {
        ring->stats.min_fill_frames = available;
    }

    tail = (uint32_t)SDL_AtomicGet(&ring->tail);
    for (i = 0u; i < frames; i++) {
        int32_t l;
        int32_t r;
        const bool have = i < available;

        if (have) {
            const uint32_t slot = (tail + i) & ring->mask;
            const int16_t *src =
                &ring->frames[(size_t)slot * MDKR_AUDIO_RING_CHANNELS];
            l = src[0];
            r = src[1];
            if (ring->starving) {
                /* Leaving a dry episode: ramp back IN from the silence the
                 * device actually heard, so resumption does not step either. */
                ring->starving = false;
                ring_arm_fade(ring);
            }
            ring->primed = true;
        } else {
            l = 0;
            r = 0;
            if (!ring->starving) {
                ring->starving = true;
                /*
                 * The device is unpaused before the first block is synthesised,
                 * so the opening callbacks legitimately find an empty ring.
                 * That is the boot prime, not starvation, and counting it would
                 * make underruns==0 unassertable by a gate — the same
                 * distinction the queue controller draws between
                 * empty_queue_observations and underruns.
                 */
                if (ring->primed) {
                    ring->stats.underruns++;
                }
                /* Entering a dry episode: ramp OUT from the last real sample
                 * rather than cutting to zero. This is the whole reason the
                 * consumer lives here instead of inside SDL. */
                ring_arm_fade(ring);
            }
            if (ring->primed) {
                ring->stats.silence_frames++;
            }
        }

        if (ring->fade_remaining > 0) {
            /* t goes 1/N .. 1 across the ramp: at the first concealed frame
             * the output is still essentially the previous sample, and by the
             * last it is fully the new signal. */
            const int32_t n = MDKR_AUDIO_RING_FADE_FRAMES;
            const int32_t step = n - ring->fade_remaining + 1;
            l = (ring->fade_from[0] * (n - step) + l * step) / n;
            r = (ring->fade_from[1] * (n - step) + r * step) / n;
            ring->fade_remaining--;
        }

        interleaved[(size_t)i * MDKR_AUDIO_RING_CHANNELS] = (int16_t)l;
        interleaved[(size_t)i * MDKR_AUDIO_RING_CHANNELS + 1] = (int16_t)r;
        ring->last_emitted[0] = (int16_t)l;
        ring->last_emitted[1] = (int16_t)r;
    }

    {
        const uint32_t consumed = available < frames ? available : frames;
        ring->stats.pulled_frames += consumed;
        /* Release the space only after the samples have been read out. */
        SDL_MemoryBarrierRelease();
        SDL_AtomicSet(&ring->tail, (int)(tail + consumed));
    }
}

void mdkr_audio_ring_stats(const MdkrAudioRing *ring,
                           MdkrAudioRingStats *out) {
    if (out == NULL) {
        return;
    }
    if (ring == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = ring->stats;
    if (out->min_fill_frames == UINT32_MAX) {
        out->min_fill_frames = 0u;
    }
}
