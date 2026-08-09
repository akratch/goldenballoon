/* a11y_speech_null.c — no voice, and it says so.
 *
 * VERIFICATION: RUN, as the refusal path. Every gate in the suite exercises the
 * same refusal this file makes permanent, because MDKR_AUDIO=0 turns every
 * platform into this one for the length of the run.
 *
 * This is the backend a platform gets when nobody has written it one: the BSDs,
 * Haiku, anything the four real backends do not claim. It is not a stub in the
 * sense of "unfinished" — degrading to text-only with a stated reason IS the
 * required behaviour for a platform with no engine, and having it as a real
 * compiled backend is what stops the absence of a voice from being a link
 * error. Same arrangement, and the same reasoning, as
 * platform/app/file_dialog_stub.cpp.
 *
 * The model still runs. Announcements are still composed, still ordered, still
 * traced under MDKR_A11Y_TRACE=1. The only thing missing is the sound.
 */
#include "a11y_speech.h"

#include <stdio.h>

bool mdkr_a11y_speech_init(char *reason, size_t reason_size) {
    if (reason != NULL && reason_size > 0u) {
        snprintf(reason, reason_size, "%s",
                 !mdkr_a11y_speech_audio_permitted()
                     ? "audio is switched off for this run (MDKR_AUDIO=0)"
                     : "this system has no speech engine the game knows how "
                       "to use, so the menus are text only");
    }
    return false;
}

void mdkr_a11y_speech_speak(const char *text) { (void)text; }

void mdkr_a11y_speech_stop(void) {}

void mdkr_a11y_speech_shutdown(void) {}

bool mdkr_a11y_speech_available(void) { return false; }

void mdkr_a11y_speech_set_rate(int percent) { (void)percent; }

void mdkr_a11y_speech_set_volume(int percent) { (void)percent; }
