#include "audio_volume.h"

#include <stddef.h>
#include <string.h>

static MdkrAudioVolumeState s_volume = { 100, 100, 100, 1u };

static int mdkr_audio_percent_valid(int value) {
    return value >= 0 && value <= MDKR_AUDIO_VOLUME_MAX;
}

int mdkr_audio_volume_publish(int master, int music, int effects) {
    if (!mdkr_audio_percent_valid(master) ||
        !mdkr_audio_percent_valid(music) ||
        !mdkr_audio_percent_valid(effects)) {
        return 0;
    }
    if (s_volume.master != master || s_volume.music != music ||
        s_volume.effects != effects) {
        s_volume.master = master;
        s_volume.music = music;
        s_volume.effects = effects;
        s_volume.generation++;
        if (s_volume.generation == 0u) {
            s_volume.generation = 1u;
        }
    }
    return 1;
}

void mdkr_audio_volume_snapshot(MdkrAudioVolumeState *out) {
    if (out != NULL) {
        *out = s_volume;
    }
}

int32_t mdkr_audio_volume_gain_q16(int percent) {
    int64_t squared;
    if (percent <= 0) {
        return 0;
    }
    if (percent >= MDKR_AUDIO_VOLUME_MAX) {
        return MDKR_AUDIO_GAIN_ONE_Q16;
    }
    squared = (int64_t)percent * (int64_t)percent;
    return (int32_t)((squared * MDKR_AUDIO_GAIN_ONE_Q16 + 5000) / 10000);
}

void mdkr_audio_gain_ramp_init(MdkrAudioGainRamp *ramp, int percent) {
    if (ramp == NULL) {
        return;
    }
    ramp->current_q16 = mdkr_audio_volume_gain_q16(percent);
    ramp->target_q16 = ramp->current_q16;
    ramp->remaining_frames = 0u;
}

void mdkr_audio_gain_ramp_target(MdkrAudioGainRamp *ramp, int percent) {
    int32_t target;
    if (ramp == NULL) {
        return;
    }
    target = mdkr_audio_volume_gain_q16(percent);
    if (target == ramp->target_q16 && ramp->remaining_frames != 0u) {
        return;
    }
    ramp->target_q16 = target;
    ramp->remaining_frames = target == ramp->current_q16
        ? 0u : MDKR_AUDIO_GAIN_RAMP_FRAMES;
}

static int16_t mdkr_audio_scale_s16(int16_t sample, int32_t gain_q16) {
    int64_t scaled = (int64_t)sample * gain_q16;
    if (scaled >= 0) {
        scaled += MDKR_AUDIO_GAIN_ONE_Q16 / 2;
    } else {
        scaled -= MDKR_AUDIO_GAIN_ONE_Q16 / 2;
    }
    return (int16_t)(scaled / MDKR_AUDIO_GAIN_ONE_Q16);
}

void mdkr_audio_gain_ramp_apply_s16(MdkrAudioGainRamp *ramp,
                                    int16_t *samples,
                                    uint32_t frames,
                                    uint32_t channels) {
    uint32_t frame;
    uint32_t channel;
    if (ramp == NULL || samples == NULL || frames == 0u || channels == 0u) {
        return;
    }
    if (ramp->remaining_frames == 0u &&
        ramp->current_q16 == MDKR_AUDIO_GAIN_ONE_Q16) {
        return;
    }
    if (ramp->remaining_frames == 0u && ramp->current_q16 == 0) {
        memset(samples, 0, (size_t)frames * channels * sizeof(*samples));
        return;
    }
    for (frame = 0u; frame < frames; frame++) {
        if (ramp->remaining_frames != 0u) {
            int64_t delta = (int64_t)ramp->target_q16 - ramp->current_q16;
            ramp->current_q16 +=
                (int32_t)(delta / (int64_t)ramp->remaining_frames);
            ramp->remaining_frames--;
            if (ramp->remaining_frames == 0u) {
                ramp->current_q16 = ramp->target_q16;
            }
        }
        for (channel = 0u; channel < channels; channel++) {
            size_t index = (size_t)frame * channels + channel;
            samples[index] =
                mdkr_audio_scale_s16(samples[index], ramp->current_q16);
        }
    }
}

void mdkr_audio_crossfade_from_s16(int16_t *samples,
                                   uint32_t frames,
                                   uint32_t channels,
                                   const int16_t *from,
                                   uint32_t fade_frames) {
    uint32_t frame;
    uint32_t channel;
    uint32_t count;
    if (samples == NULL || from == NULL || frames == 0u || channels == 0u ||
        fade_frames == 0u) {
        return;
    }
    count = frames < fade_frames ? frames : fade_frames;
    for (frame = 0u; frame < count; frame++) {
        int64_t incoming = (int64_t)frame + 1;
        int64_t previous = (int64_t)count - incoming;
        if (previous < 0) {
            previous = 0;
        }
        for (channel = 0u; channel < channels; channel++) {
            size_t index = (size_t)frame * channels + channel;
            int64_t mixed =
                (int64_t)from[channel] * previous +
                (int64_t)samples[index] * incoming;
            samples[index] = (int16_t)(mixed / (int64_t)count);
        }
    }
}

int mdkr_audio_sink_queue_fits(uint32_t queued_bytes,
                               uint32_t incoming_bytes,
                               uint32_t block_bytes,
                               uint32_t limit_blocks) {
    uint64_t limit;
    if (block_bytes == 0u || limit_blocks == 0u) {
        return 0;
    }
    limit = (uint64_t)block_bytes * limit_blocks;
    return (uint64_t)queued_bytes <= limit &&
           (uint64_t)incoming_bytes <= limit - queued_bytes;
}

void mdkr_audio_reconnect_init(MdkrAudioReconnect *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void mdkr_audio_reconnect_note_gap(MdkrAudioReconnect *state) {
    if (state != NULL) {
        state->pending = 1;
    }
}

void mdkr_audio_reconnect_prepare_stereo(MdkrAudioReconnect *state,
                                         int16_t *samples,
                                         uint32_t frames,
                                         uint32_t fade_frames) {
    if (state == NULL || !state->pending || !state->have_previous) {
        return;
    }
    mdkr_audio_crossfade_from_s16(
        samples, frames, 2u, state->previous, fade_frames);
}

void mdkr_audio_reconnect_note_accepted_stereo(MdkrAudioReconnect *state,
                                               const int16_t *samples,
                                               uint32_t frames) {
    if (state == NULL || samples == NULL || frames == 0u) {
        return;
    }
    state->previous[0] = samples[(frames - 1u) * 2u];
    state->previous[1] = samples[(frames - 1u) * 2u + 1u];
    state->have_previous = 1;
    state->pending = 0;
}
