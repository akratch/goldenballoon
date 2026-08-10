/*
 * Input latency budget census (campaign M4).
 *
 * Measures where the wall-clock time between a host input event and the frame
 * that reflects it actually goes, so the decision to optimise any one term is
 * made against numbers rather than intuition. Armed by MDKR_INPUT_LATENCY=1;
 * every entry point is a no-op otherwise, and no timestamp is taken.
 *
 * The four terms, in the order a button press travels through them:
 *
 *   queue    SDL event timestamp -> the pump that dispatched it. How long the
 *            press sat in SDL's own event queue. Millisecond resolution, which
 *            is SDL2's, and reported as such.
 *   sample   the last host capture that fed a ticket -> the commit that made
 *            that ticket DKR-visible. This is dead time: the sample is taken
 *            and then waits. It is the term just-in-time sampling removes, and
 *            structurally it is one PRESENT interval -- a whole authored tick
 *            when the presentation rate equals the tick rate.
 *   tick     commit -> commit. The authored quantum. Irreducible without
 *            changing the simulation cadence, which this port does not do.
 *   present  commit -> the swap that returns for the frame that ran on that
 *            input. Simulation, display-list build, GPU submit and the block
 *            inside the swap. Scanout beyond the swap is not observable from
 *            inside the process; bound it by hand from the backend's own
 *            [PRESENT-MODE] frameLatency row plus one refresh.
 */
#ifndef MDKR_INPUT_LATENCY_CENSUS_H
#define MDKR_INPUT_LATENCY_CENSUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool input_latency_census_enabled(void);

/* One dispatched host input event. `queued_ms` is (now - event timestamp) in
 * SDL's millisecond clock. */
void input_latency_census_note_event(unsigned queued_ms);

/* A complete host capture landed in the tick queue at `now_ns`. */
void input_latency_census_note_capture(uint64_t now_ns);

/* One authoritative ticket became DKR-visible at `now_ns`. */
void input_latency_census_note_commit(uint64_t now_ns);

/* A swap returned at `now_ns`. Only the first swap after a commit is scored:
 * it is the one carrying that ticket's input. */
void input_latency_census_note_present(uint64_t now_ns);

/* Static pipeline configuration, reported with the budget so a reader can tell
 * which pacing produced it. The presentation queue depth is not repeated here:
 * each backend already publishes its own on the [PRESENT-MODE] row, and one
 * number owned in two places drifts. Safe to call before or after any sample;
 * the last call wins. */
void input_latency_census_note_config(
    const char *present_policy, unsigned present_rate, unsigned tick_fields,
    int swap_interval, bool smoothing, bool jit);

void input_latency_census_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_INPUT_LATENCY_CENSUS_H */
