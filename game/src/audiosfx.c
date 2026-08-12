#include "audiosfx.h"
#include "runtime_contracts.h"
#include "asset_loading.h"
#include "math_util.h"
#include "memory.h"
#include "objects.h"
#include "PR/libaudio.h"
#include "video.h"
#ifdef NATIVE_PORT
#include "rollback/rollback_game_runtime.h"
#endif

#define SOUND_PARAM_DURATION(m) (m->velocityMax * 33333)
#define SOUND_PARAM_NEXT_SOUND(m) (m->velocityMin + (m->keyMin & 0xC0) * 4)
#define SOUND_PARAM_GROUP(m) (m->keyMin & 0x3F)
#define SOUND_PARAM_FLAGS(m) (m->keyMax & 0xF0)
#define SOUND_PARAM_FXMIX(m) (m->keyMax & 0xF)
#define SOUND_PARAM_PITCH_SLIDE_RATE(m) (m->detune)
#define SOUND_PARAM_PITCH(m) (m->keyBase * 100 - 6000)

ALSoundStateLists gSoundStateLists = { NULL, NULL, NULL };
SoundPlayer gSoundPlayer;
SoundPlayer *gSoundPlayerPtr = &gSoundPlayer;
s32 gSoundGlobalVolume = 256;
s16 gNumActiveSounds = 0;
s16 *gSoundGroupVolume;
static u16 gSoundGroupCount;
/* `gSoundPlayerPtr` is a static address, so pointer non-NULL is not enough to
 * establish that its event queue exists. Settings may publish their selected
 * SFX scalar before audio_init() has completed; retain that scalar, but do not
 * walk/post into a partially constructed player. */
static s32 gSoundPlayerReady;
#ifdef NATIVE_PORT
static u64 sRollbackSoundGeneration;
enum {
    ROLLBACK_AUDIO_COMMAND_STOP = 1,
    ROLLBACK_AUDIO_COMMAND_DETACH,
    ROLLBACK_AUDIO_COMMAND_PRIORITY,
    ROLLBACK_AUDIO_COMMAND_PARAM_BASE = 0x10000
};

static void sndp_stop_raw(ALSoundState *state);
static void sndp_set_priority_raw(ALSoundState *sndp, u8 priority);
static void sndp_set_param_raw(
    SoundHandle soundMask, s16 type, u32 paramValue);

static void apply_rollback_audio_command(
    const MdkrRollbackAudioCommand *command, void *context) {
    SoundHandle handle = (SoundHandle)command->resource;
    (void)context;
    if (handle == NULL || handle->state == SOUND_STATE_NONE ||
        handle->rollbackGeneration != command->generation) {
        return;
    }
    if (command->operation == ROLLBACK_AUDIO_COMMAND_STOP) {
        sndp_stop_raw(handle);
    } else if (command->operation == ROLLBACK_AUDIO_COMMAND_DETACH) {
        sndp_stop_raw(handle);
        if (handle->userHandle == (SoundHandle *)command->handle_slot) {
            handle->userHandle = NULL;
        }
    } else if (command->operation == ROLLBACK_AUDIO_COMMAND_PRIORITY) {
        sndp_set_priority_raw(handle, (u8)command->value);
    } else if (command->operation >= ROLLBACK_AUDIO_COMMAND_PARAM_BASE) {
        sndp_set_param_raw(
            handle,
            (s16)(command->operation - ROLLBACK_AUDIO_COMMAND_PARAM_BASE),
            command->value);
    }
}

static bool defer_rollback_audio_command(
    SoundHandle handle, u32 operation, u32 value,
    SoundHandle *handlePtr) {
    MdkrRollbackAudioCommand command;
    if (handle == NULL) return false;
    command.resource = handle;
    command.generation = handle->rollbackGeneration;
    command.operation = operation;
    command.value = value;
    command.handle_slot = handlePtr;
    command.apply = apply_rollback_audio_command;
    command.context = NULL;
    return mdkr_rollback_game_runtime_defer_audio_command(&command);
}
/* App-overlay ducking is an output layer, not an authored group-volume mode.
 * Keeping it separate means a paused menu may update its desired group state
 * without making live race effects audible through the overlay. */
static u8 sSoundOverlayPaused;

static u16 sndp_effective_group_volume(u8 groupId, u16 authoredVolume) {
    if (sSoundOverlayPaused && groupId != 1) {
        return 0;
    }
    return authoredVolume;
}

static u16 sndp_group_volume_for_key(ALKeyMap *keyMap) {
    s32 groupId = SOUND_PARAM_GROUP(keyMap);
    if (gSoundGroupVolume == NULL || gSoundGroupCount == 0) {
        return sndp_effective_group_volume(
            (u8) groupId, AL_SNDP_GROUP_VOLUME_MAX);
    }
    if (mdkr_audio_group_valid(groupId) && groupId < gSoundGroupCount) {
        return sndp_effective_group_volume(
            (u8) groupId, gSoundGroupVolume[groupId]);
    }
    return sndp_effective_group_volume(0, gSoundGroupVolume[0]);
}
#else
static u16 sndp_group_volume_for_key(ALKeyMap *keyMap) {
    s32 groupId = SOUND_PARAM_GROUP(keyMap);
    if (gSoundGroupVolume == NULL || gSoundGroupCount == 0) {
        return AL_SNDP_GROUP_VOLUME_MAX;
    }
    if (mdkr_audio_group_valid(groupId) && groupId < gSoundGroupCount) {
        return gSoundGroupVolume[groupId];
    }
    return gSoundGroupVolume[0];
}
#endif

/**** Debug strings ****/
const char D_800E4AB0[] = "Bad soundState: voices =%d, states free =%d, states busy =%d, type %d data %x\n";
const char D_800E4B00[] = "playing a playing sound\n";
const char D_800E4B1C[] = "Nonsense sndp event\n";
const char D_800E4B34[] = "Sound state allocate failed - sndId %d\n";
const char D_800E4B5C[] = "Don't worry - game should cope OK\n";
const char D_800E4B80[] = "WARNING: Attempt to stop NULL sound aborted\n";

static void sndp_remove_events(ALEventQueue *, ALSoundState *, u16);
void func_80065A80(ALSynth *arg0, struct PVoice_s *arg1, s16 arg2);

