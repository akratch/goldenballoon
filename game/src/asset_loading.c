#include "asset_loading.h"

#include "common.h"
#include "gzip.h"
#include "macros.h"
#include "memory.h"
#include "ultra64.h"

#include <limits.h>
#ifdef NATIVE_PORT
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef NATIVE_PORT
/* Endianness normalization boundary (PLAN.md decision #7). ROM asset data is
 * big-endian; the host is little-endian. asset_swap_lut() normalizes the master
 * LUT and any *_TABLE offset array; asset_swap_normalize() normalizes a
 * materialized asset section by its AssetSectionsEnum type. */
#include "asset_swap.h"
#include "address_domains.h"
#include "platform_os.h" /* MDKR_TRACE */
#endif

/************ .bss ************/

OSIoMesg gAssetsDmaIoMesg;
OSMesg gDmaMesg;
OSMesgQueue gDmaMesgQueue;
OSMesg gPIMesgBuf[16];
OSMesgQueue gPIMesgQueue;
#if VERSION >= VERSION_79
OSMesg gDmaMutexBuf;
OSMesgQueue gDmaMutex;
#define dmacopy_internal dmacopy_v1
#else
#define dmacopy_internal dmacopy
#endif
u32 *gAssetsLookupTable;

/*******************************/

static void asset_dma_lock(void) {
#if VERSION >= VERSION_79
    OSMesg msg = NULL;
    osRecvMesg(&gDmaMutex, &msg, OS_MESG_BLOCK);
#endif
}

static void asset_dma_unlock(void) {
#if VERSION >= VERSION_79
    osSendMesg(&gDmaMutex, (OSMesg) 1, OS_MESG_NOBLOCK);
#endif
}

static s32 asset_section_bounds(u32 assetIndex, u32 *startOut, u32 *sizeOut) {
    u32 *index;
    u32 start;
    u32 end;

    /* Word 0 is the section count; words 1 .. count + 1 are the section start
     * offsets plus the end terminator. Section `assetIndex` therefore needs
     * words assetIndex + 1 and assetIndex + 2, which exist only while
     * assetIndex < count. */
    if (gAssetsLookupTable == NULL || startOut == NULL || sizeOut == NULL ||
        assetIndex >= gAssetsLookupTable[0]) {
        return -1;
    }

    index = gAssetsLookupTable + assetIndex + 1;
    start = index[0];
    end = index[1];
    if (end < start) {
        return -1;
    }
    *startOut = start;
    *sizeOut = end - start;
    return 0;
}

// These are both defined in the generated dkr.ld file.
#ifdef NATIVE_PORT
/* Raw ROM byte offsets: the lookup-table start/end and asset-data base.
 *
 * EVERY DKR REVISION PUTS THESE SOMEWHERE ELSE (they are linker-generated on the
 * N64, so each release has its own). They default to us.v80 —
 * docs/ref/dkr.us.v80.ld + build/dkr.us.v80.map — and platform/rom_io.c re-points
 * them at load time from the row platform/rom_id.c identified. rom_io.c is the
 * ONLY writer; nothing reads them before it runs (platformInitRom precedes
 * thread3_main -> init_game -> pi_init).
 *
 * Getting these right is necessary and very nearly sufficient: with them correct,
 * all five revisions finish a 3-lap Time Trial. It is not formally sufficient,
 * because the build is compiled for v80 DATA and nothing bounds-checks a sub-entry
 * index (us.v80's GAME_TEXT has 343 entries, us.v77's 259). Measured per-revision
 * behaviour and the remaining gap: docs/ROM_REVISIONS.md. Five-revision offset
 * table: platform/segment_consts.c. */
MdkrRomOffset gDkrAssetsLutStart = 0x000ED0E0u;
MdkrRomOffset gDkrAssetsLutEnd = 0x000ED1B0u;
#define ASSETS_LUT_START_OFFSET gDkrAssetsLutStart
#define ASSETS_LUT_END_OFFSET   gDkrAssetsLutEnd
#else
extern u8 __ASSETS_LUT_START[], __ASSETS_LUT_END[];
#define ASSETS_LUT_START_OFFSET ((u32) __ASSETS_LUT_START)
#define ASSETS_LUT_END_OFFSET   ((u32) __ASSETS_LUT_END)
#endif

static s32 asset_dma_section(u32 sectionStart, u32 sectionSize, uintptr_t address,
                             s32 assetOffset, s32 size) {
    u64 romOffset;

    if (address == 0 || assetOffset < 0 || size <= 0 ||
        (u32) assetOffset > sectionSize ||
        (u32) size > sectionSize - (u32) assetOffset) {
        return -1;
    }

    romOffset = (u64) ASSETS_LUT_END_OFFSET + sectionStart +
                (u32) assetOffset;
    if (romOffset > UINT32_MAX) {
        return -1;
    }
    return dmacopy_internal((u32) romOffset, address, size);
}

