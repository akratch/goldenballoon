#include "thread30_bgload.h"
#include "game.h"
#include "macros.h"
#include "stacks.h"
#include "thread3_main.h"
#include <ultra64.h>

/************ .data ************/

s32 gThread30NeedToLoadLevel = FALSE;
s32 gThread30LevelIdToLoad = -1;
s32 gThread30CutsceneIdToLoad = -1;
s32 gThread30LoadDelay = 0;

/*******************************/

/************ .bss ************/

OSThread gThread30;
OSMesgQueue gThread30MesgQueue;
OSMesg gThread30Message[2];
u64 gThread30Stack[STACKSIZE(STACK_BGLOAD)];

/*****************************/

/**
 * Initalizes and starts thread30()
 */
void bgload_init(void) {
    osCreateMesgQueue(&gThread30MesgQueue, gThread30Message, ARRAY_COUNT(gThread30Message));
    osCreateThread(&gThread30, 30, &thread30_bgload, NULL, &gThread30Stack[STACKSIZE(STACK_BGLOAD)], 8);
    osStartThread(&gThread30);
}

/**
 * Stop thread30()
 * Official Name: amStop
 */
void bgload_kill(void) {
    osStopThread(&gThread30);
}

/**
 * Returns the value in gThread30NeedToLoadLevel.
 */
s32 bgload_active(void) {
    return gThread30NeedToLoadLevel;
}

/**
 * If gThread30NeedToLoadLevel is set, then decrement gThread30LoadDelay. If gThread30LoadDelay is 0,
 * then signal thread30 to load a level.
 */
void bgload_tick(void) {
    if (gThread30NeedToLoadLevel && gThread30LoadDelay > 0) {
        gThread30LoadDelay--;
        if (gThread30LoadDelay == 0) {
#ifdef NATIVE_PORT
            /* Cooperative single-thread model: thread30 never runs as a real
             * thread, so the OS_MESG_BLOCK recv in thread30_bgload() that clears
             * gThread30NeedToLoadLevel never fires. Perform the "background" load
             * synchronously here (native DMA == memcpy, so it is instant) — this
             * is exactly the body of thread30_bgload's loop. Without it,
             * bgload_active() stays TRUE forever and the tracks menu freezes its
             * gMenuDelay (never zooms into a world / starts a race). */
            load_level_for_menu(gThread30LevelIdToLoad, -1, gThread30CutsceneIdToLoad);
            gThread30NeedToLoadLevel = FALSE;
#else
            // Signal thread30 that the level needs to load.
            osSendMesg(&gThread30MesgQueue, (OSMesg *) OS_MESG_TYPE_LOOPBACK, OS_MESG_NOBLOCK);
#endif
        }
    }
}

/**
 * Returns the value in gThread30LoadDelay.
 */
UNUSED s32 bgload_timer(void) {
    return gThread30LoadDelay;
}

/**
 * Sets a level id and cutscene id to be loaded. Used in the tracks menu & credits menu.
 * Returns 1 if successful, or 0 if gThread30NeedToLoadLevel was already set.
 */
s32 bgload_start(s32 levelId, s32 cutsceneId) {
    if (!gThread30NeedToLoadLevel) {
        gThread30LoadDelay = 4;
        gThread30LevelIdToLoad = levelId;
        gThread30CutsceneIdToLoad = cutsceneId;
        gThread30NeedToLoadLevel = TRUE;
        return TRUE;
    }
    return FALSE;
}

/**
 * Waits for a signal from the main thread to load a level asynchronously.
 */
void thread30_bgload(UNUSED void *arg) {
    OSMesg mesg = 0;
    while (TRUE) {
        // Wait for a signal from the main thread
        osRecvMesg(&gThread30MesgQueue, &mesg, OS_MESG_BLOCK);
        if (mesg != (OSMesg) OS_MESG_TYPE_LOOPBACK) {
            continue;
        }
        // -1 means there won't be any racers loaded.
        load_level_for_menu(gThread30LevelIdToLoad, -1, gThread30CutsceneIdToLoad);
        gThread30NeedToLoadLevel = FALSE;
    }
}
