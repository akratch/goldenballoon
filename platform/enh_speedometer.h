/* enh_speedometer.h — the optional on-screen speed readout.
 *
 * Declared MDKR_ENH_PRESENTATION in platform/enhancement_registry.c, and that
 * declaration is the whole design constraint: this module READS the racer's
 * velocity and writes nothing the simulation will ever read back. It keeps no
 * smoothing accumulator, no peak hold and no "last shown" value that a later
 * tick consults, because any of those would be state the readout owns, and
 * state the readout owns is state a save/load or a rewind would have to carry.
 * Everything here is recomputed from the live object every time it is asked.
 *
 * WHERE THE NUMBER COMES FROM. Exactly where the authored dial gets it:
 * hud_speedometre() in game/src/game_ui.c takes the racer object's world
 * velocity — horizontal components only for ground vehicles, all three for the
 * plane — and drives the needle with four times that magnitude, saturating at
 * 100. The dial's printed face runs 0..150. Reading the same quantity against
 * the same face is therefore 1.5 face units per unit of needle input, so the
 * number and the needle beside it can never disagree. Kilometres per hour is
 * the exact international mile, 1.609344.
 */
#ifndef MDKR64_ENH_SPEEDOMETER_H
#define MDKR64_ENH_SPEEDOMETER_H

#include <stddef.h>

#include "structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The values Enhancements.Speedometer takes; see platform/video_config.c. */
enum {
    MDKR_SPEEDOMETER_OFF = 0,
    MDKR_SPEEDOMETER_MPH = 1,
    MDKR_SPEEDOMETER_KMH = 2
};

/* Widest string mdkr_enh_speedometer_text() can write, terminator included:
 * three digits, a space, a three-letter unit. */
#define MDKR_SPEEDOMETER_TEXT_MAX 16

/* The configured mode, clamped to the enum above. */
s32 mdkr_enh_speedometer_mode(void);

/* Player one's ground speed in world units per authoritative tick, or 0 when
 * there is no racer to read. Pure: it takes no argument and mutates nothing,
 * so forcing it to a constant is a complete positive control for every
 * assertion that depends on the readout tracking the car. */
f32 mdkr_enh_speedometer_sample(void);

/* The whole number the readout shows for `sample` in `mode`; 0 when the mode
 * shows nothing. */
s32 mdkr_enh_speedometer_reading(s32 mode, f32 sample);

/* "MPH", "KPH", or "" when the mode shows nothing. */
const char *mdkr_enh_speedometer_unit(s32 mode);

/* Writes the drawn string into `out`. Returns 0 (and empties `out`) when the
 * mode shows nothing or the buffer is too small to hold the whole reading — a
 * truncated speed is worse than no speed. */
s32 mdkr_enh_speedometer_text(char *out, size_t capacity, s32 mode, s32 reading);

/* Append the readout to `displayList` using the game's own font path, so the
 * glyphs pick up the same high-resolution reconstruction (gfx_font_sdf.c) the
 * rest of the interface gets. A no-op when the enhancement is off. */
void mdkr_enh_speedometer_draw(Gfx **displayList);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENH_SPEEDOMETER_H */
