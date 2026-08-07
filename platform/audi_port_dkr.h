#ifndef MDKR_AUDI_PORT_DKR_H
#define MDKR_AUDI_PORT_DKR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
