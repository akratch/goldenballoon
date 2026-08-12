#include "audiomgr.h"
#ifdef NATIVE_PORT
#include "asset_swap.h"
#include "audi_port_dkr.h"   /* MDKR_AUDIO_MAX_FRAME_SAMPLES mirror only */
#endif
#include "asset_loading.h"
#include "common.h"
#include "math_util.h"
#include "memory.h"
#include "objects.h"
#include "runtime_contracts.h"
#include "PR/abi.h"
#include "rcp_dkr.h"
#include "stacks.h"
#include "video.h"

/****  type define's for structures unique to audiomgr ****/
typedef union {

    struct {
        short type;
    } gen;

    struct {
        short type;
        struct AudioInfo_s *info;
    } done;

    OSScMsg app;

} AudioMsg;

typedef struct AudioInfo_s {
    short *data;        /* Output data pointer */
    short frameSamples; /* # of samples synthesized in this frame */
    OSScTask task;      /* scheduler structure */
    AudioMsg msg;       /* completion message */
} AudioInfo;

typedef struct {
    Acmd *ACMDList[NUM_ACMD_LISTS];
    AudioInfo *audioInfo[NUM_OUTPUT_BUFFERS];
    OSThread thread;
    OSMesgQueue audioFrameMsgQ;
    OSMesg audioFrameMsgBuf[MAX_MESGS];
    OSMesgQueue audioReplyMsgQ;
    OSMesg audioReplyMsgBuf[MAX_MESGS];
    ALGlobals g;
} AMAudioMgr;

typedef struct {
    ALLink node;
    u32 startAddr;
    u32 lastFrame;
    char *ptr;
} AMDMABuffer;

typedef struct {
    u8 initialized;
    AMDMABuffer *firstUsed;
    AMDMABuffer *firstFree;
} AMDMAState;

/**** audio manager globals ****/

OSSched *gAudioSched;
ALHeap *gAudioHeap; // Set but not used

AMAudioMgr __am;
static u64 audioStack[STACKSIZE(STACK_AUD)];

AMDMAState dmaState;
AMDMABuffer dmaBuffs[NUM_DMA_BUFFERS];
u32 audFrameCt = 0;
u32 nextDMA = 0;
u32 curAcmdList = 0;
u32 minFrameSize;
u32 frameSize;
u32 maxFrameSize;
s32 gAudioCmdLen; // Set but not used

/**** Anti Piracy - Sets random audio frequency ****/
s16 gAntiPiracyCRCStart;
s8 gAntiPiracyAudioFreq = FALSE;
s32 gRaceCheckFinishChecksum = Func80019808Checksum;
s32 gRaceCheckFinishFuncLength = 0xFD0;

/** Queues and storage for use with audio DMA's ****/
OSIoMesg audDMAIOMesgBuf[NUM_DMA_MESSAGES];
OSMesgQueue audDMAMessageQ;
OSMesg audDMAMessageBuf[NUM_DMA_MESSAGES];

/**** Not really sure why these are here. They are set, but never used. ****/
static s16 *gLastAudioPtr = 0;
static s32 gLastAudioFrameSamples = 0;

#ifdef NATIVE_PORT
/* M5 audio — port per-frame synthesis pump. Replaces the audio thread
 * (__amMain never runs in the cooperative model). Clean, arena-resident output
 * buffers replace the N64 RAM_END fixed block + Acmd-as-AudioInfo overlay. */
/*
 * Per-call synthesis ceiling, in stereo sample-frames. 1,792 is four source VI
 * fields at 50 Hz (1,764) rounded up to the synthesizer's 16-frame alignment;
 * the previous 1,600 was four fields at 60 Hz only, so on a PAL source a
 * refill that had to cover a long host gap was silently truncated and the
 * deficit was never recovered. Four fields at 60 Hz (1,470) still fits.
 */
#define PORT_MAX_FRAME_SAMPLES  1792
/* Published for the ROM-free delivery model — see platform/audi_port_dkr.h.
 * This file owns the number; the header only mirrors it, and this pins them. */
_Static_assert(PORT_MAX_FRAME_SAMPLES == (int)MDKR_AUDIO_MAX_FRAME_SAMPLES,
               "audiomgr synthesis cap and its published mirror must agree");