/**
 * Sets the global volume level for all sounds.
 */
void sndp_set_global_volume(u32 volume) {
    OSIntMask mask;
    ALSoundState *state;
    ALSndpEvent evt;
#ifdef NATIVE_PORT
    if (!mdkr_rollback_game_runtime_presentation_allowed()) return;
#endif

    if (volume > 256) {
        volume = 256;
    }
    if ((s32)volume == gSoundGlobalVolume) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    gSoundGlobalVolume = volume;

    /* A pre-init update is intentionally remembered for voices created later.
     * There cannot yet be a live voice whose cached gain needs refreshing. */
    if (!gSoundPlayerReady) {
        osSetIntMask(mask);
        return;
    }

    /* Existing voices cache their computed gain. The original setter only
     * changed the scalar, so loops (most visibly vehicle engines) could ignore
     * a live settings change until some unrelated volume event arrived. A
     * dedicated coalesced event recomputes gain without overwriting the
     * per-sound volume or borrowing the authored group fade duration. */
    state = gSoundStateLists.allocHead;
    while (state != NULL) {
        sndp_remove_events(
            &gSoundPlayerPtr->evtq, state, AL_SNDP_GLOBAL_VOL_EVT);
        evt.common.type = AL_SNDP_GLOBAL_VOL_EVT;
        evt.common.state = state;
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *)&evt, 0);
        state = state->next;
    }
    osSetIntMask(mask);
}

#ifdef NATIVE_PORT
void sndp_set_overlay_pause(s32 paused) {
    OSIntMask mask;
    ALSoundState *state;
    ALSndpEvent evt;
    u8 next = paused ? TRUE : FALSE;

    if (sSoundOverlayPaused == next) {
        return;
    }
    mask = osSetIntMask(OS_IM_NONE);
    sSoundOverlayPaused = next;
    if (!gSoundPlayerReady) {
        osSetIntMask(mask);
        return;
    }
    state = gSoundStateLists.allocHead;
    while (state != NULL) {
        sndp_remove_events(
            &gSoundPlayerPtr->evtq, state, AL_SNDP_GLOBAL_VOL_EVT);
        evt.common.type = AL_SNDP_GLOBAL_VOL_EVT;
        evt.common.state = state;
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *)&evt, 0);
        state = state->next;
    }
    osSetIntMask(mask);
}

s32 sndp_overlay_pause_active(void) {
    return sSoundOverlayPaused != FALSE;
}

u16 sndp_get_effective_group_volume(u8 groupId) {
    return sndp_effective_group_volume(
        groupId, sndp_get_group_volume(groupId));
}
#endif

/**
 * Returns the global volume level applied to all sounds.
 * Official Name: gsSndpGetGlobalVolume
 */
s32 sndp_get_global_volume(void) {
    return gSoundGlobalVolume;
}

/**
 * Sets the number of active sound channels to either the passed number, or the maximum amount, whichever's lower.
 * Official Name: gsSndpLimitVoices
 */
void sndp_set_active_sound_limit(s32 numSounds) {
    if (gSoundPlayerPtr->maxSystemSoundChannels >= numSounds) {
        gSoundPlayerPtr->maxActiveSounds = numSounds;
    } else {
        gSoundPlayerPtr->maxActiveSounds = gSoundPlayerPtr->maxSystemSoundChannels;
    }
}

/**
 * Initialise a sound player and ready it for use with the sound event system.
 * Official Name: gsSndpNew
 */
void sndp_init_player(audioMgrConfig *c) {
    u32 i;
    ALSoundState *sounds;
    ALEvent evt;
    ALEventListItem *items;

    /* A launcher-owned process can boot more than one arena-backed engine.
     * Every pointer in these process globals belonged to the prior arena, so
     * retire the complete graph before allocating this epoch's player. Keep
     * only the scalar user volume and monotonic rollback generation. */
    gSoundPlayerReady = FALSE;
    gSoundStateLists.allocHead = NULL;
    gSoundStateLists.allocTail = NULL;
    gSoundStateLists.freeHead = NULL;
    gSoundGroupVolume = NULL;
    gSoundGroupCount = 0u;
    gNumActiveSounds = 0;
#ifdef NATIVE_PORT
    sSoundOverlayPaused = FALSE;
#endif
    *gSoundPlayerPtr = (SoundPlayer){0};

    /*
     * Init member variables
     */
    gSoundPlayerPtr->maxSystemSoundChannels = c->maxChannels;
    gSoundPlayerPtr->maxActiveSounds = c->maxChannels;
    gSoundPlayerPtr->lastSoundState = NULL;
    gSoundPlayerPtr->frameTime = 33000; // AL_USEC_PER_FRAME        /* time between API events */
    gSoundPlayerPtr->soundStatesArray = (ALSoundState *) alHeapAlloc(c->heap, 1, c->maxSounds * sizeof(ALSoundState));

    /*
     * init the event queue
     */
    items = (ALEventListItem *) alHeapAlloc(c->heap, 1, c->maxEvents * sizeof(ALEventListItem));
    alEvtqNew(&gSoundPlayerPtr->evtq, items, c->maxEvents);

    /*
     * Link all sound states into a linked list.
     */
    gSoundStateLists.freeHead = (ALSoundState *) gSoundPlayerPtr->soundStatesArray;
    for (i = 1; i < (u32) c->maxSounds; i++) {
        sounds = gSoundPlayerPtr->soundStatesArray;
        alLink((ALLink *) &sounds[i].next, (ALLink *) &(&sounds[i] - 1)->next);
    }

    /*
     * Allocate and initialize the per-group volume table.
     * Each group starts with maximum volume (32767).
     */
    /* alHeapAlloc(heap, count, size): every other call site here passes count 1
     * and the whole byte length as size. */
    gSoundGroupVolume = alHeapAlloc(c->heap, 1, c->numGroups * (s32) sizeof(*gSoundGroupVolume));
    gSoundGroupCount = c->numGroups;
    for (i = 0; i < c->numGroups; i++) {
        gSoundGroupVolume[i] = AL_SNDP_GROUP_VOLUME_MAX;
    }

    /*
     * add ourselves to the driver
     */
    gSoundPlayerPtr->drvr = &alGlobals->drvr;
    gSoundPlayerPtr->node.next = NULL;
    gSoundPlayerPtr->node.handler = sndp_voice_handler;
    gSoundPlayerPtr->node.clientData = gSoundPlayerPtr;
    alSynAddPlayer(gSoundPlayerPtr->drvr, &gSoundPlayerPtr->node);

    /*
     * Start responding to API events
     */
    evt.type = AL_SNDP_API_EVT;
    alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evt, gSoundPlayerPtr->frameTime);
    gSoundPlayerPtr->nextDelta = alEvtqNextEvent(&gSoundPlayerPtr->evtq, &gSoundPlayerPtr->nextEvent);
    gSoundPlayerReady = TRUE;
}

