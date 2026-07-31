/*
 * sched.h — clean-room display/audio task scheduler declarations.
 *
 * Declares the client and task-queue types (OSScClient, OSScTask,
 * OSSched) and the entry points used to hand RSP/RDP graphics and audio
 * work to the retrace-driven scheduler thread; this is a
 * declaration-only compatibility surface with no vendor implementation
 * present.
 */

#ifndef __sched__
#define __sched__

#include <ultra64.h>
#include "macros.h"
#include "os_internal_rsp.h"

#define OS_SC_STACKSIZE      0x2000

#define OS_SC_RETRACE_MSG       1
#define OS_SC_DONE_MSG          2
#define OS_SC_RDP_DONE_MSG      3
#define OS_SC_PRE_NMI_MSG       4
#define OS_SC_LAST_MSG          4	/* this should have highest number */
#define OS_SC_MAX_MESGS         8

#define OS_SC_ID_NONE   0
#define OS_SC_ID_AUDIO  1
#define OS_SC_ID_VIDEO  2
#define OS_SC_ID_PRENMI 3

#define OSMESG_SWAP_BUFFER 0
#define MESG_SKIP_BUFFER_SWAP 8

typedef struct {
    short type;
    char  misc[30];
} OSScMsg;

typedef struct OSScTask_s {
    struct OSScTask_s   *next;          /* must stay the first field */
    u32                 state;
    u32			flags;
    void		*framebuffer;	/* used by graphics tasks */

    OSTask              list;
    OSMesgQueue         *msgQ;
    OSMesg              msg;
#ifndef _FINALROM                      /* conditionally-compiled     */
    OSTime              startTime;      /* members like these need to */
    OSTime              totalTime;      /* stay at the struct's tail, */
#endif                                  /* or layout drifts by build  */
    s32                 unk58;
    s32                 unk5C;
    s32                 unk60;
    s32                 unk64;
    s32                 unk68;          /* Added by Rare?             */
    s32                 unk6C;          /* Task ID, used in debug functions in JFG */
} OSScTask;                             /* config (debug vs. FINALROM) */

/*
 * OSScTask flags:
 */
#define OS_SC_NEEDS_RDP	        0x0001	/* uses the RDP */
#define OS_SC_NEEDS_RSP	        0x0002  /* uses the RSP */
#define OS_SC_DRAM_DLIST        0x0004  /* SP & DP communicate through DRAM */
#define OS_SC_PARALLEL_TASK     0x0010	/* must be first gfx task on list */
#define OS_SC_LAST_TASK	        0x0020	/* last task in queue for frame */
#define OS_SC_SWAPBUFFER        0x0040	/* swapbuffers when gfx task done */

#define OS_SC_RCP_MASK		0x0003	/* mask for needs bits */
#define OS_SC_TYPE_MASK		0x0007	/* complete type mask */
/*
 * OSScClient:
 *
 * Lets a thread register itself with the scheduler so it can receive
 * the frame-related messages the scheduler thread posts out.
 */
typedef struct SCClient_s {
    u8                  id;   /* Client ID, added by Rareware to single out individual scheduler clients */
    struct SCClient_s   *next;  /* next client in the list      */
    OSMesgQueue         *msgQ;  /* where to send the frame msg  */
} OSScClient;

typedef struct {
    OSScMsg     retraceMsg;
    OSScMsg     prenmiMsg;
    OSMesgQueue interruptQ;
    OSMesg      intBuf[OS_SC_MAX_MESGS];
    OSMesgQueue cmdQ;
    OSMesg      cmdMsgBuf[OS_SC_MAX_MESGS];
    OSThread    thread;
    OSScClient  *clientList;
    OSScTask    *audioListHead;
    OSScTask    *gfxListHead;
    OSScTask    *audioListTail;
    OSScTask    *gfxListTail;
    OSScTask    *curRSPTask;
    OSScTask    *curRDPTask;
   OSScTask    *unkTask;
    u32         frameCount;
    s32         doAudio;
} OSSched;

void            osCreateScheduler(OSSched *s, void *stack, OSPri priority,
                                  u8 mode, u8 numFields);
void            osScAddClient(OSSched *s, OSScClient *c, OSMesgQueue *msgQ, u8 id);
OSMesgQueue    *osScGetInterruptQ(OSSched *s);
void            func_80079760(OSSched *s);
void            osScRemoveClient(OSSched *s, OSScClient *c);
OSMesgQueue     *osScGetCmdQ(OSSched *s);

#endif

