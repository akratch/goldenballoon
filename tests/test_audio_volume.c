#include "audio_volume.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void test_state(void) {
    MdkrAudioVolumeState before;
    MdkrAudioVolumeState after;
    mdkr_audio_volume_snapshot(&before);
    expect("default master", before.master == 100);
    expect("default music", before.music == 100);
    expect("default effects", before.effects == 100);
    expect("reject negative", !mdkr_audio_volume_publish(-1, 100, 100));
    expect("reject high", !mdkr_audio_volume_publish(100, 101, 100));
    mdkr_audio_volume_snapshot(&after);
    expect("invalid publish is atomic",
           memcmp(&before, &after, sizeof(before)) == 0);
    expect("valid publish", mdkr_audio_volume_publish(75, 60, 40));
    mdkr_audio_volume_snapshot(&after);
    expect("valid values published",
           after.master == 75 && after.music == 60 && after.effects == 40);
    expect("generation advanced", after.generation != before.generation);
    before = after;
    expect("identical publish accepted", mdkr_audio_volume_publish(75, 60, 40));
    mdkr_audio_volume_snapshot(&after);
    expect("identical publish does not churn generation",
           after.generation == before.generation);
}

static void test_gain(void) {
    MdkrAudioGainRamp ramp;
    int16_t unity[] = { -32768, -1234, 0, 1234, 32767 };
    int16_t original[5];
    int16_t half[] = { 16000, -16000 };
    int16_t mute[] = { 100, -200, 300, -400 };

    expect("zero gain", mdkr_audio_volume_gain_q16(0) == 0);
    expect("half perceptual gain",
           mdkr_audio_volume_gain_q16(50) == MDKR_AUDIO_GAIN_ONE_Q16 / 4);
    expect("unity gain",
           mdkr_audio_volume_gain_q16(100) == MDKR_AUDIO_GAIN_ONE_Q16);

    memcpy(original, unity, sizeof(unity));
    mdkr_audio_gain_ramp_init(&ramp, 100);
    mdkr_audio_gain_ramp_apply_s16(&ramp, unity, 5, 1);
    expect("unity is byte exact", memcmp(unity, original, sizeof(unity)) == 0);

    mdkr_audio_gain_ramp_init(&ramp, 50);
    mdkr_audio_gain_ramp_apply_s16(&ramp, half, 1, 2);
    expect("stereo gain", half[0] == 4000 && half[1] == -4000);

    mdkr_audio_gain_ramp_init(&ramp, 0);
    mdkr_audio_gain_ramp_apply_s16(&ramp, mute, 2, 2);
    expect("mute clears every channel",
           mute[0] == 0 && mute[1] == 0 && mute[2] == 0 && mute[3] == 0);
}

static void test_ramp_and_reconnect(void) {
    MdkrAudioGainRamp ramp;
    int16_t samples[MDKR_AUDIO_GAIN_RAMP_FRAMES * 2];
    int16_t reconnect[] = { 10000, -10000, 10000, -10000,
                            10000, -10000, 10000, -10000 };
    const int16_t previous[] = { 0, 0 };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        samples[i] = 12000;
    }
    mdkr_audio_gain_ramp_init(&ramp, 100);
    mdkr_audio_gain_ramp_target(&ramp, 0);
    mdkr_audio_gain_ramp_apply_s16(
        &ramp, samples, MDKR_AUDIO_GAIN_RAMP_FRAMES, 2);
    expect("ramp begins without a hard mute", samples[0] > 11000);
    expect("ramp is monotonic", samples[200] < samples[100]);
    expect("ramp reaches silence", samples[MDKR_AUDIO_GAIN_RAMP_FRAMES * 2 - 1] == 0);
    expect("ramp converges", ramp.current_q16 == 0 && ramp.remaining_frames == 0);

    mdkr_audio_crossfade_from_s16(reconnect, 4, 2, previous, 4);
    expect("reconnect starts gently", reconnect[0] == 2500 && reconnect[1] == -2500);
    expect("reconnect ends at new stream", reconnect[6] == 10000 && reconnect[7] == -10000);
}