static s16 *sPortOutBuf[NUM_OUTPUT_BUFFERS];
static u32  sPortOutIdx;
static u32  sPortAcmdIdx;
int gDkrAudioReady = 0;   /* set once amCreateAudioMgr has finished */
#endif

/**** private routines ****/
static void __amMain(UNUSED void *arg);
static s32 __amDMA(s32 addr, s32 len, void *state);
static ALDMAproc __amDmaNew(AMDMAState **state);
static u32 __amHandleFrameMsg(AudioInfo *info, AudioInfo *lastInfo);
static void __amHandleDoneMsg(AudioInfo *info);
static void __clearAudioDMA(void);

/**** Debug strings ****/
const char D_800E49F0[] =
    "audio manager: RCP audio interface bug caused DMA from bad address - move audiomgr.c in the makelist!\n";

/******************************************************************************
 * Audio Manager API
 *****************************************************************************/
void amCreateAudioMgr(ALSynConfig *c, OSPri pri, OSSched *audSched) {
    s32 i;
    f32 fsize;
    uintptr_t *asset;
    u32 *assetAudioTable;
    s32 *asset8;
    s32 assetOffset;
    s32 assetSize;
    s32 checksum;
    u8 *crc_region_start;
    u8 *crc_region;

    gAudioSched = audSched;
    gAudioHeap = c->heap;

#ifdef NATIVE_PORT
    /* Bring up the software aspMain mixer that the abi.h a* macros now call
     * (platform/mixer.c). One-time reset of its RSP-emulator state. */
    mixerInit();
#endif

    c->dmaproc = &__amDmaNew;
    c->outputRate = osAiSetFrequency(OUTPUT_RATE);

    /*
     * Calculate the frame sample parameters from the
     * video field rate and the output rate
     */
#ifdef NATIVE_PORT
    if (gVideoRefreshRate == 0) {
        gVideoRefreshRate = 60;   /* guard: audio_init can precede video_init */
    }
#endif
    fsize = (f32) c->outputRate * 2 / (f32) gVideoRefreshRate;
    frameSize = (s32) fsize;
    if (frameSize < fsize) {
        frameSize++;
    }
    if (frameSize & 0xf) {
        frameSize = (frameSize & ~0xf) + 0x10;
    }
    minFrameSize = frameSize - 16;
    maxFrameSize = frameSize + EXTRA_SAMPLES + 16;

    if (c->fxType[0] == AL_FX_CUSTOM) {
        assetAudioTable = asset_table_load(ASSET_AUDIO_TABLE);
#ifdef NATIVE_PORT
        asset8 = NULL;
        if (!mdkr_audio_fx_span(assetAudioTable,
                                (size_t) MAX(asset_table_size(ASSET_AUDIO_TABLE), 0),
                                (size_t) MAX(asset_table_size(ASSET_AUDIO), 0),
                                &assetOffset, &assetSize)) {
            stubbed_printf("[AUDIO] invalid custom-FX asset span; using dry mixer\n");
            if (assetAudioTable != NULL) {
                mempool_free(assetAudioTable);
            }
            c->fxType[0] = AL_FX_NONE;
            c->params = NULL;
            alInit(&__am.g, c);
        } else {
            asset8 = mempool_alloc(assetSize, COLOUR_TAG_CYAN);
            if (asset8 == NULL) {
                stubbed_printf("[AUDIO] custom-FX allocation failed; using dry mixer\n");
                mempool_free(assetAudioTable);
                c->fxType[0] = AL_FX_NONE;
                c->params = NULL;
                alInit(&__am.g, c);
            } else {
                asset_load(ASSET_AUDIO, (uintptr_t) asset8, assetOffset, assetSize);
                /* The custom-FX param blob is a raw BE s32[] inside ASSET_AUDIO,
                 * which the load-time swapper deliberately punts on. */
                asset_swap_lut(asset8, assetSize);
                c->params = asset8;
                /* No `c[1].maxVVoices = 0` here. The original writes one
                 * audioMgrConfig past `c`, which in audio_init is a single stack
                 * struct: on N64 a harmless out-of-bounds stack scribble, on LP64
                 * a real stack-buffer-overflow that ASan flags. Omitting it is
                 * behaviour-preserving because nothing reads the value back --
                 * alInit() takes only c[0]. The matching build below keeps the
                 * write. */
                alInit(&__am.g, c);
                c->params = NULL;
                mempool_free(asset8);
                mempool_free(assetAudioTable);
            }
        }
#else
        assetOffset = assetAudioTable[ASSET_AUDIO_8];
        assetSize = assetAudioTable[ASSET_AUDIO_9] - assetOffset;
        asset8 = mempool_alloc_safe(assetSize, COLOUR_TAG_CYAN);
        asset_load(ASSET_AUDIO, (u32) asset8, assetOffset, assetSize);
        c->params = asset8;
        c[1].maxVVoices = 0;
        alInit(&__am.g, c);
        mempool_free(asset8);
        /* Matching build intentionally retains the original ownership bug. */
#endif
    } else {
        alInit(&__am.g, c);
    }

    dmaBuffs[0].node.prev = 0;
    dmaBuffs[0].node.next = 0;

    for (i = 0; i < NUM_DMA_BUFFERS - 1; i++) {
        alLink((ALLink *) &(dmaBuffs[i + 1]), (ALLink *) &(dmaBuffs[i]));
        dmaBuffs[i].ptr = alHeapAlloc(c->heap, 1, DMA_BUFFER_LENGTH);
    }
    /* last buffer already linked, but still needs buffer */
    dmaBuffs[i].ptr = alHeapAlloc(c->heap, 1, DMA_BUFFER_LENGTH);
#ifdef ANTI_TAMPER
    // Antipiracy measure
    gAntiPiracyCRCStart = DMA_BUFFER_LENGTH;
    checksum = (s32) &race_check_finish - gAntiPiracyCRCStart;
    crc_region_start = (u8 *) checksum;
    crc_region = &crc_region_start[gAntiPiracyCRCStart];
    gAntiPiracyAudioFreq = FALSE;
    i = 0;
    for (checksum = 0; i < gRaceCheckFinishFuncLength; i++) {
        checksum += crc_region[i];
        gAntiPiracyCRCStart++;
    }
    if (checksum != gRaceCheckFinishChecksum) {
        gAntiPiracyAudioFreq = TRUE;
    }
#endif

    for (i = 0; i < NUM_ACMD_LISTS; i++) {
        __am.ACMDList[i] = (Acmd *) alHeapAlloc(c->heap, 1, AUDBUF_SIZE);
    }

#ifdef NATIVE_PORT
    /* The N64 code carves the 3 AudioInfo output buffers out of a fixed block
     * at the very top of RDRAM (RAM_END) and overlays them on ACMDList[2..4]
     * (see __amMain). That fixed N64 address is meaningless on the port and the
     * overlay is fragile on LP64. Allocate clean, arena-resident stereo-s16
     * output buffers instead; amAudioSynthFrame() writes synthesized PCM here.
     * Arena-resident so the mixer reconstructs the address the ALSave filter
     * truncates to s32 (save.c). */
    (void) asset;
    for (i = 0; i < NUM_OUTPUT_BUFFERS; i++) {
        sPortOutBuf[i] = (s16 *) alHeapAlloc(c->heap, 1, PORT_MAX_FRAME_SAMPLES * 4);
    }
    sPortOutIdx = 0;
    sPortAcmdIdx = 0;
#else
    asset = mempool_alloc_fixed((maxFrameSize * 12), (u8 *) ((RAM_END - 0x200) - (maxFrameSize * 12)), COLOUR_TAG_CYAN);

    /**** initialize the done messages ****/
    for (i = 0; i < NUM_OUTPUT_BUFFERS; i++) {
        /*
         * The original expression indexed ACMDList[2..4], relying on the
         * adjacent audioInfo member as an implicit array continuation. That is
         * out-of-bounds C even though the historical layout made the addresses
         * coincide. Name the member that owns these three AudioInfo pointers.
         */
        __am.audioInfo[i] =
            (AudioInfo *) alHeapAlloc(c->heap, 1, 120);
        __am.audioInfo[i]->data = (short *)asset;
        asset += maxFrameSize;
    }
#endif

    osCreateMesgQueue(&__am.audioReplyMsgQ, __am.audioReplyMsgBuf, MAX_MESGS);
    osCreateMesgQueue(&__am.audioFrameMsgQ, __am.audioFrameMsgBuf, MAX_MESGS);
    osCreateMesgQueue(&audDMAMessageQ, audDMAMessageBuf, NUM_DMA_MESSAGES);
    osCreateThread(&__am.thread, 4, __amMain, 0, (void *) (audioStack + STACKSIZE(STACK_AUD)), pri);

#ifdef NATIVE_PORT
    /* The audio manager is now fully initialized; allow the per-frame pump. */
    gDkrAudioReady = 1;
#endif
}

