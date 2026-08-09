/* a11y_speech.h — the voice that reads platform/a11y_model.c out loud.
 *
 * a11y_model.c decides WHAT the app would say and keeps it as text. This layer
 * decides whether anything says it, and on which thread. The split is what lets
 * every accessibility gate assert behaviour with no audio device: the model's
 * `[SPEAK]` stream exists whether or not a backend ever comes up.
 *
 * TWO PIECES, AND THEY ARE NOT THE SAME PIECE.
 *
 *   The backend  — five functions, one implementation per platform, selected in
 *                  CMakeLists.txt exactly the way platform/app/file_dialog_* is.
 *                  It knows about a speech engine and nothing else.
 *   The worker   — a11y_speech_worker.c. One drain thread, one bounded handoff
 *                  queue, the MDKR_AUDIO refusal, and the live-config plumbing.
 *                  It is the ONLY caller of the backend.
 *
 * WHY A THREAD. A synthesiser call takes as long as the sentence takes to say.
 * On the render thread that is a multi-second stall, so the backend's _speak()
 * is deliberately allowed to block and is only ever called from the worker.
 *
 * WHY THE WORKER DOES NOT POP THE MODEL ITSELF. a11y_model.c is single-owner by
 * design and NOT an SPSC queue: mdkr_a11y_announce() writes s_head too, when a
 * full ring drops its oldest entry, so a push and a pop on two threads are a
 * genuine data race on the same indices, not a benign one. The push side lives
 * in platform/app/ui_common.cpp and cannot be locked from here. So the pop stays
 * on the thread that pushes — mdkr_a11y_speech_service_pump(), called once per
 * frame from the UI/render thread — and what crosses to the worker is a
 * mutex-guarded copy of the text. The frame pays one memcpy; the sentence is
 * spoken somewhere else.
 *
 * AUDIO SAFETY. MDKR_AUDIO=0 is the project's hard rule and a test run that
 * starts talking is exactly what it exists to prevent. The worker refuses to
 * bring a backend up at all under it, and every backend's _init() re-checks —
 * see mdkr_a11y_speech_audio_permitted() below. A refused run is not a broken
 * run: the app stays text-only and says so, once, on stderr.
 *
 * VERIFICATION STATUS OF THE BACKENDS, because an unverified backend that reads
 * as working is worse than no backend at all. Each file repeats its own line at
 * the top; the summary, as of the sprint that wrote them:
 *
 *   macOS    RUN. Speech produced and measured through a11y_speech_worker.c.
 *   Linux    COMPILED ONLY, by a real Linux gcc. Never run, so nobody has heard
 *            speech-dispatcher say anything.
 *   Windows  COMPILED ONLY, by x86_64-w64-mingw32-g++. Never run.
 *   Browser  COMPILED ONLY, by emcc. Never loaded in a browser, and nothing
 *            calls it yet — the wasm build has no app shell.
 *   Null     RUN, as the refusal path every MDKR_AUDIO=0 gate takes.
 *
 * Task 6 of the sprint owns docs/ACCESSIBILITY.md and the screen-reader route;
 * this comment is the record until it exists.
 */
#ifndef MDKR64_A11Y_SPEECH_H
#define MDKR64_A11Y_SPEECH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One sentence, in a player's words, saying why the app is not speaking. */
#define MDKR_A11Y_SPEECH_REASON_MAX 192

/* Speech rate and volume, as the Accessibility.SpeechRate / .SpeechVolume
 * schema rows define them. Mirrored here so a backend does not have to include
 * video_config.h to clamp its own input. */
#define MDKR_A11Y_SPEECH_RATE_MIN    50
#define MDKR_A11Y_SPEECH_RATE_NORMAL 100
#define MDKR_A11Y_SPEECH_RATE_MAX    300
#define MDKR_A11Y_SPEECH_VOLUME_MAX  100

