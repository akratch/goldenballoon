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
} MdkrAudioQueueControllerStats;

typedef struct MdkrAudioQueueController {
    uint32_t last_counter;
    bool have_last_counter;
    MdkrAudioQueueControllerStats stats;
} MdkrAudioQueueController;

void mdkr_audio_queue_controller_init(MdkrAudioQueueController *controller);

/* Choose one synthesis block. frame_size is the desired queue depth and
 * max_samples is the synth arena's per-call capacity. With no sink, the
 * result is deterministic and independent of host time. With a live sink,
 * the result replaces the estimated frames drained since the previous call
 * and corrects occupancy toward one frame_size. The result is a multiple of
 * 16, matching the synthesizer contract. uint32 counter subtraction is
 * intentionally wrap-safe. */
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
