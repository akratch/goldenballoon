/* a11y_speech_worker.c — the drain worker: one voice, off the render thread.
 *
 * See a11y_speech.h for the split between this file and the backends, and for
 * why the model is popped here rather than on the worker thread. The short
 * version: a11y_model.c is single-owner, not SPSC, so the pop stays on the
 * thread that pushes and only a copy of the text crosses over.
 *
 * The frame's share of the work is bounded and small: read four live config
 * values, memcpy at most a few hundred bytes under a mutex nobody holds for
 * long, and return. Everything that can take a human-perceptible amount of
 * time — bringing an engine up, saying a sentence — happens on the worker.
 */
#include "a11y_speech.h"

#include "a11y_model.h"
#include "video_config.h"

#include <stdio.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include <SDL.h>
#endif

/*
 * Utterances the backend may be behind by. Bounded for the same reason the
 * model's ring is bounded, and dropping the same end: a player who has tabbed
 * through twenty settings wants the twentieth read to them, not the first, and
 * a voice engine slower than the player's fingers must fall behind by losing
 * stale lines rather than by growing.
 *
 * Eight rather than one because a burst is normal — arriving in a panel emits a
 * section line and a focus line in the same frame — and one would throw away
 * the section header of every panel a player enters.
 */
#define BACKLOG_MAX 8

typedef enum ServiceState {
    SERVICE_OFF = 0,   /* nothing running; speech is switched off */
    SERVICE_STARTING,  /* thread up, backend coming up ON that thread */
    SERVICE_RUNNING,   /* backend up, worker draining */
    SERVICE_TEXT_ONLY  /* refused, permanently: the reason will not change */
} ServiceState;

static ServiceState s_state;
static char         s_reason[MDKR_A11Y_SPEECH_REASON_MAX];
static unsigned     s_dropped;
static unsigned     s_spoken;
static bool         s_atexitRegistered;

/* Handoff ring. Written by the pump (UI thread), read by the worker. */
static char s_text[BACKLOG_MAX][MDKR_A11Y_TEXT_MAX];
static int  s_head;
static int  s_count;
static bool s_speaking;   /* the worker is inside mdkr_a11y_speech_speak() */

#ifndef __EMSCRIPTEN__
static bool        s_quit;
static SDL_mutex  *s_lock;
static SDL_cond   *s_wake;
static SDL_Thread *s_thread;
/* The worker's answer to "did the engine come up?", published under s_lock.
 *
 * WHY _init() IS NOT CALLED FROM THE PUMP. Bringing an engine up is I/O.
 * Measured on macOS, [AVSpeechSynthesisVoice speechVoices] alone costs 96 ms;
 * Linux's spd_open() opens a socket to a daemon and Windows' CoCreateInstance
 * walks the registry. Any of those on the render thread is a dropped frame on
 * the tick the player switches speech on — a stutter caused by the
 * accessibility feature, which is precisely what the worker exists to prevent.
 * So the thread is created first and its first act is to open the engine. */
static bool s_initDone;
static bool s_initOk;
static char s_initReason[MDKR_A11Y_SPEECH_REASON_MAX];
/* The voice settings the player currently has. Published by the pump, consumed
 * by the worker just before each utterance, so the backend's rate/volume state
 * is only ever touched by the one thread that speaks. */
static SDL_atomic_t s_wantRate;
static SDL_atomic_t s_wantVolume;
static int          s_appliedRate;
static int          s_appliedVolume;

/* NULL-tolerant: the public counters are readable when no worker is running,
 * and that is precisely when there is no mutex to take. */
static void lock_backlog(void) {
    if (s_lock != NULL) SDL_LockMutex(s_lock);
}
static void unlock_backlog(void) {
    if (s_lock != NULL) SDL_UnlockMutex(s_lock);
}
#else
/* The browser build has no worker: see speak_inline() below. Single-threaded,
 * so the lock is the absence of one. */
static int s_wantRate;
static int s_wantVolume;
static int s_appliedRate;
static int s_appliedVolume;

static void lock_backlog(void)   {}
static void unlock_backlog(void) {}
#endif

/* --- the handoff ring ----------------------------------------------------- */

static void backlog_push(const char *text) {
    int index;

    lock_backlog();
    if (s_count == BACKLOG_MAX) {
        s_head = (s_head + 1) % BACKLOG_MAX;
        s_count--;
        s_dropped++;
    }
    index = (s_head + s_count) % BACKLOG_MAX;
    /* The model refuses anything that would not fit whole, so this cannot
     * truncate: MDKR_A11Y_TEXT_MAX here is the same MDKR_A11Y_TEXT_MAX there. */
    memcpy(s_text[index], text, strlen(text) + 1u);
    s_count++;
    unlock_backlog();
#ifndef __EMSCRIPTEN__
    SDL_CondSignal(s_wake);
#endif
}

