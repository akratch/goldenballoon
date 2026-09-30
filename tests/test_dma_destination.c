/*
 * Native PI DMA writes to the caller's host pointer.
 *
 * Links the PRODUCTION platform/pi_dma.c (osPiStartDma, osPiRawStartDma)
 * with a recording platform_rom_read, and requires every DMA to land at the
 * pointer the caller passed. It guards against both earlier ways of
 * rebuilding the destination as an arena token:
 *
 *  - 1.7.0 rebuilt every destination from its low 32 bits
 *    (dkr_lo32_to_ptr), so a full host pointer whose low bits alias the
 *    arena received the ROM data at an arena address. The full-width
 *    pointers below catch that.
 *  - The later conditional helper widened only a low pointer (at most
 *    UINT32_MAX) whose rebuilt address lands inside the live arena, so a
 *    genuine low host allocation with those bits was still redirected. The
 *    low pointers below, which fall in the test arena's low-32 window
 *    (0x01000000..0x01FFFFFF), catch that.
 *
 * The arena helpers are defined here exactly as stubs_dkr.c defines them,
 * with the arena's high half placed away from every pointer the test passes,
 * so a reconstructing entry still links and fails the address checks.
 *
 * Pointers other than the two real buffers are compared only, never written
 * through: the recording platform_rom_read does not touch its destination.
 */
#include <ultra64.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform_os.h"

/* PRinternal/piint.h spells this __osPiRawStartDma; pi_dma.c defines it. */
s32 osPiRawStartDma(s32 dir, u32 devAddr, void *dramAddr, u32 size);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "DMA destination line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* --- arena helpers as in platform/stubs_dkr.c ---------------------------- */
uintptr_t g_dkrArenaHi;
void *g_dkrArenaBase;
uint32_t g_dkrArenaSize;

void *dkr_lo32_to_ptr(uint32_t lo32) {
    if (lo32 == 0) return NULL;
    return (void *)(g_dkrArenaHi | (uintptr_t)lo32);
}

/* --- recording engine boundary -------------------------------------------- */
static void *sLastDestination;
static uint32_t sLastOffset;
static int32_t sLastLength;
static int sReads;

int platform_rom_read(uint32_t romOffset, void *dst, int32_t len) {
    sLastDestination = dst;
    sLastOffset = romOffset;
    sLastLength = len;
    sReads++;
    return 0x5A;
}

static OSMesgQueue *sLastQueue;
static OSMesg sLastMessage;
static int sMessages;

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flags) {
    (void)flags;
    sLastQueue = mq;
    sLastMessage = msg;
    sMessages++;
    return 0;
}

static int dma_lands_at_checked(void *destination) {
    OSIoMesg io;
    OSMesgQueue queue;
    int reads = sReads, messages = sMessages;
    sLastDestination = (void *)(uintptr_t)1;
    CHECK(osPiStartDma(&io, OS_MESG_PRI_NORMAL, OS_READ, 0x1234, destination, 0x40, &queue) == 0x5A);
    CHECK(sReads == reads + 1 && sLastDestination == destination);
    CHECK(sLastOffset == 0x1234 && sLastLength == 0x40);
    CHECK(sMessages == messages + 1 && sLastQueue == &queue && sLastMessage == (OSMesg)&io);

    sLastDestination = (void *)(uintptr_t)1;
    CHECK(osPiRawStartDma(OS_READ, 0x5678, destination, 0x20) == 0x5A);
    CHECK(sReads == reads + 2 && sLastDestination == destination);
    CHECK(sLastOffset == 0x5678 && sLastLength == 0x20);
    CHECK(sMessages == messages + 1); /* the raw entry posts nothing */
    return 0;
}

static int dma_lands_at(void *destination) {
    if (!dma_lands_at_checked(destination)) return 0;
    fprintf(stderr, "DMA destination: passed %p, recorded %p\n", destination, sLastDestination);
    return 1;
}

int main(void) {
    static unsigned char heapLike[64];
    unsigned char stackBuffer[64];
    uintptr_t host = (uintptr_t)stackBuffer;

#if UINTPTR_MAX > UINT32_MAX
    /* An arena whose high half differs from both real buffers': rebuilding
     * either pointer from its low 32 bits lands somewhere else. */
    g_dkrArenaHi = ((host >> 32) + 0x40) << 32;
    g_dkrArenaBase = (void *)(g_dkrArenaHi | 0x01000000u);
    g_dkrArenaSize = 0x01000000u;
    CHECK(dkr_lo32_to_ptr((uint32_t)host) != (void *)stackBuffer);
#else
    /* 32-bit hosts: a pointer IS its low 32 bits, so reconstruction cannot
     * move it. The checks still pin the pass-through and the message. */
    g_dkrArenaHi = 0;
#endif

    if (dma_lands_at(stackBuffer) || dma_lands_at(heapLike)) return 1;
    if (dma_lands_at(NULL)) return 1;

    /* Low host pointers, on every host: the arena's first byte, one inside
     * it and its last byte, plus addresses outside the window and the top
     * of the 32-bit range. Compared, never written through. */
    const uintptr_t lows[] = {0x01000000u, 0x01600020u, 0x01FFFFFFu,
                              0x00001234u, 0x02000000u, UINT32_MAX};
    for (size_t i = 0; i < sizeof lows / sizeof lows[0]; ++i) {
        if (dma_lands_at((void *)lows[i])) return 1;
    }

#if UINTPTR_MAX > UINT32_MAX
    /* Synthetic host pointers whose low 32 bits are an address inside the
     * arena, or the arena's own start: compared, never written through. */
    const uint32_t aliases[] = {0x01000000u, 0x01600020u, 0x01FFFFFFu, 0x00001234u};
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; ++i) {
        void *alias = (void *)((((host >> 32) + 1) << 32) | aliases[i]);
        CHECK(dkr_lo32_to_ptr(aliases[i]) != alias);
        if (dma_lands_at(alias)) return 1;
    }
#endif

    /* No queue: the read still happens and nothing is posted. */
    int messages = sMessages;
    CHECK(osPiStartDma(NULL, OS_MESG_PRI_NORMAL, OS_READ, 0, stackBuffer, 8, NULL) == 0x5A);
    CHECK(sLastDestination == stackBuffer && sMessages == messages);
    /* No I/O message: the queue still receives the (null) completion. */
    OSMesgQueue queue;
    CHECK(osPiStartDma(NULL, OS_MESG_PRI_NORMAL, OS_READ, 0, stackBuffer, 8, &queue) == 0x5A);
    CHECK(sMessages == messages + 1 && sLastQueue == &queue && sLastMessage == (OSMesg)0);

    puts("DMA destination: PASS");
    return 0;
}
