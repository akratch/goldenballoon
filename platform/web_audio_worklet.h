/*
 * web_audio_worklet.h — web-only low-latency audio output backend for mdkr64.
 *
 * Vendored + adapted from mgb64/src/platform/web_audio_worklet.h. On the browser
 * the default SDL emscripten audio path is a MAIN-THREAD ScriptProcessorNode: the
 * once-per-frame pump AND the SPN callback both run on the main thread, so any GC
 * / WebGPU pipeline compile / level-load stall starves audio, forcing a deep
 * device buffer (a hard latency floor).
 *
 * This backend moves the DRAIN off the main thread: a plain-JS
 * AudioWorkletProcessor owns a ring buffer and pulls 128-sample render quanta at
 * audio-thread priority, immune to main-thread stalls. The engine still
 * synthesises on the main thread (welded to the 60 Hz sim tick — mdkr64's
 * dkr_audio_pump) and posts finished PCM into the ring via postMessage — so this
 * needs NO SharedArrayBuffer, NO cross-origin isolation, and does NOT touch the
 * WebGPU build. It is web-only and behind webAudioOutputActive(); if the browser
 * lacks AudioWorklet the caller falls back to the SDL emscripten audio path.
 *
 * mdkr64 feeds the SAME mixer sample buffers here as it feeds the native SDL
 * device (osAiSetNextBuffer in audi_port_dkr.c) — the leaf swap is at that sink.
 */
#ifndef WEB_AUDIO_WORKLET_H
#define WEB_AUDIO_WORKLET_H

#ifdef __EMSCRIPTEN__

/* Start the AudioWorklet output backend. srcRate = the engine PCM rate (22050),
 * channels = 2. Returns 1 if the backend was started (browser supports
 * AudioWorklet), 0 to fall back to the SDL path. The worklet module loads
 * asynchronously; pushes before it is ready are briefly buffered. */
int webAudioOutputInit(int srcRate, int channels);

/* 1 once webAudioOutputInit has selected this backend (module may still be
 * loading). 0 if init was never called or the browser refused it. */
int webAudioOutputActive(void);

/* Push final stereo s16 PCM (size bytes, 4 bytes/frame) to the ring. Returns 0
 * on success to mirror SDL_QueueAudio's contract. */
int webAudioOutputPush(const void *buf, unsigned size);

/* Estimated ring occupancy in bytes, for the pump's occupancy controller.
 * Derived on the main thread from the worklet's last reported ring count plus
 * the audio clock (ctx.currentTime), so it is synchronous and needs no shared
 * memory. */
unsigned webAudioOutputQueuedBytes(void);

/* Resume the AudioContext (autoplay policy needs a user gesture). Safe to call
 * repeatedly; also reachable via the shell's existing audioContext resume. */
void webAudioOutputResume(void);

/* Disconnect the node, close its message port, and await AudioContext.close().
 * Idempotent; safe while addModule() is still pending. */
void webAudioOutputShutdown(void);

#endif /* __EMSCRIPTEN__ */
#endif /* WEB_AUDIO_WORKLET_H */
