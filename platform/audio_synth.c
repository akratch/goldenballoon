/*
 * Clean-room native audio engine: the synthesis driver.
 *
 * Voice allocation and stealing, the parameter-update queues, the per-frame
 * command-list build, the heap, and the alSyn* voice API the players drive.
 * Split out of audio_compat.c; see audio_compat_internal.h.
 */

#include "audio_compat_internal.h"

/*
 * SFX/CSP voice reverb-routing trace (MDKR_AUDIO_VOICE_TRACE_JSONL).
 *
 * Two event kinds share one JSONL stream:
 *
 *   "fxmix" — emitted whenever a voice's FX (reverb) mix is set via
 *   alSynSetFXMix (the SFX player per-sound, and the CSP player for MIDI
 *   reverb control changes). Records which aux bus the voice's envmixer is
 *   currently a source of, the wet-send amount, and how many aux buses the
 *   synth built. tests/check_cave_reverb_bus.py reads it.
 *
 *   "music" — emitted from alSynStartVoiceParams (the CSP/music note-on start
 *   path, which the SFX player never uses) with the same bus/max_aux fields, so
 *   music voices' bus membership is observable even though they never call
 *   alSynSetFXMix. tests/check_music_bus_isolation.py reads it.
 *
 * Music (CSP) voices stay on bus 0 (AL_FX_CUSTOM); SFX voices are re-parented
 * to bus 1 (AL_FX_BIGROOM) for the cave/tunnel echo, so on a reverb-line route
 * a fxmix>0 voice only appears on bus 1 once the second aux bus exists.
 */
#ifdef NATIVE_PORT
static FILE *s_native_voice_trace_fp = NULL;
static int s_native_voice_trace_init = 0;

static FILE *native_voice_trace_fp(void)
{
    const char *path;

    if (s_native_voice_trace_init) {
        return s_native_voice_trace_fp;
    }

    s_native_voice_trace_init = 1;
    path = getenv("MDKR_AUDIO_VOICE_TRACE_JSONL");
    if (path != NULL && *path != '\0') {
        s_native_voice_trace_fp = mdkr_fopen_utf8(path, "w");
    }

    return s_native_voice_trace_fp;
}

static void native_voice_trace_fxmix(ALSynth *s, ALVoice *voice, u8 fxmix)
{
    FILE *fp = native_voice_trace_fp();

    if (fp == NULL || voice == NULL || voice->pvoice == NULL) {
        return;
    }

    fprintf(fp,
            "{\"event\":\"fxmix\",\"bus\":%d,\"fxmix\":%d,\"max_aux\":%d}\n",
            (s32)voice->pvoice->unkDC, (s32)fxmix,
            s != NULL ? s->maxAuxBusses : -1);
}

static void native_voice_trace_music(ALSynth *s, ALVoice *voice, u8 fxmix)
{
    FILE *fp = native_voice_trace_fp();

    if (fp == NULL || voice == NULL || voice->pvoice == NULL) {
        return;
    }

    fprintf(fp,
            "{\"event\":\"music\",\"bus\":%d,\"fxmix\":%d,\"max_aux\":%d}\n",
            (s32)voice->pvoice->unkDC, (s32)fxmix,
            s != NULL ? s->maxAuxBusses : -1);
}
#else
#define native_voice_trace_fxmix(s, voice, fxmix) ((void)0)
#define native_voice_trace_music(s, voice, fxmix) ((void)0)
#endif

static void enqueue_voice_update(ALVoice *voice, ALParam *update)
{
    ALFilter *filter;

    if (voice == NULL || voice->pvoice == NULL || update == NULL) {
        if (update != NULL) {
            __freeParam(update);
        }
        return;
    }

    filter = voice->pvoice->channelKnob;
    if (filter == NULL || filter->setParam == NULL) {
        __freeParam(update);
        return;
    }

    filter->setParam(filter, AL_FILTER_ADD_UPDATE, update);
}