/* True when nothing is queued and nothing is being said. */
static bool backlog_idle(void) {
    bool idle;
    lock_backlog();
    idle = s_count == 0 && !s_speaking;
    unlock_backlog();
    return idle;
}

/* Barge-in. Throw the queue away first, then cut the engine off: doing it in
 * this order means the worker cannot pick up one more stale line in between.
 * _stop() is called with the mutex released — it may have to wait on a
 * synthesiser, and the worker needs the mutex to notice it has been cut off. */
static void backlog_purge(void) {
    lock_backlog();
    s_head = 0;
    s_count = 0;
    unlock_backlog();
    mdkr_a11y_speech_stop();
}

/* --- the voice settings --------------------------------------------------- */

static int clamp_int(int value, int low, int high) {
    if (value < low)  return low;
    if (value > high) return high;
    return value;
}

static void publish_voice(const MdkrVideoConfig *config) {
    const int rate = clamp_int((int)config->values[MDKR_A11Y_SPEECH_RATE].number,
                               MDKR_A11Y_SPEECH_RATE_MIN,
                               MDKR_A11Y_SPEECH_RATE_MAX);
    const int volume = clamp_int(
        (int)config->values[MDKR_A11Y_SPEECH_VOLUME].number, 0,
        MDKR_A11Y_SPEECH_VOLUME_MAX);
#ifndef __EMSCRIPTEN__
    SDL_AtomicSet(&s_wantRate, rate);
    SDL_AtomicSet(&s_wantVolume, volume);
#else
    s_wantRate = rate;
    s_wantVolume = volume;
#endif
}

/* Runs on whichever thread is about to call _speak(), which is the contract
 * the setters are documented under. */
static void apply_voice(void) {
#ifndef __EMSCRIPTEN__
    const int rate = SDL_AtomicGet(&s_wantRate);
    const int volume = SDL_AtomicGet(&s_wantVolume);
#else
    const int rate = s_wantRate;
    const int volume = s_wantVolume;
#endif
    if (rate != s_appliedRate) {
        mdkr_a11y_speech_set_rate(rate);
        s_appliedRate = rate;
    }
    if (volume != s_appliedVolume) {
        mdkr_a11y_speech_set_volume(volume);
        s_appliedVolume = volume;
    }
}

/* --- the worker ----------------------------------------------------------- */

#ifndef __EMSCRIPTEN__
static int worker_main(void *unused) {
    char reason[MDKR_A11Y_SPEECH_REASON_MAX];
    bool ok;

    (void)unused;

    /* First act: open the engine, here rather than on the frame. */
    reason[0] = '\0';
    ok = mdkr_a11y_speech_init(reason, sizeof reason);
    SDL_LockMutex(s_lock);
    s_initOk = ok;
    snprintf(s_initReason, sizeof s_initReason, "%s", reason);
    s_initDone = true;
    SDL_UnlockMutex(s_lock);
    if (!ok) {
        return 0;  /* the pump reaps the thread and reports text-only */
    }

    for (;;) {
        char text[MDKR_A11Y_TEXT_MAX];

        SDL_LockMutex(s_lock);
        while (s_count == 0 && !s_quit) {
            SDL_CondWait(s_wake, s_lock);
        }
        if (s_quit) {
            SDL_UnlockMutex(s_lock);
            break;
        }
        memcpy(text, s_text[s_head], strlen(s_text[s_head]) + 1u);
        s_head = (s_head + 1) % BACKLOG_MAX;
        s_count--;
        s_speaking = true;
        SDL_UnlockMutex(s_lock);

        apply_voice();
        mdkr_a11y_speech_speak(text);  /* blocks for the length of the line */

        SDL_LockMutex(s_lock);
        s_speaking = false;
        s_spoken++;
        SDL_UnlockMutex(s_lock);
    }
    return 0;
}
#else
/* No thread in the browser build. The Web Speech API is asynchronous by
 * construction — speechSynthesis.speak() hands the sentence to the browser and
 * returns, and the backend keeps its own one-deep queue — so there is nothing
 * here that could stall a frame and nothing to move off it. */
