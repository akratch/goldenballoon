/* a11y_speech_web.c — the browser voice, through the Web Speech API.
 *
 * VERIFICATION: COMPILED ONLY, by emcc under -Wall -Wextra -Werror. Never
 * loaded in a browser and never heard, and nothing calls it yet: the wasm build
 * sets MDKR_APP=OFF, so there is no shell there to announce anything. It
 * compiles and it is ready; that is the whole claim.
 *
 * WHY THERE IS NO THREAD HERE. a11y_speech_worker.c runs the browser build
 * single-threaded and calls _speak() straight from the frame pump, which is
 * safe because nothing in this file blocks: speechSynthesis.speak() hands the
 * sentence to the browser and returns, and the browser synthesises it off the
 * page's main thread. The reason the native backends need a worker — a
 * synthesiser call that lasts as long as the sentence — simply does not exist
 * on this platform.
 *
 * WHY THE QUEUE IS IN JAVASCRIPT. speechSynthesis has an unbounded internal
 * queue and no way to ask how deep it is, so handing it every utterance would
 * read a player twenty stale settings rows one after another. The shim below
 * keeps exactly one utterance pending and replaces it when a newer one arrives
 * — the same drop-oldest rule the model and the worker both apply, enforced at
 * the last place that can still enforce it.
 */
#include "a11y_speech.h"

#include <emscripten.h>

#include <stdio.h>

EM_JS(int, mdkr_web_speech_supported, (void), {
    return (typeof speechSynthesis !== "undefined" &&
            typeof SpeechSynthesisUtterance !== "undefined") ? 1 : 0;
});

EM_JS(void, mdkr_web_speech_say, (const char *text, int rate_percent,
                                  int volume_percent), {
    var state = Module.__mdkrSpeech || (Module.__mdkrSpeech = {
        active: false, pending: null
    });
    var start = function(item) {
        state.active = true;
        var utterance = new SpeechSynthesisUtterance(item.text);
        utterance.rate = Math.max(0.1, Math.min(10, item.rate / 100));
        utterance.volume = Math.max(0, Math.min(1, item.volume / 100));
        var done = function() {
            state.active = false;
            var next = state.pending;
            state.pending = null;
            if (next) { start(next); }
        };
        utterance.onend = done;
        utterance.onerror = done;
        speechSynthesis.speak(utterance);
    };
    var item = {
        text: UTF8ToString(text), rate: rate_percent, volume: volume_percent
    };
    /* Drop the oldest: a pending line the player has already moved past is
       worth less than the one they are on now. */
    if (state.active) { state.pending = item; } else { start(item); }
});

EM_JS(void, mdkr_web_speech_cancel, (void), {
    var state = Module.__mdkrSpeech;
    if (state) { state.pending = null; state.active = false; }
    if (typeof speechSynthesis !== "undefined") { speechSynthesis.cancel(); }
});

static bool s_ready;
static int  s_ratePercent = MDKR_A11Y_SPEECH_RATE_NORMAL;
static int  s_volumePercent = MDKR_A11Y_SPEECH_VOLUME_MAX;

static void report(char *reason, size_t reason_size, const char *text) {
    if (reason != NULL && reason_size > 0u) {
        snprintf(reason, reason_size, "%s", text);
    }
}

bool mdkr_a11y_speech_init(char *reason, size_t reason_size) {
    if (!mdkr_a11y_speech_audio_permitted()) {
        report(reason, reason_size,
               "audio is switched off for this run (MDKR_AUDIO=0)");
        return false;
    }
    if (s_ready) {
        return true;
    }
    if (!mdkr_web_speech_supported()) {
        report(reason, reason_size,
               "this browser has no speech synthesis, so the game cannot read "
               "the menus out loud here");
        return false;
    }
    s_ready = true;
    report(reason, reason_size, "");
    return true;
}

void mdkr_a11y_speech_speak(const char *text) {
    if (!s_ready || text == NULL || text[0] == '\0') {
        return;
    }
    mdkr_web_speech_say(text, s_ratePercent, s_volumePercent);
}

void mdkr_a11y_speech_stop(void) {
    if (!s_ready) {
        return;
    }
    mdkr_web_speech_cancel();
}

void mdkr_a11y_speech_shutdown(void) {
    if (!s_ready) {
        return;
    }
    mdkr_web_speech_cancel();
    s_ready = false;
}

bool mdkr_a11y_speech_available(void) {
    return s_ready;
}

/* SpeechSynthesisUtterance.rate is already a multiplier on the voice's normal
 * pace, which is exactly what the schema's percentage means — the only mapping
 * needed is the divide by a hundred, done in the shim above. */
void mdkr_a11y_speech_set_rate(int percent) {
    if (percent < MDKR_A11Y_SPEECH_RATE_MIN) percent = MDKR_A11Y_SPEECH_RATE_MIN;
    if (percent > MDKR_A11Y_SPEECH_RATE_MAX) percent = MDKR_A11Y_SPEECH_RATE_MAX;
    s_ratePercent = percent;
}

void mdkr_a11y_speech_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > MDKR_A11Y_SPEECH_VOLUME_MAX) {
        percent = MDKR_A11Y_SPEECH_VOLUME_MAX;
    }
    s_volumePercent = percent;
}
