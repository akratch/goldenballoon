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
#include "asset_subentry.h"
#include "asset_enums.h"
#include "custom_track_section.h"
#include "custom_track_decode.h"
#include "custom_track_model_payload.h"
#include "custom_track_model.h"
#include "custom_track_texture.h"
#include "custom_track_header.h"
#include "object_layout.h"
#include "structs.h"
#include "platform_os.h" /* MDKR_TRACE */

/* The master LUT is an array of 4-byte ROM offsets: word 0 is the section count
 * and words 1 .. count + 1 are the section starts plus the end terminator (see
 * asset_section_bounds). Every size downstream is a difference of two adjacent
 * words, and platform/asset_subentry.c makes the same assumption one level down
 * for the per-section offset tables. */
_Static_assert(sizeof(u32) == 4, "asset lookup table words are 4 bytes wide");

static const MdkrCustomTrackSession *sCustomTrackSession;
static void asset_custom_track_env_selftest(void);
static const u32 sCustomTrackIndices[MDKR_CUSTOM_TRACK_SECTION_COUNT][2] = {
    {ASSET_LEVEL_HEADERS, ASSET_LEVEL_HEADERS_TABLE},
    {ASSET_LEVEL_OBJECT_MAPS, ASSET_LEVEL_OBJECT_MAPS_TABLE},
    {ASSET_LEVEL_NAMES, ASSET_LEVEL_NAMES_TABLE},
    {ASSET_LEVEL_MODELS, ASSET_LEVEL_MODELS_TABLE},
    {ASSET_TEXTURES_3D, ASSET_TEXTURES_3D_TABLE}
};

static const MdkrCustomTrackSection *asset_custom_section(u32 index, int table) {
    if (!sCustomTrackSession) return NULL;
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i)
        if (sCustomTrackIndices[i][table] == index)
            return mdkr_custom_track_session_section(sCustomTrackSession,
                                                     (MdkrCustomTrackSectionKind)i);
    return NULL;
}

s32 asset_retail_entry_count(u32 tableIndex, s32 logicalCount) {
    const MdkrCustomTrackSection *section = asset_custom_section(tableIndex, 1);
    if (!section) return logicalCount;
    size_t count = mdkr_custom_track_section_retail_count(section);
    if (logicalCount < 0 || count > (size_t)logicalCount) {
        fprintf(stderr, "[FATAL] custom table inventory mismatch\n");
        abort();
    }
    return (s32)count;
}
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

#ifdef NATIVE_PORT
typedef struct AssetCustomRetailRead {
    u32 start, size;
} AssetCustomRetailRead;

s16 *asset_object_behavior_catalog(size_t *count) {
    if (!count) return NULL;
    *count = 0;
    if (!gAssetsLookupTable) return NULL;
    const s32 translation_size = asset_table_size(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE);
    const s32 table_size = asset_table_size(ASSET_OBJECT_HEADERS_TABLE);
    const s32 objects_size = asset_table_size(ASSET_OBJECTS);
    if (translation_size <= 0 || translation_size % 2 || table_size < 8 ||
        table_size % 4 || objects_size <= 0) return NULL;
    u8 *translation = malloc((size_t)translation_size);
    u32 *table = malloc((size_t)table_size);
    s16 *catalog = malloc((size_t)translation_size);
    s16 *behaviors = NULL;
    size_t words = 0;
    if (!translation || !table || !catalog ||
        asset_load(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE, (uintptr_t)translation, 0, translation_size) != translation_size ||
        asset_load(ASSET_OBJECT_HEADERS_TABLE, (uintptr_t)table, 0, table_size) != table_size) goto fail;
    asset_swap_normalize(ASSET_OBJECT_HEADERS_TABLE, table, (u32)table_size);
    if (!mdkr_custom_track_measure_table(table, (size_t)table_size / 4, (u32)objects_size, &words) || words < 2)
        goto fail;
    const size_t headers = words - 2;
    behaviors = malloc((headers ? headers : 1) * sizeof *behaviors);
    if (!behaviors) goto fail;
    _Static_assert(offsetof(ObjectHeader, behaviorId) == 0x54, "retail behavior byte layout drift");
    for (size_t i = 0; i < headers; ++i) {
        behaviors[i] = -1;
        if (table[i + 1] - table[i] <= offsetof(ObjectHeader, behaviorId)) continue;
        u8 behavior;
        if (asset_load(ASSET_OBJECTS, (uintptr_t)&behavior,
                (s32)(table[i] + offsetof(ObjectHeader, behaviorId)), 1) != 1) goto fail;
        if (behavior < 128) behaviors[i] = behavior;
    }
    for (size_t i = 0; i < (size_t)translation_size / 2; ++i) {
        const u32 header = (u32)translation[2 * i] << 8 | translation[2 * i + 1];
        catalog[i] = header <= INT16_MAX && header < headers ? behaviors[header] : -1;
    }
    *count = (size_t)translation_size / 2;
    free(behaviors); free(table); free(translation);
    return catalog;
fail:
    free(behaviors); free(catalog); free(table); free(translation);
    return NULL;
}