/**
 * Copy of _sndpVoiceHandler from libultra.
 * Processes all queued events that are due at the current time.
 */
ALMicroTime sndp_voice_handler(void *node) {
    SoundPlayer *sndp = (SoundPlayer *) node;
    ALSndpEvent evt;
    u32 eventType = AL_SNDP_API_EVT;

    do {
        switch (sndp->nextEvent.type) {
            case (AL_SNDP_API_EVT):
                evt.common.type = eventType;
                alEvtqPostEvent(&sndp->evtq, (ALEvent *) &evt, sndp->frameTime);
                break;

            default:
                sndp_handle_event(sndp, (ALSndpEvent *) &sndp->nextEvent);
                break;
        }
        sndp->nextDelta = alEvtqNextEvent(&sndp->evtq, &sndp->nextEvent);

#ifdef NATIVE_PORT
        /* Empty-queue guard -- same hazard as __CSPVoiceHandler (csplayer.c),
         * and slightly worse here because the `default:` arm would feed the
         * type = AL_EVTQ_EMPTY_EVT (-1) sentinel into sndp_handle_event() as if
         * it were a real event. An empty queue returns delta 0, which re-enters
         * this do/while with nothing that can ever repopulate the queue, so it
         * spins forever at 100% CPU (and returning 0 would only push the spin
         * out into __allocVoiceHandler's for-loop, which advances
         * client->samplesLeft by our return value). Re-arm the self-perpetuating
         * API heartbeat and leave with a nonzero delta instead: the player idles
         * one frame and stays responsive. See docs/OPEN_ITEMS.md. */
        if (sndp->nextEvent.type == AL_EVTQ_EMPTY_EVT) {
            sndp->nextEvent.type = AL_SNDP_API_EVT;
            sndp->nextDelta      = sndp->frameTime;
            break;
        }
#endif

    } while (sndp->nextDelta == 0);
    sndp->curTime += sndp->nextDelta;
    return sndp->nextDelta;
}

/**
 * Modified version of _handleEvent from libultra.
 * Processes the current event for the sound player.
 */
