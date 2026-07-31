/*
 * Taj challenge lifecycle TEST DRIVER (native port only).
 *
 * MDKR_TAJ_OUTCOME=win|loss|abort selects a fixture arm. Win rejects the
 * carpet's final-lap event until the human naturally finishes, so production
 * assigns the human first. Loss contributes one carpet final-lap completion
 * event after the human has naturally reached lap two, so production assigns
 * the carpet first and the human second. Abort requests the production
 * CHALLENGE_END_QUIT path after a warmup.
 *
 * MDKR_TAJ_REQUEST=car|hover|plane is used only for already-completed replay
 * arms. It requests the same init_racer_for_challenge() acceptance seam after
 * the target vehicle has loaded in Timber's Island; first-time arms still use
 * the real balloon-threshold offer, Taj dialogue, and transformation sequence.
 *
 * MDKR_TAJ_BREAK_COMPLETION=car|hover|plane|all suppresses only the matching
 * first-completion award predicate. It cannot alter laps, finishing order,
 * dialogue teardown, flags, or EEPROM data.
 */

#ifdef NATIVE_PORT

#include <stdlib.h>
#include <string.h>

#include "mdkr_taj.h"
#include "mdkr_trace.h"

enum {
    MDKR_TAJ_OUTCOME_NONE = 0,
    MDKR_TAJ_OUTCOME_WIN = 1,
    MDKR_TAJ_OUTCOME_LOSS = -1,
    MDKR_TAJ_OUTCOME_ABORT = 2,
    MDKR_TAJ_ABORT_TICKS = 300,
    MDKR_TAJ_PROBE_INTERVAL = 120,
};

static int s_initialized;
static int s_probe;
static int s_outcome;
static int s_requested_vehicle = -1;
static int s_ticks;
static int s_loss_event_sent;
static int s_win_hold_sent;
static const char *s_break_completion;

static int mdkr_taj_parse_vehicle(const char *value) {
    if (value == NULL) {
        return -1;
    }
    if (!strcmp(value, "car")) {
        return 0;
    }
    if (!strcmp(value, "hover")) {
        return 1;
    }
    if (!strcmp(value, "plane")) {
        return 2;
    }
    return -1;
}

static void mdkr_taj_init(void) {
    const char *value;

    if (s_initialized) {
        return;
    }
    s_initialized = 1;
    value = getenv("MDKR_TAJ_PROBE");
    s_probe = value != NULL && value[0] != '\0' && value[0] != '0';
    value = getenv("MDKR_TAJ_OUTCOME");
    if (value != NULL && !strcmp(value, "win")) {
        s_outcome = MDKR_TAJ_OUTCOME_WIN;
    } else if (value != NULL && !strcmp(value, "loss")) {
        s_outcome = MDKR_TAJ_OUTCOME_LOSS;
    } else if (value != NULL && !strcmp(value, "abort")) {
        s_outcome = MDKR_TAJ_OUTCOME_ABORT;
    }
    s_requested_vehicle = mdkr_taj_parse_vehicle(getenv("MDKR_TAJ_REQUEST"));
    s_break_completion = getenv("MDKR_TAJ_BREAK_COMPLETION");
}

int mdkr_taj_requested_vehicle(void) {
    mdkr_taj_init();
    return s_requested_vehicle;
}

int mdkr_taj_test_action(
    int active, int human_lap, int human_finished, int ai_lap,
    int ai_finished,
    int *tick) {
    mdkr_taj_init();
    if (!active) {
        if (tick != NULL) {
            *tick = s_ticks;
        }
        return MDKR_TAJ_ACTION_NONE;
    }
    s_ticks++;
    if (tick != NULL) {
        *tick = s_ticks;
    }
    if (s_outcome == MDKR_TAJ_OUTCOME_ABORT &&
        s_ticks == MDKR_TAJ_ABORT_TICKS) {
        return MDKR_TAJ_ACTION_ABORT;
    }
    if (s_outcome == MDKR_TAJ_OUTCOME_LOSS && !s_loss_event_sent &&
        human_lap >= 2 && !human_finished && !ai_finished) {
        s_loss_event_sent = 1;
        return MDKR_TAJ_ACTION_AI_FINISH;
    }
    if (s_outcome == MDKR_TAJ_OUTCOME_WIN && !human_finished &&
        !ai_finished && ai_lap >= 3) {
        if (!s_win_hold_sent) {
            s_win_hold_sent = 1;
            return MDKR_TAJ_ACTION_AI_HOLD_FIRST;
        }
        return MDKR_TAJ_ACTION_AI_HOLD;
    }
    return MDKR_TAJ_ACTION_NONE;
}

int mdkr_taj_completion_gate_enabled(int vehicle) {
    int broken;

    mdkr_taj_init();
    if (s_break_completion == NULL || s_break_completion[0] == '\0') {
        return 1;
    }
    if (!strcmp(s_break_completion, "all")) {
        return 0;
    }
    broken = mdkr_taj_parse_vehicle(s_break_completion);
    return broken != vehicle;
}

int mdkr_taj_probe_enabled(void) {
    mdkr_taj_init();
    return s_probe && mdkr_trace_enabled();
}

int mdkr_taj_should_probe(int tick) {
    return mdkr_taj_probe_enabled() &&
           (tick == 1 || (tick % MDKR_TAJ_PROBE_INTERVAL) == 0);
}

void mdkr_taj_probe_state(const char *phase, const MdkrTajProbe *p) {
    if (!mdkr_taj_probe_enabled() || phase == NULL || p == NULL) {
        return;
    }
    mdkr_trace(
        "[TAJ] phase=%s vehicle=%d flags=0x%x balloons=%d "
        "thresholds=%d,%d,%d racers=%d tick=%d "
        "human=%d,%d,%d,%.2f,%.2f,%.2f "
        "ai=%d,%d,%d,%.2f,%.2f,%.2f "
        "reason=%d menu=%d gate=%d",
        phase, p->vehicle, p->flags, p->balloons,
        p->thresholds[0], p->thresholds[1], p->thresholds[2],
        p->racer_count, p->tick,
        p->human_lap, p->human_finished, p->human_place,
        p->human_x, p->human_y, p->human_z,
        p->ai_lap, p->ai_finished, p->ai_place,
        p->ai_x, p->ai_y, p->ai_z,
        p->reason, p->menu, p->completion_gate);
}

#endif
