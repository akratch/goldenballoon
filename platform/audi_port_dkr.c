/**
 * audi_port_dkr.c — native audio host driver for the mdkr64 (DKR) port.
 *
 * DKR's audio synthesizer runs natively; its Acmd output is executed inline by
 * platform/mixer.c (the abi.h a* macros are overridden — see platform/mixer.h).
 * The N64 audio thread (__amMain) never runs in the cooperative model, so this
 * file DRIVES synthesis synchronously from an independent fixed-ticket clock:
 *
 *   dkr_audio_advance_fields()  (called after one ordered game tick)
 *   dkr_audio_service_tick()    (called after one ordered game tick)
 *     -> consume at most one due two-field audio quantum
 *     -> pick its sample count (queue occupancy, or fixed deterministic count)
 *     -> amAudioSynthFrame(n)   (audiomgr.c: __clearAudioDMA + alAudioFrame ->
 *                                synthesis straight into an arena PCM buffer)
 *     -> osAiSetNextBuffer(buf, n*4)   (queue to the SDL device below)
 *
 * Host output is an SDL2 audio device in queue mode (stereo s16 @ OUTPUT_RATE).
 * The sample production is decoupled from the SDL sink so an M8 web build can
 * swap an AudioWorklet in without touching the service clock. Under --headless-frames
 * the SDL device is not opened, but synthesis still runs (so CI exercises the
 * whole DSP path); set MDKR_AUDIO_DUMP=out.wav to capture the PCM for RMS/peak
 * validation, or MDKR_AUDIO_RMS=1 to print running RMS/peak to stderr.
 * MDKR_AUDIO_SINK_DUMP=accepted.wav is a separate native diagnostic that
 * records only post-recovery PCM accepted by SDL's application queue.
 *
 * MDKR_AUDIO_RMS=1 additionally emits one
 *
 *   [AUDIO] music seq=<id> tempo=<bpm> playing=<0|1> at sample=<n>
 *
 * line every time the sequence player's programmed tempo or sequence changes,
 * where <n> is the sample-frame offset into the MDKR_AUDIO_DUMP capture at which
 * the change took effect. tests/check_audio_output.py uses it to window its beat
 * measurement to a constant-tempo stretch and to compare the *measured* beat
 * period in the PCM against the BPM the sequencer says it programmed — the PCM
 * alone cannot tell "the synth is running" from "the synth is running at the
 * wrong rate".
 *
 * The same validation mode reports RAW16 loader calls/bytes and the first output
 * block containing one. tests/check_raw16_audio.py pairs that reachability
 * evidence with fixed-vs-legacy PCM captures and an independent ROM-bank decode.
 */
#ifdef NATIVE_PORT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include "web_audio_worklet.h"   /* AudioWorklet sink (drains off the main thread) */
#endif

#include "platform_os.h"
#include "audio_service_clock.h"
#include "audio_queue_controller.h"
#include "audio_sink_evidence.h"
#include "audio_volume.h"
#include "audi_port_dkr.h"
#include "fs_utf8.h"
#include "mdkr_trace.h"

/* Match the game's ultratypes without pulling the whole PR header chain (which
 * would re-include mixer.h etc.). Same underlying widths as game/include. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t  s16;
typedef int32_t  s32;

/* libultra OS timer (stubs_dkr.c) — COUNTER at 46.875 MHz. */
extern u32 osGetCount(void);

/* audiomgr.c synth entry points (declared in game/src/audiomgr.h, but this is
 * platform code and we avoid pulling the game headers). */
extern int  gDkrAudioReady;
extern short *amAudioSynthFrame(int frameSamples);
extern unsigned int amAudioGetFrameSize(void);
extern unsigned int amAudioGetMaxSamples(void);

/* audio.c sequence-player state, for the MDKR_AUDIO_RMS tempo trace. These are
 * read-only observations of the game's own accessors (game/src/audio.h); nothing
 * here writes audio state. */
extern short music_tempo(void);          /* s16, BPM as programmed (amTuneGetTempoBPM) */
extern u8    music_is_playing(void);
extern u8    music_current_sequence(void);
/* The LIVE tempo of the sequence player, which is what the synthesiser actually
 * advances the beat grid by: alCSPGetTempo() returns seqp->uspt/qnpt, i.e. the
 * microseconds per quarter note currently in force, INCLUDING any AL_MIDI_META_TEMPO
 * event embedded in the sequence data (csplayer.c __setUsptFromTempo). music_tempo()
 * only reports the BPM audio.c last programmed from gSeqSoundTable, so the two
 * diverge on any sequence that drives its own tempo. gMusicPlayer is declared void*
 * here on purpose: this is platform code and pulling libaudio.h in would re-include
 * the mixer headers this file deliberately avoids. The pointer is only passed back
 * through, and a pointer argument is ABI-identical either way. */