static void test_retargeted_master_ramp(void) {
    MdkrAudioGainRamp ramp;
    int16_t samples[384 * 2];
    int16_t previous = 12000;

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        samples[i] = 12000;
    }
    mdkr_audio_gain_ramp_init(&ramp, 100);
    mdkr_audio_gain_ramp_target(&ramp, 25);
    mdkr_audio_gain_ramp_apply_s16(&ramp, samples, 128, 2);
    expect("down-ramp stays in final mix bounds",
           samples[0] <= 12000 && samples[254] >= 0);
    expect("down-ramp is stereo coherent", samples[254] == samples[255]);

    /* A live slider can reverse direction before the first 256-frame ramp
     * completes. The replacement target must start from the current gain,
     * rather than jumping back to either endpoint. */
    previous = samples[254];
    mdkr_audio_gain_ramp_target(&ramp, 75);
    mdkr_audio_gain_ramp_apply_s16(&ramp, samples + 256, 256, 2);
    expect("retarget has no endpoint step", samples[256] >= previous &&
           samples[256] <= 12000);
    expect("retarget remains bounded", samples[766] >= 0 &&
           samples[766] <= 12000 && samples[767] == samples[766]);
    expect("retarget converges at new gain",
           ramp.remaining_frames == 0 &&
           samples[766] == 6750 && samples[767] == 6750);
}

static void test_sink_continuity_policy(void) {
    MdkrAudioReconnect reconnect;
    int16_t first[] = { 100, -100, 500, -500 };
    int16_t resumed[] = { 10000, -10000, 10000, -10000,
                          10000, -10000, 10000, -10000 };

    expect("queue accepts exact emergency ceiling",
           mdkr_audio_sink_queue_fits(4000, 2000, 1000, 6));
    expect("queue rejects one byte over emergency ceiling",
           !mdkr_audio_sink_queue_fits(4000, 2001, 1000, 6));
    expect("queue arithmetic cannot wrap",
           !mdkr_audio_sink_queue_fits(UINT32_MAX, UINT32_MAX,
                                       1, 60));
    expect("zero block is rejected",
           !mdkr_audio_sink_queue_fits(0, 1, 0, 60));

    mdkr_audio_reconnect_init(&reconnect);
    mdkr_audio_reconnect_note_accepted_stereo(&reconnect, first, 2);
    expect("accepted block remembers stereo endpoint",
           reconnect.have_previous && reconnect.previous[0] == 500 &&
           reconnect.previous[1] == -500 && !reconnect.pending);
    mdkr_audio_reconnect_note_gap(&reconnect);
    mdkr_audio_reconnect_prepare_stereo(&reconnect, resumed, 4, 4);
    expect("gap reconnect begins from prior endpoint",
           resumed[0] == 2875 && resumed[1] == -2875);
    expect("gap reconnect reaches new stream",
           resumed[6] == 10000 && resumed[7] == -10000);
    expect("prepare leaves gap armed until queue accepts",
           reconnect.pending);
    mdkr_audio_reconnect_note_accepted_stereo(&reconnect, resumed, 4);
    expect("successful queue closes reconnect state",
           !reconnect.pending && reconnect.previous[0] == 10000 &&
           reconnect.previous[1] == -10000);
}

static void test_recovery_envelope_extremes(void) {
    /* The browser worklet uses the same endpoint-to-new-stream linear
     * envelope as the native reconnect helper. Exercise the full signed-s16
     * range and a second, independent recovery so the arithmetic stays bounded
     * and every recovery reaches its new stream exactly. */
    int16_t first[] = { INT16_MIN, INT16_MAX, INT16_MIN, INT16_MAX,
                        INT16_MIN, INT16_MAX };
    int16_t second[] = { INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN,
                         INT16_MAX, INT16_MIN };
    const int16_t from_first[] = { INT16_MAX, INT16_MIN };
    const int16_t from_second[] = { INT16_MIN, INT16_MAX };

    mdkr_audio_crossfade_from_s16(first, 3, 2, from_first, 3);
    expect("extreme recovery is monotonic left",
           first[0] < from_first[0] && first[0] > INT16_MIN &&
           first[2] < first[0] && first[2] > INT16_MIN);
    expect("extreme recovery is monotonic right",
           first[1] > from_first[1] && first[1] < INT16_MAX &&
           first[3] > first[1] && first[3] < INT16_MAX);
    expect("extreme recovery reaches exact first endpoint",
           first[4] == INT16_MIN && first[5] == INT16_MAX);

    mdkr_audio_crossfade_from_s16(second, 3, 2, from_second, 3);
    expect("repeated recovery remains bounded",
           second[0] > INT16_MIN && second[0] < INT16_MAX &&
           second[1] > INT16_MIN && second[1] < INT16_MAX);
    expect("repeated recovery reaches exact second endpoint",
           second[4] == INT16_MAX && second[5] == INT16_MIN);
}

int main(void) {
    test_state();
    test_gain();
    test_ramp_and_reconnect();
    test_retargeted_master_ramp();
    test_sink_continuity_policy();
    test_recovery_envelope_extremes();
    if (s_failures != 0) {
        fprintf(stderr, "%d audio-volume test(s) failed\n", s_failures);
        return 1;
    }
    printf("audio volume tests passed\n");
    return 0;
}