void sndp_handle_event(SoundPlayer *sndp, ALSndpEvent *event) {
    ALVoiceConfig config;
    ALVoice *voice;
    s32 delta;
    s32 limitReached;
    ALSndpEvent spAC;
    ALSndpEvent nextStateEvent;
    ALSound *sound;
    ALKeyMap *keyMap;
    s32 volume;
    s32 fxMix;
    s32 isEventForSingleSound;
    ALPan pan;
    s32 lastInSequence;
    s32 isVoiceAllocated;
    ALSoundState *soundState;
    ALSoundState *nextState;

    lastInSequence = TRUE;
    isVoiceAllocated = FALSE;
    soundState = NULL;
    nextState = NULL;

    do {
        if (nextState != NULL) {
            nextStateEvent.common.state = soundState;
            nextStateEvent.common.type = event->common.type;
            nextStateEvent.common.param = event->common.param;
            event = &nextStateEvent;
        }

        soundState = event->common.state;
        sound = soundState->sound;

        if (sound == NULL) {
            u16 numFree, numAlloc;
            sndp_get_state_counts(&numFree, &numAlloc);
            return;
        }

        keyMap = sound->keyMap;
        nextState = soundState->next;

        switch (event->common.type) {
            case AL_SNDP_PLAY_EVT:
                if (soundState->state != SOUND_STATE_INIT && soundState->state != SOUND_STATE_WAIT_VOICE) {
                    return;
                }

                config.fxBus = 0;
                config.priority = soundState->priority;
                config.unityPitch = 0;

                limitReached = sndp->maxActiveSounds <= gNumActiveSounds;

                if (!limitReached || (soundState->flags & SOUND_FLAG_RETRIGGER)) {
                    // Allocate a voice for the sound
                    // for retriggered sounds, ignore the limit
                    isVoiceAllocated = alSynAllocVoice(sndp->drvr, &soundState->voice, &config);
                }
                if (isVoiceAllocated) {
                    func_80065A80(sndp->drvr, soundState->voice.pvoice, 1);
                }

                voice = &soundState->voice;
                if (!isVoiceAllocated) {
                    // No free voices available, wait for another sound to stop.
                    if ((soundState->flags & (SOUND_FLAG_RETRIGGER | SOUND_FLAG_LOOPED)) || soundState->retries > 0) {
                        // Retry on the next frame.
                        // For looped and retriggered sounds, keep retrying on each frame.
                        soundState->state = SOUND_STATE_WAIT_VOICE;
                        soundState->retries--;
                        alEvtqPostEvent(&sndp->evtq, (ALEvent *) event, 33333);
                    } else {
                        // Not a looped or retriggered sound, and all retries have been exhausted.
                        if (limitReached) {
                            // Check if we can preempt a lower-priority sound.
                            ALSoundState *iterState = gSoundStateLists.allocTail;

                            do {
                                if (iterState->priority <= soundState->priority &&
                                    iterState->state != SOUND_STATE_PREEMPT) {
                                    // Found a lower-priority sound; it can be preempted
                                    ALSndpEvent interruptEvent;

                                    interruptEvent.common.type = AL_SNDP_END_EVT;
                                    interruptEvent.common.state = iterState;
                                    iterState->state = SOUND_STATE_PREEMPT;
                                    limitReached = FALSE;
                                    alEvtqPostEvent(&sndp->evtq, (ALEvent *) &interruptEvent, 1000);
                                    alSynSetVol(sndp->drvr, &iterState->voice, (soundState->state == 1) * 0,
                                                1000); // FAKE
                                }
                                iterState = iterState->prev;
                            } while (limitReached && iterState != NULL);

                            if (!limitReached) {
                                // Retry the sound that was preempted.
                                soundState->retries = 2;
                                alEvtqPostEvent(&sndp->evtq, (ALEvent *) event, 1001);
                            } else {
                                // No lower-priority sound to preempt, so stop the sound.
                                sndp_end(soundState);
                            }
                        } else {
                            // It seems the developers made a mistake with the logic here.
                            // Should we stop the sound immediately if the maximum number of sounds hasn't been reached?
                            // Perhaps it would be better to look for a sound to preempt, just like when the limit is
                            // reached. It's strange that we only check for sounds to preempt when the limit is reached,
                            // but not when it hasn't been.
                            sndp_end(soundState);
                        }
                    }
                    return;
                }

                soundState->flags |= SOUND_FLAG_PLAYING;
                alSynStartVoice(sndp->drvr, voice, sound->wavetable);
                soundState->state = SOUND_STATE_PLAYING;
                gNumActiveSounds++;

                // Set volume
                delta = sound->envelope->attackTime / soundState->pitch / soundState->slideMult;
                volume =
                    MAX(0, sndp_group_volume_for_key(keyMap) *
                                   (sound->envelope->attackVolume * soundState->volume * sound->sampleVolume / 16129) /
                                   AL_SNDP_GROUP_VOLUME_MAX -
                               1);
                volume = (u32) (volume * gSoundGlobalVolume) >> 8;
                alSynSetVol(sndp->drvr, &soundState->voice, 0, 0);
                alSynSetVol(sndp->drvr, &soundState->voice, volume, delta);

                // Set pan
                pan = MIN(MAX((soundState->pan + sound->samplePan - AL_PAN_CENTER), AL_PAN_LEFT), AL_PAN_RIGHT);
                alSynSetPan(sndp->drvr, &soundState->voice, pan);

                // Set pitch
                alSynSetPitch(sndp->drvr, &soundState->voice, soundState->pitch * soundState->slideMult);

                // Set FX mix
                //!@bug: SOUND_PARAM_FXMIX is allocated only four bits, so it needs to be multiplied by 8
                // to scale it to a range of 0 to 127.
                // However, it's unclear why soundState->fxmix also needs to be multiplied by 8.
                // The same issue appears in the AL_SNDP_FX_EVT handler.
                fxMix = (soundState->fxmix + SOUND_PARAM_FXMIX(keyMap)) * 8;
                fxMix = MIN(127, MAX(0, fxMix));
                alSynSetFXMix(sndp->drvr, &soundState->voice, fxMix);

                // Queue the decay event
                spAC.common.type = AL_SNDP_DECAY_EVT;
                spAC.common.state = soundState;
                alEvtqPostEvent(&sndp->evtq, (ALEvent *) &spAC,
                                sound->envelope->attackTime / soundState->pitch / soundState->slideMult);
                break;
            case AL_SNDP_RELEASE_EVT:
            case AL_SNDP_STOP_EVT:
            case AL_SNDP_RELEASE_NEXT_EVT:
                // If any sound in the composite sound is in the release phase, ignore this event for all other sounds
                // in the sequence, because they haven't started yet.
                // However, if the other sound is looped, process the event anyway.
                //
                // The purpose of checking for a looped sound seems unclear.
                // Does it imply that a composite sound can't contain looped simple sounds?
                // It seems logical, but the check may still be redundant.
                if (event->common.type != AL_SNDP_RELEASE_NEXT_EVT || (soundState->flags & SOUND_FLAG_LOOPED)) {
                    switch (soundState->state) {
                        case SOUND_STATE_PLAYING:
                            sndp_remove_events(&sndp->evtq, soundState, AL_SNDP_DECAY_EVT);
                            delta = sound->envelope->releaseTime / soundState->slideMult / soundState->pitch;
                            alSynSetVol(sndp->drvr, &soundState->voice, 0, delta);
                            if (delta != 0) {
                                spAC.common.type = AL_SNDP_END_EVT;
                                spAC.common.state = soundState;
                                alEvtqPostEvent(&sndp->evtq, (ALEvent *) &spAC, delta);
                                soundState->state = SOUND_STATE_STOPPING;
                            } else {
                                sndp_end(soundState);
                            }
                            break;
                        case SOUND_STATE_WAIT_VOICE:
                        case SOUND_STATE_INIT:
                            sndp_end(soundState);
                            break;
                    }
                    if (event->common.type == AL_SNDP_RELEASE_EVT) {
                        event->common.type = AL_SNDP_RELEASE_NEXT_EVT;
                    }
                }
                break;
            case AL_SNDP_PAN_EVT:
                soundState->pan = event->common.param;
                if (soundState->state == SOUND_STATE_PLAYING) {
                    pan = MIN(MAX((soundState->pan + sound->samplePan - AL_PAN_CENTER), AL_PAN_LEFT), AL_PAN_RIGHT);
                    alSynSetPan(sndp->drvr, &soundState->voice, pan);
                }
                break;
            case AL_SNDP_PITCH_EVT:
                soundState->pitch = *(f32 *) &event->common.param;
                if (soundState->state == SOUND_STATE_PLAYING) {
                    alSynSetPitch(sndp->drvr, &soundState->voice, soundState->pitch * soundState->slideMult);
                    if (soundState->flags & SOUND_FLAG_PITCH_SLIDE) {
                        sndp_apply_pitch_slide(soundState);
                    }
                }
                break;
            case AL_SNDP_FX_EVT:
                soundState->fxmix = event->common.param;
                if (soundState->state == SOUND_STATE_PLAYING) {
                    //!@bug: unnecessary multiplication by 8
                    // The same issue appears in the AL_SNDP_PLAY_EVT handler.
                    fxMix = (soundState->fxmix + SOUND_PARAM_FXMIX(keyMap)) * 8;
                    fxMix = MIN(127, MAX(0, fxMix));
                    alSynSetFXMix(sndp->drvr, &soundState->voice, fxMix);
                }
                break;
            case AL_SNDP_VOL_EVT:
                soundState->volume = event->common.param;
                if (soundState->state == SOUND_STATE_PLAYING) {
                    volume = MAX(
                        0, sndp_group_volume_for_key(keyMap) *
                                   (sound->envelope->decayVolume * soundState->volume * sound->sampleVolume / 16129) /
                                   AL_SNDP_GROUP_VOLUME_MAX -
                               1);
                    volume = (u32) (volume * gSoundGlobalVolume) >> 8;
                    alSynSetVol(sndp->drvr, &soundState->voice, volume, 1000);
                }
                break;
            case AL_SNDP_GROUP_VOL_EVT:
                if (soundState->state == SOUND_STATE_PLAYING) {
                    delta = sound->envelope->releaseTime / soundState->slideMult / soundState->pitch;
                    volume = MAX(
                        0, sndp_group_volume_for_key(keyMap) *
                                   (sound->envelope->decayVolume * soundState->volume * sound->sampleVolume / 16129) /
                                   AL_SNDP_GROUP_VOLUME_MAX -
                               1);
                    volume = (u32) (volume * gSoundGlobalVolume) >> 8;
                    alSynSetVol(sndp->drvr, &soundState->voice, volume, delta);
                }
                break;
            case AL_SNDP_GLOBAL_VOL_EVT:
                if (soundState->state == SOUND_STATE_PLAYING) {
                    volume = MAX(
                        0, sndp_group_volume_for_key(keyMap) *
                                   (sound->envelope->decayVolume *
                                    soundState->volume * sound->sampleVolume /
                                    16129) /
                                   AL_SNDP_GROUP_VOLUME_MAX -
                               1);
                    volume = (u32)(volume * gSoundGlobalVolume) >> 8;
                    alSynSetVol(
                        sndp->drvr, &soundState->voice, volume, 1000);
                }
                break;
            case AL_SNDP_DECAY_EVT:
                /*
                 * The voice has theoretically reached its attack velocity,
                 * set up callback for release envelope - except for a looped sound
                 */
                if (!(soundState->flags & SOUND_FLAG_LOOPED)) {
                    volume = MAX(
                        0, sndp_group_volume_for_key(keyMap) *
                                   (sound->envelope->decayVolume * soundState->volume * sound->sampleVolume / 16129) /
                                   AL_SNDP_GROUP_VOLUME_MAX -
                               1);
                    volume = (u32) (volume * gSoundGlobalVolume) >> 8;
                    delta = sound->envelope->decayTime / soundState->slideMult / soundState->pitch;
                    alSynSetVol(sndp->drvr, &soundState->voice, volume, delta);

                    spAC.common.type = AL_SNDP_RELEASE_EVT;
                    spAC.common.state = soundState;
                    alEvtqPostEvent(&sndp->evtq, (ALEvent *) &spAC, delta);

                    // Start applying the pitch slide only when the decay phase is reached.
                    if (soundState->flags & SOUND_FLAG_PITCH_SLIDE) {
                        sndp_apply_pitch_slide(soundState);
                    }
                }
                break;
            case AL_SNDP_END_EVT:
                sndp_end(soundState);
                break;
            case AL_SNDP_RETRIGGER_EVT:
                if (soundState->flags & SOUND_FLAG_RETRIGGER) {
                    // Start the new sound and copy the parameters from the old one
                    // to the new one.
                    ALSoundState *newSound =
                        sndp_play(event->retrigger.bank, event->retrigger.soundIndex, soundState->userHandle);
                    if (newSound != NULL) {
                        sndp_set_param(newSound, AL_SNDP_VOL_EVT, soundState->volume);
                        if (!event && !event) {} // Fake
                        sndp_set_param(newSound, AL_SNDP_PAN_EVT, soundState->pan);
                        sndp_set_param(newSound, AL_SNDP_FX_EVT, soundState->fxmix);
                        sndp_set_param(newSound, AL_SNDP_PITCH_EVT, *(s32 *) &soundState->pitch);
                    }
                }
                break;
            default:
                break;
        }

        soundState = nextState;
        isEventForSingleSound = event->common.type & (AL_SNDP_PLAY_EVT | AL_SNDP_PITCH_EVT | AL_SNDP_DECAY_EVT |
                                                      AL_SNDP_END_EVT | AL_SNDP_RETRIGGER_EVT);

        if (soundState != NULL && !isEventForSingleSound) {
            lastInSequence = soundState->flags & SOUND_FLAG_FINAL_IN_SEQUENCE;
        }

    } while (!lastInSequence && soundState != NULL && !isEventForSingleSound);
}