/**
 * Official Name: amGo
 */
void audioStartThread(void) {
    osStartThread(&__am.thread);
}

/**
 * Official Name: amGoStop
 */
void audioStopThread(void) {
    osStopThread(&__am.thread);
}

#ifdef NATIVE_PORT
/* Synthesize one audio frame of `frameSamples` stereo sample-frames into a
 * rotating arena output buffer and return it. Reproduces the core of
 * __amHandleFrameMsg (minus the N64 triple-buffer + M_AUDTASK scheduler
 * round-trip): recycle finished DMA buffers, then alAudioFrame() — which, via
 * the mixer.h macro overrides of abi.h, runs the whole libultra synthesis
 * chain (sequence + sound players -> ADPCM decode -> resample -> envmix ->
 * reverb) directly into the output buffer through platform/mixer.c. Driven
 * once per independently due audio quantum by the platform service
 * (platform/audi_port_dkr.c). */
s16 *amAudioSynthFrame(s32 frameSamples) {
    s16 *outBuf;

    if (!gDkrAudioReady) {
        return NULL;
    }
    if (frameSamples < 16) {
        frameSamples = 16;
    }
    if (frameSamples > PORT_MAX_FRAME_SAMPLES) {
        frameSamples = PORT_MAX_FRAME_SAMPLES;
    }
    frameSamples &= ~0xf;

    __clearAudioDMA(); /* recycle finished DMA sample buffers, advance frame ct */

    outBuf = sPortOutBuf[sPortOutIdx];
    sPortOutIdx++;
    if (sPortOutIdx >= NUM_OUTPUT_BUFFERS) {
        sPortOutIdx = 0;
    }

    alAudioFrame(__am.ACMDList[sPortAcmdIdx], &gAudioCmdLen, outBuf, frameSamples);
    sPortAcmdIdx ^= 1;

    return outBuf;
}

