#ifndef _AUDIOMGR_H_
#define _AUDIOMGR_H_

#include "types.h"
#include "macros.h"
#include "asset_enums.h"
#include <ultra64.h>
#include "sched.h"
#include "PR/libaudio.h"

#define MAX_UPDATES             32
#define MAX_EVENTS              32
#ifdef NATIVE_PORT
/* LP64: the libultra audio structures (ALVoice/ALSeq/event queues/players)
 * carry 8-byte pointers, so the N64-tuned 0x29D88 heap overflows during
 * audio_init (the 2nd sequence player's event queue alloc returns NULL). The
 * heap is a static BSS array (gAudioHeapStack), so oversizing it just costs
 * BSS. 4x is comfortable headroom. */
#define AUDIO_HEAP_SIZE         (0x29D88 * 4)
#else
#define AUDIO_HEAP_SIZE         0x29D88
#endif
#define AUDBUF_SIZE             0xA000  /* Audio command buffer size */

#define MAX_VOICES              22
#define EXTRA_SAMPLES           96
#define NUM_OUTPUT_BUFFERS      3      /* Need three of these */
#define OUTPUT_RATE             22050
#define MAX_MESGS               8
#define QUIT_MSG                10

#define DMA_BUFFER_LENGTH       0x400  /* Larger buffers result in fewer DMA' but more  */
                                       /* memory being used.  */


#define NUM_ACMD_LISTS          2      /* two lists used by this example                */
#define MAX_RSP_CMDS            4096   /* max number of commands in any command list.   */
                                       /* Mainly dependent on sequences used            */

#define NUM_DMA_BUFFERS         50     /* max number of dma buffers needed.             */
                                       /* Mainly dependent on sequences and sfx's       */

#define NUM_DMA_MESSAGES        50     /* The maximum number of DMAs any one frame can  */
                                       /* have.                                         */

#define FRAME_LAG               1      /* The number of frames to keep a dma buffer.    */
                                       /* Increasing this number causes buffers to not  */
                                       /* be deleted as quickly. This results in fewer  */
                                       /* DMA's but you need more buffers.              */

#define MAX_SEQ_LENGTH  20000

void audioStartThread(void);
void audioStopThread(void);
void amCreateAudioMgr(ALSynConfig *c, OSPri pri, OSSched *audSched);

#ifdef NATIVE_PORT
/* M5 audio — per-frame synthesis pump (audiomgr.c), driven by the platform
 * audio driver (platform/audi_port_dkr.c). */
extern int gDkrAudioReady;
s16 *amAudioSynthFrame(s32 frameSamples);
u32  amAudioGetFrameSize(void);
u32  amAudioGetMinFrameSize(void);
u32  amAudioGetMaxFrameSize(void);
u32  amAudioGetMaxSamples(void);
/* Release synth globals before the arena backing them is destroyed. */
void amAudioSessionShutdown(void);
#endif

#endif
