/**
 * mod_music.c — see mod_music.h.
 */
#include "mod_music.h"

#include "mod_registry.h"
#include "mod_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dr_wav is instantiated in exactly one translation unit,
 * lib/dr_libs/dr_wav_impl.c, which is compiled with warnings off. This TU takes
 * the declarations only, and must configure the header identically or the two
 * would disagree about which entry points exist. */
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

/* Bounds on what a WAV header may claim before anything is sized from it. Both
 * are far outside anything a music replacement legitimately is, and both exist
 * only so that a header written by a stranger cannot pick an allocation. */
#define MDKR_MOD_MUSIC_RATE_MAX     384000u
#define MDKR_MOD_MUSIC_CHANNELS_MAX 8u

/* Distinct rejections written to the log before it stops listing them. Each
 * track is reported once, so this only matters for a pack whose whole music
 * directory is broken -- and in that case the first few lines say everything
 * the next dozen would. */
#define MDKR_MOD_MUSIC_REPORT_MAX 8

static const MdkrModRegistry *s_registry;
static int s_rate;
static int s_channels;

/* The decoded track, in the output format. Kept across a stop so a sequence
 * that restarts does not decode again. */
static int16_t *s_pcm;
static size_t   s_frames;      /* sample-frames in s_pcm */
static size_t   s_position;    /* next sample-frame to mix */
static int      s_track_id = -1;   /* which sequence s_pcm belongs to */
static int      s_absent_id = -1;  /* last id proven to have no usable track */
static int      s_active;
static int      s_gain_q15 = 32768;
static int      s_reports;
static int      s_enabled = 1;   /* Content.PacksEnabled; see mod_music.h */

static void report_rejection(int sequence_id, const char *reason) {
    if (s_reports >= MDKR_MOD_MUSIC_REPORT_MAX) return;
    s_reports++;
    fprintf(stderr, "[MODS] music/%d.wav: %s\n", sequence_id,
            reason != NULL ? reason : "unusable");
    if (s_reports == MDKR_MOD_MUSIC_REPORT_MAX) {
        fprintf(stderr,
                "[MODS] further unusable pack music will not be listed\n");
    }
}

static void release_track(void) {
    free(s_pcm);
    s_pcm = NULL;
    s_frames = 0;
    s_position = 0;
    s_track_id = -1;
    s_active = 0;
}

/* ------------------------------------------------------------- decoding */

/* Reads the whole of `file`'s entry. Same shape as the texture store's reader
 * and same reasoning: the size is asked for first, but only because mod_source
 * has already refused anything over its own entry ceiling before answering, and
 * the read refuses to write unless the entry fits whole. */
static unsigned char *read_pack_entry(MdkrModFile *file, size_t *out_size,
                                      const char **out_reason) {
    unsigned char *buffer;
    size_t         size = 0;
    int            result;

    *out_size = 0;
    *out_reason = NULL;

    result = mdkr_mod_source_read(file->source, file->relative, NULL, 0, &size);
    if (result != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL &&
        result != MDKR_MOD_SOURCE_OK) {
        *out_reason = mdkr_mod_source_result_text(result);
        return NULL;
    }
    if (size == 0) {
        *out_reason = "the file is empty";
        return NULL;
    }
    if (size > MDKR_MOD_MUSIC_BYTES_MAX) {
        *out_reason = "the file is larger than 64 MiB, which no music "
                      "replacement needs to be";
        return NULL;
    }

    buffer = (unsigned char *)malloc(size);
    if (buffer == NULL) {
        *out_reason = "there was not enough memory to load it";
        return NULL;
    }
    result = mdkr_mod_source_read(file->source, file->relative, buffer, size,
                                  &size);
    if (result != MDKR_MOD_SOURCE_OK) {
        free(buffer);
        *out_reason = mdkr_mod_source_result_text(result);
        return NULL;
    }
    *out_size = size;
    return buffer;
}