int asset_custom_track_session_set(const MdkrCustomTrackSession *session) {
    if (!session) { sCustomTrackSession = NULL; return 1; }
    if (!gAssetsLookupTable) return 0;
    asset_dma_lock();
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        const MdkrCustomTrackSection *section = mdkr_custom_track_session_section(
            session, (MdkrCustomTrackSectionKind)i);
        if (!section) continue;
        u32 tableStart, tableSize, dataStart, dataSize;
        size_t words = 0;
        if (asset_section_bounds(sCustomTrackIndices[i][1], &tableStart, &tableSize) ||
            asset_section_bounds(sCustomTrackIndices[i][0], &dataStart, &dataSize) ||
            tableSize < 8 || tableSize > INT32_MAX || tableSize % sizeof(u32)) {
            asset_dma_unlock();
            return 0;
        }
        u32 *retail = malloc(tableSize);
        if (!retail) { asset_dma_unlock(); return 0; }
        int valid = asset_dma_section(tableStart, tableSize, (uintptr_t)retail,
                                       0, (s32)tableSize) == 0;
        if (valid) {
            asset_swap_normalize((int)sCustomTrackIndices[i][1], retail, tableSize);
            valid = mdkr_custom_track_measure_table(retail, tableSize / sizeof(u32),
                                                    dataSize, &words) &&
                    mdkr_custom_track_section_matches_retail(section, retail, words);
        }
        free(retail);
        if (!valid) { asset_dma_unlock(); return 0; }
    }
    sCustomTrackSession = session;
    asset_dma_unlock();
    return 1;
}

static int asset_custom_retail_read(void *context, uint32_t offset,
                                    void *destination, uint32_t size) {
    const AssetCustomRetailRead *retail = context;
    return asset_dma_section(retail->start, retail->size, (uintptr_t)destination,
                             (s32)offset, (s32)size) == 0;
}
#endif

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
    /* Negative control for tests/check_subentry_bounds.py, placed at the
     * asset-loading boundary because that is the layer whose SECTION index has
     * always been checked and whose SUB-ENTRY index has not. No-op unless
     * MDKR_SUBENTRY_TEST_INDEX is set, so a normal run never enters it. */
    mdkr_asset_subentry_env_selftest();
    asset_custom_track_env_selftest();
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
#ifdef NATIVE_PORT
    const MdkrCustomTrackSection *custom = asset_custom_section(assetIndex, 1);
    if (custom) {
        size_t words = 0;
        const u32 *table = mdkr_custom_track_section_table(custom, &words);
        if (!table || words > INT32_MAX / sizeof(u32)) {
            asset_dma_unlock();
            return NULL;
        }
        out = mempool_alloc_safe((s32)(words * sizeof(u32)), COLOUR_TAG_GREY);
        if (out) memcpy(out, table, words * sizeof(u32));
        /* Session tables are already host-endian. Consumers own this pool
         * copy; they must never free or normalize the session's borrowed table. */
        asset_dma_unlock();
        return out;
    }
    /* Whole data-section normalization is not the subentry load contract.
     * Reject this alternate path until a caller needs an explicitly composed
     * and normalized section; never silently return retail-only data. */
    if (asset_custom_section(assetIndex, 0)) {
        asset_dma_unlock();
        return NULL;
    }
