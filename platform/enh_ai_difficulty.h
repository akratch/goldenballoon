/* enh_ai_difficulty.h — how hard the opponents race.
 *
 * This is the sprint's only MDKR_ENH_GAMEPLAY enhancement (see
 * platform/enhancement_registry.c), and that class is the design constraint
 * turned inside out: this setting is ALLOWED to move the authoritative
 * [SIMHASH] v3 stream, and check_enhancement_authority.py asserts that it
 * does. What it is not allowed to do is move that stream at its default.
 *
 * WHY `authored` IS A RETURN AND NOT A MULTIPLY. The default arm must be the
 * authored game bit for bit, and "x * 1.0f" is a claim about arithmetic rather
 * than a statement that nothing happened. It is not even a true claim
 * everywhere: multiplying a signalling NaN by 1.0f quiets it, and a
 * flush-to-zero mode turns a denormal product into a zero the original never
 * produced. So mdkr_enh_ai_difficulty_top_speed() returns its argument, by an
 * early return taken before any float touches an ALU, whenever the arm is
 * `authored`. tests/check_enh_ai_difficulty.py proves the consequence rather
 * than trusting the argument: it builds a binary with the call site compiled
 * out entirely (-DMDKR_ENH_AI_DIFFICULTY_OMIT) and requires the two [SIMHASH]
 * streams to be byte-identical.
 *
 * WHERE THE SCALE IS APPLIED, AND WHY THERE. handle_racer_top_speed()
 * (game/src/racer.c) is the one function that answers "how fast may this racer
 * go", for every vehicle and every caller — the car, hovercraft and plane
 * updates all multiply by it, and its own comment says it is what you change
 * to change a vehicle's baseline speed. It is also where DKR's OWN rubber band
 * lands: for a CPU racer the function reads `racer->unk124`, a per-tick value
 * func_80042D20() recomputes from the AI behaviour table and the racer's place
 * in the field, and turns it into `1 + unk124 * 0.025`. So the authored game
 * already varies opponent top speed by a wide factor through exactly this
 * expression; this enhancement scales the result of it and nothing else.
 *
 * WHO IT APPLIES TO. Opponents only, and the module — not the call site —
 * decides that, so the rule is in one place and is stated once:
 *
 *   - `playerIndex != PLAYER_COMPUTER` is a human and is never scaled.
 *   - a racer that has already finished is never scaled either, because
 *     update_player_racer() permanently relabels a finished HUMAN kart
 *     PLAYER_COMPUTER to hand it to the results choreography (racer.c). Without
 *     that second test a player who crossed the line first would be handed the
 *     opponents' speed bonus for the run-out lap.
 */
#ifndef MDKR64_ENH_AI_DIFFICULTY_H
#define MDKR64_ENH_AI_DIFFICULTY_H

#include "structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The values Enhancements.AIDifficulty takes; see platform/video_config.c.
 * Anything else — including an empty or misspelled value — resolves to
 * `authored`, because the failure mode of a typo in a config file must be the
 * authored game and not an unannounced difficulty change. */
#define MDKR_AI_DIFFICULTY_AUTHORED "authored"
#define MDKR_AI_DIFFICULTY_HARD     "hard"
#define MDKR_AI_DIFFICULTY_BRUTAL   "brutal"

/* The arm actually in force, as one of the three strings above. Latched on
 * first use: the schema declares this key MDKR_VIDEO_SCOPE_RESTART, so a race
 * that started under one arm must finish under it. */
const char *mdkr_enh_ai_difficulty_arm(void);

/* The multiplier on an opponent's authored top speed. EXACTLY 1.0f for
 * `authored`, and callers test for that literal rather than for "the setting
 * is the default string", so a future source for the arm cannot leave the
 * authored path behind. */
f32 mdkr_enh_ai_difficulty_scale(void);

/* `topSpeed` is whatever handle_racer_top_speed() had computed for `racer`.
 * Returns it UNCHANGED — by early return, not by arithmetic — at the authored
 * arm, for a NULL racer, for a human, and for a racer that has finished.
 * Otherwise returns it scaled. */
f32 mdkr_enh_ai_difficulty_top_speed(f32 topSpeed, const Object_Racer *racer);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENH_AI_DIFFICULTY_H */
