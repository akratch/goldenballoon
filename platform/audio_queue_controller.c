#include "audio_queue_controller.h"

#include <limits.h>
#include <string.h>

static uint32_t fallback_frame_size(uint32_t output_rate) {
    uint32_t frame_size = output_rate / 30u;
    return frame_size != 0u ? frame_size : 16u;
}

/*
 * Latency target, in sample-frames of host-queued PCM.
 *
 * One synthesis block was the original target, and it is correct exactly while
 * the host refills the sink once per block. That holds on a 60 Hz source: the
 * authored 2-field tick is 33.3 ms and the block is 33.4 ms of PCM, and the
 * tick lands on an exact pair of 60 Hz display refreshes, so every gap between
 * two refills is the same length.
 *
 * It does not hold on a 50 Hz source shown on a 60 Hz display. The authored
 * 2-field tick is 40 ms, which is not a whole number of 16.7 ms refreshes, so a
 * blocking present quantises consecutive ticks to 33.3 ms and 50 ms in a
 * repeating beat. The refill cadence therefore alternates around the block
 * duration, and a one-block target leaves nothing to cover the long half of the
 * beat: the sink drains 1102 frames while the refill it is waiting on carries
 * 896, and the deficit lands as an underrun the player hears as crackling.
 *
 * So the target has to cover the WORST gap, not the average one. The excess is
 * counted three times: once for the drain the long gap itself adds, and twice
 * more because this controller sizes a block from the PREVIOUS gap and is
 * therefore one step behind an alternating cadence in both directions. The
 * cushion is capped at two further blocks so a pathological host cannot walk
 * output latency upward without bound.
 *
 * A host that refills evenly keeps gap_frames at or below one block and gets
 * exactly the previous target, unchanged, including every fixed-rate schedule
 * in tests/test_audio_queue_controller.c.
 */
#define AUDIO_GAP_EXCESS_WEIGHT 3u
#define AUDIO_GAP_CUSHION_BLOCKS 2u
/* Shed 1/64 of the current excess per refill, so a one-off stall's gap is
 * forgotten over a few seconds while a standing beat holds its own level. */
#define AUDIO_GAP_DECAY_SHIFT 6u

static uint32_t audio_latency_target(MdkrAudioQueueController *controller,
                                     uint32_t frame_size,
                                     uint64_t consumed) {
    uint32_t gap;
    uint32_t excess;
    uint32_t cushion;
    uint32_t ceiling;

    if (controller == NULL) {
        return frame_size;
    }
    if (controller->gap_frames < frame_size) {
        controller->gap_frames = frame_size;
    }
    gap = consumed > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)consumed;
    if (gap > controller->gap_frames) {
        controller->gap_frames = gap;
    } else if (controller->gap_frames > frame_size) {
        uint32_t shed =
            ((controller->gap_frames - frame_size) >> AUDIO_GAP_DECAY_SHIFT) + 1u;
        controller->gap_frames = controller->gap_frames - frame_size > shed
            ? controller->gap_frames - shed
            : frame_size;
    }
    if (controller->gap_frames > controller->stats.max_gap_frames) {
        controller->stats.max_gap_frames = controller->gap_frames;
    }

    excess = controller->gap_frames - frame_size;
    ceiling = frame_size > UINT32_MAX / (AUDIO_GAP_CUSHION_BLOCKS + 1u)
        ? UINT32_MAX
        : frame_size * (AUDIO_GAP_CUSHION_BLOCKS + 1u);
    cushion = excess > UINT32_MAX / AUDIO_GAP_EXCESS_WEIGHT
        ? UINT32_MAX
        : excess * AUDIO_GAP_EXCESS_WEIGHT;
    if (cushion > ceiling - frame_size) {
        cushion = ceiling - frame_size;
    }
    return frame_size + cushion;
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

        {
            const uint32_t target =
                audio_latency_target(controller, frame_size, consumed);
            if (controller != NULL &&
                target > controller->stats.max_target_frames) {
                controller->stats.max_target_frames = target;
            }
            correction = (int64_t)target - (int64_t)queued_frames;
        }
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