/* One output frame's worth of one channel, taken from `in` (interleaved,
 * `in_channels` wide). Channels past what the source has are folded so a mono
 * track fills a stereo bus and a stereo track collapses onto a mono one; a
 * source with more channels than the output keeps the leading ones, which for
 * every layout in practice is the front pair. */
static int16_t pick_channel(const int16_t *frame, int in_channels,
                            int out_channel, int out_channels) {
    if (in_channels == out_channels) return frame[out_channel];
    if (in_channels == 1) return frame[0];
    if (out_channels == 1) {
        /* Average the source's leading pair rather than dropping a channel:
         * anything panned hard to the discarded side would otherwise vanish. */
        return (int16_t)(((int32_t)frame[0] + (int32_t)frame[1]) / 2);
    }
    return frame[out_channel < in_channels ? out_channel : in_channels - 1];
}

/* Linear-interpolating rate conversion plus the channel fold above, in one
 * pass. Fixed point rather than floating: the position accumulator has to walk
 * millions of frames without drifting, and a 32.32 step does that exactly on
 * every platform this builds for. */
static void resample_into(int16_t *out, size_t out_frames,
                          const int16_t *in, size_t in_frames,
                          int in_channels, int out_channels,
                          int in_rate, int out_rate) {
    uint64_t step;
    uint64_t position = 0;
    size_t   frame;
    int      channel;

    if (in_rate == out_rate) {
        for (frame = 0; frame < out_frames; frame++) {
            const int16_t *source = in + (size_t)in_channels * frame;
            for (channel = 0; channel < out_channels; channel++) {
                out[(size_t)out_channels * frame + channel] =
                    pick_channel(source, in_channels, channel, out_channels);
            }
        }
        return;
    }

    step = ((uint64_t)(uint32_t)in_rate << 32) / (uint64_t)(uint32_t)out_rate;
    for (frame = 0; frame < out_frames; frame++) {
        size_t   index = (size_t)(position >> 32);
        uint32_t fraction = (uint32_t)(position & 0xFFFFFFFFu);
        const int16_t *a;
        const int16_t *b;

        if (index >= in_frames) index = in_frames - 1;
        a = in + (size_t)in_channels * index;
        /* The last frame interpolates against itself rather than against the
         * start of the buffer: a wrap here would splice the track's head onto
         * its tail one frame early and click on every loop. */
        b = (index + 1 < in_frames) ? a + in_channels : a;

        for (channel = 0; channel < out_channels; channel++) {
            int32_t left = pick_channel(a, in_channels, channel, out_channels);
            int32_t right = pick_channel(b, in_channels, channel, out_channels);
            int64_t delta = (int64_t)(right - left) * (int64_t)fraction;
            out[(size_t)out_channels * frame + channel] =
                (int16_t)(left + (int32_t)(delta >> 32));
        }
        position += step;
    }
}

/* Decodes the WAV in `bytes` into a freshly allocated buffer in the output
 * format. Returns 1 on success and fills `*out_pcm`/`*out_frames`. */
