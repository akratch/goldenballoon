/**
 * mixer.h — RSP audio microcode emulator interface for the PC port.
 *
 * On N64, the Acmd macros in abi.h build command packets that are sent
 * to the RSP for hardware execution.  On PC, we replace those macros
 * with direct calls to C implementations that emulate the RSP audio
 * microcode in software.  This lets the entire libultra audio synthesis
 * chain (synthesizer → filters → envmixer → resampler → ADPCM decoder)
 * run unchanged, producing correct audio output.
 *
 * Adapted from fgsfdsfgs/perfect_dark port/src/mixer.c, adjusted for
 * GoldenEye's old libaudio API (aSetBuffer-based calling pattern).
 */

#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>

/* State types from abi.h (already typedef'd before we get here) */

/* ===== LP64 address reconstruction (mdkr64 / DKR native port) =====
 *
 * mgb64 feeds the mixer REAL host pointers because its audio path is a full
 * reimplementation (audio_compat.c). DKR keeps the STOCK libultra synthesizer,
 * whose whole address ABI is 32-bit: every buffer/state/sample address reaches
 * the a* macros as a 32-bit "physical" value —
 *   - osVirtualToPhysical(ptr)     -> u32   (env.c/resample.c/reverb.c)
 *   - the ALDMAproc return          -> s32   (audiomgr.c __amDMA)
 *   - ALSave.dramout                -> s32   (save.c: `(s32)param`)
 * On LP64 those truncate the 64-bit host pointer (and the s32 ones sign-extend).
 * The mixer can't memcpy through a truncated pointer.
 *
 * Fix (matches the arena reconstruction already proven for the F3DDKR gfx path):
 * ALL audio memory is arena-resident (the audio heap is arena-backed, banks are
 * parsed into the arena, output/cmd buffers come from the arena — see audio.c /
 * bnkf.c / audiomgr.c), and the arena is size-aligned so every arena pointer
 * shares one high-32 value. So the low 32 bits uniquely identify the pointer:
 * reconstruct with dkr_lo32_to_ptr(). This is exact for a truncated token AND
 * for a real arena pointer passed straight through (its low-32 round-trips).
 * See docs/MGB64_BACKFLOW.md (engine difference: stock synth vs GE reimpl).
 */
#ifdef NATIVE_PORT
extern void *dkr_lo32_to_ptr(uint32_t lo32);   /* platform/stubs_dkr.c */
static inline void *dkr_audio_resolve_addr(uintptr_t a) {
    return dkr_lo32_to_ptr((uint32_t)a);
}
#define MIXER_RESOLVE(a) dkr_audio_resolve_addr((uintptr_t)(a))
#else
#define MIXER_RESOLVE(a) ((void *)(a))
#endif

/* ===== C implementation function declarations ===== */

void mixerInit(void);

void mixerSegment(unsigned int seg, unsigned int base);
void mixerSetBuffer(unsigned int flags, unsigned int dmemin,
                    unsigned int dmemout, unsigned int count);
void mixerClearBuffer(unsigned int dmem, unsigned int count);
void mixerLoadBuffer(const void *addr);
void mixerLoadBufferSwap16(const void *addr);
void mixerSaveBuffer(void *addr);
void mixerADPCMdec(unsigned int flags, void *state);
void mixerResample(unsigned int flags, unsigned int pitch, void *state);
void mixerEnvMixer(unsigned int flags, void *state);
void mixerInterleave(unsigned int left, unsigned int right);
void mixerMix(unsigned int flags, int16_t gain,
              unsigned int dmemi, unsigned int dmemo);
void mixerSetVolume(unsigned int flags, int16_t vol,
                    int16_t voltgt, int16_t volrate);
void mixerSetLoop(void *state);
void mixerLoadADPCM(unsigned int count, const void *addr);
void mixerDMEMMove(unsigned int dmemin, unsigned int dmemout,
                   unsigned int count);
void mixerPoleFilter(unsigned int flags, int16_t gain, void *state);