extern void *gMusicPlayer;
extern s32   alCSPGetTempo(void *seqp);
/* Per-generation voice-ownership high-water sampler (game/src/audio.c). Same
 * "declare, don't include" rule as the accessors above. Only called when
 * MDKR_RESOURCE_STATS is on. */
extern void  mdkr_audio_voice_peaks_sample(void);
extern void  music_volume_config_set(u32 slider_val);
extern void  sndp_set_global_volume(u32 volume);
extern void  music_jingle_volume_set(u8 volume);
extern u8    sfxRelativeVolume;

#define DKR_OUTPUT_RATE   22050
#define DKR_AUDIO_CHANNELS 2
/* osGetCount() ticks at 46.875 MHz (stubs_dkr.c). Used to measure the real
 * per-pump drain so the controller is pump-rate agnostic. */
#define DKR_COUNTER_RATE  46875000u
/* Two seconds is an emergency ceiling, not an alternate latency controller.
 * The queue controller targets roughly one synth block and its ROM-free/live
 * contracts keep ordinary operation below three. The previous five-block cap
 * was close enough to a transient stall to discard valid PCM and splice
 * nonadjacent timelines, which is directly audible as a pop. */
#define DKR_QUEUE_LIMIT_FRAMES 60u
#define DKR_AUDIO_BYTES_PER_FRAME (DKR_AUDIO_CHANNELS * sizeof(s16))
#define DKR_AUDIO_RECONNECT_FRAMES 128u

/* ---- SDL device state ---------------------------------------------------- */
static SDL_AudioDeviceID s_dev;
static int   s_devOpen;
static int   s_disabled;          /* MDKR_AUDIO=0 or headless: no SDL device   */
static int   s_shutdownComplete;
static u32   s_droppedBuffers;
static u32   s_queueFailures;
static u64   s_webDroppedFrames;
static u32   s_webTerminalFailures;
static MdkrAudioReconnect s_reconnect;
static MdkrAudioSinkEvidence s_sinkEvidence;
#ifdef __EMSCRIPTEN__
static int   s_webAudio;          /* AudioWorklet backend committed (web only)  */
#endif

/* A live host sink exists (SDL device OR the web AudioWorklet) — drives the
 * queue-occupancy pump controller and gates the enqueue. */
static int audio_have_sink(void) {
#ifdef __EMSCRIPTEN__
    if (s_webAudio) return 1;
#endif
    return s_devOpen;
}

/* Host-side queued audio in bytes (the service's latency feedback signal). */
static u32 audio_queued_bytes(void) {
#ifdef __EMSCRIPTEN__
    if (s_webAudio) {
        const u32 dropped = webAudioOutputConsumeDroppedFrames();
        if (dropped != 0u) {
            s_droppedBuffers++;
            s_webDroppedFrames += dropped;
        }
        return webAudioOutputQueuedBytes();
    }
#endif
    if (!s_devOpen) return 0;
    return SDL_GetQueuedAudioSize(s_dev);
}

/* ---- pump rate controller ------------------------------------------------ */
static MdkrAudioQueueController s_queueController;
static MdkrAudioGainRamp s_masterGain;
static u32 s_appliedVolumeGeneration;
static int s_masterGainInitialized;

/* ---- independent audio-time service clock ------------------------------ */
#define DKR_AUDIO_FIELDS_PER_QUANTUM 2u
static MdkrAudioServiceClock s_serviceClock;
static u64 s_serviceCalls;
static u64 s_serviceIdle;
static u64 s_serviceNotReady;
static u64 s_serviceSamples;
static u64 s_serviceMaxQueued;
static int s_serviceTrace = -1;
static int64_t s_servicePerturb = INT64_MIN;

static int audio_service_trace_enabled(void) {
    if (s_serviceTrace < 0) {
        const char *value = getenv("MDKR_AUDIO_SERVICE_TRACE");
        s_serviceTrace = value != NULL && value[0] != '\0' &&
                         strcmp(value, "0") != 0;
    }
    return s_serviceTrace != 0;
}

static int64_t audio_service_perturb_quantum(void) {
    if (s_servicePerturb == INT64_MIN) {
        const char *value = getenv("MDKR_TEST_AUDIO_PERTURB");
        char *end = NULL;
        s_servicePerturb = -1;
        if (value != NULL && value[0] != '\0') {
            long long parsed = strtoll(value, &end, 10);
            if (end != value && *end == '\0' && parsed > 0) {
                s_servicePerturb = (int64_t)parsed;
            }
        }
    }
    return s_servicePerturb;
}

