/*
 * Clean-room native audio engine: the compressed-sequence (CSP) player.
 *
 * The MIDI and meta-event handlers, DKR's per-channel volume lanes, the
 * note-on/note-off tracing the audio fixtures read, and the voice handler that
 * drives it. Split out of audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

static void native_csp_set_uspt_from_tempo(ALCSPlayer *seqp, f32 tempo)
{
    if (seqp != NULL && seqp->target != NULL) {
        seqp->uspt = (s32)(tempo * seqp->target->qnpt);
    } else if (seqp != NULL) {
        seqp->uspt = 488;
    }
}

static void native_csp_update_channel_volumes(ALCSPlayer *seqp, u8 chan)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        if (voice_state->channel == chan &&
            voice_state->envPhase != AL_PHASE_RELEASE) {
            alSynSetVol(seqp->drvr,
                        &voice_state->voice,
                        __vsVol(voice_state, (ALSeqPlayer *)seqp),
                        __vsDelta(voice_state, seqp->curTime));
        }
    }
}

static void native_csp_update_all_volumes(ALCSPlayer *seqp)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        alSynSetVol(seqp->drvr,
                    &voice_state->voice,
                    __vsVol(voice_state, (ALSeqPlayer *)seqp),
                    __vsDelta(voice_state, seqp->curTime));
    }
}

void __CSPPostNextSeqEvent(ALCSPlayer *seqp)
{
    ALEvent event;
    s32 delta_ticks;

    if (seqp == NULL || seqp->state != AL_PLAYING || seqp->target == NULL) {
        return;
    }

    if (!__alCSeqNextDelta(seqp->target, &delta_ticks)) {
        return;
    }

    event.type = AL_SEQ_REF_EVT;
    alEvtqPostEvent(&seqp->evtq, &event, delta_ticks * seqp->uspt);
}

static void native_csp_handle_meta(ALCSPlayer *seqp, ALEvent *event)
{
    ALEventListItem *deferred_head = NULL;
    ALEventListItem *deferred_tail = NULL;
    ALEventListItem *node;
    ALMicroTime running_delta = 0;
    s32 old_uspt;
    s32 tempo;

    if (seqp == NULL || event == NULL ||
        event->msg.tempo.status != AL_MIDI_Meta ||
        event->msg.tempo.type != AL_MIDI_META_TEMPO) {
        return;
    }

    old_uspt = seqp->uspt;
    tempo = ((s32)event->msg.tempo.byte1 << 16) |
            ((s32)event->msg.tempo.byte2 << 8) |
            (s32)event->msg.tempo.byte3;
    native_csp_set_uspt_from_tempo(seqp, (f32)tempo);
    if (old_uspt <= 0) {
        return;
    }

    node = (ALEventListItem *)seqp->evtq.allocList.next;
    while (node != NULL) {
        ALEventListItem *next = (ALEventListItem *)node->node.next;

        running_delta += node->delta;
        if (node->evt.type == AL_CSP_NOTEOFF_EVT) {
            ALMicroTime absolute_delta = running_delta;

            if (next != NULL) {
                next->delta += node->delta;
                running_delta -= node->delta;
            }
            alUnlink((ALLink *)node);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
            node->node.prev = NULL;
            node->node.next = NULL;
            if (deferred_tail != NULL) {
                deferred_tail->node.next = (ALLink *)node;
                node->node.prev = (ALLink *)deferred_tail;
            } else {
                deferred_head = node;
            }
            deferred_tail = node;
            node->delta = absolute_delta;
        }

        node = next;
    }

    while (deferred_head != NULL) {
        ALEventListItem *next = (ALEventListItem *)deferred_head->node.next;
        u32 ticks = (u32)(deferred_head->delta / old_uspt);

        deferred_head->delta = ticks * seqp->uspt;
        native_seqp_repost_event_item(&seqp->evtq, deferred_head);
        deferred_head = next;
    }
}

#ifdef NATIVE_PORT
static FILE *s_native_csp_trace_fp = NULL;
static int s_native_csp_trace_init = 0;

static FILE *native_csp_trace_fp(void)
{
    const char *path;

    if (s_native_csp_trace_init) {
        return s_native_csp_trace_fp;
    }

    s_native_csp_trace_init = 1;
    path = getenv("MDKR_MUSIC_MIDI_TRACE_JSONL");
    if (path != NULL && *path != '\0') {
        s_native_csp_trace_fp = mdkr_fopen_utf8(path, "w");
    }

    return s_native_csp_trace_fp;
}

static s32 native_csp_program_for_instrument(ALCSPlayer *seqp,
                                             ALInstrument *instrument)
{
    s32 i;

    if (seqp == NULL || seqp->bank == NULL || instrument == NULL) {
        return -1;
    }

    for (i = 0; i < seqp->bank->instCount; i++) {
        if (seqp->bank->instArray[i] == instrument) {
            return i;
        }
    }

    return -1;
}

static int native_csp_program_list_contains(const char *list, s32 program)
{
    const char *cursor = list;

    if (program < 0 || list == NULL || *list == '\0') {
        return 0;
    }

    while (*cursor != '\0') {
        char *end = NULL;
        long value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        value = strtol(cursor, &end, 10);
        if (end == cursor) {
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        if (value == program) {
            return 1;
        }

        cursor = end;
    }

    return 0;
}

static int native_csp_program_should_play(ALCSPlayer *seqp, u8 chan)
{
    static int initialized = 0;
    static const char *solo_programs = NULL;
    static const char *mute_programs = NULL;
    ALInstrument *instrument;
    s32 program;

    if (!initialized) {
        initialized = 1;
        solo_programs = getenv("MDKR_MUSIC_SOLO_PROGRAMS");
        mute_programs = getenv("MDKR_MUSIC_MUTE_PROGRAMS");
    }

    if ((solo_programs == NULL || *solo_programs == '\0') &&
        (mute_programs == NULL || *mute_programs == '\0')) {
        return 1;
    }

    if (seqp == NULL || CSP_CHAN(seqp) == NULL || chan >= seqp->maxChannels) {
        return 1;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    program = native_csp_program_for_instrument(seqp, instrument);

    if (solo_programs != NULL && *solo_programs != '\0' &&
        !native_csp_program_list_contains(solo_programs, program)) {
        return 0;
    }

    if (native_csp_program_list_contains(mute_programs, program)) {
        return 0;
    }

    return 1;
}

static s32 native_csp_sound_index(ALInstrument *instrument, ALSound *sound)
{
    s32 i;

    if (instrument == NULL || sound == NULL) {
        return -1;
    }

    for (i = 0; i < instrument->soundCount; i++) {
        if (instrument->soundArray[i] == sound) {
            return i;
        }
    }

    return -1;
}

static void native_csp_trace_control(ALCSPlayer *seqp, const char *event,
                                     u8 chan, u8 key, u8 value)
{
    FILE *fp = native_csp_trace_fp();

    if (fp == NULL || seqp == NULL) {
        return;
    }

    fprintf(fp,
            "{\"event\":\"%s\",\"cur_time\":%d,\"seq_ticks\":%u,"
            "\"chan\":%u,\"key\":%u,\"value\":%u}\n",
            event,
            seqp->curTime,
            seqp->target != NULL ? seqp->target->lastTicks : 0,
            (u32)chan,
            (u32)key,
            (u32)value);
}

static void native_csp_trace_note_on(ALCSPlayer *seqp, u8 chan, u8 key,
                                     u8 vel, ALSound *sound, f32 pitch,
                                     s16 vol, ALPan pan, u8 fxmix,
                                     ALMicroTime delta_time,
                                     u32 duration_ticks)
{
    ALInstrument *instrument = NULL;
    ALKeyMap *keymap = NULL;
    ALEnvelope *envelope = NULL;
    ALWaveTable *wave = NULL;
    ALADPCMloop *adpcm_loop = NULL;
    ALRawLoop *raw_loop = NULL;
    FILE *fp = native_csp_trace_fp();

    if (fp == NULL || seqp == NULL || sound == NULL) {
        return;
    }

    if (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels) {
        instrument = CSP_CHAN(seqp)[chan].instrument;
    }
    keymap = sound->keyMap;
    envelope = sound->envelope;
    wave = sound->wavetable;
    if (wave != NULL && wave->type == AL_ADPCM_WAVE) {
        adpcm_loop = wave->waveInfo.adpcmWave.loop;
    } else if (wave != NULL && wave->type == AL_RAW16_WAVE) {
        raw_loop = wave->waveInfo.rawWave.loop;
    }

    fprintf(fp,
            "{\"event\":\"note_on\",\"cur_time\":%d,\"seq_ticks\":%u,"
            "\"chan\":%u,\"key\":%u,\"velocity\":%u,\"duration_ticks\":%u,"
            "\"program\":%d,\"sound_index\":%d,"
            "\"chan_volume\":%u,\"chan_pan\":%u,\"chan_fxmix\":%u,"
            "\"computed_pitch\":%.9g,\"computed_volume\":%d,"
            "\"computed_pan\":%u,\"attack_delta_usec\":%d,"
            "\"sample_pan\":%u,\"sample_volume\":%u,"
            "\"key_min\":%u,\"key_max\":%u,\"vel_min\":%u,\"vel_max\":%u,"
            "\"key_base\":%u,\"detune\":%d,"
            "\"attack_time\":%d,\"decay_time\":%d,\"release_time\":%d,"
            "\"attack_volume\":%u,\"decay_volume\":%u,"
            "\"wave_type\":%u,\"wave_base\":%llu,\"wave_len\":%d,"
            "\"loop_start\":%d,\"loop_end\":%d,\"loop_count\":%d}\n",
            seqp->curTime,
            seqp->target != NULL ? seqp->target->lastTicks : 0,
            (u32)chan,
            (u32)key,
            (u32)vel,
            duration_ticks,
            native_csp_program_for_instrument(seqp, instrument),
            native_csp_sound_index(instrument, sound),
            (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels)
                ? CSP_CHAN(seqp)[chan].vol
                : 0,
            (CSP_CHAN(seqp) != NULL && chan < seqp->maxChannels)
                ? CSP_CHAN(seqp)[chan].pan
                : 0,
            fxmix,
            pitch,
            vol,
            (u32)pan,
            delta_time,
            sound->samplePan,
            sound->sampleVolume,
            keymap != NULL ? keymap->keyMin : 0,
            keymap != NULL ? keymap->keyMax : 0,
            keymap != NULL ? keymap->velocityMin : 0,
            keymap != NULL ? keymap->velocityMax : 0,
            keymap != NULL ? keymap->keyBase : 0,
            keymap != NULL ? keymap->detune : 0,
            envelope != NULL ? envelope->attackTime : 0,
            envelope != NULL ? envelope->decayTime : 0,
            envelope != NULL ? envelope->releaseTime : 0,
            envelope != NULL ? envelope->attackVolume : 0,
            envelope != NULL ? envelope->decayVolume : 0,
            wave != NULL ? wave->type : 0,
            wave != NULL ? (unsigned long long)(uintptr_t)wave->base : 0ULL,
            wave != NULL ? wave->len : 0,
            adpcm_loop != NULL ? adpcm_loop->start
                : raw_loop != NULL ? raw_loop->start : 0,
            adpcm_loop != NULL ? adpcm_loop->end
                : raw_loop != NULL ? raw_loop->end : 0,
            adpcm_loop != NULL ? adpcm_loop->count
                : raw_loop != NULL ? raw_loop->count : 0);
}

static void native_csp_trace_note_off(ALCSPlayer *seqp, u8 chan, u8 key)
{
    native_csp_trace_control(seqp, "note_off", chan, key, 0);
}
#else
#define native_csp_trace_control(seqp, event, chan, key, value) ((void)0)
#define native_csp_trace_note_on(seqp, chan, key, vel, sound, pitch, vol, pan, fxmix, delta_time, duration_ticks) ((void)0)
#define native_csp_trace_note_off(seqp, chan, key) ((void)0)
#define native_csp_program_should_play(seqp, chan) (1)
#endif

static void native_csp_handle_midi(ALCSPlayer *seqp, ALEvent *event)
{
    ALMIDIEvent *midi;
    s32 status;
    u8 chan;
    u8 key;
    u8 vel;

    if (seqp == NULL || event == NULL) {
        return;
    }

    midi = &event->msg.midi;
    status = midi->status & AL_MIDI_StatusMask;
    chan = midi->status & AL_MIDI_ChannelMask;
    key = midi->byte1;
    vel = midi->byte2;
    if (chan >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return;
    }

    /*
     * DKR channel-mode messages. These share the Control Change status byte
     * (AL_MIDI_ChannelModeSelect == AL_MIDI_ControlChange == 0xB0) and are
     * distinguished by controller number. Note both act on the channel named
     * by byte2 (`vel`), NOT the status byte's channel nibble.
     */
    if (status == AL_MIDI_ControlChange &&
        (key == AL_MIDI_UNK_6A || key == AL_MIDI_UNK_6C) &&
        vel < seqp->maxChannels) {
        if (key == AL_MIDI_UNK_6C) {
            seqp->chanMask |= (u16)(1 << vel);
            return;
        }

        seqp->chanMask &= (u16)~(1 << vel);
        {
            ALVoiceState *voice_state;

            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == vel &&
                    voice_state->sound != NULL &&
                    voice_state->sound->envelope != NULL) {
                    __seqpReleaseVoice(
                        (ALSeqPlayer *)seqp, &voice_state->voice,
                        voice_state->sound->envelope->releaseTime);
                }
            }
        }
        return;
    }

    /*
     * Disabled channels still track state: program change, control change and
     * pitch bend are applied so the channel is correct when it is re-enabled,
     * but nothing that could sound a note gets through.
     */
    if ((seqp->chanMask & (1 << chan)) == 0 &&
        status != AL_MIDI_ProgramChange &&
        status != AL_MIDI_ControlChange &&
        status != AL_MIDI_PitchBendChange) {
        return;
    }

    switch (status) {
        case AL_MIDI_NoteOn:
            if (vel != 0) {
                ALSound *sound;
                ALVoiceConfig config;
                ALVoiceState *voice_state;
                ALVoice *voice;
                ALEvent queued;
                ALMicroTime delta_time;
                s32 cents;
                f32 pitch;
                s16 vol;
                ALPan pan;

                if (seqp->state != AL_PLAYING) {
                    return;
                }

                sound =
                    __lookupSoundQuick((ALSeqPlayer *)seqp, key, vel, chan);
                if (sound == NULL || sound->keyMap == NULL ||
                    sound->envelope == NULL || sound->wavetable == NULL ||
                    CSP_CHAN(seqp)[chan].instrument == NULL) {
                    return;
                }

                if (!native_csp_program_should_play(seqp, chan)) {
                    return;
                }

                voice_state =
                    __mapVoice((ALSeqPlayer *)seqp, key, vel, chan);
                if (voice_state == NULL) {
                    return;
                }

                config.priority = CSP_CHAN(seqp)[chan].priority;
                config.fxBus = 0;
                config.unityPitch = 0;
                voice = &voice_state->voice;
                if (!alSynAllocVoice(seqp->drvr, voice, &config)) {
                    __unmapVoice((ALSeqPlayer *)seqp, voice);
                    return;
                }

                voice_state->sound = sound;
                voice_state->envPhase = AL_PHASE_ATTACK;
                voice_state->phase =
                    CSP_CHAN(seqp)[chan].sustain > AL_SUSTAIN
                        ? AL_PHASE_SUSTAIN
                        : AL_PHASE_NOTEON;
                cents = ((s32)key - sound->keyMap->keyBase) * 100 +
                        sound->keyMap->detune;
                voice_state->pitch = alCents2Ratio(cents);
                voice_state->envGain = sound->envelope->attackVolume;
                voice_state->envEndTime =
                    seqp->curTime + sound->envelope->attackTime;
                voice_state->flags = 0;
                voice_state->tremelo = AL_VOL_FULL;
                voice_state->vibrato = 1.0f;

                pitch = voice_state->pitch *
                        CSP_CHAN(seqp)[chan].pitchBend *
                        voice_state->vibrato;
                pan = __vsPan(voice_state, (ALSeqPlayer *)seqp);
                vol = __vsVol(voice_state, (ALSeqPlayer *)seqp);
                delta_time = sound->envelope->attackTime;
                native_csp_trace_note_on(seqp,
                                         chan,
                                         key,
                                         vel,
                                         sound,
                                         pitch,
                                         vol,
                                         pan,
                                         CSP_CHAN(seqp)[chan].fxmix,
                                         delta_time,
                                         midi->duration);
                alSynStartVoiceParams(seqp->drvr, voice, sound->wavetable,
                                      pitch, vol, pan,
                                      CSP_CHAN(seqp)[chan].fxmix,
                                      delta_time);

                queued.type = AL_SEQP_ENV_EVT;
                queued.msg.vol.voice = voice;
                queued.msg.vol.vol = sound->envelope->decayVolume;
                queued.msg.vol.delta = sound->envelope->decayTime;
                alEvtqPostEvent(&seqp->evtq, &queued, delta_time);

                if (midi->duration != 0) {
                    queued.type = AL_CSP_NOTEOFF_EVT;
                    queued.msg.midi.status = chan | AL_MIDI_NoteOff;
                    queued.msg.midi.byte1 = key;
                    queued.msg.midi.byte2 = 0;
                    queued.msg.midi.duration = 0;
                    alEvtqPostEvent(&seqp->evtq, &queued,
                                    seqp->uspt * midi->duration);
                }
                return;
            }
            /* Treat zero-velocity note-on as note-off. */
            /* FALLTHROUGH */
        case AL_MIDI_NoteOff:
        {
            ALVoiceState *voice_state =
                __lookupVoice((ALSeqPlayer *)seqp, key, chan);

            native_csp_trace_note_off(seqp, chan, key);
            if (voice_state == NULL || voice_state->sound == NULL ||
                voice_state->sound->envelope == NULL) {
                return;
            }

            if (voice_state->phase == AL_PHASE_SUSTAIN) {
                voice_state->phase = AL_PHASE_SUSTREL;
            } else {
                voice_state->phase = AL_PHASE_RELEASE;
                __seqpReleaseVoice((ALSeqPlayer *)seqp, &voice_state->voice,
                                   voice_state->sound->envelope->releaseTime);
            }
            break;
        }

        case AL_MIDI_PolyKeyPressure:
        {
            ALVoiceState *voice_state =
                __lookupVoice((ALSeqPlayer *)seqp, key, chan);

            if (voice_state == NULL) {
                return;
            }

            voice_state->velocity = vel;
            alSynSetVol(seqp->drvr,
                        &voice_state->voice,
                        __vsVol(voice_state, (ALSeqPlayer *)seqp),
                        __vsDelta(voice_state, seqp->curTime));
            break;
        }

        case AL_MIDI_ChannelPressure:
        {
            ALVoiceState *voice_state;

            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == chan) {
                    voice_state->velocity = key;
                    alSynSetVol(seqp->drvr,
                                &voice_state->voice,
                                __vsVol(voice_state, (ALSeqPlayer *)seqp),
                                __vsDelta(voice_state, seqp->curTime));
                }
            }
            break;
        }

        case AL_MIDI_ControlChange:
            switch (key) {
                case AL_MIDI_PAN_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].pan = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel == chan) {
                            alSynSetPan(
                                seqp->drvr,
                                &voice_state->voice,
                                __vsPan(voice_state, (ALSeqPlayer *)seqp));
                        }
                    }
                    break;
                }

                /*
                 * DKR splits channel volume into two lanes that are multiplied
                 * together: `unk11` is the raw CC7 volume and `fade` is the
                 * CC8 fade-in level, both 0..127. The effective channel volume
                 * driving the voices is (unk11 * fade) / 127, so a fade can
                 * duck a channel without destroying the volume the sequence
                 * set, and vice versa.
                 */
                case AL_MIDI_VOLUME_CTRL:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].unk11 = vel;
                    CSP_CHAN(seqp)[chan].vol =
                        (u8)(((u32)CSP_CHAN(seqp)[chan].fade * (u32)vel) / 127u);
                    native_csp_update_channel_volumes(seqp, chan);
                    break;

                case AL_MIDI_UNK_8:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].fade = vel;
                    CSP_CHAN(seqp)[chan].vol =
                        (u8)(((u32)CSP_CHAN(seqp)[chan].unk11 * (u32)vel) / 127u);
                    native_csp_update_channel_volumes(seqp, chan);
                    break;

                case AL_MIDI_PRIORITY_CTRL:
                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].priority = vel;
                    break;

                case AL_MIDI_SUSTAIN_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].sustain = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel != chan ||
                            voice_state->phase == AL_PHASE_RELEASE) {
                            continue;
                        }
                        if (vel > AL_SUSTAIN) {
                            if (voice_state->phase == AL_PHASE_NOTEON) {
                                voice_state->phase = AL_PHASE_SUSTAIN;
                            }
                        } else if (voice_state->phase == AL_PHASE_SUSTAIN) {
                            voice_state->phase = AL_PHASE_NOTEON;
                        } else if (voice_state->phase == AL_PHASE_SUSTREL &&
                                   voice_state->sound != NULL &&
                                   voice_state->sound->envelope != NULL) {
                            voice_state->phase = AL_PHASE_RELEASE;
                            __seqpReleaseVoice(
                                (ALSeqPlayer *)seqp,
                                &voice_state->voice,
                                voice_state->sound->envelope->releaseTime);
                        }
                    }
                    break;
                }

                case AL_MIDI_FX1_CTRL:
                {
                    ALVoiceState *voice_state;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    CSP_CHAN(seqp)[chan].fxmix = vel;
                    for (voice_state = seqp->vAllocHead; voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (voice_state->channel == chan) {
                            alSynSetFXMix(seqp->drvr, &voice_state->voice,
                                          vel);
                        }
                    }
                    break;
                }

                /*
                 * DKR global FX-send floor. Unlike every other controller this
                 * one ignores its own channel nibble and sweeps ALL channels,
                 * clearing the stored fxmix of any channel currently sending
                 * less than `vel`.
                 *
                 * Faithful-to-original oddity: the stored fxmix is set to 0
                 * but the live voices are driven to `vel`, so stored and
                 * audible state deliberately diverge until something next
                 * touches that channel's fxmix. func_80063A90() is the only
                 * caller and it passes a CHANNEL INDEX as `vel`, so in
                 * practice the comparison runs against small values. This is
                 * reproduced as-is because the music mix depends on it.
                 */
                case AL_MIDI_UNK_5F:
                {
                    u8 i;

                    native_csp_trace_control(seqp, "control_change", chan,
                                             key, vel);
                    seqp->unk36 = vel;
                    for (i = 0; i < seqp->maxChannels; i++) {
                        ALVoiceState *voice_state;

                        if (CSP_CHAN(seqp)[i].fxmix >= vel) {
                            continue;
                        }
                        CSP_CHAN(seqp)[i].fxmix = 0;
                        for (voice_state = seqp->vAllocHead;
                             voice_state != NULL;
                             voice_state = voice_state->next) {
                            if (voice_state->channel == i) {
                                alSynSetFXMix(seqp->drvr, &voice_state->voice,
                                              vel);
                            }
                        }
                    }
                    break;
                }

                default:
                    native_csp_trace_control(seqp, "control_change_unhandled",
                                             chan, key, vel);
                    break;
            }
            break;

        case AL_MIDI_ProgramChange:
            native_csp_trace_control(seqp, "program_change", chan, key, 0);
            if (seqp->bank != NULL && key < seqp->bank->instCount &&
                seqp->bank->instArray[key] != NULL) {
                __setInstChanState((ALSeqPlayer *)seqp,
                                   seqp->bank->instArray[key],
                                   chan);
            }
            break;

        case AL_MIDI_PitchBendChange:
        {
            ALVoiceState *voice_state;
            s32 bend = (((s32)vel << 7) + key) - 8192;
            s32 cents = (CSP_CHAN(seqp)[chan].bendRange * bend) / 8192;
            f32 ratio = alCents2Ratio(cents);

            CSP_CHAN(seqp)[chan].pitchBend = ratio;
            native_csp_trace_control(seqp, "pitch_bend", chan, key, vel);
            for (voice_state = seqp->vAllocHead; voice_state != NULL;
                 voice_state = voice_state->next) {
                if (voice_state->channel == chan) {
                    alSynSetPitch(
                        seqp->drvr,
                        &voice_state->voice,
                        voice_state->pitch * ratio * voice_state->vibrato);
                }
            }
            break;
        }

        default:
            break;
    }
}

