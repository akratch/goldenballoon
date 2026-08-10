/* test_a11y_speech_worker.c — barge-in against the line the worker already has.
 *
 * WHAT THIS EXISTS TO CATCH. platform/a11y_speech_worker.c hands utterances to
 * a backend on a worker thread: the pump pops the backlog under a mutex, copies
 * the text into a local, releases the mutex, and only then speaks. A barge-in
 * (mdkr_a11y_speech_service_pump() seeing the model report the in-flight line
 * superseded) empties the backlog and calls the backend's _stop(). Land that
 * between the worker's unlock and its _speak() and BOTH halves miss: there is
 * nothing left in the backlog to throw away, and _stop() hits an engine that
 * has not started the line yet. The worker then speaks, in full, the utterance
 * the player barged in on — after the thing that was supposed to cancel it.
 *
 * WHY IT IS DETERMINISTIC RATHER THAN A STRESS LOOP. The window is a few
 * instructions wide and a test that just races two threads at it reproduces the
 * defect by luck, which makes a green run mean nothing. So the worker carries
 * one test-only hook at exactly that point (SPEECH_TEST_PRESPEAK, compiled in
 * only under MDKR_A11Y_SPEECH_TESTING, which only this target defines) and the
 * case below parks the worker in it with a semaphore, runs the barge-in on the
 * pump's own thread while it is parked, and only then lets it go. There is no
 * sleep, no retry and no timing assumption anywhere in the interleaving: the
 * worker physically cannot leave the window until the barge-in has finished.
 *
 * WHY THIS BINARY CLEARS MDKR_AUDIO, and why that is not the project's audio
 * rule being bent. The rule exists so no test run can open a device and make a
 * sound. This one cannot. The only implementation of the a11y_speech.h backend
 * interface linked into it is the fake below, which copies strings into an
 * array; linking a real backend alongside it is not a mistake that could happen
 * quietly, because all seven functions would be duplicate symbols and the link
 * would fail. SDL is here for its threads and is never initialised, so there is
 * no audio subsystem either. The service refuses to bring ANY backend up under
 * MDKR_AUDIO=0 — including a fake one — so clearing the variable is the only
 * way to exercise the worker loop at all, and the fake's own init flag is
 * asserted below so a run that somehow reached a real engine would say so.
 */
#include "a11y_model.h"
#include "a11y_speech.h"
#include "video_config.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static int clear_env(const char *name) { return _putenv_s(name, ""); }
#else
#include <unistd.h>
static int clear_env(const char *name) { return unsetenv(name); }
#endif

/* The seam. Declared here rather than in a11y_speech.h on purpose: the shipped
 * header should not carry a hook nothing shipped can use. */
extern void (*mdkr_a11y_speech_test_prespeak)(void);

/* Every wait in this file is bounded. A deadlock in threaded code under test
 * must be a failing test with a sentence attached, not a suite that hangs until
 * someone kills it. Generous, because the bound is a backstop and never part of
 * the interleaving being asserted. */
#define WAIT_MS 5000

static int s_failures;

static void expect(int condition, const char *what) {
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        s_failures++;
    }
}

/* --- the fake backend ------------------------------------------------------
 *
 * Records what it was asked to say and returns immediately. Not blocking for
 * the length of a sentence is the point: the defect is about WHICH line reaches
 * _speak(), not about how long it stays there.
 */

#define SPOKEN_MAX 8

static SDL_mutex *g_backendLock;
static SDL_sem   *g_spoke;
static char       g_spokenText[SPOKEN_MAX][MDKR_A11Y_TEXT_MAX];
static int        g_spokenCount;
static SDL_atomic_t g_stopCalls;
static SDL_atomic_t g_initCalls;
static SDL_atomic_t g_releaseOnStop;
static int        g_rate;
static int        g_volume;

/* The seam rendezvous, declared up here because the fake stop() below is one of
 * the two things that releases it. */
static SDL_sem *g_atSeam;
static SDL_sem *g_resume;

bool mdkr_a11y_speech_init(char *reason, size_t reason_size) {
    SDL_AtomicAdd(&g_initCalls, 1);
    if (reason != NULL && reason_size > 0u) {
        reason[0] = '\0';
    }
    return true;
}

void mdkr_a11y_speech_speak(const char *text) {
    SDL_LockMutex(g_backendLock);
    if (g_spokenCount < SPOKEN_MAX) {
        snprintf(g_spokenText[g_spokenCount], sizeof g_spokenText[0], "%s",
                 text != NULL ? text : "(null)");
        g_spokenCount++;
    }
    SDL_UnlockMutex(g_backendLock);
    SDL_SemPost(g_spoke);
}

