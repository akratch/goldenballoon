/*
 * Physical-voice and update-pool contract of the production synth driver
 * (platform/audio_synth.c), pinned by behaviour (issue #66).
 *
 * White-box like test_engine_guards.c: this target links the PRODUCTION
 * audio_synth.c and audio_event_queue.c and stubs the DSP filters around
 * them. The envmixer stub keeps the real update list (ctrlList/ctrlTail), and
 * drain() consumes it the way alEnvmixerPull does: a FREE_VOICE update resets
 * the voice offset and releases it, and every update goes back to the pool.
 *
 * Every alSyn* update, a steal's fade+stop pair and a stolen voice's deferred
 * free share one maxUpdates pool. A stolen voice (offset 512) cannot be freed
 * synchronously because it is still fading out the note it replaced, so
 * alSynFreeVoice queues a FREE_VOICE update. Stock libultra simply returned
 * when no update was left. The physical voice then stayed on pAllocList with
 * offset 512 for the rest of the session: never stealable (stealing requires
 * offset 0) and never freed, so each dry-pool free cost one of the 40 voices
 * for good.
 *
 *   deferred_free_unchanged  with an update available the free is still
 *                            deferred, exactly as before (positive control).
 *   dry_pool_free_no_leak    with the pool dry the voice is released anyway,
 *                            and a later allocation gets it back. Fails on
 *                            the stock early return.
 */

#include <PR/libaudio.h>
#include "synthInternals.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define EXPECT(cond, ...)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------------- */
/* Engine stubs                                                           */
/* ---------------------------------------------------------------------- */

ALGlobals *alGlobals = NULL;

#define MAX_TEST_VOICES 8
static ALEnvMixer *sEnvmixers[MAX_TEST_VOICES];
static int sEnvmixerCount;
static int sVoiceRejects;

static s32 test_envmixer_param(void *filter, s32 paramID, void *param)
{
    ALEnvMixer *envmixer = (ALEnvMixer *)filter;
    ALParam *update = (ALParam *)param;

    if (paramID != AL_FILTER_ADD_UPDATE || update == NULL) {
        return 0;
    }
    update->next = NULL;
    if (envmixer->ctrlTail != NULL) {
        envmixer->ctrlTail->next = update;
    } else {
        envmixer->ctrlList = update;
    }
    envmixer->ctrlTail = update;
    return 0;
}

void alEnvmixerNew(ALEnvMixer *e, ALHeap *hp)
{
    (void)hp;
    memset(e, 0, sizeof(*e));
    e->filter.setParam = test_envmixer_param;
    if (sEnvmixerCount < MAX_TEST_VOICES) {
        sEnvmixers[sEnvmixerCount++] = e;
    }
}