static int decode_track(const unsigned char *bytes, size_t size,
                        int16_t **out_pcm, size_t *out_frames,
                        const char **out_reason) {
    drwav    wav;
    int16_t *source = NULL;
    int16_t *converted = NULL;
    unsigned source_rate;
    unsigned source_channels;
    uint64_t declared_frames;
    uint64_t decoded_frames;
    uint64_t target_frames;
    size_t   source_bytes;
    size_t   target_bytes;

    *out_pcm = NULL;
    *out_frames = 0;
    *out_reason = NULL;

    if (!drwav_init_memory(&wav, bytes, size, NULL)) {
        *out_reason = "this is not a WAV file the decoder recognises";
        return 0;
    }
    /* Copied out before anything can uninit the decoder: the format is needed
     * after the last read, and reading it back out of a torn-down drwav would
     * be reading whatever teardown left behind. */
    declared_frames = wav.totalPCMFrameCount;
    source_rate = (unsigned)wav.sampleRate;
    source_channels = (unsigned)wav.channels;

    if (declared_frames == 0 || source_channels == 0 || source_rate == 0) {
        drwav_uninit(&wav);
        *out_reason = "the WAV declares no audio";
        return 0;
    }
    if (source_channels > MDKR_MOD_MUSIC_CHANNELS_MAX ||
        source_rate > MDKR_MOD_MUSIC_RATE_MAX) {
        drwav_uninit(&wav);
        *out_reason = "the WAV declares a channel count or sample rate this "
                      "port will not decode";
        return 0;
    }

    /* Both allocations are checked against the cap BEFORE they are made, and
     * both are computed in 64-bit so the multiply cannot wrap into a small
     * number that passes. declared_frames is a header field. */
    if (declared_frames >
        (uint64_t)(MDKR_MOD_MUSIC_BYTES_MAX /
                   (sizeof(int16_t) * (size_t)source_channels))) {
        drwav_uninit(&wav);
        *out_reason = "the WAV is longer than the 64 MiB a single track may "
                      "decode to";
        return 0;
    }
    source_bytes = (size_t)declared_frames * (size_t)source_channels *
                   sizeof(int16_t);
    source = (int16_t *)malloc(source_bytes);
    if (source == NULL) {
        drwav_uninit(&wav);
        *out_reason = "there was not enough memory to decode it";
        return 0;
    }

    decoded_frames = drwav_read_pcm_frames_s16(&wav, declared_frames, source);
    drwav_uninit(&wav);
    if (decoded_frames == 0) {
        free(source);
        *out_reason = "the WAV decoded to nothing";
        return 0;
    }

    /* Sized from what actually decoded, never from what the header claimed. */
    target_frames = (decoded_frames * (uint64_t)(unsigned)s_rate) /
                    (uint64_t)source_rate;
    if (target_frames == 0) target_frames = 1;
    if (target_frames >
        (uint64_t)(MDKR_MOD_MUSIC_BYTES_MAX /
                   (sizeof(int16_t) * (size_t)s_channels))) {
        free(source);
        *out_reason = "the WAV is longer than the 64 MiB a single track may "
                      "decode to";
        return 0;
    }
    target_bytes = (size_t)target_frames * (size_t)s_channels * sizeof(int16_t);
    converted = (int16_t *)malloc(target_bytes);
    if (converted == NULL) {
        free(source);
        *out_reason = "there was not enough memory to resample it";
        return 0;
    }

    resample_into(converted, (size_t)target_frames, source,
                  (size_t)decoded_frames, (int)source_channels, s_channels,
                  (int)source_rate, s_rate);
    free(source);

    *out_pcm = converted;
    *out_frames = (size_t)target_frames;
    return 1;
}

/* ------------------------------------------------------------------- API */

void mdkr_mod_music_init(const MdkrModRegistry *registry, int sample_rate,
                         int channels) {
    mdkr_mod_music_shutdown();
    if (sample_rate <= 0 || channels <= 0) return;
    s_registry = registry;
    s_rate = sample_rate;
    s_channels = channels;
}

void mdkr_mod_music_shutdown(void) {
    release_track();
    s_absent_id = -1;
    s_registry = NULL;
    s_rate = 0;
    s_channels = 0;
    /* s_gain_q15, s_reports and s_enabled deliberately survive: the gain and the
     * switch belong to the player's settings, not to the registry, and a report
     * budget that reset on every rescan would let the same broken pack fill the
     * log again. Same rule mod_texture_store.c states for its own s_enabled. */
}

void mdkr_mod_music_set_enabled(int enabled) {
    s_enabled = enabled != 0;
}

int mdkr_mod_music_enabled(void) {
    return s_enabled;
}

