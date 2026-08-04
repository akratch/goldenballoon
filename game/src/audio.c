#include "audio.h"
#include "runtime_contracts.h"
#include "memory.h"
#ifdef NATIVE_PORT
#include "gameplay_event_trace.h"
#include <stdlib.h>   /* getenv (MDKR_AUDIO_REVERB toggle) */
#include <string.h>
#endif

#include "asset_enums.h"
#include "asset_loading.h"
#include "audio_spatial.h"
#include "audiomgr.h"
#include "audiosfx.h"
#include "libultra/src/audio/seqchannel.h"
#include "sched.h"
#include "types.h"

/************ .data ************/

ALCSPlayer *gMusicPlayer = NULL;  // Official Name: tuneSeqPlayer
ALCSPlayer *gJinglePlayer = NULL; // Official Name: ambientSeqPlayer
u8 gMusicBaseVolume = 127;
u8 sfxRelativeVolume = 127;
u8 gCanPlayMusic = TRUE;
u8 gCanPlayJingle = FALSE;
s32 gBlockMusicChange = FALSE;
s32 audioPrevCount = 0;
f32 sMusicFadeVolume = 1.0f;
s32 gMusicSliderVolume = 256;
s32 gDelayedSoundsCount = 0;
u8 gMusicNextSeqID = SEQUENCE_NONE;
u8 gJingleNextSeqID = SEQUENCE_NONE;
UNUSED s32 D_800DC664 = 0;
UNUSED s32 D_800DC668 = 0;
s32 gGlobalMusicVolume = 256; // This is never not 256...
u8 gBlockVoiceLimitChange = FALSE;
#ifdef NATIVE_PORT
u8 gDkrReverbEnabled = TRUE;    /* M5: native reverb ON (MDKR_AUDIO_REVERB=0 disables) */
static u8 sOverlayPauseMix;
#define MDKR_PHYSICAL_VOICE_CAPACITY 40u
#define MDKR_MUSIC_VOICE_CAPACITY 24u
#define MDKR_JINGLE_VOICE_CAPACITY 16u
#define MDKR_SFX_STATE_CAPACITY 32u
#endif

/*******************************/

/************ .bss ************/

// The audio heap is located at the start of the BSS section.
#ifdef NATIVE_PORT
/* ld64 infers 32 KiB alignment for a tentative byte array this large, above
 * Mach-O's 16 KiB segment maximum. A strong zero-filled native definition
 * keeps the array's actual byte-alignment contract and avoids that heuristic. */
u8 gAudioHeapStack[AUDIO_HEAP_SIZE] = { 0 };
#else
u8 gAudioHeapStack[AUDIO_HEAP_SIZE];
#endif

ALHeap gALHeap;
ALSeqFile *gSequenceTable;
void *gMusicSequenceData;
void *gJingleSequenceData;
u8 gCurrentSequenceID;
u8 gCurrentJingleID;
s32 gMusicTempo;
u32 *gSeqLengthTable;
ALBankFile *gSequenceBank;
ALBankFile *gSoundBank; // Official Name: sfxBankPtr
SoundData *gSoundTable; // Official Name: sfxIndex
MusicData *gSeqSoundTable;
s32 gSoundCount; // Official Name: maxSound
s32 gSeqSoundCount;
u32 gSoundTableSize; // Official Name: sfxIndexSize
u32 gSeqSoundTableSize;
s16 sMusicTempo;
f32 gMusicAnimationTick;
s32 sMusicDelayTimer;
s32 sMusicDelayLength;
u8 gMusicPlaying;
u8 gJinglePlaying;
DelayedSound gDelayedSounds[8];
ALCSeq gMusicSequence;
ALCSeq gJingleSequence;
u8 gSkipResetChannels; // Stored and used by a single function, but redundant.
u8 gAudioVolumeSetting;
u32 gDynamicMusicChannelMask;
SoundHandle gGlobalSoundMask;
SoundHandle gSpatialSoundMask;
SoundHandle gRacerSoundMask;

/******************************/

/**
 * Allocate memory for all the audio systems, including sequence data, sound data and heaps.
 * Afterwards, set up the audio thread and start it.
 */
void audio_init(OSSched *sc) {
    s32 i;
    ALSynConfig synth_config;
    s32 *addrPtr;
    u32 seqfSize;
    u32 seqLength;
    UNUSED u32 pad;
    audioMgrConfig audConfig;

    seqLength = 0;
#ifdef NATIVE_PORT
    /* M5 audio: the audio heap must be ARENA memory. Every alHeapAlloc'd buffer
     * (synth state, the parsed sound/sequence banks, cmd lists, output buffers,
     * DMA sample buffers) reaches the mixer as a 32-bit-truncated "physical"
     * address (the stock libultra synth ABI), and the mixer reconstructs the
     * host pointer via dkr_lo32_to_ptr() — which only works for arena pointers
     * (they share the arena's high-32 bits). A BSS array (gAudioHeapStack) has
     * unrelated high bits and could not be reconstructed. */
    {
        u8 *heapMem = (u8 *) mempool_alloc_safe(sizeof(gAudioHeapStack), COLOUR_TAG_CYAN);
        alHeapInit(&gALHeap, heapMem, sizeof(gAudioHeapStack));
        /* bnkf.c allocates the parsed host bank structs from this heap. */
        gAudioParseHeap = &gALHeap;
    }
#else
    alHeapInit(&gALHeap, gAudioHeapStack, sizeof(gAudioHeapStack));
#endif

#ifdef NATIVE_PORT
    /* M5 audio (was silence-stubbed for M2-M4): ASSET_AUDIO is big-endian.
     * alBnkfNew (bnkf.c) is now a BE parser that builds host-native, LP64-safe,
     * arena-resident bank structs, and the audio synth is driven by the native
     * host's independently due two-field service. Raw ADPCM sample data stays in ROM,
     * byte-order-defined, fetched by the audiomgr DMA callback. */
    addrPtr = (s32 *) asset_table_load(ASSET_AUDIO_TABLE);

    gAudioBankCtlSize = addrPtr[ASSET_AUDIO_2] - addrPtr[ASSET_AUDIO_1];
    gSoundBank = (ALBankFile *) mempool_alloc_safe(gAudioBankCtlSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSoundBank, addrPtr[ASSET_AUDIO_1], gAudioBankCtlSize);
    alBnkfNew(gSoundBank, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_2]));

    gSoundTableSize = addrPtr[ASSET_AUDIO_7] - addrPtr[ASSET_AUDIO_6];
    gSoundTable = (SoundData *) mempool_alloc_safe(gSoundTableSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSoundTable, addrPtr[ASSET_AUDIO_6], gSoundTableSize);
    gSoundCount = gSoundTableSize / sizeof(SoundData);
    /* SoundData.soundBite (the ALInstrument sound index) and .range are u16 and
     * big-endian in ROM; the sfx players and spatial audio read them directly. */
    for (i = 0; i < gSoundCount; i++) {
        gSoundTable[i].soundBite = (u16) ((gSoundTable[i].soundBite << 8) | (gSoundTable[i].soundBite >> 8));
        gSoundTable[i].range = (u16) ((gSoundTable[i].range << 8) | (gSoundTable[i].range >> 8));
    }

    gSeqSoundTableSize = addrPtr[ASSET_AUDIO_6] - addrPtr[ASSET_AUDIO_5];
    gSeqSoundTable = (MusicData *) mempool_alloc_safe(gSeqSoundTableSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSeqSoundTable, addrPtr[ASSET_AUDIO_5], gSeqSoundTableSize);
    gSeqSoundCount = gSeqSoundTableSize / sizeof(MusicData);

    gAudioBankCtlSize = addrPtr[ASSET_AUDIO_0];
    gSequenceBank = (ALBankFile *) mempool_alloc_safe(gAudioBankCtlSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSequenceBank, 0, gAudioBankCtlSize);
    alBnkfNew(gSequenceBank, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_0]));

    /* Real sequence file: read the BE seqCount, then parse the raw BE seqfile
     * into a host-laid-out ALSeqFile (LP64: on-disk entries are 8 bytes,
     * ALSeqData is 16 here — cannot overlay). */
    {
        u8 *seqCountRaw;
        u8 *seqfRaw;
        s32 seqCount;
        u32 rawSize;

        seqCountRaw =
            (u8 *)mempool_alloc_safe(4, COLOUR_TAG_CYAN);
        asset_load(
            ASSET_AUDIO, (uintptr_t)seqCountRaw, addrPtr[ASSET_AUDIO_4],
            4);
        seqCount = alSeqFileCount(seqCountRaw);
        mempool_free(seqCountRaw);
        if (seqCount < 0) {
            seqCount = 0;
        }
        rawSize = seqCount * 8 + 4;
        seqfRaw = (u8 *) mempool_alloc_safe(rawSize, COLOUR_TAG_CYAN);
        asset_load(ASSET_AUDIO, (uintptr_t)seqfRaw, addrPtr[ASSET_AUDIO_4], rawSize);

        seqfSize = sizeof(ALSeqFile) + (seqCount > 0 ? (seqCount - 1) : 0) * sizeof(ALSeqData);
        gSequenceTable = (ALSeqFile *) mempool_alloc_safe(seqfSize, COLOUR_TAG_CYAN);
        alSeqFileNewFrom(gSequenceTable, seqfRaw, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_4]));
        mempool_free(seqfRaw);

        gSeqLengthTable = (u32 *) mempool_alloc_safe((seqCount > 0 ? seqCount : 1) * 4, COLOUR_TAG_CYAN);
        for (i = 0; i < seqCount; i++) {
            gSeqLengthTable[i] = gSequenceTable->seqArray[i].len;
            if (gSeqLengthTable[i] & 1) {
                gSeqLengthTable[i]++;
            }
            if (seqLength < gSeqLengthTable[i]) {
                seqLength = gSeqLengthTable[i];
            }
        }
    }
    (void) pad;