static void enqueue_filter_update(ALFilter *filter, ALParam *update)
{
    if (filter == NULL || filter->setParam == NULL || update == NULL) {
        if (update != NULL) {
            __freeParam(update);
        }
        return;
    }

    filter->setParam(filter, AL_FILTER_ADD_UPDATE, update);
}

static ALParam *alloc_voice_update(ALSynth *synth, ALVoice *voice, s16 type)
{
    ALParam *update;

    if (synth == NULL || voice == NULL || voice->pvoice == NULL) {
        return NULL;
    }

    update = __allocParam();
    if (update == NULL) {
        return NULL;
    }

    update->next = NULL;
    update->delta = synth->paramSamples + voice->pvoice->offset;
    update->type = type;
    return update;
}

static PVoice *take_first_voice(ALLink *from, ALLink *to)
{
    ALLink *node;

    if (from == NULL || to == NULL || from->next == NULL) {
        return NULL;
    }

    node = from->next;
    alUnlink(node);
    alLink(node, to);
    return (PVoice *)node;
}

static PVoice *find_stealable_voice(ALSynth *synth, s16 priority)
{
    ALLink *node;
    PVoice *candidate = NULL;

    if (synth == NULL) {
        return NULL;
    }

    for (node = synth->pAllocList.next; node != NULL; node = node->next) {
        PVoice *physical = (PVoice *)node;

        if (physical->vvoice != NULL &&
            physical->vvoice->priority <= priority &&
            physical->offset == 0) {
            candidate = physical;
            priority = physical->vvoice->priority;
        }
    }

    return candidate;
}

static s32 native_time_to_samples_no_round(ALSynth *synth, s32 micros)
{
    f32 samples;

    if (synth == NULL || synth->outputRate <= 0) {
        return 0;
    }

    samples = ((f32)micros * (f32)synth->outputRate) / 1000000.0f + 0.5f;
    return (s32)samples;
}

static s32 native_next_sample_time(ALSynth *synth, ALPlayer **client)
{
    ALPlayer *current;
    ALMicroTime best_delta = 0x7fffffff;

    if (client != NULL) {
        *client = NULL;
    }

    if (synth == NULL || synth->head == NULL || client == NULL) {
        return 0x7fffffff;
    }

    for (current = synth->head; current != NULL; current = current->next) {
        ALMicroTime delta = current->samplesLeft - synth->curSamples;

        if (delta < best_delta) {
            best_delta = delta;
            *client = current;
        }
    }

    return *client != NULL ? (*client)->samplesLeft : 0x7fffffff;
}