/* ---- validation dump ----------------------------------------------------- */
static FILE *s_wav;
static u32   s_wavDataBytes;
static int   s_rmsEnabled = -1;
static double s_rmsSumSq;        /* running sum of squares over all samples    */
static u64   s_rmsCount;
static u32   s_peak;
static u64   s_dumpFrames;
static u64   s_raw16FirstBlockSample = UINT64_MAX;
static u32   s_raw16PreviousCalls;

unsigned long long dkr_audio_output_frame_count(void) {
    return (unsigned long long)(s_rmsCount / DKR_AUDIO_CHANNELS);
}

static void wav_write_header(FILE *f, u32 dataBytes) {
    u32 byteRate = DKR_OUTPUT_RATE * DKR_AUDIO_CHANNELS * 2;
    u16 blockAlign = DKR_AUDIO_CHANNELS * 2;
    u32 riffSize = 36 + dataBytes;
    u16 fmt = 1, ch = DKR_AUDIO_CHANNELS, bits = 16;
    u32 rate = DKR_OUTPUT_RATE, sub1 = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&sub1, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
}

static void audio_dump_open(void) {
    const char *path = getenv("MDKR_AUDIO_DUMP");
    if (s_rmsEnabled < 0) {
        s_rmsEnabled = (getenv("MDKR_AUDIO_RMS") != NULL) ? 1 : 0;
    }
    if (path && *path && s_wav == NULL) {
        s_wav = mdkr_fopen_utf8(path, "wb");
        if (s_wav) {
            wav_write_header(s_wav, 0); /* placeholder; patched at close */
            printf("[AUDIO] dumping synthesized PCM to %s (%d Hz stereo s16)\n",
                   path, DKR_OUTPUT_RATE);
        }
    }
}

/* This is intentionally separate from MDKR_AUDIO_DUMP. The established dump
 * records deterministic synthesizer output before any host-only recovery, which
 * is what the level and console-reference oracles compare. This opt-in capture
 * records only native blocks SDL accepted, after a possible reconnect splice. */
static void audio_sink_evidence_open(void) {
#ifndef __EMSCRIPTEN__
    const char *path = getenv("MDKR_AUDIO_SINK_DUMP");
    if (path != NULL && path[0] != '\0') {
        FILE *capture = mdkr_fopen_utf8(path, "wb");
        if (capture == NULL || !mdkr_audio_sink_evidence_begin(
                                   &s_sinkEvidence, capture,
                                   DKR_OUTPUT_RATE, DKR_AUDIO_CHANNELS)) {
            fprintf(stderr,
                    "[AUDIO-SINK-EVIDENCE] could not open accepted-SDL PCM "
                    "capture at %s\n", path);
            if (capture != NULL) {
                fclose(capture);
            }
            mdkr_audio_sink_evidence_init(&s_sinkEvidence);
            return;
        }
        printf("[AUDIO-SINK-EVIDENCE] dumping accepted SDL PCM to %s "
               "(%d Hz stereo s16)\n", path, DKR_OUTPUT_RATE);
    }
#endif
}

/* Tap the synthesized signal: accumulate RMS/peak and (optionally) append WAV. */
static void audio_measure_and_dump(const s16 *buf, u32 bytes) {
    u32 n = bytes / 2; /* s16 samples */
    u32 i;
    if (!buf || bytes == 0) {
        return;
    }
    if (s_rmsEnabled != 1 && s_wav == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        s32 v = buf[i];
        u32 a = (v < 0) ? (u32)(-v) : (u32)v;
        if (a > s_peak) {
            s_peak = a;
        }
        s_rmsSumSq += (double)v * (double)v;
    }
    s_rmsCount += n;
    if (s_wav) {
        fwrite(buf, 1, bytes, s_wav);
        s_wavDataBytes += bytes;
    }
    s_dumpFrames++;
    if (s_rmsEnabled == 1 && (s_dumpFrames % 120u) == 0u && s_rmsCount > 0) {
        double rms = sqrt(s_rmsSumSq / (double)s_rmsCount);
        fprintf(stderr, "[AUDIO] frames=%llu samples=%llu rms=%.1f peak=%u\n",
                (unsigned long long)s_dumpFrames,
                (unsigned long long)s_rmsCount, rms, s_peak);
    }
}

/* ---- host device lifecycle ---------------------------------------------- */

void dkr_audio_out_shutdown(void);
static void dkr_audio_atexit(void) { dkr_audio_out_shutdown(); }