#else
    addrPtr = (s32 *) asset_table_load(ASSET_AUDIO_TABLE);
    gSoundBank = (ALBankFile *) mempool_alloc_safe(addrPtr[ASSET_AUDIO_2] - addrPtr[ASSET_AUDIO_1], COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSoundBank, addrPtr[ASSET_AUDIO_1], addrPtr[ASSET_AUDIO_2] - addrPtr[ASSET_AUDIO_1]);
    alBnkfNew(gSoundBank, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_2]));

    gSoundTableSize = addrPtr[ASSET_AUDIO_7] - addrPtr[ASSET_AUDIO_6];
    gSoundTable = (SoundData *) mempool_alloc_safe(gSoundTableSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSoundTable, addrPtr[ASSET_AUDIO_6], gSoundTableSize);
    gSoundCount = gSoundTableSize / sizeof(SoundData);

    gSeqSoundTableSize = addrPtr[ASSET_AUDIO_6] - addrPtr[ASSET_AUDIO_5];
    gSeqSoundTable = (MusicData *) mempool_alloc_safe(gSeqSoundTableSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSeqSoundTable, addrPtr[ASSET_AUDIO_5], gSeqSoundTableSize);
    gSeqSoundCount = gSeqSoundTableSize / sizeof(MusicData);

    gSequenceBank = (ALBankFile *) mempool_alloc_safe(addrPtr[ASSET_AUDIO_0], COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSequenceBank, 0, addrPtr[ASSET_AUDIO_0]);
    alBnkfNew(gSequenceBank, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_0]));
    gSequenceTable = (ALSeqFile *) alHeapAlloc(&gALHeap, 1, 4);
    asset_load(ASSET_AUDIO, (uintptr_t)gSequenceTable, addrPtr[ASSET_AUDIO_4], 4);

    seqfSize = (gSequenceTable->seqCount) * 8 + 4;
    gSequenceTable = mempool_alloc_safe(seqfSize, COLOUR_TAG_CYAN);
    asset_load(ASSET_AUDIO, (uintptr_t)gSequenceTable, addrPtr[ASSET_AUDIO_4], seqfSize);
    alSeqFileNew(gSequenceTable, asset_rom_offset(ASSET_AUDIO, addrPtr[ASSET_AUDIO_4]));
    gSeqLengthTable = (u32 *) mempool_alloc_safe((gSequenceTable->seqCount) * 4, COLOUR_TAG_CYAN);

    for (i = 0; i < gSequenceTable->seqCount; i++) {
        pad = (u32) (gSequenceTable + 8 + i * 8); // Fakematch
        gSeqLengthTable[i] = gSequenceTable->seqArray[i].len;
        if (gSeqLengthTable[i] & 1) {
            gSeqLengthTable[i]++;
        }
        if (seqLength < gSeqLengthTable[i]) {
            seqLength = gSeqLengthTable[i];
        }
    }
#endif /* NATIVE_PORT */

    synth_config.maxVVoices = 40;
    synth_config.maxPVoices = 40;
    synth_config.maxUpdates = 96;
    synth_config.dmaproc = NULL;
    synth_config.fxType[0] = AL_FX_CUSTOM;
    synth_config.fxType[1] = AL_FX_BIGROOM;
    synth_config.outputRate = 0;
    synth_config.heap = &gALHeap;
    amCreateAudioMgr(&synth_config, 12, sc);
#ifdef NATIVE_PORT
    {
        /* Native reverb (the libultra ALFx delay lines, drvrnew.c/reverb.c) is
         * ON. It used to be off by default because the per-frame delay-line
         * transfers ran thousands of samples off the end of r->base into the
         * neighbouring pool allocations (the ALLowPass structs), after which
         * _filterBuffer dereferenced a smashed d->lp. Root cause was NOT the
         * struct growth: it is the u32 delay tap `&r->input[-d->input]`, whose
         * unsigned negation wraps mod 2^32 on the N64 but ZERO-extends on LP64
         * (see reverb.c AL_FX_TAP). Fixed there, with a bounds guard
         * (alFxGuardTrips()) and an 8-byte delay-line tail slack for the
         * mixer's ROUND_UP_8 DMA granularity.
         * MDKR_AUDIO_REVERB=0 forces the old dry behaviour for A/B captures. */
        extern u8 alFXEnabled;
        const char *rv = getenv("MDKR_AUDIO_REVERB");
        gDkrReverbEnabled = (rv != NULL && rv[0] == '0') ? FALSE : TRUE;
        alFXEnabled = gDkrReverbEnabled;
    }
#endif
#ifdef NATIVE_PORT
    /* Music sequence-player event queue: 120 on the N64, 256 here. NOT a blind
     * bump -- measured. With the queue instrumented (MDKR_EVTQ_STATS=1, see
     * game/libultra/src/audio/event.c) the high-water marks over every path the
     * regression fixtures cover are:
     *
     *   attract-demo soak (idle title -> demo levels 18 and 28)   peak 121
     *   race_drive_time_trial, 4300 frames, music + SFX           peak  71
     *   jingle player (budget 50)                                 peak   3
     *   SFX player   (budget 150)                                 peak  56
     *
     * i.e. the demo's music legitimately wants 121 and the stock budget is 120
     * -- a one-entry shortfall, reproducible at the frame the demo level's
     * sequence starts. It is real demand, not a headless-pacing artifact: the
     * deterministic host uses one fixed frameSize per due two-field audio
     * quantum (platform/audi_port_dkr.c), so present count cannot change the
     * simulated audio timeline,
     * and queue depth is a function of how far ahead the sequence schedules
     * events, not of wall-clock speed. Confirmed by re-measuring with the queue
     * temporarily at 1024: the peak stays 121 and drops go to 0, so nothing
     * runs away -- 120 is simply one short.
     *
     * 256 is ~2.1x the measured peak. Cost is ~6.5 KB of the 16 MB arena
     * (sizeof(ALEventListItem) ~48 bytes on LP64).
     *
     * Dropping a post is no longer fatal either way -- __CSPVoiceHandler and
     * sndp_voice_handler now survive the empty queue that a drop leads to (see
     * docs/OPEN_ITEMS.md) -- but a drop still silences that player's music
     * until the next music change, so the queue should not be running dry. */
    gMusicPlayer = sound_seqplayer_init(MDKR_MUSIC_VOICE_CAPACITY, 256);