/**
 * Immediately stop the sound and release all associated resources.
 */
void sndp_end(ALSoundState *state) {
    if (state->flags & SOUND_FLAG_PLAYING) {
        alSynStopVoice(gSoundPlayerPtr->drvr, &state->voice);
        alSynFreeVoice(gSoundPlayerPtr->drvr, &state->voice);
    }
    sndp_deallocate(state);
    sndp_remove_events(&gSoundPlayerPtr->evtq, state, 0xFFFF);
}

/**
 * Changes the pitch slide and enqueues an event to continue the pitch slide on the next frame
 */
void sndp_apply_pitch_slide(ALSoundState *soundState) {
    ALSndpEvent evt;
    f32 pitch;

    pitch = alCents2Ratio(SOUND_PARAM_PITCH_SLIDE_RATE(soundState->sound->keyMap)) * soundState->pitch;
    evt.common.type = AL_SNDP_PITCH_EVT;
    evt.common.state = soundState;
    evt.common.param = *((s32 *) &pitch);
    alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evt, 33333);
}

/**
 * Modified version of _sndpRemoveEvents from libultra.
 * Removes all events associated with the given sound state, filtered by eventTypeMask, from the event queue.
 */
static void sndp_remove_events(ALEventQueue *evtq, ALSoundState *state, u16 eventTypeMask) {
    ALLink *thisNode;
    ALLink *nextNode;
    ALEventListItem *thisItem;
    ALEventListItem *nextItem;
    ALSndpEvent *thisEvent;
    OSIntMask mask;

    mask = osSetIntMask(OS_IM_NONE);

    thisNode = evtq->allocList.next;
    while (thisNode != 0) {
        nextNode = thisNode->next;
        thisItem = (ALEventListItem *) thisNode;
        nextItem = (ALEventListItem *) nextNode;
        thisEvent = (ALSndpEvent *) &thisItem->evt;
        if (thisEvent->common.state == state && (thisEvent->common.type & eventTypeMask)) {
            if (nextItem) {
                nextItem->delta += thisItem->delta;
            }
            alUnlink(thisNode);
            alLink(thisNode, &evtq->freeList);
        }
        thisNode = nextNode;
    }

    osSetIntMask(mask);
}