void dkr_audio_out_init(void) {
    SDL_AudioSpec want, have;
    const char *disable = getenv("MDKR_AUDIO");

    s_shutdownComplete = 0;
    mdkr_audio_service_clock_init(
        &s_serviceClock, DKR_AUDIO_FIELDS_PER_QUANTUM);
    mdkr_audio_queue_controller_init(&s_queueController);
    s_droppedBuffers = 0u;
    s_queueFailures = 0u;
    s_webDroppedFrames = 0u;
    s_webTerminalFailures = 0u;
    mdkr_audio_reconnect_init(&s_reconnect);
    mdkr_audio_sink_evidence_init(&s_sinkEvidence);
    s_appliedVolumeGeneration = 0u;
    s_masterGainInitialized = 0;
    audio_dump_open();
    /* Emergency fallback for a fatal libc/process exit. Normal finite and
     * window-close paths now unwind through main() and call the idempotent
     * shutdown directly. */
    atexit(dkr_audio_atexit);

    if (disable && disable[0] == '0') {
        s_disabled = 1;
        printf("[AUDIO] disabled via MDKR_AUDIO=0 (synthesis still runs)\n");
        return;
    }
    if (g_headlessFrames >= 0) {
        /*
         * A finite test normally has no output device. MDKR_TEST_HEADLESS_AUDIO
         * is an explicit test-only opt-in for a controlled sink witness: native
         * gates pair it with SDL_AUDIODRIVER=dummy; the browser gate uses mute.
         * No ordinary headless invocation opens output merely because the hook
         * exists.
         */
        const char *testHeadlessAudio = getenv("MDKR_TEST_HEADLESS_AUDIO");
        if (testHeadlessAudio && testHeadlessAudio[0] == '1') {
            /* Continue into the normal host-sink path below. */
        } else
        {
            /* Headless: no device (synthesis still runs; use MDKR_AUDIO_DUMP to
             * capture). Opening a device in CI is undesirable and often fails. */
            s_disabled = 1;
            return;
        }
    }

#ifdef __EMSCRIPTEN__
    /* Browser: prefer the AudioWorklet sink (drains off the main thread, so a
     * GC / WebGPU pipeline compile / level-load stall can't starve it). If the
     * browser lacks AudioWorklet, webAudioOutputInit returns 0 and we fall
     * through to the SDL emscripten audio device. The context is created
     * suspended and resumed on the first user gesture by the shell. */
    if (webAudioOutputInit(DKR_OUTPUT_RATE, DKR_AUDIO_CHANNELS)) {
        s_webAudio = 1;
        printf("[AUDIO] web AudioWorklet sink active: %d Hz, %d ch\n",
               DKR_OUTPUT_RATE, DKR_AUDIO_CHANNELS);
        return;
    }
    printf("[AUDIO] AudioWorklet unavailable - using SDL emscripten audio\n");
#endif

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[AUDIO] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        s_disabled = 1;
        return;
    }
    memset(&want, 0, sizeof(want));
    want.freq = DKR_OUTPUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = DKR_AUDIO_CHANNELS;
    want.samples = 1024;
    want.callback = NULL; /* queue mode */
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        s_disabled = 1;
        return;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    s_devOpen = 1;
    audio_sink_evidence_open();
    printf("[AUDIO] SDL device open: %d Hz, %d ch, %d buf\n",
           have.freq, have.channels, have.samples);
}