#else
    gMusicPlayer = sound_seqplayer_init(24, 120);
#endif
    set_voice_limit(gMusicPlayer, 18);
    gJinglePlayer = sound_seqplayer_init(16, 50);
    gMusicSequenceData = mempool_alloc_safe(seqLength, COLOUR_TAG_CYAN);
    gJingleSequenceData = mempool_alloc_safe(seqLength, COLOUR_TAG_CYAN);
#ifdef NATIVE_PORT
    /*
     * The stock 150-entry SFX queue is also too tight for the port's live-sink
     * cadence. A real Chromium/AudioWorklet race repeatedly reached 141-145/150
     * and intermittently exhausted the free list. Re-measuring without the
     * truncating drop, using a deliberately oversized queue, exposed live peaks
     * up to 195. Keep more than 2x that measured ceiling; the browser gate
     * rejects a run that consumes over half the budget.
     */
    audConfig.maxEvents = 512;
    {
        /*
         * Both-direction browser gate: a one-entry SFX queue is used below to
         * force the out-of-resource diagnostic. Exact value only; ordinary runs
         * never set MDKR_TEST_*.
         */
        const char *testSfxCapacity = getenv("MDKR_TEST_SFX_EVENT_CAPACITY");
        if (testSfxCapacity && testSfxCapacity[0] == '1' &&
            testSfxCapacity[1] == '\0') {
            audConfig.maxEvents = 1;
        }
    }
#else
    audConfig.maxEvents = 150;
#endif
    audConfig.maxSounds = 32;
    audConfig.maxChannels = AUDIO_CHANNELS;
    audConfig.numGroups = MDKR_SOUND_GROUP_COUNT;
    audConfig.heap = &gALHeap;
    sndp_init_player(&audConfig);
#ifdef NATIVE_PORT
    if (audConfig.maxEvents == 1) {
        /*
         * Deterministic positive control for the browser release gate. The
         * player's initial heartbeat has already been removed into nextEvent,
         * so the first post fills the sole slot and the second must be rejected.
         * Flush the synthetic event immediately; no game-visible audio state is
         * changed by this diagnostic arm.
         */
        ALEvent testEvent;
        extern SoundPlayer *gSoundPlayerPtr;
        testEvent.type = AL_SNDP_API_EVT;
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, &testEvent, 0);
        alEvtqPostEvent(&gSoundPlayerPtr->evtq, &testEvent, 0);
        alEvtqFlush(&gSoundPlayerPtr->evtq);
    }
#endif
    audioStartThread();
    sound_volume_change(VOLUME_NORMAL);
    mempool_free(addrPtr);
    sndp_set_active_sound_limit(10);
    gBlockMusicChange = FALSE;
    gMusicPlaying = FALSE;
    gJinglePlaying = FALSE;
    gDelayedSoundsCount = 0;
    gSkipResetChannels = FALSE;
    gAudioVolumeSetting = VOLUME_NORMAL;
#ifdef AVOID_UB
    gMusicAnimationTick = 1.0f; // Prevents a denorm crash on the character select screen.
#endif
}

#ifdef NATIVE_PORT
void mdkr_audio_heap_stats(
    u32 *usedBytes, u32 *capacityBytes, u32 *allocations) {
    uintptr_t base = (uintptr_t)gALHeap.base;
    uintptr_t current = (uintptr_t)gALHeap.cur;
    u32 used = 0;
    if (current >= base && current - base <= (uintptr_t)gALHeap.len) {
        used = (u32)(current - base);
    }
    if (usedBytes != NULL) {
        *usedBytes = used;
    }
    if (capacityBytes != NULL) {
        *capacityBytes = gALHeap.len > 0 ? (u32)gALHeap.len : 0;
    }
    if (allocations != NULL) {
        *allocations = gALHeap.count > 0 ? (u32)gALHeap.count : 0;
    }
}

static u32 mdkr_audio_link_count(
    const ALLink *node, u32 capacity, u32 *valid) {
    u32 count = 0;
    while (node != NULL && count <= capacity) {
        count++;
        node = node->next;
    }
    if (node != NULL || count > capacity) {
        *valid = FALSE;
    }
    return count;
}

static u32 mdkr_audio_voice_state_count(
    const ALVoiceState *voice, u32 capacity, u32 *valid) {
    u32 count = 0;
    while (voice != NULL && count <= capacity) {
        count++;
        voice = voice->next;
    }
    if (voice != NULL || count > capacity) {
        *valid = FALSE;
    }
    return count;
}

static u32 mdkr_audio_sound_state_count(
    const ALSoundState *state, u32 capacity, s32 reverse, u32 *valid) {
    u32 count = 0;
    while (state != NULL && count <= capacity) {
        count++;
        state = reverse ? state->prev : state->next;
    }
    if (state != NULL || count > capacity) {
        *valid = FALSE;
    }
    return count;
}

void mdkr_audio_voice_stats(MdkrAudioVoiceStats *stats) {
    extern ALSoundStateLists gSoundStateLists;
    u32 sfxAllocatedReverse;

    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->valid = TRUE;
    if (alGlobals == NULL || gMusicPlayer == NULL || gJinglePlayer == NULL) {
        stats->valid = FALSE;
        return;
    }

    stats->physicalAllocated = mdkr_audio_link_count(
        alGlobals->drvr.pAllocList.next, MDKR_PHYSICAL_VOICE_CAPACITY,
        &stats->valid);
    stats->physicalFree = mdkr_audio_link_count(
        alGlobals->drvr.pFreeList.next, MDKR_PHYSICAL_VOICE_CAPACITY,
        &stats->valid);
    stats->physicalLame = mdkr_audio_link_count(
        alGlobals->drvr.pLameList.next, MDKR_PHYSICAL_VOICE_CAPACITY,
        &stats->valid);
    stats->musicAllocated = mdkr_audio_voice_state_count(
        gMusicPlayer->vAllocHead, MDKR_MUSIC_VOICE_CAPACITY, &stats->valid);
    stats->musicFree = mdkr_audio_voice_state_count(
        gMusicPlayer->vFreeList, MDKR_MUSIC_VOICE_CAPACITY, &stats->valid);
    stats->jingleAllocated = mdkr_audio_voice_state_count(
        gJinglePlayer->vAllocHead, MDKR_JINGLE_VOICE_CAPACITY, &stats->valid);
    stats->jingleFree = mdkr_audio_voice_state_count(
        gJinglePlayer->vFreeList, MDKR_JINGLE_VOICE_CAPACITY, &stats->valid);
    stats->sfxAllocated = mdkr_audio_sound_state_count(
        gSoundStateLists.allocHead, MDKR_SFX_STATE_CAPACITY, FALSE,
        &stats->valid);
    sfxAllocatedReverse = mdkr_audio_sound_state_count(
        gSoundStateLists.allocTail, MDKR_SFX_STATE_CAPACITY, TRUE,
        &stats->valid);
    stats->sfxFree = mdkr_audio_sound_state_count(
        gSoundStateLists.freeHead, MDKR_SFX_STATE_CAPACITY, FALSE,
        &stats->valid);

    if (stats->physicalAllocated + stats->physicalFree +
            stats->physicalLame != MDKR_PHYSICAL_VOICE_CAPACITY ||
        stats->musicAllocated + stats->musicFree !=
            MDKR_MUSIC_VOICE_CAPACITY ||
        stats->jingleAllocated + stats->jingleFree !=
            MDKR_JINGLE_VOICE_CAPACITY ||
        stats->sfxAllocated + stats->sfxFree != MDKR_SFX_STATE_CAPACITY ||
        stats->sfxAllocated != sfxAllocatedReverse ||
        gMusicPlayer->mappedVoices != stats->musicAllocated ||
        gJinglePlayer->mappedVoices != stats->jingleAllocated) {
        stats->valid = FALSE;
    }
}

