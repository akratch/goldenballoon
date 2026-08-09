/* a11y_speech_linux.c — the Linux voice, through speech-dispatcher.
 *
 * VERIFICATION: COMPILED ONLY. Built clean by a real Linux gcc 14 under
 * -Wall -Wextra -Werror -Wpedantic, at both -std=c11 and -std=gnu11, and never
 * run — no one has heard speech-dispatcher say a word through this file.
 * Nothing here is a claim that Linux speech works.
 *
 * WHY dlopen AND NOT A LINK-TIME DEPENDENCY. The release tarball has to start
 * on a machine that has never heard of speech-dispatcher, and a DT_NEEDED entry
 * for libspeechd.so.2 would turn "no voice" into "will not launch" for every
 * player who does not use one. Resolved at runtime, an absent library is a
 * sentence in the settings panel instead: the app says it is text-only and why.
 *
 * WHY THE UTTERANCE LENGTH IS ESTIMATED. libspeechd's completion callbacks
 * require reaching into the SPDConnection struct, whose layout we would be
 * copying out of a header we deliberately do not include — exactly the kind of
 * ABI assumption dlopen exists to avoid. So _speak() paces itself off the
 * sentence's own length and the player's rate instead. The consequence is
 * honest and small: the gap between two utterances is approximate. Barge-in is
 * not approximate, because it does not wait for the estimate.
 *
 * WHY THE WORKER MAKES EVERY LIBRARY CALL. An SPDConnection is not documented
 * thread-safe, so _stop() raises a flag rather than cancelling from the UI
 * thread; the worker is sleeping in 20 ms slices and cancels on the next one.
 */
/* clock_gettime and nanosleep are POSIX, not ISO C, and glibc hides both unless
 * asked. The build happens to pass -std=gnu11, which would expose them by
 * accident; asking explicitly is what stops a future -std=c11 from turning this
 * file into two implicit-declaration errors. Must precede every include. */
#define _POSIX_C_SOURCE 200809L

#include "a11y_speech.h"

#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* speech-dispatcher's public constants, spelled out rather than included: this
 * file must build on a machine with no libspeechd headers at all. */
#define SPD_MODE_SINGLE 0
#define SPD_MESSAGE     2

typedef void *(*spd_open_fn)(const char *client_name,
                             const char *connection_name,
                             const char *user_name, int mode);
typedef void (*spd_close_fn)(void *connection);
typedef int  (*spd_say_fn)(void *connection, int priority, const char *text);
typedef int  (*spd_cancel_fn)(void *connection);
typedef int  (*spd_set_int_fn)(void *connection, int value);

static void          *s_library;
static void          *s_connection;
static spd_open_fn    s_open;
static spd_close_fn   s_close;
static spd_say_fn     s_say;
static spd_cancel_fn  s_cancel;
static spd_set_int_fn s_setRate;
static spd_set_int_fn s_setVolume;

static bool s_ready;
static int  s_ratePercent = MDKR_A11Y_SPEECH_RATE_NORMAL;
static atomic_int s_stopRequested;

static void report(char *reason, size_t reason_size, const char *text) {
    if (reason != NULL && reason_size > 0u) {
        snprintf(reason, reason_size, "%s", text);
    }
}

/* dlsym returns an object pointer that ISO C will not convert to a function
 * pointer directly; assigning through the address is the portable spelling and
 * the one that survives -Wpedantic. */
static bool bind_symbol(void *slot, const char *name) {
    void *symbol = dlsym(s_library, name);
    if (symbol == NULL) {
        return false;
    }
    memcpy(slot, &symbol, sizeof symbol);
    return true;
}

static void unload(void) {
    if (s_connection != NULL && s_close != NULL) {
        s_close(s_connection);
    }
    s_connection = NULL;
    if (s_library != NULL) {
        dlclose(s_library);
        s_library = NULL;
    }
    s_open = NULL;
    s_close = NULL;
    s_say = NULL;
    s_cancel = NULL;
    s_setRate = NULL;
    s_setVolume = NULL;
}