/**
 * Takes two pointers as arguments and writes the number of free and allocated sound states to them.
 * Additionally, it counts the number of allocated sound states in reverse order.
 * But why? It should be the same as in the forward order.
 * Official Name: getSoundStateCounts
 */
u16 sndp_get_state_counts(u16 *numFree, u16 *numAllocated) {
    OSIntMask mask;
    u16 allocatedCounter;
    u16 freeCounter;
    u16 allocatedRevCounter;
    ALSoundState *freePtr;
    ALSoundState *allocatedPtr;
    ALSoundState *allocatedRevPtr;

    mask = osSetIntMask(OS_IM_NONE);
    allocatedPtr = gSoundStateLists.allocHead;
    freePtr = gSoundStateLists.freeHead;
    allocatedRevPtr = gSoundStateLists.allocTail;

    for (allocatedCounter = 0; allocatedPtr != NULL; allocatedCounter++) {
        allocatedPtr = allocatedPtr->next;
    }

    for (freeCounter = 0; freePtr != NULL; freeCounter++) {
        freePtr = freePtr->next;
    }

    for (allocatedRevCounter = 0; allocatedRevPtr != NULL; allocatedRevCounter++) {
        allocatedRevPtr = allocatedRevPtr->prev;
    }

    *numFree = freeCounter;
    *numAllocated = allocatedCounter;

    osSetIntMask(mask);

    return allocatedRevCounter;
}

/**
 * Allocates a sound and sets its initial parameters,
 * including values from the ALKeyMap structure.
 * Returns NULL if there are no free sound states available.
 */
ALSoundState *sndp_allocate(UNUSED ALBank *bank, ALSound *sound) {
    s32 isLooping;
    ALKeyMap *keyMap;
    ALSoundState *state;
    u32 mask;
    s32 priority;

    state = gSoundStateLists.freeHead;
    keyMap = sound->keyMap;
    if (state != NULL) {
        mask = osSetIntMask(OS_IM_NONE);
        gSoundStateLists.freeHead = state->next;
        alUnlink((ALLink *) state);
        if (gSoundStateLists.allocHead != NULL) {
            state->next = gSoundStateLists.allocHead;
            state->prev = NULL;
            gSoundStateLists.allocHead->prev = state;
            gSoundStateLists.allocHead = state;
        } else {
            state->prev = NULL;
            state->next = NULL;
            gSoundStateLists.allocHead = state;
            gSoundStateLists.allocTail = state;
        }
        osSetIntMask(mask);

        isLooping = sound->envelope->decayTime == -1;
        priority = isLooping + AL_SNDP_PRIORITY_DEFAULT; // Looped sounds are assigned higher priority
        isLooping = 6000;                                // required to match
        state->priority = priority;
        state->state = SOUND_STATE_INIT;
        state->retries = 2;
        state->sound = sound;
        state->pitch = 1.0f;
        state->flags = SOUND_PARAM_FLAGS(keyMap);
        state->userHandle = NULL;
#ifdef NATIVE_PORT
        sRollbackSoundGeneration++;
        if (sRollbackSoundGeneration == 0) sRollbackSoundGeneration++;
        state->rollbackGeneration = sRollbackSoundGeneration;
#endif
        if (state->flags & SOUND_FLAG_PITCH_SLIDE) {
            state->slideMult = alCents2Ratio(SOUND_PARAM_PITCH(keyMap));
        } else {
            state->slideMult = alCents2Ratio(SOUND_PARAM_PITCH(keyMap) + SOUND_PARAM_PITCH_SLIDE_RATE(keyMap));
        }
        if (priority != AL_SNDP_PRIORITY_DEFAULT) {
            state->flags |= SOUND_FLAG_LOOPED;
        }
        state->fxmix = 0;
        state->pan = AL_PAN_CENTER;
        state->volume = 0x7FFF;
    }
    return state;
}

/**
 * Deallocates a sound state and returns it to the free list.
 * If the sound state is currently playing, it decrements the active sound count.
 * If the sound state has a user handle, it sets it to NULL, to notify the user that the sound has stopped.
 */
void sndp_deallocate(ALSoundState *state) {
    if (state == gSoundStateLists.allocHead) {
        gSoundStateLists.allocHead = state->next;
    }
    if (state == gSoundStateLists.allocTail) {
        gSoundStateLists.allocTail = state->prev;
    }
    alUnlink((ALLink *) state);
    if (gSoundStateLists.freeHead != NULL) {
        state->next = gSoundStateLists.freeHead;
        state->prev = NULL;
        gSoundStateLists.freeHead->prev = state;
        gSoundStateLists.freeHead = state;
    } else {
        state->prev = NULL;
        state->next = NULL;
        gSoundStateLists.freeHead = state;
    }
    if (state->flags & SOUND_FLAG_PLAYING) {
        gNumActiveSounds--;
    }
    state->state = SOUND_STATE_NONE;
    if (state->userHandle != NULL) {
        if (state == *state->userHandle) {
            *state->userHandle = NULL;
        }
        state->userHandle = NULL;
    }
}

#ifdef NATIVE_PORT
/**
 * Stop a sound and sever the sound state's back-pointer to the caller's handle
 * slot.
 *
 * sndp_stop() only posts an event; the state stays allocated until the sound
 * player drains the queue, and sndp_deallocate() then writes NULL back through
 * userHandle. Clearing only the caller's own copy of the handle leaves that
 * back-pointer aimed at the slot, so a caller that frees the memory holding it
 * hands the audio player a write into freed storage. Callers that are about to
 * release such memory must use this instead of a bare sndp_stop().
 */