void alSynNew(ALSynth *synth, ALSynConfig *config)
{
    ALHeap *heap;
    ALSave *save;
    PVoice *voices;
    ALFilter **sources;
    ALParam *params;
    s32 num_busses;
    s32 i;

    if (synth == NULL || config == NULL || config->heap == NULL) {
        return;
    }

    memset(synth, 0, sizeof(*synth));
    heap = config->heap;

    if (config->maxPVoices <= 0 || config->maxUpdates <= 0 ||
        config->outputRate <= 0) {
        return;
    }

    synth->numPVoices = config->maxPVoices;
    synth->outputRate = config->outputRate;
    synth->maxOutSamples = AL_MAX_RSP_SAMPLES;
    synth->dma = (ALDMANew)config->dmaproc;
    synth->heap = heap;

    save = alHeapAlloc(heap, 1, (s32)sizeof(*save));
    if (save == NULL) {
        return;
    }

    alSaveNew(save);
    synth->outputFilter = (ALFilter *)save;

    /*
     * DKR declares two FX buses in ALSynConfig.fxType[] (game/src/audio.c):
     * bus 0 = AL_FX_CUSTOM (the ROM music reverb, which every CSP voice keeps)
     * and bus 1 = AL_FX_BIGROOM (the big-room cave echo every SFX voice is
     * re-parented onto). The port used to build a single aux bus and hardcode
     * maxAuxBusses = 1, which silently rejected the SFX re-parent to bus 1
     * (audio_compat.c) and left the cave SFX on the music reverb. Build one aux
     * bus per configured fxType[] entry instead — for DKR that is the two
     * non-AL_FX_NONE effects — and give each its own per-bus source array.
     */
    num_busses = (s32)(sizeof(config->fxType) / sizeof(config->fxType[0]));

    synth->auxBus = alHeapAlloc(heap, num_busses, sizeof(*synth->auxBus));
    synth->mainBus = alHeapAlloc(heap, 1, sizeof(*synth->mainBus));
    if (synth->auxBus == NULL || synth->mainBus == NULL) {
        return;
    }

    synth->maxAuxBusses = num_busses;
    for (i = 0; i < num_busses; i++) {
        sources = alHeapAlloc(heap, config->maxPVoices, sizeof(*sources));
        if (sources == NULL) {
            return;
        }
        alAuxBusNew(&synth->auxBus[i], sources, config->maxPVoices);
    }

    sources = alHeapAlloc(heap, config->maxPVoices, sizeof(*sources));
    if (sources == NULL) {
        return;
    }
    alMainBusNew(synth->mainBus, sources, config->maxPVoices);

    /*
     * An FX-carrying bus gets its delay-line unit (which pulls the wet send
     * from the aux bus and folds the reverb back into the main bus); an
     * AL_FX_NONE bus is wired straight to the main bus. Mirrors stock alSynNew.
     */
    for (i = 0; i < num_busses; i++) {
        if (config->fxType[i] != AL_FX_NONE) {
            alSynAllocFX(synth, (s16)i, config, heap);
        } else {
            alMainBusParam(synth->mainBus, AL_FILTER_ADD_SOURCE,
                           &synth->auxBus[i]);
        }
    }

    voices = alHeapAlloc(heap, config->maxPVoices, sizeof(*voices));
    if (voices == NULL) {
        return;
    }
    for (i = 0; i < config->maxPVoices; i++) {
        PVoice *physical = &voices[i];

        alLink((ALLink *)physical, &synth->pFreeList);
        physical->vvoice = NULL;
        physical->offset = 0;

        alLoadNew(&physical->decoder, synth->dma, heap);
        alLoadParam(&physical->decoder, AL_FILTER_SET_SOURCE, NULL);

        alResampleNew(&physical->resampler, heap);
        alResampleParam(&physical->resampler, AL_FILTER_SET_SOURCE,
                        &physical->decoder);

        alEnvmixerNew(&physical->envmixer, heap);
        alEnvmixerParam(&physical->envmixer, AL_FILTER_SET_SOURCE,
                        &physical->resampler);

        alAuxBusParam(synth->auxBus, AL_FILTER_ADD_SOURCE,
                      &physical->envmixer);
        physical->channelKnob = (ALFilter *)&physical->envmixer;
    }

    alSaveParam(save, AL_FILTER_SET_SOURCE, synth->mainBus);

    params = alHeapAlloc(heap, config->maxUpdates, sizeof(ALStartParamAlt));
    if (params == NULL) {
        return;
    }
    synth->paramList = NULL;
    for (i = 0; i < config->maxUpdates; i++) {
        ALParam *param =
            (ALParam *)((u8 *)params + (size_t)i * sizeof(ALStartParamAlt));

        param->next = synth->paramList;
        synth->paramList = param;
    }
}

