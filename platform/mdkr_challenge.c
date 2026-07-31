/*
 * Challenge/battle end-to-end TEST DRIVER (native port only).
 *
 * MDKR_CHALLENGE_OUTCOME=win|loss asks the production challenge code to receive
 * a short sequence of the same terminal gameplay events it normally receives:
 *
 *   eggs     one completed egg hatch (three events finish);
 *   bananas  one deposited treasure banana (ten events finish);
 *   battle   one point of racer health lost (zero health eliminates).
 *
 * This file never writes raceFinished, finishPosition, courseFlagsPtr,
 * ttAmulet, a post-race stage, or save data. Those remain production decisions.
 *
 * MDKR_CHALLENGE_BREAK_GATE=eggs|bananas|battle|all is the positive control.
 * It suppresses only the corresponding final predicate. The event sequence
 * still evolves the mode score to its terminal value, but no result/progression
 * may occur; tests/check_challenge_modes.py requires that arm to stay unfinished.
 */

#ifdef NATIVE_PORT

#include <stdlib.h>
#include <string.h>

#include "enums.h"
#include "mdkr_challenge.h"
#include "mdkr_trace.h"

enum {
    MDKR_CHALLENGE_WARMUP_TICKS = 120,
    MDKR_CHALLENGE_EVENT_INTERVAL = 12,
};

static int s_initialized;
static int s_outcome;
static int s_ticks;
static int s_events;
static const char *s_break_gate;

static void mdkr_challenge_init(void) {
    const char *value;

    if (s_initialized) {
        return;
    }
    s_initialized = 1;
    value = getenv("MDKR_CHALLENGE_OUTCOME");
    if (value != NULL && !strcmp(value, "win")) {
        s_outcome = 1;
    } else if (value != NULL && !strcmp(value, "loss")) {
        s_outcome = -1;
    }
    s_break_gate = getenv("MDKR_CHALLENGE_BREAK_GATE");
}

int mdkr_challenge_test_outcome(void) {
    mdkr_challenge_init();
    return s_outcome;
}

static int mdkr_challenge_event_limit(int race_type) {
    if (race_type == RACETYPE_CHALLENGE_EGGS) {
        return 3;
    }
    if (race_type == RACETYPE_CHALLENGE_BANANAS) {
        return 10;
    }
    if (race_type == RACETYPE_CHALLENGE_BATTLE) {
        return s_outcome > 0 ? 24 : 8;
    }
    return 0;
}

int mdkr_challenge_test_event(
    int race_type, int race_ready, int finish_triggered,
    int *event_ordinal) {
    int limit;

    mdkr_challenge_init();
    if (event_ordinal != NULL) {
        *event_ordinal = -1;
    }
    if (s_outcome == 0 || !race_ready || finish_triggered) {
        return 0;
    }
    limit = mdkr_challenge_event_limit(race_type);
    if (limit == 0 || s_events >= limit) {
        return 0;
    }
    s_ticks++;
    if (s_ticks < MDKR_CHALLENGE_WARMUP_TICKS ||
        ((s_ticks - MDKR_CHALLENGE_WARMUP_TICKS) %
         MDKR_CHALLENGE_EVENT_INTERVAL) != 0) {
        return 0;
    }
    if (event_ordinal != NULL) {
        *event_ordinal = s_events;
    }
    s_events++;
    return 1;
}

int mdkr_challenge_terminal_gate_enabled(int race_type) {
    mdkr_challenge_init();
    if (s_break_gate == NULL || s_break_gate[0] == '\0') {
        return 1;
    }
    if (!strcmp(s_break_gate, "all")) {
        return 0;
    }
    if (race_type == RACETYPE_CHALLENGE_EGGS &&
        !strcmp(s_break_gate, "eggs")) {
        return 0;
    }
    if (race_type == RACETYPE_CHALLENGE_BANANAS &&
        !strcmp(s_break_gate, "bananas")) {
        return 0;
    }
    if (race_type == RACETYPE_CHALLENGE_BATTLE &&
        !strcmp(s_break_gate, "battle")) {
        return 0;
    }
    return 1;
}

void mdkr_challenge_probe_state(
    const char *phase, const MdkrChallengeProbe *p) {
    const MdkrChallengeRacerProbe empty = { 0 };
    const MdkrChallengeRacerProbe *r0;
    const MdkrChallengeRacerProbe *r1;
    const MdkrChallengeRacerProbe *r2;
    const MdkrChallengeRacerProbe *r3;

    if (!mdkr_trace_enabled() || phase == NULL || p == NULL) {
        return;
    }
    r0 = p->racer_count > 0 ? &p->racers[0] : &empty;
    r1 = p->racer_count > 1 ? &p->racers[1] : &empty;
    r2 = p->racer_count > 2 ? &p->racers[2] : &empty;
    r3 = p->racer_count > 3 ? &p->racers[3] : &empty;
    mdkr_trace(
        "[CHALLENGE] phase=%s course=%d type=%d tracks=%d racers=%d "
        "flags=0x%x tt=%d tick=%d "
        "r0=%d,%d,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d "
        "r1=%d,%d,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d "
        "r2=%d,%d,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d "
        "r3=%d,%d,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d",
        phase, p->course, p->race_type, p->tracks_mode, p->racer_count,
        p->course_flags, p->tt_amulet, p->tick,
        r0->player_index, r0->racer_index, r0->x, r0->y, r0->z,
        r0->score, r0->health, r0->egg_count, r0->finished,
        r0->finish_position,
        r1->player_index, r1->racer_index, r1->x, r1->y, r1->z,
        r1->score, r1->health, r1->egg_count, r1->finished,
        r1->finish_position,
        r2->player_index, r2->racer_index, r2->x, r2->y, r2->z,
        r2->score, r2->health, r2->egg_count, r2->finished,
        r2->finish_position,
        r3->player_index, r3->racer_index, r3->x, r3->y, r3->z,
        r3->score, r3->health, r3->egg_count, r3->finished,
        r3->finish_position);
}

void mdkr_challenge_probe_event(
    const MdkrChallengeProbe *probe, int outcome, int ordinal) {
    if (mdkr_trace_enabled() && probe != NULL) {
        mdkr_trace(
            "[CHALLENGE] phase=event course=%d type=%d outcome=%s "
            "ordinal=%d tick=%d",
            probe->course, probe->race_type,
            outcome > 0 ? "win" : "loss", ordinal, probe->tick);
    }
}

#endif
