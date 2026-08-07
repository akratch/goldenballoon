/*
 * Clean-room native audio engine: shared sequence-player state.
 *
 * Voice-to-channel mapping, per-channel performance state, voice release and
 * oscillator teardown, and the player accessors the game code posts through.
 * Split out of audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

static void native_seqp_remove_event_for_voice(ALSeqPlayer *seqp, ALVoice *voice,
                                               s16 event_type)
{
    ALLink *node;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;
        ALEventListItem *next_item = (ALEventListItem *)next;
        ALVoice *event_voice = NULL;

        if (item->evt.type == AL_SEQP_ENV_EVT) {
            event_voice = item->evt.msg.vol.voice;
        } else if (item->evt.type == AL_NOTE_END_EVT) {
            event_voice = item->evt.msg.note.voice;
        }

        if (item->evt.type == event_type && event_voice == voice) {
            if (next_item != NULL) {
                next_item->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
        }

        node = next;
    }
}

void native_seqp_repost_event_item(ALEventQueue *evtq,
                                   ALEventListItem *item)
{
    ALLink *node;
    OSIntMask mask;

    if (evtq == NULL || item == NULL) {
        return;
    }

    item->node.next = NULL;
    item->node.prev = NULL;
    mask = osSetIntMask(OS_IM_NONE);

    for (node = &evtq->allocList; node != NULL; node = node->next) {
        ALEventListItem *next_item;

        if (node->next == NULL) {
            alLink((ALLink *)item, node);
            break;
        }

        next_item = (ALEventListItem *)node->next;
        if (item->delta < next_item->delta) {
            next_item->delta -= item->delta;
            alLink((ALLink *)item, node);
            break;
        }

        item->delta -= next_item->delta;
    }

    portAudioEventQueueNoteInserted(evtq);
    osSetIntMask(mask);
}

ALVoiceState *__mapVoice(ALSeqPlayer *seqp, u8 key, u8 vel, u8 channel)
{
    ALVoiceState *voice_state;

    if (seqp == NULL || seqp->vFreeList == NULL) {
        return NULL;
    }

    /*
     * DKR per-player polyphony cap (set_voice_limit), independent of the
     * synth's shared physical-voice pool. Exceeding it DROPS the note rather
     * than stealing — the sequence just loses that voice. The comparison is
     * deliberately `limit < mapped` rather than `mapped >= limit`, matching
     * the original's effective cap of voiceLimit + 1 simultaneous voices;
     * tightening it here would audibly thin dense passages.
     */
    if (seqp->voiceLimit < seqp->mappedVoices) {
        return NULL;
    }

    voice_state = seqp->vFreeList;
    seqp->vFreeList = voice_state->next;
    memset(voice_state, 0, sizeof(*voice_state));
    seqp->mappedVoices++;
    voice_state->channel = channel;
    voice_state->key = key;
    voice_state->velocity = vel;
    voice_state->voice.clientPrivate = voice_state;

    if (seqp->vAllocTail != NULL) {
        seqp->vAllocTail->next = voice_state;
    } else {
        seqp->vAllocHead = voice_state;
    }
    seqp->vAllocTail = voice_state;

    return voice_state;
}

void __unmapVoice(ALSeqPlayer *seqp, ALVoice *voice)
{
    ALVoiceState *prev = NULL;
    ALVoiceState *cur;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    for (cur = seqp->vAllocHead; cur != NULL; cur = cur->next) {
        if (&cur->voice == voice) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                seqp->vAllocHead = cur->next;
            }

            if (seqp->vAllocTail == cur) {
                seqp->vAllocTail = prev;
            }

            cur->next = seqp->vFreeList;
            seqp->vFreeList = cur;
            if (seqp->mappedVoices != 0) {
                seqp->mappedVoices--;
            }
            return;
        }

        prev = cur;
    }
}