void mdkr_a11y_speech_stop(void) {
    SDL_AtomicAdd(&g_stopCalls, 1);
    /* The teardown case arms this, and only that case. reap_worker() bumps the
     * barge-in stamp under the mutex, releases it, calls _stop(), and only then
     * joins — so releasing a parked worker from inside _stop() is the one
     * release point that is provably after the stamp and before the join.
     * Nothing on the main thread can order those three, because the join is
     * what would block waiting for the worker this releases. */
    if (SDL_AtomicGet(&g_releaseOnStop) != 0) {
        SDL_AtomicSet(&g_releaseOnStop, 0);
        SDL_SemPost(g_resume);
    }
}

void mdkr_a11y_speech_shutdown(void) {}

bool mdkr_a11y_speech_available(void) { return true; }

void mdkr_a11y_speech_set_rate(int percent) { g_rate = percent; }

void mdkr_a11y_speech_set_volume(int percent) { g_volume = percent; }

/* Snapshot of the utterance list, taken after the worker has been joined so the
 * answer is final rather than merely current. */
static int spoken_count(void) {
    int count;
    SDL_LockMutex(g_backendLock);
    count = g_spokenCount;
    SDL_UnlockMutex(g_backendLock);
    return count;
}

static const char *spoken_at(int index) {
    return index >= 0 && index < spoken_count() ? g_spokenText[index] : "(none)";
}

static void print_spoken(const char *label) {
    int index;
    const int count = spoken_count();
    printf("     %s: %d utterance(s)", label, count);
    for (index = 0; index < count; index++) {
        printf(" [%d]=\"%s\"", index, g_spokenText[index]);
    }
    printf("\n");
}

/* --- the fake model --------------------------------------------------------
 *
 * Touched only by the main thread, which is the thread the real model is
 * single-owner on (see the ownership paragraph in a11y_model.h). The worker
 * never sees it: what crosses the thread boundary is the worker's own
 * mutex-guarded backlog, which is the structure under test.
 */

#define PENDING_MAX 8

static char g_pending[PENDING_MAX][MDKR_A11Y_TEXT_MAX];
static int  g_pendingCount;
static bool g_cancelled;
static int  g_nextCalls;
static int  g_finishCalls;

static void model_queue(const char *text) {
    if (g_pendingCount >= PENDING_MAX) {
        expect(0, "the fake model has room for the queued line");
        return;
    }
    snprintf(g_pending[g_pendingCount], sizeof g_pending[0], "%s", text);
    g_pendingCount++;
}

int mdkr_a11y_next(char *out, size_t out_size, MdkrA11yCategory *out_category) {
    int index;

    g_nextCalls++;
    if (g_pendingCount == 0 || out == NULL || out_size < MDKR_A11Y_TEXT_MAX) {
        return 0;
    }
    snprintf(out, out_size, "%s", g_pending[0]);
    for (index = 1; index < g_pendingCount; index++) {
        memcpy(g_pending[index - 1], g_pending[index], sizeof g_pending[0]);
    }
    g_pendingCount--;
    if (out_category != NULL) {
        *out_category = MDKR_A11Y_CAT_FOCUS;
    }
    return 1;
}

void mdkr_a11y_finish(void) { g_finishCalls++; }

bool mdkr_a11y_in_flight_cancelled(void) { return g_cancelled; }

/* --- the fake live config -------------------------------------------------- */

static MdkrVideoConfig g_config;

const MdkrVideoConfig *mdkr_video_config_current(void) { return &g_config; }

static void config_speech(bool on) {
    memset(&g_config, 0, sizeof g_config);
    g_config.values[MDKR_A11Y_SPEECH].number = on ? 1.0f : 0.0f;
    g_config.values[MDKR_A11Y_SPEECH_RATE].number =
        (float)MDKR_A11Y_SPEECH_RATE_NORMAL;
    g_config.values[MDKR_A11Y_SPEECH_VOLUME].number =
        (float)MDKR_A11Y_SPEECH_VOLUME_MAX;
}

/* --- harness --------------------------------------------------------------- */

static bool wait_for(SDL_sem *sem, const char *what) {
    if (SDL_SemWaitTimeout(sem, WAIT_MS) == 0) {
        return true;
    }
    printf("FAIL timed out after %d ms waiting for %s\n", WAIT_MS, what);
    s_failures++;
    return false;
}

static void reset_state(void) {
    SDL_LockMutex(g_backendLock);
    g_spokenCount = 0;
    SDL_UnlockMutex(g_backendLock);
    SDL_AtomicSet(&g_stopCalls, 0);
    SDL_AtomicSet(&g_initCalls, 0);
    SDL_AtomicSet(&g_releaseOnStop, 0);
    while (SDL_SemTryWait(g_spoke) == 0) { /* drain the previous case */ }
    while (SDL_SemTryWait(g_atSeam) == 0) { /* ditto */ }
    while (SDL_SemTryWait(g_resume) == 0) { /* ditto */ }
    g_pendingCount = 0;
    g_cancelled = false;
    g_nextCalls = 0;
    g_finishCalls = 0;
    mdkr_a11y_speech_test_prespeak = NULL;
    config_speech(true);
}