void dkr_audio_out_shutdown(void) {
    int had_device;
    int had_web = 0;
    if (s_shutdownComplete) {
        return;
    }
    s_shutdownComplete = 1;
    dkr_audio_service_summary();
    had_device = s_devOpen;
#ifdef __EMSCRIPTEN__
    had_web = s_webAudio;
    if (s_webAudio) {
        webAudioOutputShutdown();
        s_webAudio = 0;
    }
#endif
    if (s_devOpen) {
        SDL_CloseAudioDevice(s_dev);
        s_devOpen = 0;
    }
    if (s_droppedBuffers != 0u) {
        fprintf(stderr,
                "[AUDIO-SINK] warning: deliberate emergency drops=%u; "
                "audio continuity repair was requested with a short crossfade "
                "(web dropped frames=%llu)\n",
                (unsigned)s_droppedBuffers,
                (unsigned long long)s_webDroppedFrames);
    }
    if (s_queueFailures > s_webTerminalFailures) {
        fprintf(stderr,
                "[AUDIO-SINK] warning: native enqueue failures=%u; "
                "the next accepted block was armed for continuity repair\n",
                (unsigned)(s_queueFailures - s_webTerminalFailures));
    }
    if (s_webTerminalFailures != 0u) {
        fprintf(stderr,
                "[AUDIO-SINK] error: Web Audio backend failed after it was "
                "committed; %u PCM block(s) were terminally silent "
                "(no recovery sink exists)\n",
                (unsigned)s_webTerminalFailures);
    }
    if (s_wav) {
        wav_write_header(s_wav, s_wavDataBytes); /* patch RIFF/data sizes */
        fclose(s_wav);
        s_wav = NULL;
    }
    if (s_sinkEvidence.capture != NULL) {
        mdkr_audio_sink_evidence_finish(&s_sinkEvidence);
        if (fclose(s_sinkEvidence.capture) != 0) {
            s_sinkEvidence.capture_failed = 1;
        }
        s_sinkEvidence.capture = NULL;
        printf("[AUDIO-SINK-EVIDENCE] accepted_blocks=%llu "
               "accepted_bytes=%llu repaired_blocks=%llu "
               "rejected_blocks=%llu dropped_blocks=%llu capture_bytes=%u "
               "capture_failed=%d\n",
               (unsigned long long)s_sinkEvidence.accepted_blocks,
               (unsigned long long)s_sinkEvidence.accepted_bytes,
               (unsigned long long)s_sinkEvidence.repaired_blocks,
               (unsigned long long)s_sinkEvidence.rejected_blocks,
               (unsigned long long)s_sinkEvidence.dropped_blocks,
               (unsigned)s_sinkEvidence.capture_bytes,
               s_sinkEvidence.capture_failed);
    }
    if ((s_rmsEnabled == 1 || s_wavDataBytes) && s_rmsCount > 0) {
        double rms = sqrt(s_rmsSumSq / (double)s_rmsCount);
        printf("[AUDIO] TOTAL frames=%llu samples=%llu rms=%.1f peak=%u\n",
               (unsigned long long)s_dumpFrames,
               (unsigned long long)s_rmsCount, rms, s_peak);
    }
    /* Reverb delay-line bounds guard (reverb.c). Non-zero means a delay-line
     * DMA tried to leave its allocation and was clamped — always a bug; the
     * regression fixtures assert this line reads 0. */
    {
        extern u32 alFxGuardTrips(void);
        u32 trips = alFxGuardTrips();
        if (trips != 0u || s_rmsEnabled == 1) {
            printf("[AUDIO] fx-guard trips=%u\n", (unsigned)trips);
        }
    }
    if (s_rmsEnabled == 1) {
        /* Master-bus headroom: how often and by how much the final mix wanted
         * to exceed s16 full scale (platform/mixer.c). */
        extern void mixerGetMainBusClip(uint32_t *hits, uint32_t *overPeak,
                                        uint32_t *samples);
        uint32_t hits = 0, over = 0, tot = 0;
        mixerGetMainBusClip(&hits, &over, &tot);
        printf("[AUDIO] mainbus clip: %u/%u samples (%.5f%%), worst pre-clamp "
               "magnitude %u (%.2f dBFS)\n",
               (unsigned)hits, (unsigned)tot,
               tot ? 100.0 * (double)hits / (double)tot : 0.0,
               (unsigned)over,
               over ? 20.0 * log10((double)over / 32768.0) : 0.0);
        {
            extern void mixerGetRaw16Stats(uint32_t *calls, uint64_t *bytes,
                                            int *legacy);
            uint32_t calls = 0;
            uint64_t bytes = 0;
            int legacy = 0;
            mixerGetRaw16Stats(&calls, &bytes, &legacy);
            if (s_raw16FirstBlockSample == UINT64_MAX) {
                printf("[AUDIO] raw16 mode=%s loads=%u bytes=%llu "
                       "first_block_sample=none\n",
                       legacy ? "legacy" : "fixed",
                       (unsigned)calls, (unsigned long long)bytes);
            } else {
                printf("[AUDIO] raw16 mode=%s loads=%u bytes=%llu "
                       "first_block_sample=%llu\n",
                       legacy ? "legacy" : "fixed",
                       (unsigned)calls, (unsigned long long)bytes,
                       (unsigned long long)s_raw16FirstBlockSample);
            }
        }
    }
    printf("[AUDIO-SHUTDOWN] device=%d web=%d complete=1\n",
           had_device, had_web);
}

/* ===== AI (Audio Interface) — SDL queue-based (replaces stubs_dkr.c) ===== */

u32 osAiGetStatus(void) { return 0; }

s32 osAiSetFrequency(u32 freq) {
    /* The synth uses the returned value as its output rate; honor the request. */
    return (s32)freq;
}

u32 osAiGetLength(void) {
    /* Queue occupancy in bytes — the pump's frame-sizing feedback signal. */
    return audio_queued_bytes();
}