u32 amAudioGetFrameSize(void)    { return frameSize; }
u32 amAudioGetMinFrameSize(void) { return minFrameSize; }
u32 amAudioGetMaxFrameSize(void) { return maxFrameSize; }
u32 amAudioGetMaxSamples(void)   { return PORT_MAX_FRAME_SAMPLES; }

void amAudioSessionShutdown(void) {
    gDkrAudioReady = 0;
    alClose(&__am.g);
    bzero(&__am, sizeof(__am));
    bzero(&dmaState, sizeof(dmaState));
    bzero(dmaBuffs, sizeof(dmaBuffs));
    bzero(audDMAIOMesgBuf, sizeof(audDMAIOMesgBuf));
    bzero(&audDMAMessageQ, sizeof(audDMAMessageQ));
    bzero(audDMAMessageBuf, sizeof(audDMAMessageBuf));
    bzero(sPortOutBuf, sizeof(sPortOutBuf));
    sPortOutIdx = 0u;
    sPortAcmdIdx = 0u;
    audFrameCt = 0u;
    nextDMA = 0u;
    curAcmdList = 0u;
    minFrameSize = 0u;
    frameSize = 0u;
    maxFrameSize = 0u;
    gAudioCmdLen = 0;
    gLastAudioPtr = NULL;
    gLastAudioFrameSamples = 0;
}
#endif

/******************************************************************************
 *
 * Audio Manager implementation. This thread wakes up at every retrace,
 * and builds an audio task, which it returns to the scheduler, who then
 * is responsible for its finally execution on the RSP. Once the task has
 * finished execution, the scheduler sends back a message saying the task
 * is complete. The audio is triple buffered because the switching to a new
 * audio buffer does not occur exactly at the gfx swapbuffer time.  With
 * 3 buffers you ensure that the program does not destroy data before it is
 * played.
 *
 *****************************************************************************/
