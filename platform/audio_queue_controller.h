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
    /*
     * Decisions that sized against a MEASURED drain rather than one inferred
     * from elapsed host time. Reported so a gate can assert the sink is
     * actually feeding ground truth in: the latency behaviour of the two paths
     * is similar enough on a healthy host that nothing else observable
     * distinguishes them, which would let the wiring rot silently.
     */
    uint64_t measured_decisions;
} MdkrAudioQueueControllerStats;

typedef struct MdkrAudioQueueController {
    uint32_t last_counter;
    bool have_last_counter;
    /* Decaying high-water mark of the drain observed between two refills. */
    uint32_t gap_frames;
    /* The host's own request granularity, in frames — see the setter. */
    uint32_t device_period;
    /* Ground-truth drain since the previous refill, when the sink can measure
     * it. See mdkr_audio_queue_controller_note_drain. */
    uint32_t measured_drain;
    bool have_measured_drain;
    /* The latency target the most recent decision sized against, in frames.
     * The sink reads it to tell "recovered" from "still short after the
     * per-call synthesis cap truncated the correction". */
    uint32_t target_frames;
    MdkrAudioQueueControllerStats stats;
} MdkrAudioQueueController;

void mdkr_audio_queue_controller_init(MdkrAudioQueueController *controller);

/*
 * Tell the controller the NEGOTIATED device period (SDL's `have.samples`).
 *
 * The host does not drain continuously; it asks for a whole period at a time
 * and is served short if the sink holds less. A latency target expressed only
 * in synthesis blocks is therefore blind to the one quantity that differs most
 * between backends: WASAPI shared mode, PulseAudio/PipeWire, and CoreAudio all
 * negotiate different periods, and a target below the period is a structural
 * shortfall no amount of refill-gap tracking can see.
 *
 * Setting it floors the target at one period. On every host that grants the
 * 256-frame request this is far below one synthesis block and changes nothing;
 * it only engages where a backend insists on a buffer coarser than the block
 * that refills it, which is exactly the case that used to be served short.
 *
 * Zero (the default) means "unknown", and the target behaves as it always did.
 */
void mdkr_audio_queue_controller_set_device_period(
    MdkrAudioQueueController *controller, uint32_t device_period);

/*
 * Report the frames the host ACTUALLY consumed since the previous refill.
 *
 * Without this the controller can only infer the drain from elapsed
 * osGetCount() time, and that inference has to be defended with a stall guard:
 * any interval implying more than four blocks is assumed to be a suspended
 * process rather than real playback, and is replaced with half a block. The
 * guard is right about suspension and badly wrong about a long frametime
 * hitch — which is precisely the event the sink must recover from. When it
 * fires it discards the gap measurement, so the adaptive latency target
 * collapses back to one block at the exact moment it needed to grow, and the
 * controller goes blind while the sink is starving.
 *
 * A callback-driven sink does not have to guess: the audio thread counts the
 * frames it handed the device. Passing that count here replaces the estimate
 * with truth, and the stall guard is then unnecessary — a measured drain
 * cannot exceed what was really played, so there is nothing to defend against.
 *
 * Call once per refill, before choose(). The value is consumed by that call.
 */
void mdkr_audio_queue_controller_note_drain(
    MdkrAudioQueueController *controller, uint32_t frames);

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