s32 osAiSetNextBuffer(void *buf, u32 size) {
    /* Tap for validation FIRST (records what the synth produced regardless of
     * whether a device consumes it). */
    audio_measure_and_dump((const s16 *)buf, size);

#ifdef __EMSCRIPTEN__
    if (s_webAudio && buf && size > 0) {
        const int pushed = webAudioOutputPush(buf, size);
        if (pushed > 0) {
            s_droppedBuffers++;
            s_webDroppedFrames += size / DKR_AUDIO_BYTES_PER_FRAME;
        } else if (pushed < 0) {
            s_queueFailures++;
            /* The browser backend was already committed, so no SDL device is
             * available to receive a recovery block. Keep this state terminal
             * and explicit rather than claiming the loss was crossfaded. */
            s_webTerminalFailures++;
        }
        return 0;
    }
#endif

    if (s_devOpen && buf && size > 0) {
        u32 blockBytes = amAudioGetFrameSize() * DKR_AUDIO_BYTES_PER_FRAME;
        u32 queued;
        s16 *samples = (s16 *)buf;
        if (blockBytes == 0) {
            blockBytes = (DKR_OUTPUT_RATE / 15u) * DKR_AUDIO_BYTES_PER_FRAME;
        }
        queued = SDL_GetQueuedAudioSize(s_dev);
        if (mdkr_audio_sink_queue_fits(
                queued, size, blockBytes, DKR_QUEUE_LIMIT_FRAMES)) {
            const u32 frameCount = size / DKR_AUDIO_BYTES_PER_FRAME;
            const int repairPending = frameCount != 0u &&
                                      s_reconnect.pending &&
                                      s_reconnect.have_previous;
            mdkr_audio_reconnect_prepare_stereo(
                &s_reconnect, samples, frameCount,
                DKR_AUDIO_RECONNECT_FRAMES);
            if (SDL_QueueAudio(s_dev, buf, size) == 0) {
                mdkr_audio_reconnect_note_accepted_stereo(
                    &s_reconnect, samples, frameCount);
                /* SDL queue mode copies on success. Capture the exact repaired
                 * PCM only after that acceptance, never a rejected candidate. */
                if (s_sinkEvidence.capture != NULL) {
                    mdkr_audio_sink_evidence_accepted(
                        &s_sinkEvidence, samples, size, repairPending);
                }
            } else {
                s_queueFailures++;
                mdkr_audio_reconnect_note_gap(&s_reconnect);
                if (s_sinkEvidence.capture != NULL) {
                    mdkr_audio_sink_evidence_rejected(&s_sinkEvidence);
                }
            }
        } else {
            s_droppedBuffers++;
            mdkr_audio_reconnect_note_gap(&s_reconnect);
            if (s_sinkEvidence.capture != NULL) {
                mdkr_audio_sink_evidence_dropped(&s_sinkEvidence);
            }
        }
    }
    return 0;
}

/* ---- music tempo trace (MDKR_AUDIO_RMS=1) -------------------------------- */
/* One line per change of (sequence, tempo, playing), stamped with the sample-
 * frame offset into the capture. s_rmsCount is the s16 count and is only
 * accumulated while measuring, which is exactly when this trace is armed, so
 * s_rmsCount/2 is the WAV sample-frame index the change lands at. */
static int s_traceSeq  = -1;
static int s_traceBpm  = -32768;
static int s_traceUspt = -1;
static int s_tracePlay = -1;

static void audio_trace_music(void) {
    int seq, bpm, play, uspt;
    if (s_rmsEnabled != 1) {
        return;
    }
    seq  = (int) music_current_sequence();
    bpm  = (int) music_tempo();
    play = music_is_playing() ? 1 : 0;
    uspt = (gMusicPlayer != NULL) ? (int) alCSPGetTempo(gMusicPlayer) : 0;
    if (seq == s_traceSeq && bpm == s_traceBpm && play == s_tracePlay &&
        uspt == s_traceUspt) {
        return;
    }
    s_traceSeq = seq; s_traceBpm = bpm; s_tracePlay = play; s_traceUspt = uspt;
    printf("[AUDIO] music seq=%d tempo=%d uspt=%d playing=%d at sample=%llu\n",
           seq, bpm, uspt, play, (unsigned long long)(s_rmsCount / 2u));
}

static void audio_trace_raw16(void) {
    extern void mixerGetRaw16Stats(uint32_t *calls, uint64_t *bytes,
                                    int *legacy);
    uint32_t calls = 0;
    uint64_t bytes = 0;
    int legacy = 0;

#ifdef __EMSCRIPTEN__
    /* The real-browser gate has a live AudioWorklet but does not dump/RMS the
     * copyrighted PCM. Still expose one reachability line when RAW16 first
     * enters that sink, so wasm coverage is measured rather than inferred. */
    if (s_rmsEnabled != 1 && !s_webAudio) {
        return;
    }
#else
    if (s_rmsEnabled != 1) {
        return;
    }
#endif
    mixerGetRaw16Stats(&calls, &bytes, &legacy);
    if (s_raw16PreviousCalls == 0 && calls > 0) {
        /*
         * amAudioSynthFrame has produced this entire block, but it has not yet
         * reached osAiSetNextBuffer/audio_measure_and_dump. Therefore the
         * current capture length is the exact start of the first output block
         * whose synthesis issued a RAW16 load.
         */
        if (s_rmsEnabled == 1) {
            s_raw16FirstBlockSample = s_rmsCount / DKR_AUDIO_CHANNELS;
        }
#ifdef __EMSCRIPTEN__
        printf("[AUDIO] raw16 active mode=%s loads=%u bytes=%llu\n",
               legacy ? "legacy" : "fixed", (unsigned)calls,
               (unsigned long long)bytes);
#endif
    }
    s_raw16PreviousCalls = calls;
}

