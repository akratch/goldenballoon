#ifndef MDKR_AUDI_PORT_DKR_H
#define MDKR_AUDI_PORT_DKR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Two delivery constants published here because they are load-bearing OUTSIDE
 * the translation unit that owns each of them.
 *
 * tests/test_audio_resilience.c models the whole native delivery loop — the
 * real controller and the real ring against a modelled device — and its
 * conclusions ("the current design does not starve") only transfer to the
 * shipped port if it is sized like the shipped port. It used to copy both
 * numbers as bare literals with a comment naming the file they came from,
 * which is a comment, not a constraint: retuning either constant would leave
 * the model asserting things about a configuration nobody ships.
 *
 * Each is _Static_assert-ed against its owning definition at that definition's
 * site, so a divergence is a build failure rather than a stale test.
 */
#define MDKR_AUDIO_RING_FRAMES       65536u  /* audi_port_dkr.c ring capacity  */
#define MDKR_AUDIO_MAX_FRAME_SAMPLES 1792u   /* audiomgr.c per-call synth cap  */

void dkr_audio_out_init(void);
void dkr_audio_out_shutdown(void);

/* Fixed-ticket clock input and ordered post-game-tick service are deliberately
 * split. Presentation count cannot change the amount of credited audio time. */
void dkr_audio_advance_fields(unsigned fields, bool rebase);
void dkr_audio_service_tick(void);
void dkr_audio_service_summary(void);

/*
 * TEST-ONLY main-loop hitch injector, called at the tick boundary immediately
 * before the audio service runs.
 *
 * MDKR_TEST_MAINLOOP_STALL_NS=<n> busy-spins the main loop for <n> nanoseconds,
 * which is what a frametime hitch (shader compile, level load, GC-equivalent,
 * a slow present) does to audio delivery: it delays the next refill by that
 * much. MDKR_TEST_MAINLOOP_STALL_EVERY=<k> repeats the stall every k-th tick
 * (default 1, i.e. every tick); MDKR_TEST_MAINLOOP_STALL_AFTER=<t> skips the
 * first t ticks so boot is not perturbed.
 *
 * It busy-spins rather than sleeping on purpose: a real hitch keeps the CPU
 * busy and does not yield, so a sleeping stub would let the OS scheduler make
 * the host look healthier than it is. Nothing here touches gameplay state,
 * simulation order, or synthesis input -- only host wall time moves.
 */
void dkr_audio_test_mainloop_stall(void);
/* Diagnostic timestamp for audio/pause gates; zero when no measured output has
 * been produced. This never drives synthesis or gameplay. */
unsigned long long dkr_audio_output_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDI_PORT_DKR_H */
