/*
 * Clean-room native audio engine: bank/sequence parsing, the filter chain, the
 * synthesis driver, and the compressed-sequence (CSP) player.
 *
 * This replaces the decompiled SGI libultra synthesiser that mdkr64 used to
 * compile out of game/libultra/src/audio/. It implements the same libaudio ABI
 * the game code calls, but with real host pointers and explicit big-endian
 * parsing rather than overlay-and-patch on the raw N64 image.
 *
 * Provenance: ported from the author's GoldenEye port mgb64
 * (src/platform/audio_compat.c) — shared first-party work. The DKR-specific
 * extensions live in the clearly marked block at the end of this file, and the
 * bank/seqfile layer is mdkr64's own arena-resident parser.
 */

#include "audio_compat_internal.h"
#include "decomp_names.h"

ALGlobals *alGlobals = NULL;

/* ===================================================================
 * DKR extensions (early definitions)
 *
 * These three live here rather than in the DKR block at the end of the
 * file because the engine above uses them: alFxPull() reads alFXEnabled
 * and the two pan entry points call modify_panning().
 * =================================================================== */

/* Reverb master switch. game/src/audio.c sets this from MDKR_AUDIO_REVERB
 * via alFxReverbSet(); alFxPull() gates the wet stage on it. */
u8 alFXEnabled = TRUE;

/* "Audio Options" stereo mode. Global to the synth, not per voice.
 * Mirrors StereoPanMode in libultra/src/audio/synstartvoiceparam.h. */
static s16 s_stereoPanMode = 0; /* STEREO */

void set_stereo_pan_mode(s32 panMode)
{
    s_stereoPanMode = (s16)panMode;
}

s32 get_stereo_pan_mode(void)
{
    return s_stereoPanMode;
}

/*
 * Fold a 0..127 pan (64 = centre) into the selected stereo mode. Every pan
 * value reaching the mixer passes through here — both the note-on pan and
 * later CC10 updates — so the raw pan is never used directly.
 *   STEREO     full width, unchanged
 *   HEADPHONES half width: deviation from centre is halved (~32..95)
 *   MONO       collapsed to centre
 */
s32 modify_panning(s32 pan)
{
    switch (s_stereoPanMode) {
        case 0: /* STEREO */
            return pan;
        case 2: /* HEADPHONES */
            return ((pan - AL_PAN_CENTER) >> 1) + AL_PAN_CENTER;
        default: /* MONO */
            return AL_PAN_CENTER;
    }
}


/* ===================================================================
 * DKR EXTENSIONS
 *
 * Everything below is specific to Diddy Kong Racing and has no
 * counterpart in the mgb64 engine above: the extra per-channel volume
 * lanes, the sequence-player accessors DKR's game code calls, the
 * aux-bus re-parent helper, and the reverb bounds-guard readout.
 *
 * The mutating accessors post MIDI events onto the player's queue
 * rather than writing channel state directly, exactly as the routines
 * they replace did. That keeps them ordered against the sequence's own
 * events and routes them through the one MIDI handler, so a programmatic
 * volume change and a sequenced one cannot disagree.
 * =================================================================== */

/* ---- reverb ------------------------------------------------------ */

void alFxReverbSet(u8 setting)
{
    alFXEnabled = setting;
}

u8 _alFxEnabled(void)
{
    return alFXEnabled;
}

/*
 * Reverb delay-line bounds-guard trip count. Non-zero means a delay-line
 * transfer tried to leave its allocation and was clamped; the audio
 * fixtures assert this stays 0. The counter itself lives with the
 * transfer planner in audio_fx_transfer.c.
 */
u32 alFxGuardTrips(void)
{
    return (u32)portAudioFxGuardTripCount();
}

/* ---- sequence player: polyphony ---------------------------------- */

void set_voice_limit(ALCSPlayer *seqp, u8 voiceLimit)
{
    if (seqp != NULL) {
        seqp->voiceLimit = voiceLimit;
    }
}

/*
 * DKR never constructs a stock ALSeqPlayer — both the music and jingle
 * players are ALCSPlayers — so this exists only to satisfy the libaudio
 * ABI, matching the empty definition it replaces.
 */
void alSeqpNew(ALSeqPlayer *seqp, ALSeqpConfig *config)
{
    (void)seqp;
    (void)config;
}

/* ---- sequence player: accessors ---------------------------------- */

static void csp_post_midi(ALCSPlayer *seqp, u8 status, u8 byte1, u8 byte2)
{
    ALEvent evt;

    if (seqp == NULL) {
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = AL_SEQP_MIDI_EVT;
    evt.msg.midi.ticks = 0;
    evt.msg.midi.status = status;
    evt.msg.midi.byte1 = byte1;
    evt.msg.midi.byte2 = byte2;
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}

void alCSPSetBank(ALCSPlayer *seqp, ALBank *b)
{
    if (seqp == NULL) {
        return;
    }

    seqp->bank = b;
    if (b != NULL) {
        __initFromBank((ALSeqPlayer *)seqp, b);
    }
}

u8 alCSPGetChlVol(ALCSPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].vol;
}