/* The service comes up STARTING: the worker's first act is to open the engine,
 * and only a later pump promotes it to RUNNING. Pumping until the drain loop
 * runs is how that is observed from outside — mdkr_a11y_next() is the first
 * thing the pump only reaches once the backend is up. Setup, not interleaving;
 * nothing about the case being asserted depends on how many frames it takes. */
static bool start_service(void) {
    int frame;

    for (frame = 0; frame < 3000; frame++) {
        mdkr_a11y_speech_service_pump();
        if (g_nextCalls > 0) {
            expect(SDL_AtomicGet(&g_initCalls) == 1,
                   "the fake backend is the one the service brought up");
            return true;
        }
        SDL_Delay(1);
    }
    printf("FAIL the speech service never reached RUNNING (reason: \"%s\")\n",
           mdkr_a11y_speech_service_reason());
    s_failures++;
    return false;
}

/* --- the seam ---------------------------------------------------------------
 *
 * Runs ON the worker thread, at the exact instruction the defect needs. It
 * clears itself first: that write and the read that dispatched it are the same
 * thread, so the one-shot needs no synchronisation, and without it the worker
 * would park again on the replacement line.
 *
 * The main thread publishes the pointer before it queues the line that will be
 * popped, so the ordering is the backlog mutex's: the pointer is written, then
 * backlog_push() takes and releases the mutex, then the worker takes the same
 * mutex to pop. The worker cannot observe the pop without observing the write.
 */
static void park_at_seam(void) {
    mdkr_a11y_speech_test_prespeak = NULL;
    SDL_SemPost(g_atSeam);
    SDL_SemWait(g_resume);
}

/* --- cases ------------------------------------------------------------------ */

/* 1. The defect itself. */
static void test_barge_in_kills_the_line_already_popped(void) {
    printf("-- barge-in against a line the worker has already popped\n");
    reset_state();
    if (!start_service()) {
        return;
    }

    /* Park the worker between its pop and its _speak(). */
    mdkr_a11y_speech_test_prespeak = park_at_seam;
    model_queue("the line the player barged in on");
    mdkr_a11y_speech_service_pump();
    if (!wait_for(g_atSeam, "the worker to reach the pre-speak seam")) {
        SDL_SemPost(g_resume);  /* never leave it parked; the join would hang */
        mdkr_a11y_speech_service_shutdown();
        return;
    }

    /* The barge-in, on the pump's own thread, exactly as the shell does it:
     * the model reports the in-flight line superseded and offers a newer one.
     * The backlog is empty (the worker has the line) and the engine is idle
     * (the worker has not spoken it yet), so this is the interleaving in which
     * both halves of the old purge miss. */
    g_cancelled = true;
    model_queue("what the player asked for instead");
    mdkr_a11y_speech_service_pump();
    g_cancelled = false;
    expect(SDL_AtomicGet(&g_stopCalls) >= 1,
           "the barge-in did reach the backend's stop()");

    SDL_SemPost(g_resume);

    if (!wait_for(g_spoke, "the worker to speak something after the barge-in")) {
        mdkr_a11y_speech_service_shutdown();
        return;
    }
    /* Shut down first: the join is what makes the utterance list final, so
     * "was never spoken" is an assertion rather than a snapshot. */
    mdkr_a11y_speech_service_shutdown();

    if (spoken_count() != 1 ||
        strcmp(spoken_at(0), "what the player asked for instead") != 0) {
        printf("FAIL the cancelled line was spoken anyway\n");
        printf("     first utterance : \"%s\"\n", spoken_at(0));
        printf("     expected        : \"what the player asked for instead\"\n");
        print_spoken("everything spoken");
        s_failures++;
    } else {
        expect(1, "the line the player barged in on is never spoken");
        expect(1, "the line the player asked for instead is");
    }
}

/* 2. That the stamp is a generation and not a sticky cancel flag.
 *
 * The obvious wrong fix — a "cancelled" boolean the backend or the worker sets
 * on stop() and tests on speak() — passes case 1 and then eats the REPLACEMENT
 * utterance whenever a barge-in lands with nothing in flight, which is the one
 * line the player actually wants to hear. */