ALVoiceState *__lookupVoice(ALSeqPlayer *seqp, u8 key, u8 channel)
{
    ALVoiceState *voice_state;

    if (seqp == NULL) {
        return NULL;
    }

    for (voice_state = seqp->vAllocHead; voice_state != NULL;
         voice_state = voice_state->next) {
        if (voice_state->key == key &&
            voice_state->channel == channel &&
            voice_state->phase != AL_PHASE_RELEASE &&
            voice_state->phase != AL_PHASE_SUSTREL) {
            return voice_state;
        }
    }

    return NULL;
}

ALSound *__lookupSound(ALSeqPlayer *seqp, u8 key, u8 vel, u8 chan)
{
    ALInstrument *instrument;
    s32 i;

    if (seqp == NULL || chan >= seqp->maxChannels ||
        CSP_CHAN(seqp) == NULL) {
        return NULL;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    if (instrument == NULL) {
        return NULL;
    }

    for (i = 0; i < instrument->soundCount; i++) {
        ALSound *sound = instrument->soundArray[i];
        ALKeyMap *keymap;

        if (sound == NULL || sound->keyMap == NULL) {
            continue;
        }

        keymap = sound->keyMap;
        if (key >= keymap->keyMin && key <= keymap->keyMax &&
            vel >= keymap->velocityMin && vel <= keymap->velocityMax) {
            return sound;
        }
    }

    return NULL;
}

ALSound *__lookupSoundQuick(ALSeqPlayer *seqp, u8 key, u8 vel, u8 chan)
{
    ALInstrument *instrument;
    s32 left;
    s32 right;

    if (seqp == NULL || chan >= seqp->maxChannels ||
        CSP_CHAN(seqp) == NULL) {
        return NULL;
    }

    instrument = CSP_CHAN(seqp)[chan].instrument;
    if (instrument == NULL || instrument->soundCount <= 0) {
        return NULL;
    }

    left = 0;
    right = instrument->soundCount - 1;
    while (left <= right) {
        s32 mid = (left + right) / 2;
        ALSound *sound = instrument->soundArray[mid];
        ALKeyMap *keymap;

        if (sound == NULL || sound->keyMap == NULL) {
            return __lookupSound(seqp, key, vel, chan);
        }

        keymap = sound->keyMap;
        if (key >= keymap->keyMin && key <= keymap->keyMax &&
            vel >= keymap->velocityMin && vel <= keymap->velocityMax) {
            return sound;
        }

        if (key < keymap->keyMin ||
            (vel < keymap->velocityMin && key <= keymap->keyMax)) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return NULL;
}

s16 __vsVol(ALVoiceState *voice_state, ALSeqPlayer *seqp)
{
    u32 note_gain;
    u32 mix_gain;

    if (voice_state == NULL || seqp == NULL || voice_state->sound == NULL ||
        voice_state->channel >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return 0;
    }

    note_gain = ((u32)voice_state->tremelo * voice_state->velocity *
                 voice_state->envGain) >> 6;
    mix_gain = ((u32)voice_state->sound->sampleVolume * (u16)seqp->vol *
                CSP_CHAN(seqp)[voice_state->channel].vol) >> 14;
    note_gain *= mix_gain;
    note_gain >>= 15;

    return (s16)note_gain;
}

ALMicroTime __vsDelta(ALVoiceState *voice_state, ALMicroTime t)
{
    ALMicroTime remaining;

    if (voice_state == NULL) {
        return AL_GAIN_CHANGE_TIME;
    }

    remaining = voice_state->envEndTime - t;
    return remaining >= 0 ? remaining : AL_GAIN_CHANGE_TIME;
}

ALPan __vsPan(ALVoiceState *voice_state, ALSeqPlayer *seqp)
{
    s32 pan;

    if (voice_state == NULL || seqp == NULL || voice_state->sound == NULL ||
        voice_state->channel >= seqp->maxChannels || CSP_CHAN(seqp) == NULL) {
        return AL_PAN_CENTER;
    }

    pan = CSP_CHAN(seqp)[voice_state->channel].pan -
          AL_PAN_CENTER + voice_state->sound->samplePan;
    if (pan < AL_PAN_LEFT) {
        pan = AL_PAN_LEFT;
    } else if (pan > AL_PAN_RIGHT) {
        pan = AL_PAN_RIGHT;
    }

    return (ALPan)pan;
}

void __seqpReleaseVoice(ALSeqPlayer *seqp, ALVoice *voice,
                        ALMicroTime delta_time)
{
    ALEvent event;
    ALVoiceState *voice_state;

    if (seqp == NULL || voice == NULL) {
        return;
    }

    voice_state = (ALVoiceState *)voice->clientPrivate;
    if (voice_state == NULL) {
        return;
    }

    if (voice_state->envPhase == AL_PHASE_ATTACK) {
        native_seqp_remove_event_for_voice(seqp, voice, AL_SEQP_ENV_EVT);
    }

    voice_state->velocity = 0;
    voice_state->envPhase = AL_PHASE_RELEASE;
    voice_state->envGain = 0;
    voice_state->envEndTime = seqp->curTime + delta_time;

    alSynSetPriority(seqp->drvr, voice, 0);
    alSynSetVol(seqp->drvr, voice, 0, delta_time);

    event.type = AL_NOTE_END_EVT;
    event.msg.note.voice = voice;
    alEvtqPostEvent(&seqp->evtq, &event, delta_time);
}

char __voiceNeedsNoteKill(ALSeqPlayer *seqp, ALVoice *voice,
                          ALMicroTime kill_time)
{
    ALLink *node;
    ALMicroTime event_time = 0;

    if (seqp == NULL || voice == NULL) {
        return FALSE;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;

        event_time += item->delta;
        if (item->evt.type == AL_NOTE_END_EVT &&
            item->evt.msg.note.voice == voice) {
            if (event_time <= kill_time) {
                return FALSE;
            }

            if (next != NULL) {
                ((ALEventListItem *)next)->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
            return TRUE;
        }

        node = next;
    }

    return TRUE;
}

void __resetPerfChanState(ALSeqPlayer *seqp, s32 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL ||
        chan < 0 || chan >= seqp->maxChannels) {
        return;
    }

    CSP_CHAN(seqp)[chan].fxId = AL_FX_NONE;
    CSP_CHAN(seqp)[chan].fxmix = AL_DEFAULT_FXMIX;
    CSP_CHAN(seqp)[chan].pan = AL_PAN_CENTER;
    CSP_CHAN(seqp)[chan].vol = AL_VOL_FULL;
    CSP_CHAN(seqp)[chan].priority = AL_DEFAULT_PRIORITY;
    CSP_CHAN(seqp)[chan].sustain = 0;
    CSP_CHAN(seqp)[chan].bendRange = 200;
    CSP_CHAN(seqp)[chan].pitchBend = 1.0f;
    /*
     * Both volume lanes start at full so their product is AL_VOL_FULL, which
     * is the reset volume above. The SGI original never initialised the raw
     * CC7 lane at all and relied on a CC7 arriving before any CC8; a sequence
     * that faded in first would multiply by uninitialised memory and could
     * silence the channel. Seeding both removes that ordering dependency.
     */
    CSP_CHAN(seqp)[chan].fade = AL_VOL_FULL;
    CSP_CHAN(seqp)[chan].unk11 = AL_VOL_FULL;
}

void __setInstChanState(ALSeqPlayer *seqp, ALInstrument *instrument, s32 chan)
{
    if (seqp == NULL || CSP_CHAN(seqp) == NULL || instrument == NULL ||
        chan < 0 || chan >= seqp->maxChannels) {
        return;
    }

    CSP_CHAN(seqp)[chan].instrument = instrument;
    CSP_CHAN(seqp)[chan].pan = instrument->pan;
    CSP_CHAN(seqp)[chan].vol = instrument->volume;
    CSP_CHAN(seqp)[chan].priority = instrument->priority;
    CSP_CHAN(seqp)[chan].bendRange = instrument->bendRange;
}

void __initChanState(ALSeqPlayer *seqp)
{
    s32 i;

    if (seqp == NULL || CSP_CHAN(seqp) == NULL) {
        return;
    }

    for (i = 0; i < seqp->maxChannels; i++) {
        CSP_CHAN(seqp)[i].instrument = NULL;
        __resetPerfChanState(seqp, i);
    }
}

void __initFromBank(ALSeqPlayer *seqp, ALBank *bank)
{
    ALInstrument *first_instrument = NULL;
    s32 i;

    if (seqp == NULL || bank == NULL || CSP_CHAN(seqp) == NULL) {
        return;
    }

    for (i = 0; i < bank->instCount && first_instrument == NULL; i++) {
        first_instrument = bank->instArray[i];
    }
    if (first_instrument == NULL) {
        return;
    }

    for (i = 0; i < seqp->maxChannels; i++) {
        __resetPerfChanState(seqp, i);
        __setInstChanState(seqp, first_instrument, i);
    }

    if (bank->percussion != NULL && seqp->maxChannels > 9) {
        __resetPerfChanState(seqp, 9);
        __setInstChanState(seqp, bank->percussion, 9);
    }
}

void __seqpStopOsc(ALSeqPlayer *seqp, ALVoiceState *voice_state)
{
    ALLink *node;

    if (seqp == NULL || voice_state == NULL) {
        return;
    }

    node = seqp->evtq.allocList.next;
    while (node != NULL) {
        ALLink *next = node->next;
        ALEventListItem *item = (ALEventListItem *)node;
        ALEventListItem *next_item = (ALEventListItem *)next;

        if ((item->evt.type == AL_TREM_OSC_EVT ||
             item->evt.type == AL_VIB_OSC_EVT) &&
            item->evt.msg.osc.vs == voice_state) {
            if (seqp->stopOsc != NULL) {
                seqp->stopOsc(item->evt.msg.osc.oscState);
            }
            if (next_item != NULL) {
                next_item->delta += item->delta;
            }
            alUnlink(node);
            alLink(node, &seqp->evtq.freeList);
            portAudioEventQueueNoteRemoved(&seqp->evtq);
        }

        node = next;
    }
}

void alSeqpSetBank(ALSeqPlayer *seqp, ALBank *bank)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_BANK_EVT;
    event.msg.spbank.bank = bank;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPPlay(ALCSPlayer *seqp)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_PLAY_EVT;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPSetSeq(ALCSPlayer *seqp, ALCSeq *seq)
{
    ALEvent event;

    if (seqp == NULL) {
        return;
    }

    event.type = AL_SEQP_SEQ_EVT;
    event.msg.spseq.seq = seq;
    alEvtqPostEvent(&seqp->evtq, &event, 0);
}

void alCSPSetVol(ALCSPlayer *seqp, s16 vol)
{
    ALVoiceState *voice;

    if (seqp == NULL) {
        return;
    }

    seqp->vol = vol;
    for (voice = seqp->vAllocHead; voice != NULL; voice = voice->next) {
        alSynSetVol(seqp->drvr,
                    &voice->voice,
                    __vsVol(voice, (ALSeqPlayer *)seqp),
                    __vsDelta(voice, seqp->curTime));
    }
}

void alCSPStop(ALCSPlayer *seqp)
{
    ALVoiceState *voice;

    if (seqp == NULL) {
        return;
    }

    for (voice = seqp->vAllocHead; voice != NULL;) {
        ALVoiceState *next = voice->next;

        if (seqp->drvr != NULL) {
            alSynStopVoice(seqp->drvr, &voice->voice);
            alSynFreeVoice(seqp->drvr, &voice->voice);
        }

        if (voice->flags != 0 && seqp->stopOsc != NULL) {
            __seqpStopOsc((ALSeqPlayer *)seqp, voice);
        }

        __unmapVoice((ALSeqPlayer *)seqp, &voice->voice);
        voice = next;
    }

    alEvtqFlush(&seqp->evtq);
    seqp->state = AL_STOPPED;
    seqp->nextDelta =
        seqp->frameTime > 0 ? seqp->frameTime : AL_USEC_PER_FRAME;
    seqp->nextEvent.type = AL_SEQP_API_EVT;
}
