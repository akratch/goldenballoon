#ifndef MDKR64_MDKR_TAJ_H
#define MDKR64_MDKR_TAJ_H

enum MdkrTajAction {
    MDKR_TAJ_ACTION_NONE = 0,
    MDKR_TAJ_ACTION_AI_FINISH = 1,
    MDKR_TAJ_ACTION_ABORT = 2,
    MDKR_TAJ_ACTION_AI_HOLD = 3,
    MDKR_TAJ_ACTION_AI_HOLD_FIRST = 4,
};

typedef struct MdkrTajProbe {
    int vehicle;
    unsigned int flags;
    int balloons;
    int thresholds[3];
    int racer_count;
    int tick;
    int human_lap;
    int human_finished;
    int human_place;
    float human_x;
    float human_y;
    float human_z;
    int ai_lap;
    int ai_finished;
    int ai_place;
    float ai_x;
    float ai_y;
    float ai_z;
    int reason;
    int menu;
    int completion_gate;
} MdkrTajProbe;

/* Replay-only acceptance seam. -1 means no request; 0..2 are player vehicles. */
int mdkr_taj_requested_vehicle(void);

/*
 * Advances the native fixture clock and returns the one narrow external event
 * requested by MDKR_TAJ_OUTCOME. A win rejects only a carpet final-lap event;
 * a loss completes only the carpet's final lap; an abort invokes the same
 * production teardown as the pause-menu quit action.
 */
int mdkr_taj_test_action(
    int active, int human_lap, int human_finished, int ai_lap,
    int ai_finished,
    int *tick);

/* Positive-control gate for the first-completion persistence predicate. */
int mdkr_taj_completion_gate_enabled(int vehicle);

int mdkr_taj_probe_enabled(void);
int mdkr_taj_should_probe(int tick);
void mdkr_taj_probe_state(const char *phase, const MdkrTajProbe *probe);

#endif