static void test_a_purge_on_silence_does_not_eat_the_next_line(void) {
    printf("-- a barge-in with nothing in flight, then a new line\n");
    reset_state();
    if (!start_service()) {
        return;
    }

    g_cancelled = true;
    mdkr_a11y_speech_service_pump();  /* purge: empty backlog, idle engine */
    g_cancelled = false;
    expect(SDL_AtomicGet(&g_stopCalls) >= 1, "the purge reached stop()");

    model_queue("the announcement after the barge-in");
    mdkr_a11y_speech_service_pump();
    (void)wait_for(g_spoke, "the line queued after the purge to be spoken");
    mdkr_a11y_speech_service_shutdown();

    expect(spoken_count() == 1 &&
               strcmp(spoken_at(0), "the announcement after the barge-in") == 0,
           "a purge on silence does not cancel the line that follows it");
    if (spoken_count() != 1) {
        print_spoken("what was spoken");
    }
}

/* 3. Non-vacuity: with no barge-in at all, everything queued is still said, in
 * order. A stamp that discarded too eagerly would pass case 1 by saying
 * nothing, and this is what stops that being a green run. */
static void test_without_a_barge_in_every_line_is_spoken(void) {
    printf("-- no barge-in: both queued lines are spoken, in order\n");
    reset_state();
    if (!start_service()) {
        return;
    }

    model_queue("first line");
    model_queue("second line");
    mdkr_a11y_speech_service_pump();
    (void)wait_for(g_spoke, "the first line");
    (void)wait_for(g_spoke, "the second line");
    mdkr_a11y_speech_service_shutdown();

    expect(spoken_count() == 2, "both lines reached the backend");
    expect(strcmp(spoken_at(0), "first line") == 0 &&
               strcmp(spoken_at(1), "second line") == 0,
           "and in the order the model produced them");
    if (spoken_count() != 2) {
        print_spoken("what was spoken");
    }
    expect(g_rate == MDKR_A11Y_SPEECH_RATE_NORMAL &&
               g_volume == MDKR_A11Y_SPEECH_VOLUME_MAX,
           "the live voice settings reached the backend before the utterance");
}

/* 4. Teardown while a line is parked in the same window. The stamp is bumped by
 * the reaper too, and this is why: without it the worker says a line nobody
 * will hear and the join waits out the whole sentence. */
static void test_shutdown_does_not_speak_a_parked_line(void) {
    printf("-- shutdown while a line sits in the pre-speak window\n");
    reset_state();
    if (!start_service()) {
        return;
    }

    mdkr_a11y_speech_test_prespeak = park_at_seam;
    model_queue("a line the player will never hear");
    mdkr_a11y_speech_service_pump();
    if (!wait_for(g_atSeam, "the worker to reach the pre-speak seam")) {
        SDL_SemPost(g_resume);
        mdkr_a11y_speech_service_shutdown();
        return;
    }

    /* Speech switched off, so the next pump takes the service down. The worker
     * is released from inside the reaper's own _stop() (see the fake above),
     * which is the only point ordered after the stamp and before the join —
     * releasing it any earlier would let it race the stamp and make this case
     * decide by luck, and any later is inside a join that is waiting for it. */
    config_speech(false);
    SDL_AtomicSet(&g_releaseOnStop, 1);
    mdkr_a11y_speech_service_pump();
    expect(SDL_AtomicGet(&g_releaseOnStop) == 0,
           "the reaper's stop() ran, so the worker was released after the stamp");

    expect(spoken_count() == 0,
           "a line cancelled by teardown is not spoken on the way out");
    if (spoken_count() != 0) {
        print_spoken("what was spoken");
    }
}

int main(void) {
    unsigned droppedBefore;
    unsigned droppedAfter;

    SDL_SetMainReady();
    (void)clear_env("MDKR_AUDIO");

    g_backendLock = SDL_CreateMutex();
    g_spoke = SDL_CreateSemaphore(0);
    g_atSeam = SDL_CreateSemaphore(0);
    g_resume = SDL_CreateSemaphore(0);
    if (g_backendLock == NULL || g_spoke == NULL || g_atSeam == NULL ||
        g_resume == NULL) {
        printf("FAIL SDL would not create the test's own primitives: %s\n",
               SDL_GetError());
        return 1;
    }

    droppedBefore = mdkr_a11y_speech_service_dropped();
    test_barge_in_kills_the_line_already_popped();
    droppedAfter = mdkr_a11y_speech_service_dropped();
    expect(droppedAfter == droppedBefore + 1u,
           "the cancelled line is counted as dropped, not lost silently");

    test_a_purge_on_silence_does_not_eat_the_next_line();
    test_without_a_barge_in_every_line_is_spoken();
    test_shutdown_does_not_speak_a_parked_line();

    SDL_DestroySemaphore(g_resume);
    SDL_DestroySemaphore(g_atSeam);
    SDL_DestroySemaphore(g_spoke);
    SDL_DestroyMutex(g_backendLock);

    if (s_failures != 0) {
        printf("FAILURES: %d\n", s_failures);
        return 1;
    }
    printf("all speech worker barge-in assertions passed\n");
    return 0;
}