/**
 * Main function for handling ingame audio. Loops continuously as long as the scheduler feeds it updates.
 */
static void __amMain(UNUSED void *arg) {
    s32 done = 0;
    AudioMsg *msg = NULL;
    AudioInfo *lastInfo = 0;

    osScAddClient(gAudioSched, (OSScClient *) &audioStack, &__am.audioFrameMsgQ, OS_MESG_BLOCK);

    while (!done) {
        (void) osRecvMesg(&__am.audioFrameMsgQ, (OSMesg *) &msg, OS_MESG_BLOCK);
        switch (msg->gen.type) {
            case OS_SC_RETRACE_MSG:
                __amHandleFrameMsg(
                    __am.audioInfo[(u32)audFrameCt % NUM_OUTPUT_BUFFERS],
                    lastInfo);
                /* wait for done message */
                osRecvMesg(&__am.audioReplyMsgQ, (OSMesg *) &lastInfo, OS_MESG_BLOCK);
                __amHandleDoneMsg(lastInfo);
                break;
            case OS_SC_PRE_NMI_MSG:
                /* what should we really do here? quit? ramp down volume? */
                break;
            case QUIT_MSG:
                done = 1;
                break;
            default:
                break;
        }
    }

    alClose(&__am.g);
}

/******************************************************************************
 *
 * __amHandleFrameMsg. First, clear the past audio dma's, then calculate
 * the number of samples you will need for this frame. This value varies
 * due to the fact that audio is synchronised off of the video interupt
 * which can have a small amount of jitter in it. Varying the number of
 * samples slightly will allow you to stay in synch with the video. This
 * is an advantageous thing to do, since if you are in synch with the
 * video, you will have fewer graphics yields. After you've calculated
 * the number of frames needed, call alAudioFrame, which will call all
 * of the synthesizer's players (sequence player and sound player) to
 * generate the audio task list. If you get a valid task list back, put
 * it in a task structure and send a message to the scheduler to let it
 * know that the next frame of audio is ready for processing.
 *
 *****************************************************************************/
static u32 __amHandleFrameMsg(AudioInfo *info, AudioInfo *lastInfo) {
    s16 *audioPtr;
    Acmd *cmdp;
    int samplesLeft = 0;
    OSScTask *t;
    u32 ret;

    __clearAudioDMA(); /* call once a frame, before doing alAudioFrame */

#ifdef NATIVE_PORT
    /* The HLE mixer accepts the live arena pointer; downstream Acmd packing
     * converts it to the deliberate 32-bit audio token at the command seam. */
    audioPtr = info->data;
#else
    audioPtr = (s16 *) osVirtualToPhysical(info->data);
#endif

    if (lastInfo) {
        s16 *outputDataPointer;
        s32 frameSamples;

        gLastAudioPtr = outputDataPointer = lastInfo->data;
        gLastAudioFrameSamples = frameSamples = lastInfo->frameSamples << 2;
        osAiSetNextBuffer(outputDataPointer, frameSamples);
#ifdef ANTI_TAMPER
        // Antipiracy measure
        if (gAntiPiracyAudioFreq) {
            osAiSetFrequency(rand_range(0, 10000) + OUTPUT_RATE);
        }
#endif
    }

    /* calculate how many samples needed for this frame to keep the DAC full */
    /* this will vary slightly frame to frame, must recalculate every frame */
    samplesLeft = osAiGetLength() >> 2; /* divide by four, to convert bytes */
                                        /* to stereo 16 bit samples */
    info->frameSamples = (16 + (frameSize - samplesLeft + EXTRA_SAMPLES)) & ~0xf;
    if ((u32) info->frameSamples < minFrameSize) {
        info->frameSamples = minFrameSize;
    }

    cmdp = alAudioFrame(__am.ACMDList[curAcmdList], &gAudioCmdLen, audioPtr, info->frameSamples);

    t = &info->task;

    t->next = 0;                    /* paranoia */
    t->msgQ = &__am.audioReplyMsgQ; /* reply to when finished */
    // TODO: This should be &info->msg, but the struct is likely modified.
    t->msg = (OSMesg) &info->data; /* reply with this message */
    t->flags = OS_SC_NEEDS_RSP;
    t->unk58 = -1;
    t->unk60 = 0xFF;
    t->unk5C = 0;
    t->unk64 = 0;

    t->list.t.data_ptr = (u64 *) __am.ACMDList[curAcmdList];
    t->list.t.data_size = (cmdp - __am.ACMDList[curAcmdList]) * sizeof(Acmd);
    t->list.t.type = M_AUDTASK;
    t->list.t.ucode_boot = (u64 *) rspbootTextStart;
#ifdef NATIVE_PORT
    t->list.t.ucode_boot_size = 0; /* HLE never executes the placeholder blob. */
#else
    t->list.t.ucode_boot_size =
        (s32) ((uintptr_t) rspbootTextEnd - (uintptr_t) rspbootTextStart);
#endif
    t->list.t.flags = OS_TASK_DP_WAIT;
    t->list.t.ucode = (u64 *) aspMainTextStart;
    t->list.t.ucode_data = (u64 *) aspMainDataStart;
    t->list.t.ucode_data_size = SP_UCODE_DATA_SIZE;
    t->list.t.yield_data_ptr = NULL;
    t->list.t.yield_data_size = 0;
    t->unk6C = 1;

    ret = osSendMesg(osScGetCmdQ(gAudioSched), (OSMesg) t, OS_MESG_NOBLOCK);

    curAcmdList ^= 1; /* swap which acmd list you use each frame */

    return ret;
}