static u32 gMdkrVoicePeakPhysical;
static u32 gMdkrVoicePeakMusic;

/*
 * Per-generation live-ownership high-water marks.
 *
 * resource_state (game/src/game.c) samples voice ownership at a level boundary,
 * which is the only place the allocator model is quiescent enough to compare
 * generations. That sample point cannot see music ownership at all any more:
 * the level teardown calls music_stop() -> alCSPStop(), and the clean-room
 * alCSPStop() releases synchronously -- it walks vAllocHead, stops and frees
 * every physical voice and unmaps every voice state before returning. The SGI
 * alCSPStop() it replaced only POSTED AL_SEQP_STOPPING_EVT and let the audio
 * thread unwind the voices over later frames, so the boundary sample still
 * caught them mapped. Measured on the plateau route: music voices run 3-14 of
 * 24 for the whole race and are 0/24 in every boundary row.
 *
 * So sample the peak between boundaries instead. Called once per due audio service
 * (platform/audi_port_dkr.c) and only under MDKR_RESOURCE_STATS, which is the
 * mode both resource-plateau gates run in; shipping builds never walk these
 * lists per service quantum.
 */
void mdkr_audio_voice_peaks_sample(void) {
    MdkrAudioVoiceStats stats;

    mdkr_audio_voice_stats(&stats);
    if (!stats.valid) {
        return;
    }
    if (stats.physicalAllocated > gMdkrVoicePeakPhysical) {
        gMdkrVoicePeakPhysical = stats.physicalAllocated;
    }
    if (stats.musicAllocated > gMdkrVoicePeakMusic) {
        gMdkrVoicePeakMusic = stats.musicAllocated;
    }
}

/* Read and clear, so each reported window is exactly one level generation. */
void mdkr_audio_voice_peaks_take(u32 *physicalPeak, u32 *musicPeak) {
    if (physicalPeak != NULL) {
        *physicalPeak = gMdkrVoicePeakPhysical;
    }
    if (musicPeak != NULL) {
        *musicPeak = gMdkrVoicePeakMusic;
    }
    gMdkrVoicePeakPhysical = 0;
    gMdkrVoicePeakMusic = 0;
}
#endif

/**
 * Depending on whether or not the audio volume is set to normal and the argument is false, reset sound effect channel
 * volumes.
 */
void sound_volume_reset(u8 skipReset) {
    if (gAudioVolumeSetting == VOLUME_NORMAL) {
        gSkipResetChannels = skipReset;
        if (gSkipResetChannels == FALSE) {
            gGlobalMusicVolume = 256;
            music_volume_set(gMusicBaseVolume);
            // Effectively sets all volumes to the max (AL_SNDP_GROUP_VOLUME_MAX)
            sndp_set_group_volume(0, gGlobalMusicVolume * 128 - 1);
            sndp_set_group_volume(1, gGlobalMusicVolume * 128 - 1);
            sndp_set_group_volume(2, gGlobalMusicVolume * 128 - 1);
            sndp_set_group_volume(4, gGlobalMusicVolume * 128 - 1);
        }
    }
}

/**
 * Changes the volume of each sound channel depending on what value is passed through.
 * Official Name: amSetMuteMode
 */
