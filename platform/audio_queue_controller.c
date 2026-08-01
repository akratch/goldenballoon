#include "audio_queue_controller.h"

#include <limits.h>
#include <string.h>

static uint32_t fallback_frame_size(uint32_t output_rate) {
    uint32_t frame_size = output_rate / 30u;
    return frame_size != 0u ? frame_size : 16u;
}

void mdkr_audio_queue_controller_init(MdkrAudioQueueController *controller) {
    if (controller == NULL) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->stats.min_queued_frames = UINT32_MAX;
}

uint32_t mdkr_audio_queue_controller_choose(
    MdkrAudioQueueController *controller,
    bool have_sink,
    uint32_t now_counter,
    uint32_t counter_rate,
    uint32_t output_rate,
    uint32_t frame_size,
    uint32_t max_samples,
    uint32_t queued_frames) {
    uint64_t produce;

    if (frame_size == 0u) {
        frame_size = fallback_frame_size(output_rate);
    }
    if (max_samples < 16u) {
        return 0u;
    }

    if (!have_sink) {
        produce = frame_size;
    } else {
        uint64_t consumed;
        int64_t correction;
        int64_t signed_produce;

        if (controller == NULL || !controller->have_last_counter) {
            consumed = frame_size / 2u;
            if (controller != NULL) {
                controller->have_last_counter = true;
            }
        } else if (counter_rate == 0u) {
            consumed = frame_size / 2u;
            controller->stats.stall_guards++;
        } else {
            uint32_t elapsed = now_counter - controller->last_counter;
            consumed = (uint64_t)elapsed * output_rate / counter_rate;
            if (consumed > (uint64_t)frame_size * 4u) {
                consumed = frame_size / 2u;
                controller->stats.stall_guards++;
            }
        }
        if (controller != NULL) {
            controller->last_counter = now_counter;
            controller->stats.live_decisions++;
            controller->stats.estimated_consumed_frames += consumed;
            if (queued_frames == 0u) {
                controller->stats.empty_queue_observations++;
            }
            if (queued_frames < controller->stats.min_queued_frames) {
                controller->stats.min_queued_frames = queued_frames;
            }
            if (queued_frames > controller->stats.max_queued_frames) {
                controller->stats.max_queued_frames = queued_frames;
            }
        }

        correction = (int64_t)frame_size - (int64_t)queued_frames;
        if (consumed > (uint64_t)INT64_MAX) {
            signed_produce = INT64_MAX;
        } else if (correction > 0 &&
                   (int64_t)consumed > INT64_MAX - correction) {
            signed_produce = INT64_MAX;
        } else {
            signed_produce = (int64_t)consumed + correction;
        }
        produce = signed_produce > 0 ? (uint64_t)signed_produce : 0u;
    }

    if (produce < 16u) {
        produce = 16u;
    }
    if (produce > max_samples) {
        produce = max_samples;
    }
    produce &= ~UINT64_C(15);

    if (controller != NULL && have_sink) {
        controller->stats.produced_frames += produce;
        if (produce > controller->stats.max_produced_frames) {
            controller->stats.max_produced_frames = (uint32_t)produce;
        }
    }
    return (uint32_t)produce;
}