Acmd *alAudioFrame(Acmd *cmd_list, s32 *cmd_len, s16 *out_buf, s32 out_len)
{
    ALSynth *synth;
    Acmd *cmd_end = cmd_list;
    s16 tmp = 0;

    if (cmd_len == NULL) {
        return cmd_list;
    }

    *cmd_len = 0;
    if (alGlobals == NULL || cmd_list == NULL || out_buf == NULL ||
        out_len <= 0) {
        return cmd_list;
    }

    synth = &alGlobals->drvr;
    if (synth->head == NULL || synth->outputFilter == NULL ||
        synth->outputRate <= 0) {
        return cmd_list;
    }

    for (;;) {
        ALPlayer *client = NULL;

        synth->paramSamples = native_next_sample_time(synth, &client);
        if (client == NULL || client->handler == NULL ||
            synth->paramSamples - synth->curSamples >= out_len) {
            break;
        }

        synth->paramSamples &= ~0xf;
        client->samplesLeft += native_time_to_samples_no_round(
            synth, client->handler(client));
    }

    synth->paramSamples &= ~0xf;

    while (out_len > 0) {
        ALFilter *output = synth->outputFilter;
        Acmd *cmd_ptr = cmd_end;
        s32 chunk_samples = out_len;

        if (chunk_samples > synth->maxOutSamples) {
            chunk_samples = synth->maxOutSamples;
        }

        aSegment(cmd_ptr++, 0, 0);
        if (output->setParam == NULL || output->handler == NULL) {
            break;
        }
        output->setParam(output, AL_FILTER_SET_DRAM, out_buf);
        cmd_end = output->handler(output, &tmp, chunk_samples,
                                  synth->curSamples, cmd_ptr);

        out_len -= chunk_samples;
        out_buf += chunk_samples << 1;
        synth->curSamples += chunk_samples;
    }

    *cmd_len = (s32)(cmd_end - cmd_list);
    _collectPVoices(synth);
    return cmd_end;
}

ALParam *__allocParam(void)
{
    ALSynth *synth;
    ALParam *param;

    if (alGlobals == NULL) {
        return NULL;
    }

    synth = &alGlobals->drvr;
    param = synth->paramList;
    if (param != NULL) {
        synth->paramList = param->next;
        param->next = NULL;
    }

    return param;
}

void __freeParam(ALParam *param)
{
    ALSynth *synth;

    if (alGlobals == NULL || param == NULL) {
        return;
    }

    synth = &alGlobals->drvr;
    param->next = synth->paramList;
    synth->paramList = param;
}

void _collectPVoices(ALSynth *synth)
{
    ALLink *node;

    if (synth == NULL) {
        return;
    }

    while ((node = synth->pLameList.next) != NULL) {
        alUnlink(node);
        alLink(node, &synth->pFreeList);
    }
}

void _freePVoice(ALSynth *synth, PVoice *pvoice)
{
    if (synth == NULL || pvoice == NULL) {
        return;
    }

    alUnlink((ALLink *)pvoice);
    alLink((ALLink *)pvoice, &synth->pLameList);
}

s32 _timeToSamples(ALSynth *synth, s32 micros)
{
    return native_time_to_samples_no_round(synth, micros) & ~0xf;
}

void alInit(ALGlobals *globals, ALSynConfig *config)
{
    if (alGlobals != NULL || globals == NULL || config == NULL) {
        return;
    }

    alGlobals = globals;
    alSynNew(&alGlobals->drvr, config);
}

void alClose(ALGlobals *globals)
{
    if (alGlobals == NULL) {
        return;
    }

    if (globals != NULL) {
        alSynDelete(&globals->drvr);
    } else {
        alSynDelete(&alGlobals->drvr);
    }

    alGlobals = NULL;
}

void alCopy(void *src, void *dest, s32 len)
{
    if (src == NULL || dest == NULL || len <= 0) {
        return;
    }

    memcpy(dest, src, (size_t)len);
}

f32 alCents2Ratio(s32 cents)
{
    return audio_exp2f((f32)cents / 1200.0f);
}

void alHeapInit(ALHeap *hp, u8 *base, s32 len)
{
    if (hp == NULL) {
        return;
    }

    hp->base = base;
    hp->cur = base;
    hp->len = len;
    hp->count = 0;
}

/*
 * ARENA-RESIDENT BUMP ALLOCATOR — do not replace with calloc().
 *
 * mgb64 can malloc audio memory because its mixer receives real pointers. DKR
 * cannot: platform/mixer.h's MIXER_RESOLVE() rebuilds every address the mixer
 * touches from its low 32 bits via dkr_lo32_to_ptr() (= g_dkrArenaHi | lo32),
 * which is only correct for pointers inside the size-aligned ARENA. A malloc'd
 * buffer has different high bits, so the reconstruction would hand the mixer a
 * wild pointer. game/src/audio.c backs ALHeap with arena memory, so bump-
 * allocating within it keeps every buffer arena-resident and the round-trip
 * exact. Same invariant the deleted heapalloc.c maintained.
 */
