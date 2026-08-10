/* a11y_race.h — the race, said out loud.
 *
 * A blind player cannot read the position counter, the lap counter or the
 * finish board, and those three are most of what a race tells you about itself.
 * This layer turns them into text through platform/a11y_model.c, so the same
 * `[SPEAK]` stream the shell produces also carries what is happening on track.
 *
 * WHERE THE EVENTS COME FROM. Nothing here observes the race directly. The port
 * already publishes an ordered gameplay-event stream
 * (platform/gameplay_event_trace.h) and already latches the port-1 human
 * racer's own state once per authoritative tick from race_check_finish(), which
 * is the function that decides position, lap and finish in the first place.
 * This layer subscribes to the first and reads the second. Adding a second set
 * of observation points inside game/src would mean two places that could
 * disagree about what a race is doing, and the announcements would be the ones
 * that were wrong.
 *
 * WHY IT CANNOT MOVE THE GAME. It reads published scalars and writes text. No
 * call into the race, no RNG, no allocation, no state the simulation can see —
 * which is what lets tests/check_a11y_race.py assert that the authoritative
 * `[SIMHASH]` v3 stream is byte-identical with announcements on and off.
 *
 * COALESCING. Positions swap constantly in a tight pack, and a voice that
 * restarts every time is worse than silence: each new line cancels the one
 * being spoken, so the player hears fragments and learns nothing. Position
 * announcements therefore have a minimum spacing; laps, the final lap, the
 * start, the finish and item pickups do not, because those happen once each.
 */
#ifndef MDKR64_A11Y_RACE_H
#define MDKR64_A11Y_RACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimum authoritative ticks between two spoken position changes.
 *
 * "Position three of eight" is seven syllables: about a second and a half at
 * the default speech rate, and the model treats any newer utterance as barging
 * in on the one in flight. Sixty ticks is two seconds at the game's thirty-hertz
 * update, so the line has time to finish before the next one can start. Lower
 * and a mid-pack scrap produces an unbroken stream of half-spoken numbers;
 * much higher and the player stops learning that they are being overtaken.
 */
#define MDKR_A11Y_RACE_POSITION_MIN_TICKS 60

/* Install the observer and read the enable state. Safe to call more than once;
 * safe to call in a build that never starts a race. */
void mdkr_a11y_race_init(void);

/* Master switch, from Accessibility.Speech + Accessibility.SpeechRace. */
bool mdkr_a11y_race_enabled(void);

/*
 * Per-category switches, so a player can keep the lap calls and drop the
 * position calls (or the reverse) instead of choosing between all of it and
 * none of it. `name` is one of "position", "lap", "event".
 */
bool mdkr_a11y_race_set_category(const char *name, bool enabled);

/*
 * The port-1 human racer's authoritative race state for this tick, published
 * from the existing race_check_finish() probe. Everything is a plain scalar the
 * game already computed; this never reaches back into the race.
 */
void mdkr_a11y_race_publish(int racePosition, int racerCount,
                            int lap, int lapCount,
                            int raceFinished, int finishPosition,
                            int itemQuantity, int itemType);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_A11Y_RACE_H */