#endif
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
#ifdef NATIVE_PORT
    if (asset_custom_section(assetIndex, 0) || asset_custom_section(assetIndex, 1)) {
        asset_dma_unlock();
        return NULL;
    }
#endif
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
#ifdef NATIVE_PORT
    if (asset_custom_section(assetIndex, 0) || asset_custom_section(assetIndex, 1)) {
        asset_dma_unlock();
        return 0;
    }
#endif
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
#ifdef NATIVE_PORT
    const MdkrCustomTrackSection *custom = asset_custom_section(assetIndex, 0);
    if (custom) {
        AssetCustomRetailRead retail = {start, sectionSize};
        result = mdkr_custom_track_section_load(custom, assetOffset, size,
                    (void *)address, size > 0 ? (size_t)size : 0,
                    asset_custom_retail_read, &retail);
        asset_dma_unlock();
        return result ? size : 0;
    }
#endif
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
#ifdef NATIVE_PORT
    /* A pointer cannot represent a mixed ROM/host section. Fail closed for
     * session-backed sections; their consumers must use checked asset_load. */
    if (asset_custom_section(assetIndex, 0) || asset_custom_section(assetIndex, 1)) {
        asset_dma_unlock();
        return NULL;
    }
#endif
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
#ifdef NATIVE_PORT
    const MdkrCustomTrackSection *custom = asset_custom_section(assetIndex, 0);
    if (custom) {
        sectionSize = mdkr_custom_track_section_size(custom);
        asset_dma_unlock();
        return (s32)sectionSize;
    }
    custom = asset_custom_section(assetIndex, 1);
    if (custom) {
        size_t words = 0;
        mdkr_custom_track_section_table(custom, &words);
        asset_dma_unlock();
        return words <= INT32_MAX / sizeof(u32) ? (s32)(words * sizeof(u32)) : 0;
    }
#endif
    if (asset_section_bounds(assetIndex, &start, &sectionSize) != 0 ||
        sectionSize > INT32_MAX) {
        asset_dma_unlock();
        return 0;
    }
    asset_dma_unlock();
    return (s32) sectionSize;
}

#ifdef NATIVE_PORT
typedef struct MdkrPulseCorpus {
    const u32 *table;
    u32 count;
    unsigned checked;
    unsigned refused;
    size_t model;
} MdkrPulseCorpus;

static int asset_pulse_texture_selftest(void *context, uint32_t id) {
    MdkrPulseCorpus *corpus = context;
    const size_t capacity = 16u * 1024u * 1024u;
    if (id >= corpus->count) return 0;
    u32 begin = corpus->table[id], length = corpus->table[id + 1] - begin;
    if (!length || length > capacity) return 0;
    u8 *payload = malloc(length), *scratch = malloc(capacity);
    MdkrTrackTextureInfo info;
    int loaded = payload && scratch &&
        asset_load(ASSET_TEXTURES_3D, (uintptr_t)payload, (s32)begin, (s32)length) == (s32)length;
    int valid = loaded && mdkr_track_texture_pulse_payload(
        payload, length, scratch, capacity, &info) == MDKR_TRACK_TEXTURE_OK;
    if (!valid && loaded && mdkr_track_texture_payload(payload, length, scratch, capacity, &info) == MDKR_TRACK_TEXTURE_OK) {
        const u8 *frame = payload[0x1D] ? scratch : payload;
        size_t decodedSize = length;
        if (payload[0x1D]) mdkr_custom_track_decode(payload + 32, length - 32,
            scratch, capacity, &decodedSize);
        fprintf(stderr, "[CUSTOM_TRACK_PULSE_LAYOUT] width=%u height=%u format=%u frames=%u decoded=%zu meaningful=%zu\n",
            frame[0], frame[1], frame[2], info.frames, decodedSize, info.meaningful_bytes);
    }
    if (!valid) fprintf(stderr, "[CUSTOM_TRACK_PULSE_TEXTURE_REFUSED] model=%zu id=%u\n", corpus->model, id);
    free(scratch);
    free(payload);
    if (valid) ++corpus->checked;
    else ++corpus->refused;
    /* Finish the compatibility census before failing its aggregate gate. */
    return loaded;
}