bool mdkr_a11y_speech_init(char *reason, size_t reason_size) {
    /* Two sonames, newest first. .so.2 is what a runtime package installs; the
     * unversioned link only exists where the -dev package does. */
    static const char *const k_candidates[] = {
        "libspeechd.so.2", "libspeechd.so"
    };
    size_t index;

    if (!mdkr_a11y_speech_audio_permitted()) {
        report(reason, reason_size,
               "audio is switched off for this run (MDKR_AUDIO=0)");
        return false;
    }
    if (s_ready) {
        return true;
    }

    for (index = 0u; index < sizeof k_candidates / sizeof k_candidates[0];
         index++) {
        s_library = dlopen(k_candidates[index], RTLD_LAZY | RTLD_LOCAL);
        if (s_library != NULL) {
            break;
        }
    }
    if (s_library == NULL) {
        report(reason, reason_size,
               "speech-dispatcher is not installed, so the game has no voice "
               "to speak with. Install it (the speech-dispatcher package) and "
               "turn this back on.");
        return false;
    }
    if (!bind_symbol(&s_open, "spd_open") ||
        !bind_symbol(&s_close, "spd_close") ||
        !bind_symbol(&s_say, "spd_say") ||
        !bind_symbol(&s_cancel, "spd_cancel") ||
        !bind_symbol(&s_setRate, "spd_set_voice_rate") ||
        !bind_symbol(&s_setVolume, "spd_set_volume")) {
        unload();
        report(reason, reason_size,
               "the installed speech-dispatcher library is not the one this "
               "game knows how to talk to");
        return false;
    }

    /* SINGLE, not THREADED: the threaded mode exists to deliver callbacks, and
     * this backend deliberately takes none. */
    s_connection = s_open("mdkr64", "a11y", NULL, SPD_MODE_SINGLE);
    if (s_connection == NULL) {
        unload();
        report(reason, reason_size,
               "speech-dispatcher is installed but is not running, so there is "
               "nothing to speak through");
        return false;
    }
    s_ready = true;
    report(reason, reason_size, "");
    return true;
}

/* ~13 characters a second at the normal rate, scaled by the rate the player
 * chose, plus a fixed allowance for the daemon to start talking. */
static double utterance_seconds(const char *text) {
    double ratio = (double)s_ratePercent / 100.0;
    if (ratio < 0.1) {
        ratio = 0.1;
    }
    return 0.4 + ((double)strlen(text) / 13.0) / ratio;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void mdkr_a11y_speech_speak(const char *text) {
    struct timespec slice;
    double deadline;

    if (!s_ready || text == NULL || text[0] == '\0') {
        return;
    }
    atomic_store(&s_stopRequested, 0);
    if (s_say(s_connection, SPD_MESSAGE, text) < 0) {
        return;
    }

    slice.tv_sec = 0;
    slice.tv_nsec = 20 * 1000 * 1000; /* 20 ms */
    deadline = monotonic_seconds() + utterance_seconds(text);
    while (monotonic_seconds() < deadline) {
        if (atomic_exchange(&s_stopRequested, 0) != 0) {
            s_cancel(s_connection);
            return;
        }
        nanosleep(&slice, NULL);
    }
}

void mdkr_a11y_speech_stop(void) {
    /* Deliberately does not touch the connection: see the file header. */
    atomic_store(&s_stopRequested, 1);
}

void mdkr_a11y_speech_shutdown(void) {
    if (!s_ready) {
        return;
    }
    s_ready = false;
    /* The service has already joined the worker, so this thread is the only
     * one left that could be holding the connection. */
    if (s_cancel != NULL && s_connection != NULL) {
        s_cancel(s_connection);
    }
    unload();
}

bool mdkr_a11y_speech_available(void) {
    return s_ready;
}

/* speech-dispatcher's rate and volume are both -100..100 with 0 the middle.
 * The schema's rate is a percentage centred on 100, so the two halves scale
 * differently: 50 percent lands on -100, 100 on 0, 300 on +100. */
void mdkr_a11y_speech_set_rate(int percent) {
    int rate;

    if (percent < MDKR_A11Y_SPEECH_RATE_MIN) percent = MDKR_A11Y_SPEECH_RATE_MIN;
    if (percent > MDKR_A11Y_SPEECH_RATE_MAX) percent = MDKR_A11Y_SPEECH_RATE_MAX;
    s_ratePercent = percent;
    rate = percent >= MDKR_A11Y_SPEECH_RATE_NORMAL
               ? (percent - MDKR_A11Y_SPEECH_RATE_NORMAL) * 100 /
                     (MDKR_A11Y_SPEECH_RATE_MAX - MDKR_A11Y_SPEECH_RATE_NORMAL)
               : (percent - MDKR_A11Y_SPEECH_RATE_NORMAL) * 100 /
                     (MDKR_A11Y_SPEECH_RATE_NORMAL - MDKR_A11Y_SPEECH_RATE_MIN);
    if (s_ready && s_setRate != NULL) {
        s_setRate(s_connection, rate);
    }
}

void mdkr_a11y_speech_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > MDKR_A11Y_SPEECH_VOLUME_MAX) {
        percent = MDKR_A11Y_SPEECH_VOLUME_MAX;
    }
    if (s_ready && s_setVolume != NULL) {
        s_setVolume(s_connection, percent * 2 - 100);
    }
}