static void native_csp_handle_next_seq_event(ALCSPlayer *seqp)
{
    ALEvent event;

    if (seqp == NULL || seqp->target == NULL) {
        return;
    }

    alCSeqNextEvent(seqp->target, &event);
    switch (event.type) {
        case AL_SEQ_MIDI_EVT:
            native_csp_handle_midi(seqp, &event);
            __CSPPostNextSeqEvent(seqp);
            break;

        case AL_TEMPO_EVT:
            native_csp_handle_meta(seqp, &event);
            __CSPPostNextSeqEvent(seqp);
            break;

        case AL_SEQ_END_EVT:
            seqp->state = AL_STOPPING;
            event.type = AL_SEQP_STOP_EVT;
            alEvtqPostEvent(&seqp->evtq, &event, AL_EVTQ_END);
            break;

        case AL_TRACK_END:
        case AL_CSP_LOOPSTART:
        case AL_CSP_LOOPEND:
            __CSPPostNextSeqEvent(seqp);
            break;

        default:
            break;
    }
}

static void native_csp_stop_allocated_voices(ALCSPlayer *seqp)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return;
    }

    while ((voice_state = seqp->vAllocHead) != NULL) {
        alSynStopVoice(seqp->drvr, &voice_state->voice);
        alSynFreeVoice(seqp->drvr, &voice_state->voice);
        if (voice_state->flags != 0) {
            __seqpStopOsc((ALSeqPlayer *)seqp, voice_state);
        }
        __unmapVoice((ALSeqPlayer *)seqp, &voice_state->voice);
    }
}