/* Private ROM-backed integration fixture. Runs before any consumer caches
 * asset IDs and restores retail state before boot continues. Never exports
 * ROM bytes; synthetic payloads are not decoded as actual game assets. */
static void asset_custom_track_env_selftest(void) {
    const char *enabled = getenv("MDKR_CUSTOM_TRACK_LOADER_SELFTEST");
    if (!enabled || strcmp(enabled, "1") != 0) return;
#define TRACK_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "[FATAL] custom track loader selftest line=%d\n", __LINE__); \
    abort(); } } while (0)
    TRACK_CHECK(sCustomTrackSession == NULL);
    size_t behavior_count = 0;
    s16 *behavior_catalog = asset_object_behavior_catalog(&behavior_count);
    TRACK_CHECK(behavior_catalog != NULL && behavior_count > 0);
    fprintf(stderr, "[CUSTOM_TRACK_PREBOOT_BEHAVIORS] slots=%zu PASS\n", behavior_count);
    free(behavior_catalog);
    MdkrCustomTrackSectionInput inputs[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    MdkrCustomTrackPayload payloads[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    u32 *tables[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    u32 ends[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    s32 physicalSizes[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    size_t words[MDKR_CUSTOM_TRACK_SECTION_COUNT], totalWords = 0;
    u8 markers[MDKR_CUSTOM_TRACK_SECTION_COUNT][4];
    u8 retailTail[MDKR_CUSTOM_TRACK_SECTION_COUNT][2];
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        u32 data = sCustomTrackIndices[i][0], table = sCustomTrackIndices[i][1];
        s32 tableSize = asset_table_size(table);
        physicalSizes[i] = asset_table_size(data);
        tables[i] = asset_table_load(table);
        TRACK_CHECK(tableSize > 0 && tableSize % 4 == 0 && physicalSizes[i] > 0);
        TRACK_CHECK(mdkr_custom_track_measure_table(tables[i], (size_t)tableSize / 4,
                                                    (u32)physicalSizes[i], &words[i]));
        ends[i] = tables[i][words[i] - 2];
        if (i == MDKR_CUSTOM_TRACK_HEADERS) {
            const u32 textureTables[] = {ASSET_TEXTURES_2D_TABLE, ASSET_TEXTURES_3D_TABLE};
            const u32 textureSections[] = {ASSET_TEXTURES_2D, ASSET_TEXTURES_3D};
            u32 textureCounts[2];
            unsigned skyReferences = 0;
            for (unsigned bank = 0; bank < 2; ++bank) {
                s32 tableBytes = asset_table_size(textureTables[bank]);
                s32 dataBytes = asset_table_size(textureSections[bank]);
                u32 *table = asset_table_load(textureTables[bank]);
                size_t tableWords = 0;
                TRACK_CHECK(tableBytes > 0 && tableBytes % 4 == 0 && dataBytes > 0);
                TRACK_CHECK(mdkr_custom_track_measure_table(table, (size_t)tableBytes / 4,
                    (u32)dataBytes, &tableWords));
                TRACK_CHECK(tableWords >= 2 && tableWords - 2 <= 0x8000);
                textureCounts[bank] = (u32)(tableWords - 2);
                mempool_free(table);
            }
            s32 miscTableSize = asset_table_size(ASSET_MISC_TABLE);
            s32 miscSize = asset_table_size(ASSET_MISC);
            u32 *miscTable = asset_table_load(ASSET_MISC_TABLE);
            size_t miscWords = 0;
            unsigned colours = 0, pulses = 0;
            TRACK_CHECK(miscTableSize > 0 && miscTableSize % 4 == 0 && miscSize > 0 && miscSize % 4 == 0);
            /* Unlike course tables, MISC offsets are 32-bit word offsets. */
            TRACK_CHECK(mdkr_custom_track_measure_table(miscTable,
                (size_t)miscTableSize / 4, (u32)miscSize / 4, &miscWords));
            for (size_t entry = 0; entry + 2 < words[i]; ++entry) {
                u8 header[MDKR_TRACK_HEADER_BYTES];
                u32 begin = tables[i][entry], length = tables[i][entry + 1] - begin;
                TRACK_CHECK(length >= sizeof header);
                TRACK_CHECK(asset_load(data, (uintptr_t)header, (s32)begin, sizeof header) == sizeof header);
                if (!mdkr_track_header_scalars_valid(header, sizeof header)) {
                    fprintf(stderr, "[CUSTOM_TRACK_SCALAR_REFUSED] header=%zu world=%u race=%u vehicle=%u mask=%u fov=%u\n",
                        entry, header[0], header[0x4C], header[0x4D], header[0x4E], header[0x9C]);
                    TRACK_CHECK(0);
                }
                TRACK_CHECK(mdkr_track_header_misc_valid(header, sizeof header, (u32)(miscWords - 2)));
                TRACK_CHECK(mdkr_track_header_sky_texture_valid(header, sizeof header,
                    textureCounts[0], textureCounts[1]));
                if (header[0x49] == 0xFF) ++skyReferences;
                TRACK_CHECK(mdkr_track_header_wave_texture_valid(header, sizeof header,
                    textureCounts[0], textureCounts[1]));
                for (unsigned field = 0; field < 8; ++field) {
                    const u8 *p = header + (field < 7 ? 0x74 + field * 4 : 0xAC);
                    u32 id = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
                    if (id == UINT32_MAX) continue;
                    u32 size = (miscTable[id + 1] - miscTable[id]) * 4;
                    TRACK_CHECK(size > 0 && size <= (u32)miscSize);
                    u8 *record = malloc(size);
                    TRACK_CHECK(record != NULL);
                    TRACK_CHECK(asset_load(ASSET_MISC, (uintptr_t)record, (s32)(miscTable[id] * 4), (s32)size) == (s32)size);
                    int valid = field < 7 ? mdkr_track_colour_cycle_valid(record, size)
                                          : mdkr_track_pulse_cycle_valid(record, size);
                    if (!valid) fprintf(stderr, "[CUSTOM_TRACK_LIGHT_REFUSED] header=%zu field=%u id=%u size=%u\n",
                                        entry, field, id, size);
                    free(record);
                    TRACK_CHECK(valid);
                    if (field < 7) ++colours; else ++pulses;
                }
            }
            mempool_free(miscTable);
            fprintf(stderr, "[CUSTOM_TRACK_SCALAR_CORPUS] headers=%zu PASS\n", words[i] - 2);
            fprintf(stderr, "[CUSTOM_TRACK_LIGHT_CORPUS] colour_refs=%u pulse_refs=%u PASS\n", colours, pulses);
            fprintf(stderr, "[CUSTOM_TRACK_SKY_CORPUS] refs=%u PASS\n", skyReferences);
        }
        if (i == MDKR_CUSTOM_TRACK_TEXTURES) {
            const size_t textureCapacity = 16u * 1024u * 1024u;
            u8 *scratch = malloc(textureCapacity);
            unsigned accepted = 0, palettes = 0, refused = 0;
            TRACK_CHECK(scratch != NULL);
            for (size_t entry = 0; entry + 2 < words[i]; ++entry) {
                u32 begin = tables[i][entry], length = tables[i][entry + 1] - begin;
                if (!length) continue;
                TRACK_CHECK(length <= textureCapacity);
                u8 *payload = malloc(length);
                MdkrTrackTextureInfo info;
                TRACK_CHECK(payload != NULL);
                TRACK_CHECK(asset_load(data, (uintptr_t)payload, (s32)begin, (s32)length) == (s32)length);
                MdkrTrackTextureResult result = mdkr_track_texture_payload(
                    payload, length, scratch, textureCapacity, &info);
                if (result == MDKR_TRACK_TEXTURE_OK) ++accepted;
                else if (result == MDKR_TRACK_TEXTURE_PALETTE_REQUIRED) ++palettes;
                else {
                    ++refused;
                    fprintf(stderr, "[CUSTOM_TRACK_TEXTURE_REFUSED] entry=%zu size=%u\n", entry, length);
                }
                free(payload);
            }
            free(scratch);
            fprintf(stderr, "[CUSTOM_TRACK_TEXTURE_CORPUS] accepted=%u palette=%u refused=%u\n",
                    accepted, palettes, refused);
        }
        if (i == MDKR_CUSTOM_TRACK_MODELS || i == MDKR_CUSTOM_TRACK_OBJECT_MAPS) {
            /* Compatibility corpus: decode every retail entry without
             * publishing geometry or exporting any ROM-derived bytes. */
            size_t capacity = i == MDKR_CUSTOM_TRACK_MODELS ? 0x82A00 : 0x3000;
            u8 *decoded = malloc(capacity);
            TRACK_CHECK(decoded != NULL);
            unsigned decodedCount = 0;
            unsigned waveModels = 0, waveSegments = 0;
            unsigned lightingPairs = 0;
            size_t maximumCollisionBudget = 0;
            u32 textureCount = 0;
            u32 *textureTable = NULL;
            MdkrPulseCorpus pulseCorpus = {0};
            if (i == MDKR_CUSTOM_TRACK_MODELS) {
                s32 textureTableBytes = asset_table_size(ASSET_TEXTURES_3D_TABLE);
                textureTable = asset_table_load(ASSET_TEXTURES_3D_TABLE);
                size_t textureWords = 0;
                TRACK_CHECK(textureTableBytes > 0 && textureTableBytes % 4 == 0);
                TRACK_CHECK(mdkr_custom_track_measure_table(textureTable,
                    (size_t)textureTableBytes / 4,
                    (u32)asset_table_size(ASSET_TEXTURES_3D), &textureWords));
                TRACK_CHECK(textureWords >= 2 && textureWords - 2 <= 0x8000);
                textureCount = (u32)(textureWords - 2);
                pulseCorpus.table = textureTable;
                pulseCorpus.count = textureCount;
            }
            for (size_t entry = 0; entry + 2 < words[i]; ++entry) {
                u32 begin = tables[i][entry], length = tables[i][entry + 1] - begin;
                if (!length) continue;
                u8 *compressed = malloc(length);
                size_t written = 0;
                TRACK_CHECK(compressed != NULL);
                TRACK_CHECK(asset_load(data, (uintptr_t)compressed, (s32)begin, (s32)length) == (s32)length);
                TRACK_CHECK(mdkr_custom_track_decode(compressed, length, decoded, capacity, &written));
                TRACK_CHECK(written > 0 && written <= capacity);
                if (i == MDKR_CUSTOM_TRACK_MODELS) {
                    TRACK_CHECK(mdkr_track_model_validate_spans(decoded, written));
                    pulseCorpus.model = entry;
                    TRACK_CHECK(mdkr_track_model_pulse_textures_valid(decoded, written,
                        asset_pulse_texture_selftest, &pulseCorpus));
                    u32 waveCount = 0;
                    TRACK_CHECK(mdkr_track_model_wave_count(decoded, written, &waveCount));
                    TRACK_CHECK(mdkr_track_model_wave_basis_valid(decoded, written));
                    /* Validate relationships, not just each independent blob.
                     * Several headers may legitimately share one model. */
                    for (size_t h = 0; h + 2 < words[MDKR_CUSTOM_TRACK_HEADERS]; ++h) {
                        u8 header[MDKR_TRACK_HEADER_BYTES];
                        u32 beginHeader = tables[MDKR_CUSTOM_TRACK_HEADERS][h];
                        TRACK_CHECK(asset_load(ASSET_LEVEL_HEADERS, (uintptr_t)header,
                            (s32)beginHeader, sizeof header) == sizeof header);
                        u32 modelId = (u32)header[0x34] << 8 | header[0x35];
                        if (modelId != entry) continue;
                        if (waveCount) {
                            TRACK_CHECK(mdkr_track_header_wave_render_valid(header, sizeof header, 1));
                            TRACK_CHECK(mdkr_track_header_wave_render_valid(header, sizeof header, 2));
                        }
                        int valid = mdkr_track_model_lighting_links_valid(decoded, written,
                            header, sizeof header);
                        if (!valid) fprintf(stderr,
                            "[CUSTOM_TRACK_LIGHT_LINK_REFUSED] header=%zu model=%zu\n", h, entry);
                        TRACK_CHECK(valid);
                        ++lightingPairs;
                    }
                    waveModels += waveCount != 0;
                    waveSegments += waveCount;
                    TRACK_CHECK(mdkr_track_model_validate_indices(decoded, written));
                    TRACK_CHECK(mdkr_track_model_validate_visibility(decoded, written));
                    TRACK_CHECK(mdkr_track_model_validate_regions(decoded, written));
                    size_t collisionBudget = 0;
                    TRACK_CHECK(mdkr_track_model_collision_budget(decoded, written, &collisionBudget));
                    if (collisionBudget > maximumCollisionBudget) maximumCollisionBudget = collisionBudget;
                    u8 *scratch = malloc(capacity), *repacked = malloc(capacity);
                    u8 *allocation = malloc(capacity + 32);
                    size_t encoded = 0, repackedDecoded = 0;
                    TRACK_CHECK(scratch && repacked && allocation);
                    u8 *arena = (u8 *)(((uintptr_t)allocation + 16) & ~(uintptr_t)15);
                    arena[-1] = arena[capacity] = 0xD5;
                    TRACK_CHECK(mdkr_track_model_repack(compressed, length,
                        scratch, capacity, repacked, capacity, NULL, 0,
                        textureCount, textureCount, capacity, &encoded, &repackedDecoded));
                    TRACK_CHECK(repackedDecoded == written);
                    TRACK_CHECK(encoded > 5 && encoded <= capacity);
                    size_t staged = (capacity - encoded) & ~(size_t)15;
                    TRACK_CHECK(written <= staged);
                    memcpy(arena + staged, repacked, encoded);
                    gzip_inflate_sized(arena + staged, arena, (s32)encoded);
                    TRACK_CHECK(gzip_inflate_output == arena + written);
                    TRACK_CHECK(!memcmp(arena, decoded, written));
                    TRACK_CHECK(arena[-1] == 0xD5 && arena[capacity] == 0xD5);
                    free(allocation); free(repacked); free(scratch);
                }
                if (i == MDKR_CUSTOM_TRACK_OBJECT_MAPS) {
                    size_t records = 0;
                    s32 translationBytes = asset_table_size(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE);
                    TRACK_CHECK(translationBytes > 0 && translationBytes % 2 == 0);
                    TRACK_CHECK(mdkr_level_object_map_validate(decoded, written,
                                        (size_t)translationBytes / 2, &records));
                }
                free(compressed);
                ++decodedCount;
            }
            free(decoded);
            TRACK_CHECK(decodedCount > 0);
            fprintf(stderr, "[CUSTOM_TRACK_DECODE] kind=%u entries=%u PASS\n", i, decodedCount);
            if (i == MDKR_CUSTOM_TRACK_MODELS)
                fprintf(stderr, "[CUSTOM_TRACK_REPACK] entries=%u native_staging=1 PASS\n", decodedCount);
            if (i == MDKR_CUSTOM_TRACK_MODELS)
                fprintf(stderr, "[CUSTOM_TRACK_COLLISION_BUDGET] maximum=%zu arena=%zu\n", maximumCollisionBudget, capacity);
            if (i == MDKR_CUSTOM_TRACK_MODELS)
                fprintf(stderr, "[CUSTOM_TRACK_WAVE_CORPUS] models=%u segments=%u PASS\n", waveModels, waveSegments);
            if (i == MDKR_CUSTOM_TRACK_MODELS) {
                mempool_free(textureTable);
                fprintf(stderr, "[CUSTOM_TRACK_PULSE_TEXTURE_SUMMARY] accepted=%u refused=%u\n",
                    pulseCorpus.checked, pulseCorpus.refused);
                TRACK_CHECK(pulseCorpus.refused == 0);
                TRACK_CHECK(pulseCorpus.checked > 0);
                fprintf(stderr, "[CUSTOM_TRACK_PULSE_TEXTURE_CORPUS] selected=%u PASS\n", pulseCorpus.checked);
                TRACK_CHECK(lightingPairs == words[MDKR_CUSTOM_TRACK_HEADERS] - 2);
                fprintf(stderr, "[CUSTOM_TRACK_LIGHT_LINK_CORPUS] pairs=%u PASS\n", lightingPairs);
            }
            if (i == MDKR_CUSTOM_TRACK_OBJECT_MAPS)
                fprintf(stderr, "[CUSTOM_TRACK_MAP] entries=%u PASS\n", decodedCount);
        }
        TRACK_CHECK(ends[i] >= 2);
        TRACK_CHECK(asset_load(data, (uintptr_t)retailTail[i], (s32)ends[i] - 2, 2) == 2);
        memset(markers[i], 0xA0 + i, sizeof(markers[i]));
        payloads[i] = (MdkrCustomTrackPayload){markers[i], sizeof(markers[i])};
        inputs[i] = (MdkrCustomTrackSectionInput){(MdkrCustomTrackSectionKind)i,
                      tables[i], words[i], &payloads[i], 1};
        totalWords += words[i] + 1;
    }
    MdkrCustomTrackSession *session = mdkr_custom_track_session_create(
        inputs, MDKR_CUSTOM_TRACK_SECTION_COUNT, sizeof(markers), totalWords);
    TRACK_CHECK(session != NULL);
    TRACK_CHECK(asset_custom_track_session_set(session));
    /* A structurally valid but wrong retail prefix must not replace this
     * installed session, even though its size and last offset are unchanged. */
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        u32 firstOffset = tables[i][0];
        tables[i][0] = firstOffset + 1;
        MdkrCustomTrackSession *wrong = mdkr_custom_track_session_create(
            inputs, MDKR_CUSTOM_TRACK_SECTION_COUNT, sizeof(markers), totalWords);
        tables[i][0] = firstOffset;
        TRACK_CHECK(wrong && !asset_custom_track_session_set(wrong));
        TRACK_CHECK(sCustomTrackSession == session);
        mdkr_custom_track_session_destroy(wrong);
    }
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        u32 data = sCustomTrackIndices[i][0], table = sCustomTrackIndices[i][1];
        u8 bytes[6];
        TRACK_CHECK(asset_retail_entry_count(table, (s32)words[i] - 1) == (s32)words[i] - 2);
        TRACK_CHECK(asset_table_size(data) == (s32)ends[i] + 4);
        TRACK_CHECK(asset_table_size(table) == (s32)((words[i] + 1) * 4));
        u32 *copy = asset_table_load(table);
        TRACK_CHECK(copy && memcmp(copy, tables[i], (words[i] - 1) * 4) == 0);
        TRACK_CHECK(copy[words[i] - 1] == ends[i] + 4 && copy[words[i]] == UINT32_MAX);
        copy[0] = 123; /* Mutating/freeing a consumer copy must not change session. */
        mempool_free(copy);
        copy = asset_table_load(table);
        TRACK_CHECK(copy && copy[0] == tables[i][0]);
        mempool_free(copy);
        TRACK_CHECK(asset_load(data, (uintptr_t)bytes, (s32)ends[i] - 2, 6) == 6);
        TRACK_CHECK(memcmp(bytes, retailTail[i], 2) == 0 && memcmp(bytes + 2, markers[i], 4) == 0);
        memset(bytes, 0x55, sizeof(bytes));
        TRACK_CHECK(asset_load(data, (uintptr_t)bytes, (s32)ends[i] + 3, 2) == 0);
        for (size_t j = 0; j < sizeof(bytes); ++j) TRACK_CHECK(bytes[j] == 0x55);
        TRACK_CHECK(asset_rom_offset(data, ends[i]) == NULL);
        TRACK_CHECK(asset_table_load(data) == NULL);
    }
    TRACK_CHECK(asset_custom_track_session_set(NULL));
    mdkr_custom_track_session_destroy(session);
    for (unsigned i = 0; i < MDKR_CUSTOM_TRACK_SECTION_COUNT; ++i) {
        TRACK_CHECK(asset_table_size(sCustomTrackIndices[i][0]) == physicalSizes[i]);
        TRACK_CHECK(asset_retail_entry_count(sCustomTrackIndices[i][1], (s32)words[i] - 2) == (s32)words[i] - 2);
        mempool_free(tables[i]);
    }
    fprintf(stderr, "[CUSTOM_TRACK_LOADER] PASS sections=5 detached=1\n");
#undef TRACK_CHECK
}
#endif

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
