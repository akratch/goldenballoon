/* Opt-in evidence for PCM the native sink accepted into its output ring. */
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

/*
 * Call accepted only once the block has actually entered the output ring —
 * that, and not "reached the speaker", is what this capture is evidence of.
 * The sink cannot refuse a block any more (a ring push is a memcpy into
 * storage the port owns), so there is no rejected counter; the remaining loss
 * modes are the backlog limiter, which counts a dropped block and writes
 * nothing, and ring overflow, which overwrites frames already captured here
 * and is accounted separately by the ring's evicted_frames/overflows. A
 * capture can therefore contain frames the device never played, and that is
 * the intended reading: it proves what the sink accepted, not what was heard.
 */
void mdkr_audio_sink_evidence_accepted(MdkrAudioSinkEvidence *evidence,
                                       const void *pcm,
                                       uint32_t bytes,
                                       int repaired);
void mdkr_audio_sink_evidence_dropped(MdkrAudioSinkEvidence *evidence);

/* Patches the WAV header and flushes the stream without closing it. */
void mdkr_audio_sink_evidence_finish(MdkrAudioSinkEvidence *evidence);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDIO_SINK_EVIDENCE_H */