static ALMicroTime native_csp_voice_handler(void *node)
{
    ALCSPlayer *seqp = (ALCSPlayer *)node;
    ALEvent event;

    if (seqp == NULL) {
        return AL_USEC_PER_FRAME;
    }

    do {
        switch (seqp->nextEvent.type) {
            case AL_SEQ_REF_EVT:
                native_csp_handle_next_seq_event(seqp);
                break;

            case AL_SEQP_API_EVT:
                event.type = AL_SEQP_API_EVT;
                alEvtqPostEvent(&seqp->evtq, &event, seqp->frameTime);
                break;

            case AL_NOTE_END_EVT:
                if (seqp->nextEvent.msg.note.voice != NULL) {
                    ALVoice *voice = seqp->nextEvent.msg.note.voice;
                    ALVoiceState *voice_state =
                        (ALVoiceState *)voice->clientPrivate;

                    alSynStopVoice(seqp->drvr, voice);
                    alSynFreeVoice(seqp->drvr, voice);
                    if (voice_state != NULL && voice_state->flags != 0) {
                        __seqpStopOsc((ALSeqPlayer *)seqp, voice_state);
                    }
                    __unmapVoice((ALSeqPlayer *)seqp, voice);
                }
                break;

            case AL_SEQP_ENV_EVT:
                if (seqp->nextEvent.msg.vol.voice != NULL) {
                    ALVoice *voice = seqp->nextEvent.msg.vol.voice;
                    ALVoiceState *voice_state =
                        (ALVoiceState *)voice->clientPrivate;
                    ALMicroTime delta = seqp->nextEvent.msg.vol.delta;

                    if (voice_state != NULL) {
                        if (voice_state->envPhase == AL_PHASE_ATTACK) {
                            voice_state->envPhase = AL_PHASE_DECAY;
                        }
                        voice_state->envEndTime = seqp->curTime + delta;
                        voice_state->envGain = seqp->nextEvent.msg.vol.vol;
                        alSynSetVol(
                            seqp->drvr,
                            voice,
                            __vsVol(voice_state, (ALSeqPlayer *)seqp),
                            delta);
                    }
                }
                break;

            case AL_TREM_OSC_EVT:
            case AL_VIB_OSC_EVT:
                break;

            case AL_SEQP_MIDI_EVT:
            case AL_CSP_NOTEOFF_EVT:
                native_csp_handle_midi(seqp, &seqp->nextEvent);
                break;

            case AL_SEQP_META_EVT:
                native_csp_handle_meta(seqp, &seqp->nextEvent);
                break;

            case AL_SEQP_VOL_EVT:
                seqp->vol = seqp->nextEvent.msg.spvol.vol;
                native_csp_update_all_volumes(seqp);
                break;

            case AL_SEQP_PLAY_EVT:
                if (seqp->state != AL_PLAYING) {
                    seqp->state = AL_PLAYING;
                    __CSPPostNextSeqEvent(seqp);
                }
                break;

            case AL_SEQP_STOP_EVT:
                if (seqp->state == AL_STOPPING) {
                    native_csp_stop_allocated_voices(seqp);
                    seqp->state = AL_STOPPED;
                }
                break;

            case AL_SEQP_STOPPING_EVT:
                if (seqp->state == AL_PLAYING) {
                    ALVoiceState *voice_state;

                    alEvtqFlushType(&seqp->evtq, AL_SEQ_REF_EVT);
                    alEvtqFlushType(&seqp->evtq, AL_CSP_NOTEOFF_EVT);
                    alEvtqFlushType(&seqp->evtq, AL_SEQP_MIDI_EVT);
                    for (voice_state = seqp->vAllocHead;
                         voice_state != NULL;
                         voice_state = voice_state->next) {
                        if (__voiceNeedsNoteKill(
                                (ALSeqPlayer *)seqp,
                                &voice_state->voice,
                                KILL_TIME)) {
                            __seqpReleaseVoice((ALSeqPlayer *)seqp,
                                               &voice_state->voice,
                                               KILL_TIME);
                        }
                    }
                    seqp->state = AL_STOPPING;
                    event.type = AL_SEQP_STOP_EVT;
                    alEvtqPostEvent(&seqp->evtq, &event, AL_EVTQ_END);
                }
                break;

            case AL_SEQP_PRIORITY_EVT:
                if (seqp->nextEvent.msg.sppriority.chan < seqp->maxChannels) {
                    CSP_CHAN(seqp)[seqp->nextEvent.msg.sppriority.chan]
                        .priority =
                        seqp->nextEvent.msg.sppriority.priority;
                }
                break;

            case AL_SEQP_SEQ_EVT:
                seqp->target = (ALCSeq *)seqp->nextEvent.msg.spseq.seq;
                native_csp_set_uspt_from_tempo(seqp, 500000.0f);
                if (seqp->bank != NULL) {
                    __initFromBank((ALSeqPlayer *)seqp, seqp->bank);
                }
                break;

            case AL_SEQP_BANK_EVT:
                seqp->bank = seqp->nextEvent.msg.spbank.bank;
                __initFromBank((ALSeqPlayer *)seqp, seqp->bank);
                break;

            default:
                break;
        }

        seqp->nextDelta = portAudioEventQueueNextOrHeartbeat(
            &seqp->evtq,
            &seqp->nextEvent,
            AL_SEQP_API_EVT,
            seqp->frameTime);
    } while (seqp->nextDelta == 0);

    seqp->curTime += seqp->nextDelta;
    return seqp->nextDelta;
}