/* --- Audio safety --------------------------------------------------------
 *
 * Inline, in the header, on purpose: every backend's _init() opens with it and
 * the worker checks it before it would even load one. Five copies of the same
 * getenv() is how one platform ends up talking in CI, so there is one copy and
 * the call sites share it. Matches platform/audi_port_dkr.c's test exactly —
 * one rule, one spelling, for the device and for the voice.
 */
static inline bool mdkr_a11y_speech_audio_permitted(void) {
    const char *disable = getenv("MDKR_AUDIO");
    return !(disable != NULL && disable[0] == '0');
}

/* --- The backend interface -----------------------------------------------
 *
 * Exactly one a11y_speech_{mac,win,linux,web,null}.* compiles per platform.
 * All seven functions are called ONLY from a11y_speech_worker.c.
 */

/* Brings the platform voice up. True when the backend can speak.
 *
 * On false, `reason` (when non-NULL and non-empty in size) receives a
 * player-readable sentence — "speech-dispatcher is not installed", not an
 * error code — and the app degrades to text-only rather than failing. Never
 * leaves a half-initialised engine behind. Called on the UI thread, before the
 * worker thread exists. */
bool mdkr_a11y_speech_init(char *reason, size_t reason_size);

/* Speaks one utterance and returns when the backend is ready for the next one.
 *
 * Native backends achieve that by blocking for the length of the sentence, or
 * until _stop() cuts it off — which is the entire reason this runs on a worker
 * thread. The browser backend cannot block a wasm main thread and instead
 * returns once the page has accepted the utterance, keeping its own one-deep
 * queue. Either way, a return means "hand me the next one". */
void mdkr_a11y_speech_speak(const char *text);

/* Cuts off whatever is being said, now, discarding the rest of it. Callable
 * from ANY thread, including while _speak() is blocked in the worker: barge-in
 * is requested by the UI thread the instant the model reports the in-flight
 * utterance superseded. */
void mdkr_a11y_speech_stop(void);

/* Releases the engine. After it, _available() is false until a new _init(). */
void mdkr_a11y_speech_shutdown(void);

/* Whether a successful _init() has left an engine this backend can speak
 * through. False before the first _init() and after _shutdown(). */
bool mdkr_a11y_speech_available(void);

/* Percent of the voice's normal speaking rate (50..300) and percent of full
 * loudness (0..100). Applied to the NEXT utterance, not the one in flight.
 * Called from the worker thread only, immediately before _speak(), so a
 * backend may store them without synchronisation. */
void mdkr_a11y_speech_set_rate(int percent);
void mdkr_a11y_speech_set_volume(int percent);

/* --- The drain worker ----------------------------------------------------- */

/* Once per frame, from the UI/render thread — the same thread that announces.
 *
 * Reads the live Accessibility.Speech / .SpeechRate / .SpeechVolume values,
 * brings the backend up or down to match, pops everything a11y_model.c has,
 * and hands it to the worker. Cheap and non-blocking on every path: when
 * speech is off (the default) it is one config read and a return. */
void mdkr_a11y_speech_service_pump(void);

/* Stops the worker and releases the backend. Registered with atexit() by the
 * first successful start, so no caller has to find every exit path; calling it
 * directly is safe and idempotent. */
void mdkr_a11y_speech_service_shutdown(void);

/* Why the app is text-only, or "" while it is speaking. Also "" before the
 * first pump, because nothing has been decided yet. */
const char *mdkr_a11y_speech_service_reason(void);

/* Utterances the worker has finished speaking, and utterances it dropped
 * because the engine was slower than the player. Diagnostic only — dropping is
 * the designed behaviour, not a fault, which is exactly why it needs to be
 * countable from outside: a designed loss that leaves no trace cannot be told
 * apart from a bug. */
unsigned mdkr_a11y_speech_service_spoken(void);
unsigned mdkr_a11y_speech_service_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_A11Y_SPEECH_H */