void *alHeapDBAlloc(u8 *file, s32 line, ALHeap *hp, s32 num, s32 size)
{
    s32 bytes;
    u8 *ptr;

    (void)file;
    (void)line;

    if (hp == NULL || num <= 0 || size <= 0) {
        return NULL;
    }

    /* Overflow-safe: reject before the multiply can wrap. */
    if (num > (s32)(0x7FFFFFFF / size)) {
        return NULL;
    }

    bytes = (num * size + AL_CACHE_ALIGN) & ~AL_CACHE_ALIGN;
    if (bytes < 0 || hp->cur + bytes > hp->base + hp->len) {
        return NULL;
    }

    ptr = hp->cur;
    hp->cur += bytes;
    hp->count++;
    /* The stock allocator hands back zeroed memory and callers rely on it
     * (e.g. PVoice::unkDC's implicit aux-bus-0 parentage). */
    memset(ptr, 0, (size_t)bytes);
    return ptr;
}

void alSynDelete(ALSynth *s)
{
    if (s == NULL) {
        return;
    }

    s->head = NULL;
}

void alSynSetPriority(ALSynth *s, ALVoice *voice, s16 priority)
{
    (void)s;
    if (voice == NULL) {
        return;
    }

    voice->priority = priority;
}

void alSynAddPlayer(ALSynth *s, ALPlayer *client)
{
    OSIntMask mask;

    if (s == NULL || client == NULL) {
        return;
    }

    mask = osSetIntMask(OS_IM_NONE);
    client->samplesLeft = s->curSamples;
    client->next = s->head;
    s->head = client;
    osSetIntMask(mask);
}

s32 alSynAllocVoice(ALSynth *s, ALVoice *voice, ALVoiceConfig *config)
{
    PVoice *physical;

    if (s == NULL || voice == NULL || config == NULL) {
        return 0;
    }

    voice->priority = config->priority;
    voice->unityPitch = config->unityPitch;
    voice->table = NULL;
    voice->fxBus = config->fxBus;
    voice->state = AL_STOPPED;
    voice->pvoice = NULL;

    physical = take_first_voice(&s->pLameList, &s->pAllocList);
    if (physical == NULL) {
        physical = take_first_voice(&s->pFreeList, &s->pAllocList);
    }

    if (physical != NULL) {
        physical->offset = 0;
    } else {
        physical = find_stealable_voice(s, config->priority);
        if (physical != NULL) {
            ALVoice *old_voice = physical->vvoice;
            s16 old_priority = old_voice != NULL ? old_voice->priority : -1;
            ALFilter *filter = physical->channelKnob;
            ALParam *fade = __allocParam();
            ALParam *stop = __allocParam();

            if (fade == NULL || stop == NULL) {
                native_csp_trace_physical_voice("physical_steal_reject",
                                                old_priority,
                                                config->priority);
                __freeParam(fade);
                __freeParam(stop);
                return 0;
            }

            native_csp_trace_physical_voice("physical_steal", old_priority,
                                            config->priority);

            physical->offset = 512;
            if (old_voice != NULL) {
                old_voice->pvoice = NULL;
            }

            fade->next = NULL;
            fade->delta = s->paramSamples;
            fade->type = AL_FILTER_SET_VOLUME;
            fade->data.i = 0;
            fade->moredata.i = physical->offset - 64;
            enqueue_filter_update(filter, fade);

            stop->next = NULL;
            stop->delta = s->paramSamples + physical->offset;
            stop->type = AL_FILTER_STOP_VOICE;
            enqueue_filter_update(filter, stop);
        }
    }

    if (physical == NULL) {
        native_csp_trace_physical_voice("physical_voice_reject", -1,
                                        config->priority);
        return 0;
    }

    physical->vvoice = voice;
    voice->pvoice = physical;
    return 1;
}