void sndp_stop_and_detach(SoundHandle *handlePtr) {
    ALSoundState *state;

    if (handlePtr == NULL) {
        return;
    }
    state = *handlePtr;
    if (state == NULL) {
        return;
    }
    /* Sever the back-pointer and the caller's slot NOW, unconditionally.
     * Detaching is a memory-safety contract with a caller that is about to
     * free the storage holding the slot; only the audible stop may be
     * deferred for resimulation. A deferred command that is later DISCARDED
     * (rollback forget_all) never runs its apply callback, so deferring the
     * detach itself would leave the live voice's userHandle aimed into freed
     * memory and sndp_deallocate() would write through it at voice end. */
    if (state->userHandle == handlePtr) {
        state->userHandle = NULL;
    }
    *handlePtr = NULL;
    if (defer_rollback_audio_command(
            state, ROLLBACK_AUDIO_COMMAND_DETACH, 0u, NULL)) {
        return;
    }
    sndp_stop(state);
}

u64 sndp_rollback_generation(SoundHandle handle) {
    return handle != NULL ? handle->rollbackGeneration : 0;
}

/** Stop only the exact host voice previewed by a rollback event.
 *
 * ALSoundState comes from a fixed pool. A pointer alone is therefore subject
 * to ABA reuse after a short one-shot ends; generation makes cancellation a
 * no-op instead of stopping the unrelated replacement voice. */
void sndp_stop_rollback_voice(
    SoundHandle handle, u64 generation, SoundHandle *handlePtr) {
    if (handle == NULL || generation == 0 ||
        handle->rollbackGeneration != generation ||
        handle->state == SOUND_STATE_NONE) {
        return;
    }
    sndp_stop(handle);
    if (handle->userHandle == handlePtr) handle->userHandle = NULL;
    if (handlePtr != NULL && *handlePtr == handle) *handlePtr = NULL;
}
#endif

/**
 * Sets the priority for voice allocation and for preempting another sound.
 * Official Name: gsSndpSetPriority
 */
static void sndp_set_priority_raw(ALSoundState *sndp, u8 priority) {
    if (sndp != NULL) sndp->priority = priority;
}

void sndp_set_priority(ALSoundState *sndp, u8 priority) {
#ifdef NATIVE_PORT
    if (defer_rollback_audio_command(
            sndp, ROLLBACK_AUDIO_COMMAND_PRIORITY, priority, NULL)) return;
#endif
    sndp_set_priority_raw(sndp, priority);
}

/**
 * Retrieves the current sound state, refer to the SoundStates enum.
 * Official Name: gsSndpGetState
 */
UNUSED u8 sndp_get_state(ALSoundState *sndp) {
    if (sndp != NULL) {
        return sndp->state;
    } else {
        return SOUND_STATE_NONE;
    }
}

/**
 * Plays the sound with the default priority, store the handle in the provided pointer.
 */
ALSoundState *sndp_play(ALBank *bnk, s16 sndIndx, ALSoundState **handlePtr) {
    return sndp_play_with_priority(bnk, sndIndx, 0, handlePtr);
}

/**
 * Plays the sound with the given priority.
 * For composite sounds, creates all the individual sounds in the sequence to start playing at the correct times,
 * though this part of the code contains bugs.
 * Also, for retriggered sounds, schedules an event to restart them.
 */
ALSoundState *sndp_play_with_priority(ALBank *bank, s16 sndIndx, u8 priority, ALSoundState **handlePtr) {
    ALSound *sound;
    ALSoundState *lastSoundState;
    ALSoundState *soundState;
    ALMicroTime totalDuration;
    s16 retriggeredSoundID;
    ALMicroTime retriggerInterval;
    ALMicroTime duration;
    ALKeyMap *keyMap;

    lastSoundState = NULL;
    retriggeredSoundID = 0;
    totalDuration = 0;

    if (sndIndx == SOUND_NONE) {
        return NULL;
    }

    do {
#ifdef NATIVE_PORT
        /* sndIndx is 1-based into instArray[0]->soundArray. The first index
         * comes from a caller that has already validated it, but every
         * subsequent one is SOUND_PARAM_NEXT_SOUND() out of the previous
         * sound's keymap -- a 10-bit ROM field, so up to 1023 regardless of how
         * many sounds the bank actually holds. Stop the chain instead of
         * indexing past soundArray. */
        if (!mdkr_sound_id_valid((u32) (sndIndx - 1), (u32) bank->instArray[0]->soundCount)) {
            break;
        }
#endif
        sound = bank->instArray[0]->soundArray[sndIndx - 1];
        soundState = sndp_allocate(bank, sound);
        if (soundState != NULL) {
            ALSndpEvent playEvent;

            gSoundPlayerPtr->lastSoundState = soundState;

            playEvent.common.type = AL_SNDP_PLAY_EVT;
            playEvent.common.state = soundState;
            duration = SOUND_PARAM_DURATION(sound->keyMap);

            // zero priority means default priority which is set in sndp_allocate
            if (priority != 0) {
                soundState->priority = priority;
            }

            if (soundState->flags & SOUND_FLAG_RETRIGGER) {
                //!@bug probably:
                // If the sequence contains multiple retriggered sounds, only the last one will be retriggered.
                // Also, there seems to be confusion between durations, intervals, etc.
                // Needs verification, but SOUND_PARAM_DURATION likely refers to the duration of each individual sound.
                // If so, then each sound should begin with a delay of `totalDuration` (which is handled correctly
                // here). And if `retriggerInterval` is set to duration + 1, the retriggering will occur at correct
                // intervals, but the first retrigger will happen at the wrong time if the retriggered sound is part of
                // a composite sound. Possibly, the developers didn't intend for retriggered sounds to be part of
                // composite sequences — but that's unclear.
                soundState->flags &= ~SOUND_FLAG_RETRIGGER;
                alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &playEvent, totalDuration + 1);
                retriggerInterval = duration + 1;
                retriggeredSoundID = sndIndx;
            } else {
                //!@bug: Duration handling is definitely incorrect here.
                // A delay of totalDuration + 1 should be used, as done above,
                // otherwise sounds will start at the wrong time.
                alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &playEvent, duration + 1);
            }

            lastSoundState = soundState;
        }

        // Potential UB: 'duration' may be uninitialized, but loop exits before `totalDuration` is used,
        // so this has no observable effect
        totalDuration += duration;

        keyMap = sound->keyMap;
        sndIndx = SOUND_PARAM_NEXT_SOUND(keyMap);

    } while (sndIndx != SOUND_NONE && soundState != NULL);

    if (lastSoundState != NULL) {
        lastSoundState->flags |= SOUND_FLAG_FINAL_IN_SEQUENCE;

        // Only the last sound in the sequence stores a pointer to the user handle.
        lastSoundState->userHandle = handlePtr;

        // Schedule the retrigger event.
        if (retriggeredSoundID != SOUND_NONE) {
            ALSndpEvent evtRetrigger;
            lastSoundState->flags |= SOUND_FLAG_RETRIGGER;
            evtRetrigger.retrigger.type = AL_SNDP_RETRIGGER_EVT;
            evtRetrigger.retrigger.state = lastSoundState;
            evtRetrigger.retrigger.soundIndex = retriggeredSoundID;
            evtRetrigger.retrigger.bank = bank;
            alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evtRetrigger, retriggerInterval);
        }
    }

    if (handlePtr != NULL) {
        *handlePtr = lastSoundState;
    }
    return lastSoundState;
}

