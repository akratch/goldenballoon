#ifndef GFX_FRAME_WAIT_H
#define GFX_FRAME_WAIT_H

/*
 * Frame-wait attribution, the second half of the [TEXPACK-FRAME] row
 * (gfx_pc_dkr.c). Its own header because the WebGPU backend cannot include
 * gfx_pc_dkr.h (the F3DDKR headers redefine its GfxDimensions).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Armed by MDKR_TEXTURE_PACK_TIMING=1 only; the census alone does not arm it.
 * A frame that took 50 ms while its thread ran for 2 ms waited on something;
 * each blocking point of the real-time frame loop is bracketed so the row
 * says which. begin() is one cached flag test and returns 0 when the rows
 * are off; end() and note_pace() ignore a 0 start, so a site costs a flag
 * test and reads no clock in an ordinary run.
 *
 * Inside GFX_DKR_WAIT_GAME (the game pass: host boundary exit to the next
 * entry) sit START, ACQUIRE, SUBMIT and PRESENT, and the walk/end-of-frame
 * terms the row already had. The rest are the host boundary's own.
 *
 * With presentation smoothing on, the presentation subloop replays the held
 * display list from INSIDE the host boundary. That whole replay is REPLAY;
 * its walk is also in the row's walk total (reported again as replayWalkUs,
 * so the game pass's own walk is walk minus that), and its acquire, submit
 * and present stay inside REPLAY rather than in their own buckets. So the
 * parts still tile the frame: game + REPLAY + the boundary waits.
 */
typedef enum GfxDkrWait {
    GFX_DKR_WAIT_GAME = 0,  /* game pass, boundary exit -> next entry */
    GFX_DKR_WAIT_PACE,      /* the pacer's sleep (pace_sleep_until) */
    GFX_DKR_WAIT_EVENTS,    /* SDL event pump and late input sample */
    GFX_DKR_WAIT_SWAP,      /* SDL_GL_SwapWindow */
    GFX_DKR_WAIT_FENCE,     /* GL frame-queue fence wait after the swap */
    GFX_DKR_WAIT_AUDIO,     /* audio time advance + service (synth, sink) */
    GFX_DKR_WAIT_START,     /* gfx_start_frame: backend admission, frame open */
    GFX_DKR_WAIT_ACQUIRE,   /* WebGPU surface acquire, occlusion retries too */
    GFX_DKR_WAIT_SUBMIT,    /* WebGPU queue submit */
    GFX_DKR_WAIT_PRESENT,   /* WebGPU surface present */
    GFX_DKR_WAIT_REPLAY,    /* a presentation replay walk, whole */
    GFX_DKR_WAIT_COUNT
} GfxDkrWait;

uint64_t gfx_dkr_wait_begin(void);
void gfx_dkr_wait_end(GfxDkrWait which, uint64_t start);
/* One pacer sleep that began at `start` (a gfx_dkr_wait_begin value): what it
 * was asked to sleep and how late past its target it woke, both in the
 * pacer's own clock. */
void gfx_dkr_wait_note_pace(uint64_t start, uint64_t requested_ns,
                            uint64_t late_ns);

#ifdef __cplusplus
}
#endif

#endif /* GFX_FRAME_WAIT_H */
