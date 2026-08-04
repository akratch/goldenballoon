/* Opt-in evidence for PCM that a native SDL queue accepted. */
#ifndef MDKR_AUDIO_SINK_EVIDENCE_H
#define MDKR_AUDIO_SINK_EVIDENCE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrAudioSinkEvidence {
    FILE *capture;
    uint32_t capture_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    uint64_t accepted_blocks;
    uint64_t accepted_bytes;
    uint64_t repaired_blocks;
    uint64_t rejected_blocks;
    uint64_t dropped_blocks;
    int capture_failed;
} MdkrAudioSinkEvidence;

void mdkr_audio_sink_evidence_init(MdkrAudioSinkEvidence *evidence);

/* Begins a WAV capture on an already-open binary stream. The caller owns the
 * stream and closes it after mdkr_audio_sink_evidence_finish(). */
int mdkr_audio_sink_evidence_begin(MdkrAudioSinkEvidence *evidence,
                                   FILE *capture,
                                   uint32_t sample_rate,
                                   uint16_t channels);

/* Call accepted only after the native queue operation has returned success.
 * Rejected and dropped blocks are deliberately counted but never written. */
void mdkr_audio_sink_evidence_accepted(MdkrAudioSinkEvidence *evidence,
                                       const void *pcm,
                                       uint32_t bytes,
                                       int repaired);
void mdkr_audio_sink_evidence_rejected(MdkrAudioSinkEvidence *evidence);
void mdkr_audio_sink_evidence_dropped(MdkrAudioSinkEvidence *evidence);

/* Patches the WAV header and flushes the stream without closing it. */
void mdkr_audio_sink_evidence_finish(MdkrAudioSinkEvidence *evidence);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDIO_SINK_EVIDENCE_H */
