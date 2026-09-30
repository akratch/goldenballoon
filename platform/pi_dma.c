/**
 * pi_dma.c — native PI (cartridge) DMA for the mdkr64 port.
 *
 * On the N64 osPiStartDma queues a cartridge-to-RDRAM transfer and posts
 * the I/O message when it completes. Natively the ROM image is in memory, so
 * a DMA is a copy out of it (platform_rom_read) and the completion message is
 * posted at once.
 *
 * The destination is the caller's host pointer, unchanged. Both native
 * callers (asset DMA and audio) pass full host pointers; never rebuild one
 * from its low 32 bits as an arena token (address_domains.h). Kept apart from
 * stubs_dkr.c so tests/test_dma_destination.c can link this real entry point.
 */
#include <ultra64.h>

#include "address_domains.h"
#include "platform_os.h"

s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 dir, u32 devAddr,
                 void *dramAddr, u32 size, OSMesgQueue *mq) {
    int readStatus;
    (void)pri; (void)dir;
    void *dst = mdkr_dma_destination(dramAddr);
    readStatus = platform_rom_read(devAddr, dst, (int32_t)size);
    if (mq) osSendMesg(mq, mb ? (OSMesg)mb : (OSMesg)0, OS_MESG_NOBLOCK);
    return readStatus;
}

s32 osPiRawStartDma(s32 dir, u32 devAddr, void *dramAddr, u32 size) {
    int readStatus;
    (void)dir;
    void *dst = mdkr_dma_destination(dramAddr);
    readStatus = platform_rom_read(devAddr, dst, (int32_t)size);
    return readStatus;
}