void alCSPNew(ALCSPlayer *seqp, ALSeqpConfig *config)
{
    ALHeap *heap;
    ALEventListItem *events;
    ALVoiceState *voices;
    s32 i;

    if (seqp == NULL || config == NULL || alGlobals == NULL) {
        return;
    }

    memset(seqp, 0, sizeof(*seqp));
    heap = config->heap;
    seqp->drvr = &alGlobals->drvr;
    seqp->chanMask = 0xFFFF;
    seqp->uspt = 488;
    seqp->state = AL_STOPPED;
    seqp->vol = 0x7fff;
    seqp->frameTime = AL_USEC_PER_FRAME;
    seqp->maxChannels = config->maxChannels;
    seqp->debugFlags = config->debugFlags;
    seqp->initOsc = (ALOscInit)config->initOsc;
    seqp->updateOsc = (ALOscUpdate)config->updateOsc;
    seqp->stopOsc = (ALOscStop)config->stopOsc;
    seqp->nextEvent.type = AL_SEQP_API_EVT;

    if (seqp->maxChannels == 0 || seqp->maxChannels > AL_MAX_CHANNELS) {
        seqp->maxChannels = AL_MAX_CHANNELS;
    }

    CSP_CHAN(seqp) =
        alHeapAlloc(heap, seqp->maxChannels, sizeof(ALChanState_Custom));
    __initChanState((ALSeqPlayer *)seqp);

    voices = alHeapAlloc(heap, config->maxVoices, sizeof(*voices));
    for (i = 0; voices != NULL && i < config->maxVoices; i++) {
        voices[i].next = seqp->vFreeList;
        seqp->vFreeList = &voices[i];
    }

    events = alHeapAlloc(heap, config->maxEvents, sizeof(*events));
    alEvtqNew(&seqp->evtq, events, config->maxEvents);

    seqp->node.next = NULL;
    seqp->node.handler = native_csp_voice_handler;
    seqp->node.clientData = seqp;
    alSynAddPlayer(&alGlobals->drvr, &seqp->node);
}

s32 alCSPGetState(ALCSPlayer *seqp)
{
    if (seqp == NULL) {
        return AL_STOPPED;
    }

    return seqp->state;
}