/**
 * Set up the peripheral interface message queues and scheduling.
 * This will send messages when DMA reads are finished.
 * After, allocate space and load the asset table into RAM.
 * Official Name: piInit
 */
void pi_init(void) {
    u32 assetTableSize;
    osCreateMesgQueue(&gPIMesgQueue, gPIMesgBuf, ARRAY_COUNT(gPIMesgBuf));
    osCreateMesgQueue(&gDmaMesgQueue, &gDmaMesg, 1);
    osCreatePiManager((OSPri) 150, &gPIMesgQueue, gPIMesgBuf, ARRAY_COUNT(gPIMesgBuf));

#if VERSION >= VERSION_79
    osCreateMesgQueue(&gDmaMutex, &gDmaMutexBuf, 1);
    osSendMesg(&gDmaMutex, (OSMesg) 1, OS_MESG_NOBLOCK);
#endif

    assetTableSize = ASSETS_LUT_END_OFFSET - ASSETS_LUT_START_OFFSET;
    gAssetsLookupTable = (u32 *) mempool_alloc_safe(assetTableSize, COLOUR_TAG_GREY);
    mempool_locked_set((u8 *) gAssetsLookupTable);
    if (dmacopy_internal(ASSETS_LUT_START_OFFSET,
                         (uintptr_t)gAssetsLookupTable,
                         (s32)assetTableSize) != 0) {
#ifdef NATIVE_PORT
        fprintf(stderr, "[FATAL] failed to load the asset lookup table\n");
        abort();
#else
        return;
#endif
    }
#ifdef NATIVE_PORT
    /* Master asset LUT: array of BE u32 ROM offsets. Normalize so every size
     * computed as (LUT[i+1] - LUT[i]) downstream is native-endian. */
    asset_swap_lut(gAssetsLookupTable, assetTableSize);
#endif
}

/**
 * Returns the memory address containing an asset section loaded from ROM.
 * Official Name: piRomLoad
 */
u32 *asset_table_load(u32 assetIndex) {
    u32 *out;
    u32 sectionSize;
    u32 start;
#ifdef NATIVE_PORT
    u32 assetType = assetIndex; /* AssetSectionsEnum before the +1 index shift */
#endif

    asset_dma_lock();
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        sectionSize == 0 || sectionSize > INT32_MAX) {
        asset_dma_unlock();
        return NULL;
    }
    out = (u32 *) mempool_alloc_safe((s32) sectionSize, COLOUR_TAG_GREY);
    if (out == 0) {
        asset_dma_unlock();
        return NULL;
    }

    if (asset_dma_section(start, sectionSize, (uintptr_t) out, 0,
                          (s32) sectionSize) != 0) {
        mempool_free(out);
        asset_dma_unlock();
        return NULL;
    }

#ifdef NATIVE_PORT
    MDKR_TRACE("asset_table_load type=%u size=%u -> %p", assetType,
               (unsigned) sectionSize, (void *) out);
    /* Primary hook: normalize the just-materialized section BE->host in place.
     * Dispatches on the AssetSectionsEnum type; covers all wholesale *_TABLE /
     * FONTS / PARTICLES / id-list / structured-record loads. No-op for punted
     * types (audio, misc, string blobs). See platform/asset_swap.h. */
    asset_swap_normalize((int) assetType, out, sectionSize);
#endif

    asset_dma_unlock();
    return out;
}

/**
 * Loads a gzip compressed asset from the ROM file.
 * Returns a pointer to the decompressed data.
 * Official name: piRomLoadCompressed
 */
UNUSED u8 *asset_table_load_zipped(u32 assetIndex, s32 extraMemory) {
    u32 sectionSize;
    u32 start;
    u32 decompressedSize;
    u64 allocationSize;
    u8 header[8];
    u8 *compressed;
    u8 *out = NULL;

    asset_dma_lock();
    if (extraMemory < 0 ||
        asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        sectionSize < sizeof(header) || sectionSize > INT32_MAX ||
        asset_dma_section(start, sectionSize, (uintptr_t) header, 0,
                          sizeof(header)) != 0) {
        asset_dma_unlock();
        return NULL;
    }
    decompressedSize = (u32) byteswap32(header);
    allocationSize = (u64) decompressedSize + (u32) extraMemory + sectionSize;
    if (decompressedSize == 0 || allocationSize > INT32_MAX) {
        asset_dma_unlock();
        return NULL;
    }
    out = (u8 *) mempool_alloc_safe((s32) allocationSize, COLOUR_TAG_GREY);
    compressed = out + decompressedSize + (u32) extraMemory;
    if (asset_dma_section(start, sectionSize, (uintptr_t) compressed, 0,
                          (s32) sectionSize) != 0) {
        mempool_free(out);
        asset_dma_unlock();
        return NULL;
    }
#ifdef NATIVE_PORT
    /* `compressed` is the tail of the same allocation `out` starts, so the pool
     * slot would report the destination's end, not the compressed span. Hand the
     * decoder the section size that was actually DMA'd. */
    gzip_inflate_sized(compressed, out, (s32) sectionSize);
#else
    gzip_inflate(compressed, out);
#endif
    asset_dma_unlock();
    return out;
}