typedef struct PortMixerStats {
    uint32_t adpcmDecCalls;
    uint32_t adpcmClampHits;
    uint32_t resampleCalls;
    uint32_t resampleClampHits;
    uint32_t envMixerCalls;
    uint32_t envMixerSampleFrames;
    uint32_t envMixerClampHits;
    uint32_t envSampleXor;
    uint32_t mixCalls;
    uint32_t mixClampHits;
    uint32_t poleFilterCalls;
    uint32_t poleFilterSampleFrames;
    uint32_t poleFilterClampHits;
    uint32_t poleSampleXor;
    uint32_t poleFilterPeak;
    uint32_t saveBufferCalls;
    uint32_t saveBufferBytes;
    uint32_t saveBufferDmemoutCalls;
    /* Master-bus headroom telemetry (mdkr64). mainMixClampHits counts samples
     * that saturated while being mixed INTO AL_MAIN_L/R_OUT — i.e. the final
     * bus, the only clip the listener hears — and mainMixOverPeak is the
     * largest pre-clamp magnitude seen there. 32768 => exactly full scale;
     * e.g. 36000 => the mix wanted +0.8 dB more headroom than s16 provides.
     * mainMixSamples is the denominator: master-bus mix-op samples, NOT emitted
     * PCM samples (alMainBusPull issues one aMix per aux source per channel, so
     * each output sample is touched once per source). Use the ratio to compare
     * runs, and count railed samples in the dumped WAV for absolute clipping.
     * Costs one predictable branch per master-bus sample. */
    uint32_t mainMixClampHits;
    uint32_t mainMixOverPeak;
    uint32_t mainMixSamples;
    uint32_t raw16LoadCalls;
    uint64_t raw16LoadBytes;
} PortMixerStats;

void mixerGetStats(PortMixerStats *out);

/* Master-bus clip telemetry without pulling in PortMixerStats — callable from
 * TUs (audi_port_dkr.c) that must NOT include this header's a* macro block. */
void mixerGetMainBusClip(uint32_t *hits, uint32_t *overPeak, uint32_t *samples);

/* RAW16 endian-boundary telemetry without exposing PortMixerStats. `legacy` is
 * non-zero only for the explicit MDKR_RAW16=legacy broken-direction control. */
void mixerGetRaw16Stats(uint32_t *calls, uint64_t *bytes, int *legacy);

/* ===== Replace abi.h packet-building macros =====
 *
 * The pkt parameter is kept so that ptr++ still increments the command
 * list pointer (filters rely on the return value for sizing).  The
 * Acmd memory is allocated but unused on the port.
 */

#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aADPCMdec
#undef aResample
#undef aEnvMixer
#undef aInterleave
#undef aMix
#undef aSetVolume
#undef aSetLoop
#undef aLoadADPCM
#undef aDMEMMove
#undef aPoleFilter

/* (void)(pkt) ensures ptr++ side effects are evaluated even though
 * the Acmd packet data isn't used on the port. */
/* DMEM offsets (dmemin/dmemout/count) are small buffer indices, not memory
 * addresses — passed through verbatim. Real memory addresses (the `s`/`d`/`a`
 * args of Load/Save/ADPCMdec/Resample/EnvMixer/SetLoop/LoadADPCM/PoleFilter)
 * arrive 32-bit-truncated on LP64, so they go through MIXER_RESOLVE(). */
#define aSegment(pkt, s, b)          ((void)(pkt), mixerSegment((s), (b)))
#define aClearBuffer(pkt, d, c)      ((void)(pkt), mixerClearBuffer((d), (c)))
#define aSetBuffer(pkt, f, i, o, c)  ((void)(pkt), mixerSetBuffer((f), (i), (o), (c)))
#define aLoadBuffer(pkt, s)          ((void)(pkt), mixerLoadBuffer((const void *)MIXER_RESOLVE(s)))
#define aLoadBufferSwap16(pkt, s)    ((void)(pkt), mixerLoadBufferSwap16((const void *)MIXER_RESOLVE(s)))
#define aSaveBuffer(pkt, s)          ((void)(pkt), mixerSaveBuffer((void *)MIXER_RESOLVE(s)))
#define aADPCMdec(pkt, f, s)         ((void)(pkt), mixerADPCMdec((f), (void *)MIXER_RESOLVE(s)))
#define aResample(pkt, f, p, s)      ((void)(pkt), mixerResample((f), (p), (void *)MIXER_RESOLVE(s)))
#define aEnvMixer(pkt, f, s)         ((void)(pkt), mixerEnvMixer((f), (void *)MIXER_RESOLVE(s)))
#define aInterleave(pkt, l, r)       ((void)(pkt), mixerInterleave((l), (r)))
#define aMix(pkt, f, g, i, o)        ((void)(pkt), mixerMix((f), (int16_t)(g), (i), (o)))
#define aSetVolume(pkt, f, v, t, r)  ((void)(pkt), mixerSetVolume((f), (int16_t)(v), (int16_t)(t), (int16_t)(r)))
#define aSetLoop(pkt, a)             ((void)(pkt), mixerSetLoop((void *)MIXER_RESOLVE(a)))
#define aLoadADPCM(pkt, c, d)        ((void)(pkt), mixerLoadADPCM((c), (const void *)MIXER_RESOLVE(d)))
#define aDMEMMove(pkt, i, o, c)      ((void)(pkt), mixerDMEMMove((i), (o), (c)))
#define aPoleFilter(pkt, f, g, s)    ((void)(pkt), mixerPoleFilter((f), (int16_t)(g), (void *)MIXER_RESOLVE(s)))

#endif /* MIXER_H */