/******************************************************************************
 *
 * __amHandleDoneMsg. Really just debugging info in this frame. Checks
 * to make sure we completed before we were out of samples.
 *
 *****************************************************************************/
static void __amHandleDoneMsg(UNUSED AudioInfo *info) {
    s32 samplesLeft;
    static int firstTime = 1;

    samplesLeft = osAiGetLength() >> 2;
    if (samplesLeft == 0 && !firstTime) {
        stubbed_printf("audio: ai out of samples\n");
        firstTime = 0;
    }
}

/******************************************************************************
 *
 * __amDMA This routine handles the dma'ing of samples from rom to ram.
 * First it checks the current buffers to see if the samples needed are
 * already in place. Because buffers are linked sequentially by the
 * addresses where the samples are on rom, it doesn't need to check all
 * of them, only up to the address that it needs. If it finds one, it
 * returns the address of that buffer. If it doesn't find the samples
 * that it needs, it will initiate a DMA of the samples that it needs.
 * In either case, it updates the lastFrame variable, to indicate that
 * this buffer was last used in this frame. This is important for the
 * __clearAudioDMA routine.
 *
 *****************************************************************************/
static s32 __amDMA(s32 addr, s32 len, UNUSED void *state) {
    void *foundBuffer;
    s32 delta, addrEnd, buffEnd;
    AMDMABuffer *dmaPtr, *lastDmaPtr;
    UNUSED s32 pad;

    lastDmaPtr = NULL;
    delta = addr & 1;
    dmaPtr = dmaState.firstUsed;
    addrEnd = addr + len;

    /* first check to see if a currently existing buffer contains the
       sample that you need.  */

    while (dmaPtr) {
        buffEnd = dmaPtr->startAddr + DMA_BUFFER_LENGTH;
        if (dmaPtr->startAddr > (u32) addr) { /* since buffers are ordered */
            break;                            /* abort if past possible */
        } else if (addrEnd <= buffEnd) {      /* yes, found a buffer with samples */
            dmaPtr->lastFrame = audFrameCt;   /* mark it used */
            foundBuffer = dmaPtr->ptr + addr - dmaPtr->startAddr;
            return (int) osVirtualToPhysical(foundBuffer);
        }
        lastDmaPtr = dmaPtr;
        dmaPtr = (AMDMABuffer *) dmaPtr->node.next;
    }

    /* get here, and you didn't find a buffer, so dma a new one */

    /* get a buffer from the free list */
    dmaPtr = dmaState.firstFree;

    if (!dmaPtr && !lastDmaPtr) {
        lastDmaPtr = dmaState.firstUsed;
    }

    /*
     * if you get here and dmaPtr is null, send back a bogus
     * pointer, it's better than nothing
     */
    if (!dmaPtr) {
        stubbed_printf("OH DEAR - No audio DMA buffers left\n");
        return (int) osVirtualToPhysical(lastDmaPtr->ptr) + delta;
    }

    dmaState.firstFree = (AMDMABuffer *) dmaPtr->node.next;
    alUnlink((ALLink *) dmaPtr);

    /* add it to the used list */
    if (lastDmaPtr) { /* if you have other dmabuffers used, add this one */
                      /* to the list, after the last one checked above */
        alLink((ALLink *) dmaPtr, (ALLink *) lastDmaPtr);
    } else if (dmaState.firstUsed) { /* if this buffer is before any others */
                                     /* jam at begining of list */
        lastDmaPtr = dmaState.firstUsed;
        dmaState.firstUsed = dmaPtr;
        dmaPtr->node.next = (ALLink *) lastDmaPtr;
        dmaPtr->node.prev = 0;
        lastDmaPtr->node.prev = (ALLink *) dmaPtr;
    } else { /* no buffers in list, this is the first one */
        dmaState.firstUsed = dmaPtr;
        dmaPtr->node.next = 0;
        dmaPtr->node.prev = 0;
    }

    foundBuffer = dmaPtr->ptr;
    addr -= delta;
    dmaPtr->startAddr = addr;
    dmaPtr->lastFrame = audFrameCt; /* mark it */

    osPiStartDma(&audDMAIOMesgBuf[nextDMA++], OS_MESG_PRI_HIGH, OS_READ, addr, foundBuffer, DMA_BUFFER_LENGTH,
                 &audDMAMessageQ);

    return (int) osVirtualToPhysical(foundBuffer) + delta;
}