int mdkr_mod_music_begin(int sequence_id) {
    char           relative[64];
    MdkrModFile    file;
    unsigned char *bytes;
    size_t         size = 0;
    const char    *reason = NULL;
    int16_t       *pcm = NULL;
    size_t         frames = 0;

    s_active = 0;
    s_position = 0;
    /* Custom content off: this track is the game's own, and the sequence player
     * must not be muted for it. Checked before the cache below so a pack track
     * that is already decoded is still declined -- the decoded pixels-equivalent
     * is kept for the same reason the texture store keeps its own, so switching
     * back on costs the next track start and nothing more. */
    if (!s_enabled) return 0;
    if (s_registry == NULL || s_rate <= 0 || s_channels <= 0) return 0;
    if (sequence_id < 0) return 0;

    /* Already decoded: a sequence that restarts (every lap, every retry) must
     * not re-read and re-resample the same file. */
    if (s_pcm != NULL && s_track_id == sequence_id) {
        s_active = 1;
        return 1;
    }
    /* Already proven to have no usable track. Without this, a pack that
     * overrides one sequence would still pay a pack open -- for a zip, a
     * central-directory parse -- every time any other sequence started. */
    if (s_absent_id == sequence_id) return 0;

    snprintf(relative, sizeof relative, "music/%d.wav", sequence_id);
    if (!mdkr_mod_registry_open_file(s_registry, relative, &file)) {
        s_absent_id = sequence_id;
        return 0;
    }

    bytes = read_pack_entry(&file, &size, &reason);
    mdkr_mod_registry_close_file(&file);
    if (bytes == NULL) {
        report_rejection(sequence_id, reason);
        s_absent_id = sequence_id;
        return 0;
    }

    if (!decode_track(bytes, size, &pcm, &frames, &reason)) {
        free(bytes);
        report_rejection(sequence_id, reason);
        s_absent_id = sequence_id;
        return 0;
    }
    free(bytes);

    release_track();
    s_pcm = pcm;
    s_frames = frames;
    s_position = 0;
    s_track_id = sequence_id;
    s_active = 1;
    fprintf(stderr, "[MODS] music/%d.wav replaces sequence %d "
                    "(%llu frames at %d Hz)\n",
            sequence_id, sequence_id, (unsigned long long)frames, s_rate);
    return 1;
}

void mdkr_mod_music_stop(void) {
    s_active = 0;
    s_position = 0;
}

int mdkr_mod_music_active(void) {
    return s_active && s_pcm != NULL && s_frames > 0;
}

void mdkr_mod_music_set_gain_q15(int gain_q15) {
    if (gain_q15 < 0) gain_q15 = 0;
    if (gain_q15 > 32768) gain_q15 = 32768;
    s_gain_q15 = gain_q15;
}

int mdkr_mod_music_mix(int16_t *out, int frames) {
    int frame;
    int channel;

    if (out == NULL || frames <= 0) return 0;
    if (!mdkr_mod_music_active()) return 0;
    if (s_gain_q15 == 0) {
        /* Still advance: the track is playing, it is just inaudible, and a
         * paused position would resume from the wrong place when the slider
         * comes back up. */
        s_position = (s_position + (size_t)frames) % s_frames;
        return 1;
    }

    for (frame = 0; frame < frames; frame++) {
        const int16_t *source = s_pcm + s_position * (size_t)s_channels;
        for (channel = 0; channel < s_channels; channel++) {
            size_t  index = (size_t)frame * (size_t)s_channels + (size_t)channel;
            int32_t scaled =
                (int32_t)(((int64_t)source[channel] * s_gain_q15) >> 15);
            int32_t sum = (int32_t)out[index] + scaled;
            if (sum > 32767) sum = 32767;
            if (sum < -32768) sum = -32768;
            out[index] = (int16_t)sum;
        }
        s_position++;
        if (s_position >= s_frames) s_position = 0;
    }
    return 1;
}