void alSynSetVol(ALSynth *s, ALVoice *voice, s16 volume, ALMicroTime delta)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_VOLUME);

    if (update == NULL) {
        return;
    }

    /* NOTE: mgb64 scales SFX-voice volume by its Audio.SfxVolume bus here. DKR
     * has no such bus — game/src/audio.c applies its own music/SFX volumes
     * before they reach the synth — so the volume passes through unmodified. */
    update->data.i = volume;
    update->moredata.i = _timeToSamples(s, delta);
    enqueue_voice_update(voice, update);
}

void alSynSetPan(ALSynth *s, ALVoice *voice, ALPan pan)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_PAN);

    if (update == NULL) {
        return;
    }

    update->data.i = modify_panning(pan);
    enqueue_voice_update(voice, update);
}

void alSynSetPitch(ALSynth *s, ALVoice *voice, f32 pitch)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_PITCH);

    if (update == NULL) {
        return;
    }

    update->data.f = pitch;
    enqueue_voice_update(voice, update);
}

void alSynSetFXMix(ALSynth *s, ALVoice *voice, u8 fxmix)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_SET_FXAMT);

    if (update == NULL) {
        return;
    }

    update->data.i = fxmix;
    native_voice_trace_fxmix(s, voice, fxmix);
    enqueue_voice_update(voice, update);
}

void alSynStartVoice(ALSynth *s, ALVoice *voice, ALWaveTable *table)
{
    ALStartParam *update =
        (ALStartParam *)alloc_voice_update(s, voice, AL_FILTER_START_VOICE);

    if (update == NULL) {
        return;
    }

    update->wave = table;
    update->unity = voice->unityPitch;
    enqueue_voice_update(voice, (ALParam *)update);
}

void alSynStartVoiceParams(ALSynth *s, ALVoice *voice, ALWaveTable *table,
                           f32 pitch, s16 vol, ALPan pan, u8 fxmix,
                           ALMicroTime delta)
{
    ALStartParamAlt *update = (ALStartParamAlt *)alloc_voice_update(
        s, voice, AL_FILTER_START_VOICE_ALT);

    if (update == NULL) {
        return;
    }

    update->unity = voice->unityPitch;
    update->pan = (ALPan)modify_panning(pan);
    update->volume = vol;
    update->fxMix = fxmix;
    update->pitch = pitch;
    update->samples = _timeToSamples(s, delta);
    update->wave = table;
    native_voice_trace_music(s, voice, fxmix);
    enqueue_voice_update(voice, (ALParam *)update);
}

void alSynStopVoice(ALSynth *s, ALVoice *voice)
{
    ALParam *update = alloc_voice_update(s, voice, AL_FILTER_STOP_VOICE);

    if (update == NULL) {
        return;
    }

    enqueue_voice_update(voice, update);
}

void alSynFreeVoice(ALSynth *s, ALVoice *voice)
{
    ALFreeParam *update;

    if (s == NULL || voice == NULL || voice->pvoice == NULL) {
        return;
    }

    if (voice->pvoice->offset == 0) {
        _freePVoice(s, voice->pvoice);
        voice->pvoice = NULL;
        return;
    }

    update = (ALFreeParam *)alloc_voice_update(s, voice, AL_FILTER_FREE_VOICE);
    if (update == NULL) {
        return;
    }

    update->pvoice = voice->pvoice;
    enqueue_voice_update(voice, (ALParam *)update);
    voice->pvoice = NULL;
}

ALFxRef *alSynAllocFX(ALSynth *s, s16 bus, ALSynConfig *config, ALHeap *heap)
{
    ALAuxBus *aux;

    if (s == NULL || s->auxBus == NULL || s->mainBus == NULL ||
        bus < 0 || bus >= s->maxAuxBusses) {
        return NULL;
    }

    aux = &s->auxBus[bus];
    alFxNew(&aux->fx[0], config, bus, heap);
    alFxParam(&aux->fx[0], AL_FILTER_SET_SOURCE, aux);
    alMainBusParam(s->mainBus, AL_FILTER_ADD_SOURCE, &aux->fx[0]);
    return (ALFxRef *)&aux->fx[0];
}