s32 alEnvmixerParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alAuxBusNew(ALAuxBus *m, void *ptr, s32 len) { (void)m; (void)ptr; (void)len; }
s32 alAuxBusParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alMainBusNew(ALMainBus *m, void *ptr, s32 len) { (void)m; (void)ptr; (void)len; }
s32 alMainBusParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alFxNew(ALFx *r, ALSynConfig *c, s16 bus, ALHeap *hp) { (void)r; (void)c; (void)bus; (void)hp; }
s32 alFxParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alLoadNew(ALLoadFilter *f, ALDMANew dma, ALHeap *hp) { (void)f; (void)dma; (void)hp; }
s32 alLoadParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alResampleNew(ALResampler *r, ALHeap *hp) { (void)r; (void)hp; }
s32 alResampleParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void alSaveNew(ALSave *r) { (void)r; }
s32 alSaveParam(void *f, s32 id, void *p) { (void)f; (void)id; (void)p; return 0; }
void mixerSegment(unsigned int seg, unsigned int base) { (void)seg; (void)base; }
OSIntMask osSetIntMask(OSIntMask mask) { return mask; }
FILE *mdkr_fopen_utf8(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
s32 modify_panning(s32 pan) { return pan; }
f32 audio_exp2f(f32 exponent) { return exp2f(exponent); }

void native_csp_trace_physical_voice(const char *event, s16 old_priority,
                                     s16 new_priority)
{
    (void)old_priority;
    (void)new_priority;
    if (event != NULL && strcmp(event, "physical_voice_reject") == 0) {
        sVoiceRejects++;
    }
}

/* ---------------------------------------------------------------------- */
/* Harness                                                                */
/* ---------------------------------------------------------------------- */

static ALGlobals sGlobals;
static u8 sHeapBytes[64 * 1024];
static ALHeap sHeap;

static ALSynth *synth_new(s32 maxPVoices, s32 maxUpdates)
{
    ALSynConfig config;

    memset(&sGlobals, 0, sizeof(sGlobals));
    memset(sEnvmixers, 0, sizeof(sEnvmixers));
    sEnvmixerCount = 0;
    sVoiceRejects = 0;
    alGlobals = NULL;
    alHeapInit(&sHeap, sHeapBytes, (s32)sizeof(sHeapBytes));

    memset(&config, 0, sizeof(config));
    config.maxVVoices = maxPVoices;
    config.maxPVoices = maxPVoices;
    config.maxUpdates = maxUpdates;
    config.fxType[0] = AL_FX_NONE;
    config.fxType[1] = AL_FX_NONE;
    config.outputRate = 22050;
    config.heap = &sHeap;
    alInit(&sGlobals, &config);
    return &sGlobals.drvr;
}

static int list_has(const ALLink *head, const PVoice *pvoice)
{
    const ALLink *node;

    for (node = head->next; node != NULL; node = node->next) {
        if (node == (const ALLink *)pvoice) {
            return 1;
        }
    }
    return 0;
}

static int free_params(const ALSynth *synth)
{
    const ALParam *param;
    int count = 0;

    for (param = synth->paramList; param != NULL; param = param->next) {
        count++;
    }
    return count;
}

/* What alEnvmixerPull does with each update once its time is reached. */
static void drain(ALSynth *synth)
{
    int i;

    for (i = 0; i < sEnvmixerCount; i++) {
        ALEnvMixer *envmixer = sEnvmixers[i];

        while (envmixer->ctrlList != NULL) {
            ALParam *update = envmixer->ctrlList;

            envmixer->ctrlList = update->next;
            if (update->type == AL_FILTER_FREE_VOICE) {
                PVoice *pvoice = (PVoice *)((ALFreeParam *)update)->pvoice;
                pvoice->offset = 0;
                _freePVoice(synth, pvoice);
            }
            __freeParam(update);
        }
        envmixer->ctrlTail = NULL;
    }
    _collectPVoices(synth);
}

/* One physical voice: `low` takes it, `high` steals it (offset 512, two
 * updates for the fade and the stop of the note it replaces). */
static PVoice *steal_one(ALSynth *synth, ALVoice *low, ALVoice *high)
{
    ALVoiceConfig lowConfig = { 1, 0, 0 };
    ALVoiceConfig highConfig = { 5, 0, 0 };
    PVoice *pvoice;

    EXPECT(alSynAllocVoice(synth, low, &lowConfig) == 1,
           "the only physical voice was not allocated");
    pvoice = low->pvoice;
    EXPECT(pvoice != NULL && pvoice->offset == 0,
           "a fresh voice must start at offset 0");
    EXPECT(alSynAllocVoice(synth, high, &highConfig) == 1,
           "a higher-priority voice could not steal");
    EXPECT(high->pvoice == pvoice && low->pvoice == NULL,
           "the steal did not move the physical voice");
    EXPECT(pvoice != NULL && pvoice->offset == 512,
           "a stolen voice must carry the 512-sample fade offset");
    return pvoice;
}

static void deferred_free_unchanged(void)
{
    ALSynth *synth = synth_new(1, 4);
    ALVoice low, high, next;
    ALVoiceConfig nextConfig = { 1, 0, 0 };
    PVoice *pvoice;

    memset(&low, 0, sizeof(low));
    memset(&high, 0, sizeof(high));
    memset(&next, 0, sizeof(next));
    pvoice = steal_one(synth, &low, &high);
    EXPECT(free_params(synth) == 2, "the steal should hold two updates, holds %d",
           4 - free_params(synth));

    alSynFreeVoice(synth, &high);
    EXPECT(high.pvoice == NULL, "a freed voice must drop its physical voice");
    EXPECT(free_params(synth) == 1,
           "the stolen voice's free must be queued as an update");
    EXPECT(list_has(&synth->pAllocList, pvoice),
           "the stolen voice must stay allocated until its fade has played");
    EXPECT(pvoice->offset == 512,
           "the deferred free must not reset the offset early");

    drain(synth);
    EXPECT(list_has(&synth->pFreeList, pvoice),
           "the consumed FREE_VOICE update did not release the voice");
    EXPECT(free_params(synth) == 4, "updates leaked: %d of 4 free",
           free_params(synth));
    EXPECT(alSynAllocVoice(synth, &next, &nextConfig) == 1 &&
               next.pvoice == pvoice && pvoice->offset == 0,
           "the released voice was not reusable");
    EXPECT(sVoiceRejects == 0, "unexpected physical_voice_reject");
}

static void dry_pool_free_no_leak(void)
{
    ALSynth *synth = synth_new(1, 2);
    ALVoice low, high, next;
    ALVoiceConfig nextConfig = { 127, 0, 0 };
    PVoice *pvoice;

    memset(&low, 0, sizeof(low));
    memset(&high, 0, sizeof(high));
    memset(&next, 0, sizeof(next));
    pvoice = steal_one(synth, &low, &high);
    EXPECT(free_params(synth) == 0, "the steal should have emptied the pool");

    alSynFreeVoice(synth, &high);
    EXPECT(high.pvoice == NULL,
           "a free that found the pool dry left the voice attached");
    EXPECT(!list_has(&synth->pAllocList, pvoice),
           "a free that found the pool dry leaked the physical voice "
           "(still allocated, offset %d)", (int)pvoice->offset);
    EXPECT(pvoice->offset == 0,
           "a released voice must be stealable again (offset %d)",
           (int)pvoice->offset);

    drain(synth);
    EXPECT(free_params(synth) == 2, "updates leaked: %d of 2 free",
           free_params(synth));
    EXPECT(alSynAllocVoice(synth, &next, &nextConfig) == 1 &&
               next.pvoice == pvoice,
           "the voice lost to the dry pool never came back");
    EXPECT(sVoiceRejects == 0,
           "physical_voice_reject after a dry-pool free (%d)", sVoiceRejects);
}

int main(void)
{
    deferred_free_unchanged();
    dry_pool_free_no_leak();
    if (failures != 0) {
        fprintf(stderr, "audio_synth_voice: %d failure(s)\n", failures);
        return 1;
    }
    printf("audio_synth_voice: PASS\n");
    return 0;
}