/******************************************************************************
 *
 * __amDmaNew.  Initialize the dma buffers and return the address of the
 * procedure that will be used to dma the samples from rom to ram. This
 * routine will be called once for each physical voice that is created.
 * In this case, because we know where all the buffers are, and since
 * they are not attached to a specific voice, we will only really do any
 * initialization the first time. After that we just return the address
 * to the dma routine.
 *
 *****************************************************************************/
static ALDMAproc __amDmaNew(AMDMAState **state) {

    if (!dmaState.initialized) { /* only do this once */
        dmaState.firstUsed = 0;
        dmaState.firstFree = &dmaBuffs[0];
        dmaState.initialized = 1;
    }

    *state = &dmaState; /* state is never used in this case */

    return __amDMA;
}

/******************************************************************************
 *
 * __clearAudioDMA.  Routine to move dma buffers back to the unused list.
 * First clear out your dma messageQ. Then check each buffer to see when
 * it was last used. If that was more than FRAME_LAG frames ago, move it
 * back to the unused list.
 *
 *****************************************************************************/
static void __clearAudioDMA(void) {
    u32 i;
    OSIoMesg *iomsg = 0;
    AMDMABuffer *dmaPtr, *nextPtr;

    /*
     * Don't block here. If dma's aren't complete, you've had an audio
     * overrun. (Bad news, but go for it anyway, and try and recover.)
     */
    for (i = 0; i < nextDMA; i++) {
        if (osRecvMesg(&audDMAMessageQ, (OSMesg *) &iomsg, OS_MESG_NOBLOCK) == -1) {
            stubbed_printf("Dma not done\n");
        }
        // if (logging)
        //     osLogEvent(log, 17, 2, iomsg->devAddr, iomsg->size);
    }

    dmaPtr = dmaState.firstUsed;
    while (dmaPtr) {
        nextPtr = (AMDMABuffer *) dmaPtr->node.next;

        /* remove old dma's from list */
        /* Can change FRAME_LAG value.  Should be at least one.  */
        /* Larger values mean more buffers needed, but fewer DMA's */
        if (dmaPtr->lastFrame + FRAME_LAG < audFrameCt) {
            if (dmaState.firstUsed == dmaPtr) {
                dmaState.firstUsed = (AMDMABuffer *) dmaPtr->node.next;
            }
            alUnlink((ALLink *) dmaPtr);
            if (dmaState.firstFree) {
                alLink((ALLink *) dmaPtr, (ALLink *) dmaState.firstFree);
            } else {
                dmaState.firstFree = dmaPtr;
                dmaPtr->node.next = 0;
                dmaPtr->node.prev = 0;
            }
        }
        dmaPtr = nextPtr;
    }

    nextDMA = 0; /* reset */
    audFrameCt++;
}