void sound_volume_change(s32 behaviour) {
#ifdef NATIVE_PORT
    /* This is always the game's latest authored mode. Native overlay ducking
     * is a separate output layer and must never replace this state. */
    gAudioVolumeSetting = behaviour;
#endif
    switch (behaviour) {
        case VOLUME_LOWER: // Mute most sound effects and half the volume of music.
            sndp_set_group_volume(0, 0);
            sndp_set_group_volume(1, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(2, 0);
            sndp_set_group_volume(4, 0);
#ifdef NATIVE_PORT
            music_volume_set(gMusicBaseVolume);
            music_jingle_volume_set(sfxRelativeVolume);
#else
            alCSPSetVol(gMusicPlayer, (s16) (gMusicBaseVolume * gMusicSliderVolume >> 2));
            alCSPSetVol(gJinglePlayer, 0);
#endif
            break;
        case VOLUME_LOWER_AMBIENT: // Mute the ambient channel, making course elements stop making noise.
            sndp_set_group_volume(0, 0);
            sndp_set_group_volume(1, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(2, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(4, AL_SNDP_GROUP_VOLUME_MAX);
            break;
        case VOLUME_UNK03:
            sndp_set_group_volume(0, 0);
            sndp_set_group_volume(1, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(2, 0);
            sndp_set_group_volume(4, 0);
            break;
        default: // Restore sound back to normal.
            sndp_set_group_volume(0, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(1, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(2, AL_SNDP_GROUP_VOLUME_MAX);
            sndp_set_group_volume(4, AL_SNDP_GROUP_VOLUME_MAX);
#ifdef NATIVE_PORT
            music_volume_set(gMusicBaseVolume);
            music_jingle_volume_set(sfxRelativeVolume);
#else
            alCSPSetVol(gMusicPlayer, (s16) (gMusicBaseVolume * gMusicSliderVolume));
            alCSPSetVol(gJinglePlayer, (s16) (sndp_get_global_volume() * sfxRelativeVolume));
#endif
            break;
    }
#ifndef NATIVE_PORT
    gAudioVolumeSetting = behaviour;
#endif
}

#ifdef NATIVE_PORT
s32 sound_volume_behaviour(void) {
    return gAudioVolumeSetting;
}

s32 sound_overlay_pause_active(void) {
    return sOverlayPauseMix != FALSE;
}

void sound_overlay_pause_set(s32 paused) {
    u8 next = paused ? TRUE : FALSE;

    if (sOverlayPauseMix == next) {
        return;
    }
    sOverlayPauseMix = next;
    sndp_set_overlay_pause(next);
    /* Recompute sequence-player gains from their authored state. The helpers
     * compose the independent overlay layer and retain user slider/fade state. */
    music_volume_set(gMusicBaseVolume);
    music_jingle_volume_set(sfxRelativeVolume);
}
#endif

/**
 * Prevents changing of background music.
 */
void music_change_off(void) {
    gBlockMusicChange = TRUE;
}

/**
 * Allows changing of background music.
 */
void music_change_on(void) {
    gBlockMusicChange = FALSE;
}

/**
 * Queue a new music sequence to play if not blocked.
 * Stops any playing existing music beforehand.
 * Official Name: amTunePlay
 */
void music_play(u8 seqID) {
    if (gBlockMusicChange == FALSE && gMusicSliderVolume != 0) {
#ifdef NATIVE_PORT
        GAMEPLAY_EVENT_TRACE(
            GAMEPLAY_EVENT_MUSIC, seqID, 0, 0, 0);
#endif
        gCurrentSequenceID = seqID;
        gMusicBaseVolume = 127;
        if (gCanPlayMusic) {
            music_sequence_start(gCurrentSequenceID, gMusicPlayer);
        }
        gMusicTempo = alCSPGetTempo(gMusicPlayer);
        audioPrevCount = osGetCount();
        gMusicPlaying = TRUE;
        gDynamicMusicChannelMask = MUSIC_CHAN_MASK_NONE;
    }
}

/**
 * Update the background music voice limit if not prevented from doing so.
 * Official Name: amTuneVoiceLimit
 */
void music_voicelimit_set(u8 voiceLimit) {
    if (gBlockVoiceLimitChange == FALSE) {
        set_voice_limit(gMusicPlayer, voiceLimit);
    }
}

/**
 * Prevent the background music voice limit from being changed.
 */
void music_voicelimit_change_off(void) {
    gBlockVoiceLimitChange = TRUE;
}

/**
 * Allow the background music voice limit to be changed.
 */
void music_voicelimit_change_on(void) {
    gBlockVoiceLimitChange = FALSE;
}

/**
 * Update the jingle players voice limit.
 */
void music_jingle_voicelimit_set(u8 voiceLimit) {
    set_voice_limit(gJinglePlayer, voiceLimit);
}

UNUSED void func_80000C68(u8 arg0) {
    func_80063A90(gMusicPlayer, arg0);
}

/**
 * Enables the timer to start fading in or out the background music.
 * Give it a positive number to fade in, otherwise, give it a negative one to fade out.
 */
void music_fade(s32 time) {
    sMusicDelayTimer = 0;
    sMusicDelayLength = (time * 60) >> 8;
}

/**
 * Sets the background music volume back to normal.
 */
void music_volume_reset(void) {
    sMusicDelayTimer = 0;
    sMusicDelayLength = 0;
    sMusicFadeVolume = 1.0f;
    music_volume_set(gMusicBaseVolume);
}

/**
 * Run every frame, this handles the transitions in and out of music sequences.
 * If there's something in the queue, then begin to play that.
 * Additionally, it also handles the delayed audio queue, counting down and playing any sounds.
 * Official Name: amAudioTick
 */
void sound_update_queue(u8 updateRate) {
    s32 i;
    s32 j;

#ifdef NATIVE_PORT
    /* Audio is silence-stubbed for M2 (see audio_init): the sequence players and
     * banks are NULL, so the per-frame music/queue processing has nothing to
     * drive and would deref NULL. Skip it wholesale until audio lands in M5. */
    if (gMusicPlayer == NULL) {
        return;
    }
#endif

    if (sMusicDelayLength > 0) {
        sMusicDelayTimer += updateRate;
        sMusicFadeVolume = ((f32) sMusicDelayTimer) / ((f32) sMusicDelayLength);
        if (sMusicFadeVolume > 1.0) {
            sMusicDelayTimer = 0;
            sMusicDelayLength = 0;
            sMusicFadeVolume = 1.0f;
        }
        music_volume_set(gMusicBaseVolume);
    } else if (sMusicDelayLength < 0) {
        sMusicDelayTimer -= updateRate;
        sMusicFadeVolume = 1.0f - ((f32) sMusicDelayTimer) / ((f32) sMusicDelayLength);
        if (sMusicFadeVolume < 0.0) {
            sMusicDelayTimer = 0;
            sMusicDelayLength = 0;
            sMusicFadeVolume = 0.0f;
        }
        music_volume_set(gMusicBaseVolume);
    }

    if (gDelayedSoundsCount > 0) {
        for (i = 0; i < gDelayedSoundsCount;) {
            gDelayedSounds[i].timer -= updateRate;
            if (gDelayedSounds[i].timer <= 0) {
                j = i;
                sound_play(gDelayedSounds[i].soundId, gDelayedSounds[i].handlePtr);

                gDelayedSoundsCount -= 1;
                while (j < gDelayedSoundsCount) {
                    gDelayedSounds[i].soundId = gDelayedSounds[i + 1].soundId;
                    gDelayedSounds[i].timer = gDelayedSounds[i + 1].timer;
                    gDelayedSounds[i].handlePtr = gDelayedSounds[i + 1].handlePtr;
                    j++;
                }
                j++;
            } else {
                i++;
            }
        }
    }

    music_sequence_init(gMusicPlayer, gMusicSequenceData, &gMusicNextSeqID, &gMusicSequence);
    music_sequence_init(gJinglePlayer, gJingleSequenceData, &gJingleNextSeqID, &gJingleSequence);
    if (sMusicTempo == -1 && gMusicPlayer->target) {
        sMusicTempo = 60000000 / alCSPGetTempo(gMusicPlayer);
    }
}

/**
 * Add a sound to a queue to play after a set time has passed.
 * Delay time is in seconds. (1.0f = 1 second)
 */
void sound_play_delayed(u16 soundId, SoundHandle *handlePtr, f32 delayTime) {
    if (gDelayedSoundsCount < 8) {
        gDelayedSounds[gDelayedSoundsCount].soundId = soundId;
        gDelayedSounds[gDelayedSoundsCount].handlePtr = handlePtr;
        gDelayedSounds[gDelayedSoundsCount].timer = delayTime * 60.0f;
        gDelayedSoundsCount++;
    }
}

/**
 * Clear the delayed sound queue.
 */
void sound_clear_delayed(void) {
    gDelayedSoundsCount = 0;
}

/**
 * Return the channel mask of the music player.
 */
u16 music_channel_get_mask(void) {
    return gMusicPlayer->chanMask;
}

/**
 * Sets the channels in the sequence on or off based on the channel mask given.
 * Official Name: amTuneSetChlMask
 */
void music_dynamic_set(u16 channelMask) {
    u32 i;
    if (gMusicNextSeqID) {
        gDynamicMusicChannelMask = channelMask;
    } else {
        gMusicPlayer->chanMask = channelMask;
        for (i = 0; i != AUDIO_CHANNELS; i++) {
            if (channelMask & (1 << i)) {
                music_channel_on(i);
            } else {
                music_channel_off(i);
            }
        }
    }
}

/**
 * Mute the sequence channel, preventing it from playing any sound.
 * Official Name: amTuneMuteChl
 */
void music_channel_off(u8 channel) {
    if (channel < AUDIO_CHANNELS) {
        alSeqChOff(gMusicPlayer, channel);
    }
}

/**
 * Return true if the given channel is currently active.
 */
s32 music_channel_active(s32 channel) {
    return (gMusicPlayer->chanMask & (1 << channel)) == 0;
}

/**
 * Unmute the sequence channel so it can play sound.
 * Official Name: amTuneUnmuteChl
 */
void music_channel_on(u8 channel) {
    if (channel < AUDIO_CHANNELS) {
        alSeqChOn(gMusicPlayer, channel);
    }
}

/**
 * Set the panning level of the given channel for the music player.
 */
void music_channel_pan_set(u8 channel, ALPan pan) {
    if (channel < AUDIO_CHANNELS) {
        alCSPSetChlPan(gMusicPlayer, channel, pan);
    }
}

/**
 * Set the volume of the given channel for the music player.
 * Official Name: amTuneSetChlVolume
 */
void music_channel_volume_set(u8 channel, u8 volume) {
    if (channel < AUDIO_CHANNELS) {
        alCSPSetChlVol(gMusicPlayer, channel, volume);
    }
}

/**
 * Return the volume of the given channel in the music player.
 */
UNUSED u8 music_channel_volume(u8 channel) {
    if (channel >= AUDIO_CHANNELS) {
        return 0;
    } else {
        return alCSPGetChlVol(gMusicPlayer, channel);
    }
}

/**
 * Set this channel to fade in over time.
 */
void music_channel_fade_set(u8 channel, ALPan fade) {
    if (channel < AUDIO_CHANNELS) {
        alCSPSetFadeIn(gMusicPlayer, channel, fade);
    }
}

/**
 * Return the fade volume of the given channel.
 */
u8 music_channel_fade(u8 channel) {
    if (channel >= AUDIO_CHANNELS) {
        return 0;
    }
    return alCSPGetFadeIn(gMusicPlayer, channel);
}

/**
 * Resets all audio channels for the music player to the default state.
 * This is being enabled, centre panning and at normal volume.
 * Official Name: amTuneResetChls
 */
void music_channel_reset_all(void) {
    u32 channel;
    if (gBlockMusicChange == FALSE) {
        for (channel = 0; channel < AUDIO_CHANNELS; channel++) {
            music_channel_on(channel);
            music_channel_fade_set(channel, 127);
            music_channel_volume_set(channel, 127);
        }
    }
}

UNUSED u8 func_80001358(u8 chan1, u8 chan2, s32 arg2) {
    /*
     * Channel 100 is the original no-channel sentinel. If both arms are absent,
     * retail returned an uninitialized stack byte. That cannot be a portable
     * contract; zero represents an absent fade-in channel and makes the
     * returned remaining fade distance deterministically 127.
     */
    u8 val_1f = 0;
    u8 vol;
    s32 updatedVol;

    if (chan1 != 100) {
        val_1f = arg2 + alCSPGetChlVol(gMusicPlayer, chan1);
        if (val_1f > 127) {
            val_1f = 127;
        }
        alCSPSetChlVol(gMusicPlayer, chan1, val_1f);
    }

    if (chan2 != 100) {
        updatedVol = alCSPGetChlVol(gMusicPlayer, chan2);
        vol = (updatedVol > arg2) ? updatedVol - arg2 : 0;
        alCSPSetChlVol(gMusicPlayer, chan2, vol);
        return vol;
    } else {
        return 127 - val_1f; //!@bug: This could be uninitialized!
    }
}

/**
 * Retrieves the FX mix levels of all audio channels.
 */
UNUSED void music_get_fx_mix_all_channels(u8 *channelFXMix) {
    s32 channelIdx;
    for (channelIdx = 0; channelIdx < gMusicPlayer->maxChannels; channelIdx++) {
        channelFXMix[channelIdx] = alSeqpGetChlFXMix((ALSeqPlayer *) gMusicPlayer, channelIdx);
    }
}

/**
 * Multiplies the current tempo of the background music.
 * Since it calls music_tempo and multiplies it by the result, calling this repeatedly can recursively change the
 * music's speed.
 * Official Name: amTuneScaleTempo
 */
void music_tempo_set_relative(f32 tempo) {
    music_tempo_set((s32) ((f32) (u32) (music_tempo() & 0xFF) * tempo));
}

/**
 * Set the tempo of the current playing background music.
 * Official name: amTuneSetTempoBPM
 */
void music_tempo_set(s32 tempo) {
    if (tempo != 0) {
        f32 inv_tempo = (1.0f / tempo);
        alCSPSetTempo(gMusicPlayer, (s32) (inv_tempo * 60000000.0f));
        sMusicTempo = tempo;
    }
}

/**
 * Return the tempo of the current playing background music.
 * Official name: amTuneGetTempoBPM
 */
s16 music_tempo(void) {
    return sMusicTempo;
}

/**
 * Returns true if background music is currently playing.
 */
u8 music_is_playing(void) {
    return (alCSPGetState(gMusicPlayer) == AL_PLAYING);
}

/**
 * Counts up using the internal timer.
 * Loops itself round, so the final result will return 0.0f - 1.0f.
 */
f32 music_animation_fraction(void) {
    f32 tmp;
    u32 cnt = osGetCount();
    if ((u32) audioPrevCount < cnt) {
        gMusicAnimationTick += (f32) (cnt - audioPrevCount) / 46875.0f;
    } else {
        gMusicAnimationTick += (f32) ((cnt - audioPrevCount) - 1) / 46875.0f;
    }
    if (gMusicPlaying == FALSE) {
        sMusicTempo = 182;
    }
    for (tmp = 120000.0f / (f32) sMusicTempo; tmp < gMusicAnimationTick; gMusicAnimationTick -= tmp) {
        ;
    }
    audioPrevCount = (s32) cnt;
    return gMusicAnimationTick / tmp;
}

/**
 * Writes the music and sound tempo, as well as the volume to the arguments.
 */
UNUSED void sound_get_properties(u8 poolID, u8 *tempo, u8 *volume, u8 *reverb) {
    *tempo = gSeqSoundTable[poolID].tempo;
    *volume = gSeqSoundTable[poolID].volume;
    *reverb = gSeqSoundTable[poolID].reverb;
}

/**
 * Play a jingle, but only if there isn't one playing already.
 * Official NAme: amAmbientPlay
 */
void music_jingle_play_safe(u8 jingleID) {
    if (music_jingle_playing() == SEQUENCE_NONE) {
        music_sequence_start(gCurrentJingleID = jingleID, gJinglePlayer);
        gJinglePlaying = TRUE;
    }
}

/**
 * Sets the tempo for the jingle player.
 * Official Name: amAmbientSetTempoBPM
 */
void sound_jingle_tempo_set(s32 tempo) {
    f32 inv_tempo = (1.0f / tempo);
    alCSPSetTempo(gJinglePlayer, (s32) (inv_tempo * 60000000.0f));
}

/**
 * Stops the background music.
 * Official Name: amTuneStop
 */
void music_stop(void) {
    if (gBlockMusicChange == FALSE) {
        music_sequence_stop(gMusicPlayer);
    }
}

/**
 * Set background music to play or not.
 * If the setting changed, then either stop or start music.
 */
UNUSED void music_enabled_set(u8 setting) {
    if (setting != gCanPlayMusic) {
        gCanPlayMusic = setting;
        if (setting) {
            music_play(gCurrentSequenceID);
        } else {
            music_stop();
        }
    }
}

/**
 * Return whether background music can be played.
 */
u8 music_can_play(void) {
    return gCanPlayMusic;
}

/**
 * Stops the currently playing jingle.
 * Official Name: amAmbientStop
 */
void music_jingle_stop(void) {
    if (music_jingle_playing() == SEQUENCE_NONE) {
        gCurrentJingleID = SEQUENCE_NONE;
        music_sequence_stop(gJinglePlayer);
    }
}

/**
 * Return the currently playing music.
 * Official Name: amTuneGetSeqNo
 */
u8 music_current_sequence(void) {
    if (gCurrentSequenceID != SEQUENCE_NONE && gMusicPlayer->state == AL_PLAYING) {
        return gCurrentSequenceID;
    } else {
        return SEQUENCE_NONE;
    }
}

/**
 * Return the next music sequence to be played if there is one.
 * Otherwise, return what's currently playing.
 */
UNUSED u8 music_next(void) {
    if (gMusicNextSeqID) {
        return gMusicNextSeqID;
    } else {
        return gCurrentSequenceID;
    }
}

/**
 * Return the currently playing jingle.
 * Official Name: amAmbientGetSeqNo
 */
u8 music_jingle_current(void) {
    return gCurrentJingleID;
}

/**
 * Set the volume of the music.
 * Update music volume with this new setting.
 * Official Name: amTuneSetVolume
 */
void music_volume_set(u8 volume) {
    f32 normalized_vol;
#ifdef NATIVE_PORT
    s32 output_volume;
#endif

    gMusicBaseVolume = volume;
    normalized_vol = gMusicSliderVolume * gMusicBaseVolume * sMusicFadeVolume;
#ifdef NATIVE_PORT
    output_volume = (s32) (gGlobalMusicVolume * normalized_vol) >> 8;
    if (gAudioVolumeSetting == VOLUME_LOWER || sOverlayPauseMix) {
        output_volume >>= 2;
    }
    alCSPSetVol(gMusicPlayer, (s16) output_volume);
#else
    alCSPSetVol(gMusicPlayer, (s16) ((s32) (gGlobalMusicVolume * normalized_vol) >> 8));
#endif
}

/**
 * Set the user configured music volume.
 * Update music volume with this new setting.
 * Official Name: amTuneSetGlobalVolume
 */
void music_volume_config_set(u32 slider_val) {
    f32 normalized_vol;
#ifdef NATIVE_PORT
    s32 volume;
#endif

    slider_val = (slider_val <= 256) ? slider_val : 256;
    gMusicSliderVolume = slider_val;
    normalized_vol = gMusicSliderVolume * gMusicBaseVolume * sMusicFadeVolume;
#ifdef NATIVE_PORT
    volume = (s32) (gGlobalMusicVolume * normalized_vol) >> 8;
    if (gAudioVolumeSetting == VOLUME_LOWER || sOverlayPauseMix) {
        volume >>= 2;
    }
    alCSPSetVol(gMusicPlayer, (s16) volume);
#else
    alCSPSetVol(gMusicPlayer, (s16) ((s32) (gGlobalMusicVolume * normalized_vol) >> 8));
#endif
}

/**
 * Return the baseline music volume, unaffected by user config.
 * Official Name: amTuneGetVolume
 */
u8 music_volume(void) {
    return gMusicBaseVolume;
}

/**
 * Return the music volume set by the player.
 */
s32 music_volume_config(void) {
    return gMusicSliderVolume;
}

/**
 * Set the volume for the jingle player.
 * The jingle player scales with sfx volume rather than music volume.
 * Official Name: amAmbientSetVolume
 */
void music_jingle_volume_set(u8 arg0) {
    sfxRelativeVolume = arg0;
#ifdef NATIVE_PORT
    alCSPSetVol(
        gJinglePlayer,
        (gAudioVolumeSetting == VOLUME_LOWER || sOverlayPauseMix)
            ? 0
            : (s16) (sndp_get_global_volume() * sfxRelativeVolume));
#else
    alCSPSetVol(gJinglePlayer, (s16) (sndp_get_global_volume() * sfxRelativeVolume));
#endif
}

/**
 * Set the panning level for every channel in the jingle player.
 * Official Name: amAmbientSetPan
 */
void music_jingle_pan_set(ALPan pan) {
    u32 iChan;
    for (iChan = 0; iChan < AUDIO_CHANNELS; iChan++) {
        alCSPSetChlPan(gJinglePlayer, iChan, pan);
    }
}

/**
 * Plays a sequence just once, allowing it to coexist with the music if necessary.
 * Examples include getting silver coins, challenge keys, or getting the locked message.
 * Official Name: amDittyPlay
 */
void music_jingle_play(u8 seqID) {
#ifdef NATIVE_PORT
    GAMEPLAY_EVENT_TRACE(GAMEPLAY_EVENT_MUSIC, seqID, 1, 0, 0);
#endif
    gCanPlayJingle = TRUE;
    music_sequence_start(gCurrentJingleID = seqID, gJinglePlayer);
}

/**
 * If there's a jingle playing, return that, otherwise, return 0.
 * Official Name: amDittyPlaying
 */
u32 music_jingle_playing(void) {
    if (gCurrentJingleID && gCanPlayJingle && (gJinglePlayer->state == AL_PLAYING)) {
        return gCurrentJingleID;
    }
    gCanPlayJingle = FALSE;
    return SEQUENCE_NONE;
}

/**
 * Sets the volume for every sound channel.
 * Tries to set up to 64, regardless of if there are 64 sound channels or not.
 * !@bug: This can cause an out of bounds array index.
 */
UNUSED void sound_channel_volume_all(u16 volume) {
    u32 i;
    for (i = 0; i < MDKR_SOUND_GROUP_COUNT; i++) {
        sndp_set_group_volume(i, volume << 8);
    }
}

/**
 * Return the audible distance of the sound effect.
 */
u16 sound_distance(u16 soundId) {
    if (!mdkr_sound_id_valid(soundId, gSoundCount)) {
        return 0;
    }
    return gSoundTable[soundId].range;
}

/**
 * Add the requested sound to the queue and update the mask to show that this sound is playing at that source.
 * If no soundmask is provided, then instead use the global mask.
 * Official Name: amSndPlay
 */
void sound_play(u16 soundID, SoundHandle *handlePtr) {
    f32 pitch;
    s32 soundBite;

    if (!mdkr_sound_id_valid(soundID, gSoundCount)) {
        if (handlePtr != NULL) {
            *handlePtr = NULL;
        }
        stubbed_printf("amSndPlay: Illegal sound effects table index\n");
        return;
    }
    soundBite = gSoundTable[soundID].soundBite;
    if (soundBite == 0) {
        if (handlePtr != NULL) {
            *handlePtr = NULL;
        }
        return;
    }
#ifdef NATIVE_PORT
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_SOUND, soundID, soundBite, 0, 0);
#endif
    pitch = gSoundTable[soundID].pitch / 100.0f;
    if (handlePtr != NULL) {
        sndp_play_with_priority(gSoundBank->bankArray[0], soundBite, gSoundTable[soundID].priority, handlePtr);
        if (*handlePtr != NULL) {
            sndp_set_param(*handlePtr, AL_SNDP_VOL_EVT, gSoundTable[soundID].volume * 256);
            sndp_set_param(*handlePtr, AL_SNDP_PITCH_EVT, *((u32 *) &pitch));
        }
    } else {
        handlePtr = &gGlobalSoundMask;
        sndp_play_with_priority(gSoundBank->bankArray[0], soundBite, gSoundTable[soundID].priority, &gGlobalSoundMask);
        if (*handlePtr != NULL) {
            sndp_set_param(*handlePtr, AL_SNDP_VOL_EVT, gSoundTable[soundID].volume * 256);
            sndp_set_param(*handlePtr, AL_SNDP_PITCH_EVT, *((u32 *) &pitch));
        }
    }
}

/**
 * Creates a spatial audio reference, then plays a sound.
 * This then makes the audio pan around in 3D space.
 * If it is not given a mask, then it will use the global mask.
 */
void sound_play_spatial(u16 soundID, f32 x, f32 y, f32 z, SoundHandle *handlePtr) {
    if (handlePtr == NULL) {
        handlePtr = &gSpatialSoundMask;
    }

    sound_play(soundID, handlePtr);

    if (*handlePtr != NULL) {
        audspat_calculate_echo(*handlePtr, x, y, z);
    }
}

/**
 * Official Name: amSndPlayDirect
 */
void sound_play_direct(u16 soundID, SoundHandle *handlePtr) {
    if (soundID <= 0 || sound_count() < soundID) {
        stubbed_printf("amSndPlayDirect: Somebody tried to play illegal sound %d\n", soundID);
        if (handlePtr) {
            *handlePtr = NULL;
        }
        return;
    }
#ifdef NATIVE_PORT
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_SOUND, soundID, soundID, 1, 0);
#endif
    if (handlePtr) {
        sndp_play(gSoundBank->bankArray[0], (s16) soundID, handlePtr);
    } else {
        sndp_play(gSoundBank->bankArray[0], (s16) soundID, &gRacerSoundMask);
    }
}

/**
 * Set the volume of the sound relative to the baseline volume of the sound ID.
 * Official Name: amSndSetVol
 */
void sound_volume_set_relative(u16 soundID, SoundHandle soundHandle, u8 volume) {
    s32 newVolume = ((s32) (gSoundTable[soundID].volume * (volume / 127.0f))) * 256;
    if (soundHandle) {
        sndp_set_param(soundHandle, AL_SNDP_VOL_EVT, newVolume);
    }
}

/**
 * Updates the volume of the given sound mask.
 */
UNUSED void sound_volume_set(SoundHandle soundHandle, u8 arg1) {
    if (soundHandle != NULL) {
        sndp_set_param(soundHandle, AL_SNDP_VOL_EVT, arg1 * 256);
    }
}

/**
 * Updates the pitch of the given sound mask.
 * Official name: amSndSetPitchDirect
 */
UNUSED void sound_pitch_set(SoundHandle soundHandle, u32 pitch) {
    u32 *pitchAddr = &pitch;
    if (soundHandle != NULL) {
        sndp_set_param(soundHandle, AL_SNDP_PITCH_EVT, *pitchAddr);
    }
}

/**
 * Return the number of playable sounds in the audio table.
 * Official name: amGetSfxCount
 */
u16 sound_count(void) {
    return gSoundBank->bankArray[0]->instArray[0]->soundCount;
}

/**
 * Return mumber of playable sequences in the table.
 */
u8 music_sequence_count(void) {
    return gSequenceTable->seqCount;
}

/**
 * Writes the sound table address, size and element count into the arguments.
 * Official Name: amGetSfxSettings
 */
void sound_table_properties(SoundData **table, s32 *size, s32 *count) {
    if (table != NULL) {
        *table = gSoundTable;
    }
    if (size != NULL) {
        *size = gSoundTableSize;
    }
    if (count != NULL) {
        *count = gSoundCount;
    }
}

/**
 * Writes the music sound table address, size and element count into the arguments.
 */
UNUSED void music_table_properties(MusicData **table, s32 *size, s32 *count) {
    if (table != NULL) {
        *table = gSeqSoundTable;
    }
    if (size != NULL) {
        *size = gSeqSoundTableSize;
    }
    if (count != NULL) {
        *count = gSeqSoundCount;
    }
}

/**
 * Returns true if the given soundID is looped.
 * Official Name: amSoundIsLooped
 */
u8 sound_is_looped(u16 soundID) {
    if (soundID <= 0 || gSoundBank->bankArray[0]->instArray[0]->soundCount < soundID) {
        return 0;
    }
    return ((u32) (1 + gSoundBank->bankArray[0]->instArray[0]->soundArray[soundID - 1]->envelope->decayTime) == 0);
}

/**
 * Allocate space, then initialise a sequence player for audio playback.
 */
ALCSPlayer *sound_seqplayer_init(s32 maxVoices, s32 maxEvents) {
    ALCSPlayer *cseqp;
    ALSeqpConfig config;

    config.maxVoices = maxVoices;
    config.maxEvents = maxEvents;
    config.voiceLimit = maxVoices; // this member doesn't exist in other versions of ALSeqpConfig
    config.maxChannels = AUDIO_CHANNELS;
    config.heap = &gALHeap;
    config.initOsc = NULL;
    config.updateOsc = NULL;
    config.stopOsc = NULL;

    cseqp = (ALCSPlayer *) alHeapAlloc(&gALHeap, 1, sizeof(ALCSPlayer));
    alCSPNew(cseqp, &config);
    alCSPSetBank(cseqp, gSequenceBank->bankArray[0]);
    cseqp->unk36 = 127;

    return cseqp;
}

/**
 * Stop the current sequence then set the parameters for the next sequence.
 */
void music_sequence_start(u8 seqID, ALCSPlayer *seqPlayer) {
    music_sequence_stop(seqPlayer);
    if (seqID < gSequenceTable->seqCount) {
        if (seqPlayer == gMusicPlayer) {
            gMusicNextSeqID = seqID;
        } else {
            gJingleNextSeqID = seqID;
        }
    } else {
        stubbed_printf("Invalid midi sequence index\n");
    }
}

/**
 * If the sequence player is currently inactive, start a new sequence with the current properties.
 */
void music_sequence_init(ALCSPlayer *seqp, void *sequence, u8 *seqID, ALCSeq *seq) {
    s32 i;

    if ((alCSPGetState(seqp) == AL_STOPPED) && (*seqID != 0)) {
        asset_load(ASSET_AUDIO, (uintptr_t)sequence,
                   gSequenceTable->seqArray[*seqID].offset - asset_rom_offset(ASSET_AUDIO, 0),
                   (s32) gSeqLengthTable[*seqID]);
#ifdef NATIVE_PORT
        /* The DKR compressed-MIDI header (ALCMidiHdr: u32 trackOffset[16] + u32
         * division) is big-endian in ROM. alCSeqNew reads those words to locate
         * each track and compute qnpt, so byte-swap the 68-byte header in place.
         * The MIDI event stream itself is byte-oriented (running status + VLQ
         * delta times assembled byte-by-byte) and needs no swap. */
        {
            u32 *hdr = (u32 *) sequence;
            s32 w;
            for (w = 0; w < 17; w++) {
                u32 v = hdr[w];
                hdr[w] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                         ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
            }
        }
#endif
        alCSeqNew(seq, sequence);
        alCSPSetSeq(seqp, seq);
        alCSPPlay(seqp);
        if (seqp == gMusicPlayer) {
            music_volume_set(gSeqSoundTable[*seqID].volume);
            if (gSeqSoundTable[*seqID].tempo != 0) {
                music_tempo_set(gSeqSoundTable[*seqID].tempo);
            } else {
                sMusicTempo = -1;
            }
            sound_reverb_set(gSeqSoundTable[*seqID].reverb);
            gCurrentSequenceID = *seqID;
            if (gDynamicMusicChannelMask != MUSIC_CHAN_MASK_NONE) {
                for (i = 0; i < AUDIO_CHANNELS; i++) {
                    if ((1 << i) & gDynamicMusicChannelMask) {
                        music_channel_on(i);
                    } else {
                        music_channel_off(i);
                    }
                }
            }
        } else {
            music_jingle_volume_set(gSeqSoundTable[*seqID].volume);
            if (gSeqSoundTable[*seqID].tempo != 0) {
                sound_jingle_tempo_set(gSeqSoundTable[*seqID].tempo);
            }
            gCurrentJingleID = *seqID;
        }
        *seqID = SEQUENCE_NONE;
    }
}

/**
 * Stops the current playing sequence for the given sequence player.
 */
void music_sequence_stop(ALCSPlayer *seqPlayer) {
    if (gMusicPlayer == seqPlayer && gMusicPlaying) {
        alCSPStop(seqPlayer);
        gMusicPlaying = FALSE;
        gCurrentSequenceID = SEQUENCE_NONE;
        gMusicNextSeqID = SEQUENCE_NONE;
    } else if (gJinglePlayer == seqPlayer && gJinglePlaying) {
        alCSPStop(seqPlayer);
        gJinglePlaying = FALSE;
        gJingleNextSeqID = SEQUENCE_NONE;
    }
}

/**
 * Enable or disable special audio effects.
 * This includes reverb and echo.
 * Official Name: amTuneSetReverbOnOff
 */
void sound_reverb_set(u8 setting) {
#ifdef NATIVE_PORT
    /* Honor MDKR_AUDIO_REVERB=0 (see audio_init): the game turns reverb on
     * per-sequence (music_sequence_init), which would re-enable the wet path
     * that the env var asked to keep off for an A/B capture. */
    if (!gDkrReverbEnabled) {
        return;
    }
#endif
    alFxReverbSet(setting);
}

/**
 * Returns whether or not reverb is currently enabled.
 */
UNUSED u8 sound_reverb_enabled(void) {
    return gSeqSoundTable[gCurrentSequenceID].reverb;
}