static void speak_inline(void) {
    char text[MDKR_A11Y_TEXT_MAX];
    if (s_count == 0) {
        return;
    }
    memcpy(text, s_text[s_head], strlen(s_text[s_head]) + 1u);
    s_head = (s_head + 1) % BACKLOG_MAX;
    s_count--;
    apply_voice();
    mdkr_a11y_speech_speak(text);
    s_spoken++;
}
#endif

/* --- lifecycle ------------------------------------------------------------ */

static void set_reason(const char *text) {
    if (text == NULL || text[0] == '\0') {
        s_reason[0] = '\0';
        return;
    }
    snprintf(s_reason, sizeof s_reason, "%s", text);
}

static void go_text_only(const char *reason) {
    s_state = SERVICE_TEXT_ONLY;
    set_reason(reason);
    /* Once, on stderr, and only in a run that ASKED for speech. A player who
     * turned it on and hears nothing is owed the reason; a run that never
     * wanted a voice is owed silence, including in its log. */
    fprintf(stderr, "[a11y-speech] text-only: %s\n", s_reason);
    fflush(stderr);
}

/* Puts the worker up and asks it to open the engine. Returns with the service
 * STARTING, not RUNNING: whether there is a voice is not known yet, and waiting
 * here to find out is exactly the frame cost this design refuses to pay. */
static bool service_start(void) {
    /* The hard rule, checked before a backend is even asked to exist. Every
     * _init() checks it again; this is the copy that makes it impossible for a
     * new backend to forget. Cheap enough for the render thread — one getenv —
     * unlike everything below it. */
    if (!mdkr_a11y_speech_audio_permitted()) {
        go_text_only("audio is switched off for this run (MDKR_AUDIO=0)");
        return false;
    }

    s_head = 0;
    s_count = 0;
    s_speaking = false;
    /* Force the first utterance to publish both settings to the backend rather
     * than assuming whatever the engine happened to default to. */
    s_appliedRate = 0;
    s_appliedVolume = -1;

#ifndef __EMSCRIPTEN__
    s_quit = false;
    s_initDone = false;
    s_initOk = false;
    s_initReason[0] = '\0';
    s_lock = SDL_CreateMutex();
    s_wake = SDL_CreateCond();
    if (s_lock != NULL && s_wake != NULL) {
        s_thread = SDL_CreateThread(worker_main, "mdkr-a11y-speech", NULL);
    }
    if (s_lock == NULL || s_wake == NULL || s_thread == NULL) {
        if (s_wake != NULL) { SDL_DestroyCond(s_wake); s_wake = NULL; }
        if (s_lock != NULL) { SDL_DestroyMutex(s_lock); s_lock = NULL; }
        go_text_only("the game could not start its speech thread");
        return false;
    }
    s_state = SERVICE_STARTING;
#else
    /* No thread to defer to in the browser build, and nothing slow to defer:
     * the Web Speech backend's _init() is a feature test on a global. */
    {
        char reason[MDKR_A11Y_SPEECH_REASON_MAX];
        reason[0] = '\0';
        if (!mdkr_a11y_speech_init(reason, sizeof reason)) {
            go_text_only(reason[0] != '\0'
                             ? reason
                             : "this platform has no speech engine the game "
                               "can use");
            return false;
        }
    }
    s_state = SERVICE_RUNNING;
#endif

    set_reason("");
    if (!s_atexitRegistered) {
        /* Same arrangement as platform/audi_port_dkr.c's device teardown: the
         * exit paths out of the shell are many and this one is theirs. */
        atexit(mdkr_a11y_speech_service_shutdown);
        s_atexitRegistered = true;
    }
    return true;
}

#ifndef __EMSCRIPTEN__
/* Joins the worker and releases the primitives. The caller has already decided
 * the service is over; this is only the mechanics. */
static void reap_worker(bool cutSpeechOff) {
    SDL_LockMutex(s_lock);
    s_quit = true;
    s_head = 0;
    s_count = 0;
    SDL_UnlockMutex(s_lock);
    if (cutSpeechOff) {
        /* Cut the engine off before waiting: the worker may be several seconds
         * into a sentence, and _stop() is what makes the join prompt rather
         * than polite. Skipped when the backend never came up, so nothing
         * reaches a backend the worker is still inside _init() on. */
        mdkr_a11y_speech_stop();
    }
    SDL_CondBroadcast(s_wake);
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    SDL_DestroyCond(s_wake);
    s_wake = NULL;
    SDL_DestroyMutex(s_lock);
    s_lock = NULL;
}