/* ===== independently due synthesis service ===== */

static s32 dkr_choose_frame_samples(void) {
    u32 frameSize = amAudioGetFrameSize();          /* ~2 VI fields of samples */
    u32 maxSamples = amAudioGetMaxSamples();
    int haveSink = audio_have_sink();
    u32 now = haveSink ? osGetCount() : 0u;
    u32 queued = haveSink ? (audio_queued_bytes() >> 2) : 0u;

    /* Headless remains a deterministic fixed block. A live SDL/worklet sink
     * replaces the estimated drain and corrects toward one synthesis-frame of
     * latency. This shared pure controller is exercised by ROM-free simulated
     * cadence and real SDL queue-mode contracts. */
    return (s32)mdkr_audio_queue_controller_choose(
        &s_queueController, haveSink != 0, now, DKR_COUNTER_RATE,
        DKR_OUTPUT_RATE, frameSize, maxSamples, queued);
}

/* ---- output-level test seam (MDKR_AUDIO_TEST_GAIN_DB) --------------------
 *
 * A DELIBERATE DEFECT INJECTOR, not a user volume control. It exists so
 * tests/check_audio_level_reference.py can prove it is able to fail: a check on
 * absolute output level that no level error can trip would license the very
 * claim it is meant to test. The seam applies a flat dB gain to the synthesised
 * PCM before anything reads it, so the engine's own RMS/peak accounting, the
 * MDKR_AUDIO_DUMP capture, and the sink all see the same biased signal — which
 * is exactly the shape of the "systematic loudness bias" this gate hunts.
 *
 * It is REFUSED whenever a host sink exists (s_disabled == 0), so it can never
 * change what a player hears; it only ever perturbs a file-domain capture. Any
 * use is announced once, loudly, on stdout.
 */
static double s_testGain = 1.0;      /* linear */
static int    s_testGainState = -1;  /* -1 unread, 0 inactive, 1 active */

static void audio_test_gain_init(void) {
    const char *spec;
    if (s_testGainState >= 0) {
        return;
    }
    s_testGainState = 0;
    spec = getenv("MDKR_AUDIO_TEST_GAIN_DB");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    if (!s_disabled) {
        printf("[AUDIO] MDKR_AUDIO_TEST_GAIN_DB IGNORED: a host output device is "
               "active; the level test seam is file-domain only\n");
        return;
    }
    {
        char *end = NULL;
        double db = strtod(spec, &end);
        if (end == spec || (end != NULL && *end != '\0') ||
            db < -24.0 || db > 24.0) {
            printf("[AUDIO] MDKR_AUDIO_TEST_GAIN_DB=%s is not a dB value in "
                   "[-24, 24]; ignored\n", spec);
            return;
        }
        s_testGain = pow(10.0, db / 20.0);
        s_testGainState = 1;
        printf("[AUDIO] TEST GAIN INJECTED: %+.3f dB (x%.6f) — this build's audio "
               "output is DELIBERATELY WRONG\n", db, s_testGain);
    }
}

static void audio_apply_test_gain(s16 *buf, s32 frameSamples) {
    u32 n;
    u32 i;
    audio_test_gain_init();
    if (s_testGainState != 1 || buf == NULL || frameSamples <= 0) {
        return;
    }
    n = (u32)frameSamples * DKR_AUDIO_CHANNELS;
    for (i = 0; i < n; i++) {
        double v = (double)buf[i] * s_testGain;
        if (v > 32767.0) {
            v = 32767.0;
        } else if (v < -32768.0) {
            v = -32768.0;
        }
        buf[i] = (s16)(v < 0.0 ? v - 0.5 : v + 0.5);
    }
}

static void audio_sync_player_volume(void) {
    MdkrAudioVolumeState volume;
    mdkr_audio_volume_snapshot(&volume);
    if (!s_masterGainInitialized) {
        mdkr_audio_gain_ramp_init(&s_masterGain, volume.master);
        s_masterGainInitialized = 1;
    }
    if (volume.generation == s_appliedVolumeGeneration) {
        return;
    }

    music_volume_config_set((u32)((volume.music * 256 + 50) / 100));
    sndp_set_global_volume((u32)((volume.effects * 256 + 50) / 100));
    /* Jingles are deliberately on the effects bus. Refresh the currently
     * authored jingle-relative gain after changing the global SFX scalar. */
    music_jingle_volume_set(sfxRelativeVolume);
    mdkr_audio_gain_ramp_target(&s_masterGain, volume.master);
    s_appliedVolumeGeneration = volume.generation;
    if (audio_service_trace_enabled()) {
        fprintf(stderr, "[AUDIO-VOLUME] master=%d%% music=%d%% effects=%d%%\n",
                volume.master, volume.music, volume.effects);
    }
}

