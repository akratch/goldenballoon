/** enh_ai_difficulty.c — see enh_ai_difficulty.h. */
/* <ultra64.h> first, for the reason enh_speedometer.c states: PR/os_libc.h
 * redeclares the printf family, which the host's fortified <stdio.h> has
 * already turned into macros if it got in ahead. */
#include <ultra64.h>

#include <stdio.h>
#include <stdlib.h>

#include "racer.h" /* PLAYER_COMPUTER */

#include "enh_ai_difficulty.h"
#include "video_config.h"

/*
 * The two scales, and where they come from.
 *
 * They are deliberately small, and the size is measured rather than chosen for
 * how it reads. On Ancient Lake with DKR's own AI driving the player's kart,
 * the authored field wins by 186 ticks of a 4638-tick course time and the
 * whole eight-kart field finishes inside 862 ticks. One percent of top speed
 * is therefore worth about a quarter of the gap to second place, and the two
 * arms are set at the two distances that actually mean something on that
 * scale: `hard` at more than the gap to second, so the lead stops being a
 * formality, and `brutal` at more than the width of the entire field, so the
 * pack goes past.
 *
 * Both sit well inside the envelope DKR already drives this same expression
 * through. handle_racer_top_speed() turns the AI's rubber-band value
 * `racer->unk124` into `1 + unk124 * 0.025` with unk124 clamped to +/-20, so
 * the authored game itself moves opponent top speed across a 0.5x..1.5x band
 * during a normal race. A further 5% or 12% is a smaller step than the one the
 * rubber band takes between the front and the back of the field, which is what
 * keeps these arms "the opponents drive harder" rather than "the opponents are
 * in a different physics regime".
 */
#define MDKR_AI_DIFFICULTY_HARD_SCALE   1.05f
#define MDKR_AI_DIFFICULTY_BRUTAL_SCALE 1.12f

/*
 * Latched on first use. Enhancements.AIDifficulty is MDKR_VIDEO_SCOPE_RESTART,
 * so this is not a cache of a value that might move -- it is the scope. A race
 * that started under one arm finishes under it, and every racer in a given tick
 * is scaled by the same number.
 *
 * s_scale is EXACTLY 1.0f at the authored arm and every caller tests for that
 * literal, which is what lets the authored path be an early return rather than
 * a multiplication.
 */
static const char *s_arm = MDKR_AI_DIFFICULTY_AUTHORED;
static f32 s_scale = 1.0f;
static int s_resolved;

static int ai_difficulty_ci_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char) (ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char) (cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* One line when the arm resolves and one when the scale first reaches a racer,
 * so a test can tell "the setting was read" from "the setting had an effect"
 * without inferring either from the race result. Silent unless
 * MDKR_AIDIFF_TRACE=1, so a normal run's log is unchanged. */
static int ai_difficulty_trace_armed(void) {
    static int armed = -1;

    if (armed < 0) {
        const char *setting = getenv("MDKR_AIDIFF_TRACE");
        armed = (setting != NULL && setting[0] == '1') ? 1 : 0;
    }
    return armed;
}

static void ai_difficulty_resolve(void) {
    const MdkrVideoConfig *config;
    const char *value;

    if (s_resolved) {
        return;
    }
    s_resolved = 1;
    config = mdkr_video_config_current();
    value = config->values[MDKR_ENH_AI_DIFFICULTY].text;
    if (ai_difficulty_ci_equal(value, MDKR_AI_DIFFICULTY_HARD)) {
        s_arm = MDKR_AI_DIFFICULTY_HARD;
        s_scale = MDKR_AI_DIFFICULTY_HARD_SCALE;
    } else if (ai_difficulty_ci_equal(value, MDKR_AI_DIFFICULTY_BRUTAL)) {
        s_arm = MDKR_AI_DIFFICULTY_BRUTAL;
        s_scale = MDKR_AI_DIFFICULTY_BRUTAL_SCALE;
    } else {
        /* Includes the empty string and anything misspelled. The authored game
         * is the only safe reading of a value nobody recognises. */
        s_arm = MDKR_AI_DIFFICULTY_AUTHORED;
        s_scale = 1.0f;
    }
    if (ai_difficulty_trace_armed()) {
        printf("[AIDIFF] event=resolve arm=%s scale=%.6f configured=%s\n",
               s_arm, (double) s_scale, value != NULL ? value : "");
        fflush(stdout);
    }
}

const char *mdkr_enh_ai_difficulty_arm(void) {
    ai_difficulty_resolve();
    return s_arm;
}

f32 mdkr_enh_ai_difficulty_scale(void) {
    ai_difficulty_resolve();
    return s_scale;
}

f32 mdkr_enh_ai_difficulty_top_speed(f32 topSpeed, const Object_Racer *racer) {
    f32 scale = mdkr_enh_ai_difficulty_scale();

    /*
     * Four early returns, and the first one is the whole purity claim: at the
     * authored arm `topSpeed` is handed straight back, so the value the caller
     * already had is the value it keeps -- not a value that arithmetic is
     * argued to have left alone.
     */
    if (scale == 1.0f) {
        return topSpeed;
    }
    if (racer == NULL) {
        return topSpeed;
    }
    if (racer->playerIndex != PLAYER_COMPUTER) {
        return topSpeed;
    }
    /* A finished HUMAN kart is relabelled PLAYER_COMPUTER by
     * update_player_racer() so the results choreography can drive it. Scaling
     * it would hand the player the opponents' bonus for the run-out lap. */
    if (racer->raceFinished) {
        return topSpeed;
    }

    /* The first application that carries a real number. Before the lights go
     * out handle_racer_top_speed() returns 0 for everybody, and "0 * 1.08 = 0"
     * is not evidence that the scale reached the racing. */
    if (topSpeed != 0.0f && ai_difficulty_trace_armed()) {
        static int reported;
        if (!reported) {
            reported = 1;
            printf("[AIDIFF] event=apply arm=%s racer=%d in=%.6f out=%.6f\n",
                   s_arm, (int) racer->racerIndex, (double) topSpeed,
                   (double) (topSpeed * scale));
            fflush(stdout);
        }
    }
    return topSpeed * scale;
}
