/*
 * Private interface shared by the clean-room native audio engine's translation
 * units: audio_compat.c, audio_bank.c, audio_sequence.c, audio_envmix.c,
 * audio_fx.c, audio_filters.c, audio_synth.c, audio_seqplayer.c and
 * audio_cspplayer.c.
 *
 * This is not a public interface. Game code keeps calling the libaudio ABI in
 * <libaudio.h>; what lives here is only what splitting one 5,674-line
 * translation unit left crossing a file boundary -- the include prologue every
 * part needs, the three helpers that used to be file-local, and DKR's
 * channel-state cast.
 */
#ifndef MDKR_AUDIO_COMPAT_INTERNAL_H
#define MDKR_AUDIO_COMPAT_INTERNAL_H

#include <libaudio.h>
#include "synthInternals.h"
#include "seqp.h"
#include "audio_event_queue.h"
#include "audio_fx_transfer.h"

#include <limits.h>
#include <math.h>

#ifdef TARGET_N64
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

extern void *calloc(size_t count, size_t size);
extern void free(void *ptr);
extern void *memcpy(void *dest, const void *src, size_t count);
extern void *memset(void *dest, int value, size_t count);
#else
#include "fs_utf8.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Defined in audio_compat.c: the reverb master switch alFxPull() gates on, and
 * the stereo-mode fold the two pan entry points apply. */
extern u8 alFXEnabled;
s32 modify_panning(s32 pan);

/* Defined in audio_bank.c: the big-endian ctl reader alCSeqNew() also needs for
 * a non-native sequence header, and the exp2 the cents-to-ratio helper uses. */
u32 bank_ctl_u32(const u8 *data);
f32 audio_exp2f(f32 exponent);

/* Defined in audio_seqplayer.c: the CSP player reposts deferred note events
 * through the same queue path the shared player uses. */
void native_seqp_repost_event_item(ALEventQueue *evtq, ALEventListItem *item);

/* Diagnostic events share the existing MIDI JSONL sink. They are inert unless
 * that established oracle path is requested. */
void native_csp_trace_physical_voice(const char *event, s16 old_priority,
                                     s16 new_priority);

/* Defined in audio_sequence.c: the CSP player peeks the next event's delta
 * without consuming it. Declared in game/libultra/src/audio/cseq.h too, which
 * is not on the port's include path. */
char __alCSeqNextDelta(ALCSeq *seq, s32 *delta_ticks);


/*
 * DKR channel state.
 *
 * DKR's ALCSPlayer_Custom is layout-identical to the stock ALCSPlayer except
 * that chanState points at ALChanState_Custom, which extends ALChanState with
 * the per-channel `fade` (CC8) and `unk11` (raw CC7) bytes the DKR sequences
 * drive. The extra fields sit AFTER every stock field, so the common members
 * keep their offsets and only the array stride grows — which is why every
 * access goes through this cast and why alCSPNew sizes the array with
 * sizeof(ALChanState_Custom).
 */
#define CSP_CHAN(p) (((ALCSPlayer_Custom *)(p))->chanState)

#endif /* MDKR_AUDIO_COMPAT_INTERNAL_H */
