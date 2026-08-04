/**
 * Player audio-volume policy shared by the settings shell and native sink.
 *
 * Music and effects are published to DKR's authored buses. Master is applied
 * after synthesis, with a short ramp so changing it cannot introduce a click.
 * The default (100%) is an exact byte-for-byte no-op for parity captures.
 */
#ifndef MDKR64_AUDIO_VOLUME_H
#define MDKR64_AUDIO_VOLUME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_AUDIO_VOLUME_MAX 100
#define MDKR_AUDIO_GAIN_ONE_Q16 65536
#define MDKR_AUDIO_GAIN_RAMP_FRAMES 256u

typedef struct MdkrAudioVolumeState {
    int master;
    int music;
    int effects;
    uint32_t generation;
} MdkrAudioVolumeState;

typedef struct MdkrAudioGainRamp {
    int32_t current_q16;
    int32_t target_q16;
    uint32_t remaining_frames;
} MdkrAudioGainRamp;

typedef struct MdkrAudioReconnect {
    int pending;
    int have_previous;
    int16_t previous[2];
} MdkrAudioReconnect;

/* Publishes one coherent player setting. Invalid percentages are rejected. */
int mdkr_audio_volume_publish(int master, int music, int effects);
void mdkr_audio_volume_snapshot(MdkrAudioVolumeState *out);

/* A perceptual percentage curve: 50% is one quarter linear amplitude. */
int32_t mdkr_audio_volume_gain_q16(int percent);
void mdkr_audio_gain_ramp_init(MdkrAudioGainRamp *ramp, int percent);
void mdkr_audio_gain_ramp_target(MdkrAudioGainRamp *ramp, int percent);
void mdkr_audio_gain_ramp_apply_s16(MdkrAudioGainRamp *ramp,
                                    int16_t *samples,
                                    uint32_t frames,
                                    uint32_t channels);

/* Smooth a sink reconnection from the last queued sample into new PCM. */
void mdkr_audio_crossfade_from_s16(int16_t *samples,
                                   uint32_t frames,
                                   uint32_t channels,
                                   const int16_t *from,
                                   uint32_t fade_frames);

/* Pure emergency-backlog and discontinuity policy used by the SDL sink. */
int mdkr_audio_sink_queue_fits(uint32_t queued_bytes,
                               uint32_t incoming_bytes,
                               uint32_t block_bytes,
                               uint32_t limit_blocks);
void mdkr_audio_reconnect_init(MdkrAudioReconnect *state);
void mdkr_audio_reconnect_note_gap(MdkrAudioReconnect *state);
void mdkr_audio_reconnect_prepare_stereo(MdkrAudioReconnect *state,
                                         int16_t *samples,
                                         uint32_t frames,
                                         uint32_t fade_frames);
void mdkr_audio_reconnect_note_accepted_stereo(MdkrAudioReconnect *state,
                                               const int16_t *samples,
                                               uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_AUDIO_VOLUME_H */