void alCSPSetChlVol(ALCSPlayer *seqp, u8 chan, u8 vol)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_VOLUME_CTRL, vol);
}

void alCSPSetChlPan(ALCSPlayer *seqp, u8 chan, ALPan pan)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_PAN_CTRL, pan);
}

u8 alCSPGetFadeIn(ALCSPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].fade;
}

/* `pan` is a misnomer inherited from the original prototype: the payload is
 * the 0..127 fade level. */
void alCSPSetFadeIn(ALCSPlayer *seqp, u8 chan, ALPan pan)
{
    csp_post_midi(seqp, (u8)(AL_MIDI_ControlChange | chan),
                  AL_MIDI_UNK_8, pan);
}

u8 alSeqpGetChlFXMix(ALSeqPlayer *seqp, u8 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 0;
    }

    return CSP_CHAN(seqp)[chan].fxmix;
}

/*
 * Tempo is exchanged in microseconds per quarter note (the MIDI tempo
 * meta-event unit); the player stores microseconds per TICK. Going out we
 * divide by the sequence's quarter-notes-per-tick, going in the meta handler
 * multiplies by it — and also re-times any queued note-offs so they keep
 * their position in ticks.
 */
s32 alCSPGetTempo(ALCSPlayer *seqp)
{
    if (seqp == NULL || seqp->target == NULL || seqp->target->qnpt == 0.0f) {
        return 0;
    }

    return (s32)((f32)seqp->uspt / seqp->target->qnpt);
}

void alCSPSetTempo(ALCSPlayer *seqp, s32 tempo)
{
    ALEvent evt;

    if (seqp == NULL) {
        return;
    }

    /*
     * The value travels as a three-byte MIDI Set Tempo payload, so the bytes
     * below silently truncate anything outside 24 bits -- 0x1000001 would
     * arrive as 1 microsecond per quarter note, i.e. an enormous speed-up from
     * a value the caller meant as a slow-down, and a negative tempo would
     * arrive as whatever its two's-complement low bytes happened to spell.
     * Refuse instead of transmitting a different number than we were given.
     * Zero is refused for the same reason the meta handler cannot use it: the
     * player divides by microseconds-per-tick.
     */
    if (tempo <= 0 || tempo > 0xFFFFFF) {
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = AL_SEQP_META_EVT;
    evt.msg.tempo.ticks = 0;
    evt.msg.tempo.status = AL_MIDI_Meta;
    evt.msg.tempo.type = AL_MIDI_META_TEMPO;
    evt.msg.tempo.len = 3;
    evt.msg.tempo.byte1 = (u8)((tempo >> 16) & 0xFF);
    evt.msg.tempo.byte2 = (u8)((tempo >> 8) & 0xFF);
    evt.msg.tempo.byte3 = (u8)(tempo & 0xFF);
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}

/* ---- channel enable/disable -------------------------------------- */

void alSeqChOff(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_6A, chan);
}

void alSeqChOn(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_6C, chan);
}

/*
 * Post the global FX-send floor described in the AL_MIDI_UNK_5F handler.
 * The status byte carries no channel nibble because the handler sweeps
 * every channel regardless.
 *
 * The name is an alias for func_80063A90 (game/include/decomp_names.h); the
 * symbol this defines is still the raw one the vendored callers link against.
 */
void alSeqSetFxSendFloor(ALCSPlayer *seqp, u8 chan)
{
    csp_post_midi(seqp, AL_MIDI_ControlChange, AL_MIDI_UNK_5F, chan);
}

/* ---- aux bus ------------------------------------------------------ */

/*
 * Re-parent a physical voice's envmixer onto aux bus `bus`.
 *
 * Physical voices are pooled and shared across players, so a slot reused
 * from another owner may still be wired to the bus that owner wanted.
 * Every CSP note-on calls this with bus 0 to guarantee the voice is on the
 * bus its new owner expects before it sounds. No-op when it already is.
 *
 * The name is an alias for func_80065A80 (game/include/decomp_names.h); the
 * symbol this defines is still the raw one the vendored callers link against.
 */
void alSynSetVoiceAuxBus(ALSynth *drvr, PVoice *pvoice, s16 bus)
{
    if (drvr == NULL || pvoice == NULL || drvr->auxBus == NULL) {
        return;
    }

    if (bus < 0 || bus >= drvr->maxAuxBusses || bus == pvoice->unkDC) {
        return;
    }

    alAuxBusParam(&drvr->auxBus[pvoice->unkDC], AL_FILTER_UNK11,
                  &pvoice->envmixer);
    alAuxBusParam(&drvr->auxBus[bus], AL_FILTER_ADD_SOURCE, &pvoice->envmixer);
    pvoice->unkDC = bus;
}