/**
 * Loads an asset section to a specific memory address.
 * Returns the size of asset section.
 */
UNUSED s32 asset_table_load_addr(u32 assetIndex, uintptr_t address) {
    u32 start;
    u32 sectionSize;
    s32 result;

    asset_dma_lock();
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        sectionSize == 0 || sectionSize > INT32_MAX) {
        asset_dma_unlock();
        return 0;
    }
    result = asset_dma_section(start, sectionSize, address, 0,
                               (s32) sectionSize);
    asset_dma_unlock();
    return result == 0 ? (s32) sectionSize : 0;
}

/**
 * Loads part of an asset section to a specific memory address.
 * Returns the size argument.
 * Official name: piRomLoadSection
 */
s32 asset_load(u32 assetIndex, uintptr_t address, s32 assetOffset, s32 size) {
    u32 start;
    u32 sectionSize;
    s32 result;

    asset_dma_lock();
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0) {
        asset_dma_unlock();
        return 0;
    }
    result = asset_dma_section(start, sectionSize, address, assetOffset, size);
    asset_dma_unlock();
    return result == 0 ? size : 0;
}

/**
 * Returns a rom offset of an asset given its asset section and a local offset.
 * Official name: piRomGetSectionPtr
 */
u8 *asset_rom_offset(u32 assetIndex, u32 assetOffset) {
    u32 start;
    u32 sectionSize;
    u64 romOffset;

    asset_dma_lock();
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        assetOffset > sectionSize) {
        asset_dma_unlock();
        return NULL;
    }
    romOffset = (u64) ASSETS_LUT_END_OFFSET + start + assetOffset;
    asset_dma_unlock();
    return romOffset <= UINT32_MAX ? (u8 *) (uintptr_t) romOffset : NULL;
}

/**
 * Returns the size of an asset section.
 * Official name: piRomGetFileSize
 */
s32 asset_table_size(u32 assetIndex) {
    u32 start;
    u32 sectionSize;

    asset_dma_lock();
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        sectionSize > INT32_MAX) {
        asset_dma_unlock();
        return 0;
    }
    asset_dma_unlock();
    return (s32) sectionSize;
}

#define MAX_TRANSFER_SIZE 0x5000

/**
 * Copies data from the game cartridge to a ram address.
 * Official name: romCopy
 */
s32 dmacopy(u32 romOffset, uintptr_t ramAddress, s32 numBytes) {
#if VERSION >= VERSION_79
    OSMesg msg = NULL;
    s32 result;
    osRecvMesg(&gDmaMutex, &msg, OS_MESG_BLOCK);
    result = dmacopy_internal(romOffset, ramAddress, numBytes);
    osSendMesg(&gDmaMutex, (OSMesg) 1, OS_MESG_NOBLOCK);
    return result;
}

// Looks like v2 ROMs made an alternate version of this function, and this is the original.
s32 dmacopy_internal(u32 romOffset, uintptr_t ramAddress, s32 numBytes) {
#endif
    OSMesg dmaMesg;
    s32 numBytesToDMA;

    if (ramAddress == 0 || numBytes < 0) {
        return -1;
    }
    osInvalDCache((void *)ramAddress, numBytes);
    numBytesToDMA = MAX_TRANSFER_SIZE;
    while (numBytes > 0) {
        if (numBytes < numBytesToDMA) {
            numBytesToDMA = numBytes;
        }
        if (osPiStartDma(&gAssetsDmaIoMesg, OS_MESG_PRI_NORMAL, OS_READ, romOffset,
                         (void *)ramAddress, numBytesToDMA, &gDmaMesgQueue) != 0) {
            return -1;
        }
        osRecvMesg(&gDmaMesgQueue, &dmaMesg, OS_MESG_BLOCK);
        numBytes -= numBytesToDMA;
        romOffset += numBytesToDMA;
        ramAddress += numBytesToDMA;
    }
    return 0;
}