/**
 * Schedules a stop event for the given sound state and disables the retrigger flag.
 * Official Name: gsSndpStop
 */
static void sndp_stop_raw(ALSoundState *state) {
    ALSndpEvent alEvent;

    alEvent.common.type = AL_SNDP_STOP_EVT;
    alEvent.common.state = state;
    if (state != NULL) {
        state->flags &= ~SOUND_FLAG_RETRIGGER;
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &alEvent, 0);
    } else {
        // From JFG
        // osSyncPrintf("WARNING: Attempt to stop NULL sound aborted\n");
    }
}

void sndp_stop(ALSoundState *state) {
#ifdef NATIVE_PORT
    if (defer_rollback_audio_command(
            state, ROLLBACK_AUDIO_COMMAND_STOP, 0u, NULL)) return;
#endif
    sndp_stop_raw(state);
}

/**
 * Schedules a stop event for all sound states that have the specified flags set.
 */
void sndp_stop_with_flags(u8 flags) {
    OSIntMask mask;
    ALSndpEvent evt;
    ALSoundState *soundState;

#ifdef NATIVE_PORT
    if (!mdkr_rollback_game_runtime_presentation_allowed()) return;
#endif

    mask = osSetIntMask(OS_IM_NONE);
    soundState = gSoundStateLists.allocHead;
    while (soundState != NULL) {
        evt.common.type = AL_SNDP_STOP_EVT;
        evt.common.state = soundState;
        if ((soundState->flags & flags) == flags) {
            evt.common.state->flags &= ~SOUND_FLAG_RETRIGGER;
            alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evt, 0);
        }
        soundState = soundState->next;
    }
    osSetIntMask(mask);
}

/**
 * Stops all sounds from playing.
 * Official Name: gsSndpStopAll
 */
UNUSED void sndp_stop_all(void) {
    sndp_stop_with_flags(SOUND_FLAG_FINAL_IN_SEQUENCE);
}

/**
 * Stops all retriggered sounds from playing.
 * Official Name: gsSndpStopAllRetrigger
 */
UNUSED void sndp_stop_all_retrigger(void) {
    sndp_stop_with_flags(SOUND_FLAG_FINAL_IN_SEQUENCE | SOUND_FLAG_RETRIGGER);
}

/**
 * Stops all looped sounds from playing.
 * Official Name: gsSndpStopAllLooped
 */
void sndp_stop_all_looped(void) {
    sndp_stop_with_flags(SOUND_FLAG_FINAL_IN_SEQUENCE | SOUND_FLAG_LOOPED);
}

/**
 * Send a message to the sound player to update an existing property of the sound entry.
 * Official Name: gsSndpSetParam
 */
static void sndp_set_param_raw(
    SoundHandle soundMask, s16 type, u32 paramValue) {
    ALSndpEvent evt;
    evt.common.type = type;
    evt.common.state = soundMask;
    evt.common.param = paramValue;
    if (soundMask != NULL) {
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evt, 0);
    } else {
        // From JFG
        // osSyncPrintf("WARNING: Attempt to modify NULL sound aborted\n");
    }
}

void sndp_set_param(SoundHandle soundMask, s16 type, u32 paramValue) {
#ifdef NATIVE_PORT
    if (defer_rollback_audio_command(
            soundMask, ROLLBACK_AUDIO_COMMAND_PARAM_BASE + (u16)type,
            paramValue, NULL)) return;
#endif
    sndp_set_param_raw(soundMask, type, paramValue);
}

/**
 * Returns the volume level of the channel ID.
 * Official Name: gsSndpGetMasterVolume
 */
u16 sndp_get_group_volume(u8 groupID) {
    if (!mdkr_audio_group_valid(groupID) || groupID >= gSoundGroupCount) {
        return AL_SNDP_GROUP_VOLUME_MAX;
    }
    return gSoundGroupVolume[groupID];
}

/**
 * Sets the volume for the specified group and updates the volume of all sounds in that group.
 *
 * !@bug: No bounds checking is performed on the group index. In DKR, only one group is defined and memory is allocated
 * for a single group. This leads to out-of-bounds access, which can cause undefined behavior, including potential
 * crashes.
 *
 * Official Name: gsSndpSetMasterVolume
 */
void sndp_set_group_volume(u8 groupID, u16 volume) {
    OSIntMask mask;
    ALSoundState *state;
    UNUSED s32 pad;
    ALSndpEvent evt;

#ifdef NATIVE_PORT
    if (!mdkr_rollback_game_runtime_presentation_allowed()) return;
#endif

    mask = osSetIntMask(OS_IM_NONE);
    if (!mdkr_audio_group_valid(groupID) || groupID >= gSoundGroupCount) {
        osSetIntMask(mask);
        return;
    }
    state = gSoundStateLists.allocHead;
    gSoundGroupVolume[groupID] = volume;

    while (state != NULL) {
        if (SOUND_PARAM_GROUP(state->sound->keyMap) == groupID) {
            evt.common.type = AL_SNDP_GROUP_VOL_EVT;
            evt.common.state = state;
            alEvtqPostEvent(&gSoundPlayerPtr->evtq, (ALEvent *) &evt, 0);
        }
        state = state->next;
    }

    osSetIntMask(mask);
}