void dkr_audio_advance_fields(unsigned fields, bool rebase) {
    (void)mdkr_audio_service_clock_advance(
        &s_serviceClock, fields, rebase);
}

void dkr_audio_service_tick(void) {
    s32 frameSamples;
    s16 *buf;
    u32 queued;

    s_serviceCalls++;
    if (!mdkr_audio_service_clock_take(&s_serviceClock)) {
        s_serviceIdle++;
        return;
    }
    /* Retire pre-init time exactly as the old no-op pump did. Deferring it
     * would burst boot-time PCM after the manager becomes ready. */
    if (!gDkrAudioReady) {
        s_serviceNotReady++;
        return;
    }
    audio_sync_player_volume();
    frameSamples = dkr_choose_frame_samples();
    if (audio_service_perturb_quantum() > 0 &&
        s_serviceClock.stats.serviced_quanta ==
            (uint64_t)audio_service_perturb_quantum() &&
        frameSamples > 16) {
        /* Test-only PCM sensitivity control. It changes audio time for one
         * service quantum without touching gameplay state or cue generation. */
        frameSamples -= 16;
    }
    buf = amAudioSynthFrame(frameSamples);
    audio_apply_test_gain(buf, frameSamples);
    mdkr_audio_gain_ramp_apply_s16(
        &s_masterGain, buf, (u32)frameSamples, DKR_AUDIO_CHANNELS);
    s_serviceSamples += (u64)frameSamples;
    queued = audio_queued_bytes() >> 2;
    if (queued > s_serviceMaxQueued) {
        s_serviceMaxQueued = queued;
    }
    /* Sample live voice ownership BETWEEN level boundaries. resource_state can
     * only read the boundary, where alCSPStop() has already handed every music
     * voice back; the plateau gates need to know the generation actually owned
     * them. Off unless MDKR_RESOURCE_STATS is set. */
    if (mdkr_resource_trace_enabled()) {
        mdkr_audio_voice_peaks_sample();
    }
    audio_trace_raw16();
    if (buf != NULL) {
        osAiSetNextBuffer(buf, (u32)frameSamples * 4);
    }
    audio_trace_music();
}

void dkr_audio_service_summary(void) {
    if (!audio_service_trace_enabled()) {
        return;
    }
    fprintf(stderr,
            "[AUDIO-SERVICE] fields=%llu advances=%llu due=%llu "
            "serviced=%llu pending=%llu retired=%llu rebases=%llu "
            "calls=%llu idle=%llu notready=%llu samples=%llu "
            "dropped=%u maxpending=%llu maxqueued=%llu quantumfields=%u\n",
            (unsigned long long)s_serviceClock.stats.elapsed_fields,
            (unsigned long long)s_serviceClock.stats.advances,
            (unsigned long long)s_serviceClock.stats.due_quanta,
            (unsigned long long)s_serviceClock.stats.serviced_quanta,
            (unsigned long long)mdkr_audio_service_clock_pending(
                &s_serviceClock),
            (unsigned long long)s_serviceClock.stats.retired_quanta,
            (unsigned long long)s_serviceClock.stats.rebases,
            (unsigned long long)s_serviceCalls,
            (unsigned long long)s_serviceIdle,
            (unsigned long long)s_serviceNotReady,
            (unsigned long long)s_serviceSamples,
            (unsigned)s_droppedBuffers,
            (unsigned long long)s_serviceClock.stats.max_pending_quanta,
            (unsigned long long)s_serviceMaxQueued,
            DKR_AUDIO_FIELDS_PER_QUANTUM);
    if (s_queueController.stats.live_decisions != 0u) {
        fprintf(stderr,
                "[AUDIO-SINK] decisions=%llu estimateddrain=%llu produced=%llu "
                "emptyobservations=%u stalls=%u minqueued=%u maxqueued=%u "
                "maxproduced=%u\n",
                (unsigned long long)s_queueController.stats.live_decisions,
                (unsigned long long)
                    s_queueController.stats.estimated_consumed_frames,
                (unsigned long long)s_queueController.stats.produced_frames,
                (unsigned)s_queueController.stats.empty_queue_observations,
                (unsigned)s_queueController.stats.stall_guards,
                (unsigned)(s_queueController.stats.min_queued_frames == UINT32_MAX
                    ? 0u : s_queueController.stats.min_queued_frames),
                (unsigned)s_queueController.stats.max_queued_frames,
                (unsigned)s_queueController.stats.max_produced_frames);
    }
    fflush(stderr);
}

#endif /* NATIVE_PORT */
