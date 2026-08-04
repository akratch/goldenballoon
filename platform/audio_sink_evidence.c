#include "audio_sink_evidence.h"

#include <limits.h>
#include <string.h>

static void put_u16le(unsigned char *output, uint16_t value) {
    output[0] = (unsigned char)(value & 0xffu);
    output[1] = (unsigned char)(value >> 8u);
}

static void put_u32le(unsigned char *output, uint32_t value) {
    output[0] = (unsigned char)(value & 0xffu);
    output[1] = (unsigned char)((value >> 8u) & 0xffu);
    output[2] = (unsigned char)((value >> 16u) & 0xffu);
    output[3] = (unsigned char)(value >> 24u);
}

static int write_wav_header(FILE *capture, uint32_t sample_rate,
                            uint16_t channels, uint32_t data_bytes) {
    unsigned char header[44] = {0};
    const uint16_t bits_per_sample = 16u;
    const uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t riff_size = 36u + data_bytes;

    if (fseek(capture, 0, SEEK_SET) != 0) {
        return 0;
    }
    memcpy(header, "RIFF", 4u);
    put_u32le(header + 4u, riff_size);
    memcpy(header + 8u, "WAVEfmt ", 8u);
    put_u32le(header + 16u, 16u);
    put_u16le(header + 20u, 1u);
    put_u16le(header + 22u, channels);
    put_u32le(header + 24u, sample_rate);
    put_u32le(header + 28u, byte_rate);
    put_u16le(header + 32u, block_align);
    put_u16le(header + 34u, bits_per_sample);
    memcpy(header + 36u, "data", 4u);
    put_u32le(header + 40u, data_bytes);
    return fwrite(header, 1u, sizeof(header), capture) == sizeof(header) &&
           ferror(capture) == 0;
}

void mdkr_audio_sink_evidence_init(MdkrAudioSinkEvidence *evidence) {
    if (evidence != NULL) {
        memset(evidence, 0, sizeof(*evidence));
    }
}

int mdkr_audio_sink_evidence_begin(MdkrAudioSinkEvidence *evidence,
                                   FILE *capture,
                                   uint32_t sample_rate,
                                   uint16_t channels) {
    if (evidence == NULL || capture == NULL || sample_rate == 0u ||
        channels == 0u) {
        return 0;
    }
    evidence->capture = capture;
    evidence->capture_bytes = 0u;
    evidence->sample_rate = sample_rate;
    evidence->channels = channels;
    evidence->capture_failed = 0;
    if (!write_wav_header(capture, sample_rate, channels, 0u)) {
        evidence->capture_failed = 1;
        return 0;
    }
    return 1;
}

void mdkr_audio_sink_evidence_accepted(MdkrAudioSinkEvidence *evidence,
                                       const void *pcm,
                                       uint32_t bytes,
                                       int repaired) {
    size_t written;
    if (evidence == NULL || pcm == NULL || bytes == 0u) {
        return;
    }
    evidence->accepted_blocks++;
    evidence->accepted_bytes += bytes;
    if (repaired) {
        evidence->repaired_blocks++;
    }
    if (evidence->capture == NULL || evidence->capture_failed) {
        return;
    }
    if (bytes > UINT32_MAX - evidence->capture_bytes) {
        evidence->capture_failed = 1;
        return;
    }
    written = fwrite(pcm, 1u, bytes, evidence->capture);
    evidence->capture_bytes += (uint32_t)written;
    if (written != bytes) {
        evidence->capture_failed = 1;
    }
}

void mdkr_audio_sink_evidence_rejected(MdkrAudioSinkEvidence *evidence) {
    if (evidence != NULL) {
        evidence->rejected_blocks++;
    }
}

void mdkr_audio_sink_evidence_dropped(MdkrAudioSinkEvidence *evidence) {
    if (evidence != NULL) {
        evidence->dropped_blocks++;
    }
}

void mdkr_audio_sink_evidence_finish(MdkrAudioSinkEvidence *evidence) {
    if (evidence == NULL || evidence->capture == NULL) {
        return;
    }
    if (!write_wav_header(evidence->capture, evidence->sample_rate,
                          evidence->channels, evidence->capture_bytes) ||
        fflush(evidence->capture) != 0) {
        evidence->capture_failed = 1;
    }
}
