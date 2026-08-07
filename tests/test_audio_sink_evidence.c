#include "audio_sink_evidence.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static uint32_t read_u32le(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

int main(void) {
    MdkrAudioSinkEvidence evidence;
    const int16_t accepted[] = { 100, -100, 200, -200 };
    const int16_t repaired[] = { 500, -500, 600, -600 };
    unsigned char wav[44u + sizeof(accepted) + sizeof(repaired)];
    FILE *capture = tmpfile();

    expect("temporary capture opens", capture != NULL);
    if (capture == NULL) {
        return 1;
    }
    mdkr_audio_sink_evidence_init(&evidence);
    expect("valid wave capture begins",
           mdkr_audio_sink_evidence_begin(&evidence, capture, 44100u, 2u));

    mdkr_audio_sink_evidence_accepted(&evidence, accepted, sizeof(accepted), 0);
    mdkr_audio_sink_evidence_dropped(&evidence);
    mdkr_audio_sink_evidence_accepted(&evidence, repaired, sizeof(repaired), 1);
    mdkr_audio_sink_evidence_finish(&evidence);

    expect("accepted blocks only", evidence.accepted_blocks == 2u);
    expect("accepted bytes only",
           evidence.accepted_bytes == sizeof(accepted) + sizeof(repaired));
    expect("repaired accepted block counted", evidence.repaired_blocks == 1u);
    expect("dropped block counted", evidence.dropped_blocks == 1u);
    expect("capture has no write error", !evidence.capture_failed);

    rewind(capture);
    expect("complete wave read",
           fread(wav, 1u, sizeof(wav), capture) == sizeof(wav));
    expect("wave header", memcmp(wav, "RIFF", 4u) == 0 &&
           memcmp(wav + 8u, "WAVE", 4u) == 0 &&
           memcmp(wav + 36u, "data", 4u) == 0);
    expect("wave data length excludes dropped blocks",
           read_u32le(wav + 40u) == sizeof(accepted) + sizeof(repaired));
    expect("wave header retains capture sample rate",
           read_u32le(wav + 24u) == 44100u);
    expect("capture begins with first accepted PCM",
           memcmp(wav + 44u, accepted, sizeof(accepted)) == 0);
    expect("capture ends with repaired accepted PCM",
           memcmp(wav + 44u + sizeof(accepted), repaired,
                  sizeof(repaired)) == 0);
    fclose(capture);
    return s_failures == 0 ? 0 : 1;
}
