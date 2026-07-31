#ifndef MDKR64_MDKR_CHALLENGE_H
#define MDKR64_MDKR_CHALLENGE_H

/*
 * Native-only challenge-mode regression seam.
 *
 * The driver emits gameplay events; production code remains responsible for
 * deciding whether those events finish a racer, assigning every place, choosing
 * win/loss, awarding progression, and requesting a save.
 */
typedef struct MdkrChallengeRacerProbe {
    int player_index;
    int racer_index;
    float x;
    float y;
    float z;
    int score;
    int health;
    int egg_count;
    int finished;
    int finish_position;
} MdkrChallengeRacerProbe;

typedef struct MdkrChallengeProbe {
    int course;
    int race_type;
    int tracks_mode;
    int racer_count;
    unsigned int course_flags;
    int tt_amulet;
    int tick;
    MdkrChallengeRacerProbe racers[4];
} MdkrChallengeProbe;

/* Returns -1 for a requested human loss, +1 for a requested human win. */
int mdkr_challenge_test_outcome(void);

/*
 * Returns nonzero when the production caller should deliver one terminal
 * gameplay event. event_ordinal starts at zero and identifies which battle AI
 * should receive the event.
 */
int mdkr_challenge_test_event(
    int race_type, int race_ready, int finish_triggered,
    int *event_ordinal);

/* Positive-control switch: false only for the requested production gate. */
int mdkr_challenge_terminal_gate_enabled(int race_type);

void mdkr_challenge_probe_state(
    const char *phase, const MdkrChallengeProbe *probe);
void mdkr_challenge_probe_event(
    const MdkrChallengeProbe *probe, int outcome, int ordinal);

#endif
