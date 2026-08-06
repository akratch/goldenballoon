/* Bounded queue-occupancy controller shared by the game and ROM-free probes. */
#ifndef MDKR_AUDIO_QUEUE_CONTROLLER_H
#define MDKR_AUDIO_QUEUE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrAudioQueueControllerStats {
    uint64_t live_decisions;
    uint64_t estimated_consumed_frames;
    uint64_t produced_frames;
    uint32_t empty_queue_observations;
    uint32_t stall_guards;
    uint32_t min_queued_frames;
    uint32_t max_queued_frames;
    uint32_t max_produced_frames;
    /* Worst refill gap the controller has had to cover, and the deepest
     * latency target it therefore asked for. Both are frames. On a host that
     * refills once per synthesis block these stay at one block; they grow only
     * where the refill cadence is genuinely coarser than the block. */
    uint32_t max_gap_frames;
    uint32_t max_target_frames;
    /*
     * Starvation, split by severity so a gate can assert the hard case without
     * being held hostage to the soft one.
     *
     * `underruns` is the sink observed fully drained AFTER it had been fed at
     * least once -- the player heard silence or a click. The "after it had been
     * fed" qualifier is what separates this from empty_queue_observations,
     * which also counts the legitimate boot prime where the queue is empty
     * because nothing has ever been enqueued. This is the one a gate asserts
     * is zero.
     *
     * `floor_breaches` is the sink still holding PCM, but less of it than the
     * drain the controller just measured across one refill gap -- i.e. it would
     * not have survived another gap of the same length. A leading indicator,
     * reported rather than gated: at steady state the queue legitimately sits
     * near one gap's worth, so brushing the floor is normal and only a rising
     * count means the cushion is being lost.
     */
    uint32_t underruns;
    uint32_t floor_breaches;
} MdkrAudioQueueControllerStats;

typedef struct MdkrAudioQueueController {
    uint32_t last_counter;
    bool have_last_counter;
    /* Decaying high-water mark of the drain observed between two refills. */
    uint32_t gap_frames;
    MdkrAudioQueueControllerStats stats;
} MdkrAudioQueueController;

void mdkr_audio_queue_controller_init(MdkrAudioQueueController *controller);

/* Choose one synthesis block. frame_size is one synthesis block and
 * max_samples is the synth arena's per-call capacity. With no sink, the
 * result is deterministic and independent of host time. With a live sink,
 * the result replaces the estimated frames drained since the previous call
 * and corrects occupancy toward the latency target described below. The result
 * is a multiple of 16, matching the synthesizer contract. uint32 counter
 * subtraction is intentionally wrap-safe.
 *
 * The target is one frame_size while the host refills at least once per block,
 * which is the case whenever the authored tick period and the block duration
 * agree. Where they do not, the queue has to survive the LONGEST gap between
 * two refills, not the average one, so the target grows with the observed
 * excess. The gap high-water mark decays back toward one block, so a transient
 * hitch cannot leave the session permanently deeper than it needs to be. */
uint32_t mdkr_audio_queue_controller_choose(
    MdkrAudioQueueController *controller,
    bool have_sink,
    uint32_t now_counter,
    uint32_t counter_rate,
    uint32_t output_rate,
    uint32_t frame_size,
    uint32_t max_samples,
    uint32_t queued_frames);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDIO_QUEUE_CONTROLLER_H */