/* One pump's worth of "has the worker opened the engine yet?". */
static void service_poll_start(void) {
    bool done;
    bool ok = false;
    char reason[MDKR_A11Y_SPEECH_REASON_MAX];

    reason[0] = '\0';
    SDL_LockMutex(s_lock);
    done = s_initDone;
    if (done) {
        ok = s_initOk;
        snprintf(reason, sizeof reason, "%s", s_initReason);
    }
    SDL_UnlockMutex(s_lock);
    if (!done) {
        return;  /* still opening; the model keeps its lines until it is */
    }
    if (ok) {
        s_state = SERVICE_RUNNING;
        return;
    }
    reap_worker(false);
    go_text_only(reason[0] != '\0'
                     ? reason
                     : "this platform has no speech engine the game can use");
}
#endif

static void service_stop(void) {
    if (s_state != SERVICE_RUNNING && s_state != SERVICE_STARTING) {
        return;
    }
#ifndef __EMSCRIPTEN__
    {
        /* Only a backend that finished coming up may be spoken to. */
        const bool backendUp = s_state == SERVICE_RUNNING;
        s_state = SERVICE_OFF;
        reap_worker(backendUp);
    }
#else
    s_state = SERVICE_OFF;
    s_head = 0;
    s_count = 0;
    mdkr_a11y_speech_stop();
#endif
    s_speaking = false;
    mdkr_a11y_speech_shutdown();

    /* One line, at teardown, in a run that had a voice. It is the only way the
     * dropping is observable from outside: utterances the engine was too slow
     * to reach are DESIGNED to disappear, and a designed loss that leaves no
     * trace is indistinguishable from a bug. */
    fprintf(stderr, "[a11y-speech] spoke %u utterances, dropped %u\n",
            s_spoken, s_dropped);
    fflush(stderr);

    /* Anything still queued in the model was written for a voice that is now
     * gone. Discarding it here is what stops a player who switches speech back
     * on an hour later from being read the row they were on when they switched
     * it off. */
    for (;;) {
        char text[MDKR_A11Y_TEXT_MAX];
        if (!mdkr_a11y_next(text, sizeof text, NULL)) {
            break;
        }
    }
    mdkr_a11y_finish();
}

/* --- the per-frame pump --------------------------------------------------- */

void mdkr_a11y_speech_service_pump(void) {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    const bool want = config != NULL &&
                      config->values[MDKR_A11Y_SPEECH].number != 0.0f;

    if (!want) {
        service_stop();
        return;
    }
    if (s_state == SERVICE_TEXT_ONLY) {
        return;  /* refused, and nothing about a run changes that verdict */
    }
    if (s_state == SERVICE_OFF && !service_start()) {
        return;
    }
#ifndef __EMSCRIPTEN__
    if (s_state == SERVICE_STARTING) {
        service_poll_start();
        if (s_state != SERVICE_RUNNING) {
            /* Still opening the engine, or it turned out there is none. Either
             * way nothing is popped: leaving the lines in the model lets its
             * own coalescing keep them current, so a slow engine start costs
             * staleness rather than a backlog of rows the player has left. */
            return;
        }
    }
#endif
    publish_voice(config);

    /* The player has moved on: whatever is being said is now wrong. The model
     * never reports this for a CRITICAL line, which is what CRITICAL is for. */
    if (mdkr_a11y_in_flight_cancelled()) {
        backlog_purge();
        mdkr_a11y_finish();
    }

    /* Take everything the model has. Bounded twice over — the model's ring
     * holds at most MDKR_A11Y_QUEUE_MAX and the handoff at most BACKLOG_MAX —
     * so a burst costs a bounded memcpy and leaves the NEWEST lines queued. */
    for (;;) {
        char text[MDKR_A11Y_TEXT_MAX];
        if (!mdkr_a11y_next(text, sizeof text, NULL)) {
            break;
        }
        backlog_push(text);
    }

#ifdef __EMSCRIPTEN__
    speak_inline();
#endif

    /* Nothing queued and nothing being said: the utterance the model still
     * counts as in flight is over, and telling it so is what stops the next
     * announcement from being treated as a barge-in on silence. */
    if (backlog_idle()) {
        mdkr_a11y_finish();
    }
}

void mdkr_a11y_speech_service_shutdown(void) {
    service_stop();
}

const char *mdkr_a11y_speech_service_reason(void) {
    return s_reason;
}

unsigned mdkr_a11y_speech_service_spoken(void) {
    unsigned spoken;
    lock_backlog();
    spoken = s_spoken;
    unlock_backlog();
    return spoken;
}

unsigned mdkr_a11y_speech_service_dropped(void) {
    return s_dropped;
}
