/**
 * asset_swap.c  —  Per-asset-type big-endian -> host byteswappers for mdkr64.
 *
 * Self-contained: operates on raw byte buffers via explicit ON-DISK offsets
 * (the N64/BE record layout, i.e. the docs/ref/asset_fileTypes .hpp layouts,
 * cross-checked against game/include/structs.h). It deliberately does NOT cast
 * to the game's C structs: on a 64-bit host those structs have 8-byte pointer
 * fields, whereas the on-disk records use 4-byte pointer/offset fields, so the
 * host struct offsets do not match the bytes we are swapping. Explicit offsets
 * are also self-documenting and audit-friendly.
 *
 * See asset_swap.h for the integration contract and the swap/no-swap policy,
 * and docs/asset_swap_notes.md for the per-type coverage table + open questions.
 */

#include "asset_swap.h"

#include <stdint.h>
#include <stdlib.h> /* getenv, atoi (MDKR_FORCE_LAPS test hook) */
#include <string.h>

#include "asset_enums.h" /* AssetSectionsEnum */

/* --------------------------------------------------------------------------
 * Host endianness. On a big-endian host the ROM bytes already match native
 * order and every swap below is a no-op.
 * ------------------------------------------------------------------------ */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define ASSET_SWAP_HOST_BE 1
#else
#define ASSET_SWAP_HOST_BE 0
#endif

/* --------------------------------------------------------------------------
 * Primitive in-place swaps at a byte offset. All bounds are the caller's
 * responsibility except where a helper takes `size` explicitly.
 * ------------------------------------------------------------------------ */

static inline void sw16(void *base, uint32_t off) {
#if !ASSET_SWAP_HOST_BE
    uint8_t *p = (uint8_t *) base + off;
    uint8_t t = p[0];
    p[0] = p[1];
    p[1] = t;
#else
    (void) base;
    (void) off;
#endif
}

static inline void sw32(void *base, uint32_t off) {
#if !ASSET_SWAP_HOST_BE
    uint8_t *p = (uint8_t *) base + off;
    uint8_t t;
    t = p[0]; p[0] = p[3]; p[3] = t;
    t = p[1]; p[1] = p[2]; p[2] = t;
#else
    (void) base;
    (void) off;
#endif
}

/* f32 has the same 32-bit width; swapping its bit pattern is identical. */
#define swf32(base, off) sw32((base), (off))

/* Array helpers (count elements, contiguous, given element stride implied). */
static void sw16_arr(void *base, uint32_t off, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        sw16(base, off + i * 2u);
    }
}

static void sw32_arr(void *base, uint32_t off, uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        sw32(base, off + i * 4u);
    }
}

/* Read a native scalar back out AFTER its field has been swapped, for walking
 * variable-length structures. */
static inline int32_t rd32(const void *base, uint32_t off) {
    int32_t v;
    memcpy(&v, (const uint8_t *) base + off, 4);
    return v;
}
static inline int16_t rd16(const void *base, uint32_t off) {
    int16_t v;
    memcpy(&v, (const uint8_t *) base + off, 2);
    return v;
}
static inline uint8_t rd8(const void *base, uint32_t off) {
    return ((const uint8_t *) base)[off];
}

/* True if [off, off+len) fits inside a buffer of `size` bytes. */
static inline int in_bounds(uint32_t off, uint32_t len, uint32_t size) {
    return (off <= size) && (len <= size - off);
}

/* ==========================================================================
 *  Shared geometry element swappers (used by both object and level models)
 * ======================================================================== */

/* DkrVertex / Vertex — 10 bytes: s16 x,y,z + u8 r,g,b,a (rgba unswapped). */
#define DKR_VERTEX_SIZE 10u
static void swap_vertices(void *data, uint32_t off, uint32_t count, uint32_t size) {
    uint32_t i;
    if (off == 0 || !in_bounds(off, count * DKR_VERTEX_SIZE, size)) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t v = off + i * DKR_VERTEX_SIZE;
        sw16(data, v + 0); /* x */
        sw16(data, v + 2); /* y */
        sw16(data, v + 4); /* z */
        /* +6 r,g,b,a stay as bytes */
    }
}

/* DkrTriangle / Triangle — 16 bytes: 4 index/flag bytes (UNSWAPPED) + three
 * TexCoords (s16 u, s16 v) at +4..+14 (all swapped). */
#define DKR_TRIANGLE_SIZE 16u
static void swap_triangles(void *data, uint32_t off, uint32_t count, uint32_t size) {
    uint32_t i;
    if (off == 0 || !in_bounds(off, count * DKR_TRIANGLE_SIZE, size)) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t t = off + i * DKR_TRIANGLE_SIZE;
        /* +0 flags,vi0,vi1,vi2 : bytes, read as Triangle.verticesArray/.flags */
        sw16(data, t + 4);  /* uv0.u */
        sw16(data, t + 6);  /* uv0.v */
        sw16(data, t + 8);  /* uv1.u */
        sw16(data, t + 10); /* uv1.v */
        sw16(data, t + 12); /* uv2.u */
        sw16(data, t + 14); /* uv2.v */
    }
}

/* DkrBatch / TriangleBatchInfo — 12 bytes:
 *   +0 textureIndex(u8) +1 unk1/vertOverride(s8) : bytes
 *   +2 verticesOffset(s16)  +4 facesOffset(s16)  : swap
 *   +6 lightSource/miscData(u8) +7 unk7/texOffset(u8) : bytes
 *   +8 flags(u32) : swap
 * The batches array is stored with (numBatches + 1) entries: a sentinel entry
 * whose verticesOffset/facesOffset bound the last real batch. Callers pass the
 * inclusive count. */
#define DKR_BATCH_SIZE 12u
static void swap_batches(void *data, uint32_t off, uint32_t countPlus1, uint32_t size) {
    uint32_t i;
    if (off == 0 || !in_bounds(off, countPlus1 * DKR_BATCH_SIZE, size)) {
        return;
    }
    for (i = 0; i < countPlus1; i++) {
        uint32_t b = off + i * DKR_BATCH_SIZE;
        sw16(data, b + 2); /* verticesOffset */
        sw16(data, b + 4); /* facesOffset */
        sw32(data, b + 8); /* flags */
    }
}

/* DkrTextureInfo / TextureInfo — 8 bytes: s32 id (swap) + 4 bytes
 * (width,height,format,surfaceType). */
#define DKR_TEXINFO_SIZE 8u
static void swap_texinfos(void *data, uint32_t off, uint32_t count, uint32_t size) {
    uint32_t i;
    if (off == 0 || !in_bounds(off, count * DKR_TEXINFO_SIZE, size)) {
        return;
    }
    for (i = 0; i < count; i++) {
        sw32(data, off + i * DKR_TEXINFO_SIZE + 0); /* id */
    }
}

/* ==========================================================================
 *  ASSET_OBJECT_MODELS  (ObjectModel header + nested arrays; post-inflate)
 *  On-disk layout: docs/ref/asset_fileTypes/objectModel.hpp (== structs.h
 *  ObjectModel through 0x54).
 * ======================================================================== */
static void swap_object_model(void *data, uint32_t size) {
    int32_t offTex, offVtx, offTri, offBat, offAttach, offSpheres, offAnimIdx;
    int16_t nTex, nVtx, nTri, nBat, nAttach, sphSize;

    if (size < 0x58u) {
        return;
    }

    /* --- header (0x00 .. 0x54) --- */
    sw32(data, 0x00); /* textures  (offset) */
    sw32(data, 0x04); /* vertices  (offset) */
    sw32(data, 0x08); /* triangles (offset) */
    sw32(data, 0x0C); /* reservedC */
    sw32(data, 0x10); /* reserved10 */
    sw32(data, 0x14); /* attachPoints (offset) */
    sw16(data, 0x18); /* numberOfAttachPoints */
    sw16(data, 0x1A); /* unk1A */
    sw32(data, 0x1C); /* collisionSpheres (offset) */
    sw16(data, 0x20); /* collisionSpheresSize */
    sw16(data, 0x22); /* numberOfTextures */
    sw16(data, 0x24); /* numberOfVertices */
    sw16(data, 0x26); /* numberOfTriangles */
    sw16(data, 0x28); /* numberOfBatches */
    /* 0x2A,0x2B unk : bytes */
    sw32(data, 0x2C); /* fileSize */
    sw16(data, 0x30); /* reservedReferences */
    sw16(data, 0x32); /* reserved32 */
    /* 0x34..0x37 : bytes */
    sw32(data, 0x38); /* batches (offset) */
    swf32(data, 0x3C); /* unk3C */
    sw32(data, 0x40); /* reserved40 */
    sw32(data, 0x44); /* reservedAnimations */
    sw16(data, 0x48); /* reservedNumberOfAnimations */
    sw16(data, 0x4A); /* numberOfAnimatedVertices */
    sw32(data, 0x4C); /* animatedVertexIndices (offset) */
    sw16(data, 0x50); /* hasAnimatedTexture */
    sw16(data, 0x52); /* reserved52 */
    sw32(data, 0x54); /* unused54 */

    /* --- read now-native offsets + counts --- */
    offTex     = rd32(data, 0x00);
    offVtx     = rd32(data, 0x04);
    offTri     = rd32(data, 0x08);
    offAttach  = rd32(data, 0x14);
    offSpheres = rd32(data, 0x1C);
    offBat     = rd32(data, 0x38);
    offAnimIdx = rd32(data, 0x4C);
    nAttach    = rd16(data, 0x18);
    sphSize    = rd16(data, 0x20);
    nTex       = rd16(data, 0x22);
    nVtx       = rd16(data, 0x24);
    nTri       = rd16(data, 0x26);
    nBat       = rd16(data, 0x28);

    /* --- nested arrays --- */
    if (nTex > 0)  swap_texinfos(data, (uint32_t) offTex, (uint32_t) nTex, size);
    if (nVtx > 0)  swap_vertices(data, (uint32_t) offVtx, (uint32_t) nVtx, size);
    if (nTri > 0)  swap_triangles(data, (uint32_t) offTri, (uint32_t) nTri, size);
    if (nBat > 0)  swap_batches(data, (uint32_t) offBat, (uint32_t) nBat + 1u, size);

    /* attachPoints: s16 vertex indices. */
    if (nAttach > 0 && offAttach != 0 &&
        in_bounds((uint32_t) offAttach, (uint32_t) nAttach * 2u, size)) {
        sw16_arr(data, (uint32_t) offAttach, (uint32_t) nAttach);
    }
    /* collisionSpheres: array of (s16 vertexIndex, s16 radiusScale) = s16[size]. */
    if (sphSize > 0 && offSpheres != 0 &&
        in_bounds((uint32_t) offSpheres, (uint32_t) sphSize * 2u, size)) {
        sw16_arr(data, (uint32_t) offSpheres, (uint32_t) sphSize);
    }
    /* animatedVertexIndices: the authoritative asm (hasm_native/obj_animate.c:
     * `indices = (s16*)model->animatedVertexIndices; for i<nVerts: idx=indices[i]`)
     * reads this as s16[numberOfVertices] — one entry per MODEL vertex, -1 when
     * that vertex is not animated. NOT s32[numberOfAnimatedVertices]. structs.h
     * mistypes the field as `s32*`; the asm is ground truth (do not edit
     * structs.h). See docs/asset_swap_notes.md. */
    if (nVtx > 0 && offAnimIdx != 0 &&
        in_bounds((uint32_t) offAnimIdx, (uint32_t) nVtx * 2u, size)) {
        sw16_arr(data, (uint32_t) offAnimIdx, (uint32_t) nVtx);
    }
}

/* ==========================================================================
 *  ASSET_LEVEL_MODELS  (LevelModel header + segments + nested arrays)
 *  On-disk layout: structs.h LevelModel / LevelModelSegment (no .hpp exists;
 *  cross-checked against tracks.c generate_track() offset patching).
 * ======================================================================== */
#define LEVEL_SEGMENT_SIZE 0x44u
static void swap_level_model(void *data, uint32_t size) {
    int32_t offTex, offSeg;
    int16_t nTex, nSeg;
    int32_t k;

    if (size < 0x4Cu) {
        return;
    }

    /* --- LevelModel header (0x00 .. 0x48) --- */
    sw32(data, 0x00); /* textures (offset) */
    sw32(data, 0x04); /* segments (offset) */
    sw32(data, 0x08); /* segmentsBoundingBoxes (offset) */
    sw32(data, 0x0C); /* unkC (offset) */
    sw32(data, 0x10); /* segmentsBitfields (offset) */
    sw32(data, 0x14); /* segmentsBspTree (offset) */
    sw16(data, 0x18); /* numberOfTextures */
    sw16(data, 0x1A); /* numberOfSegments */
    sw16(data, 0x1C); /* unk1C */
    sw16(data, 0x1E); /* numberOfAnimatedTextures */
    sw32(data, 0x20); /* minimapSpriteIndex */
    sw16(data, 0x24); /* minimapRotation */
    sw16(data, 0x26); /* unk26 */
    swf32(data, 0x28); /* minimapXScale */
    swf32(data, 0x2C); /* minimapYScale */
    sw16(data, 0x30); /* minimapOffsetXAdv1 */
    sw16(data, 0x32); /* minimapOffsetYAdv1 */
    sw16(data, 0x34); /* minimapOffsetXAdv2 */
    sw16(data, 0x36); /* minimapOffsetYAdv2 */
    sw32(data, 0x38); /* minimapColor */
    sw16(data, 0x3C); /* lowerXBounds */
    sw16(data, 0x3E); /* upperXBounds */
    sw16(data, 0x40); /* lowerYBounds */
    sw16(data, 0x42); /* upperYBounds */
    sw16(data, 0x44); /* lowerZBounds */
    sw16(data, 0x46); /* upperZBounds */
    sw32(data, 0x48); /* modelSize */

    offTex = rd32(data, 0x00);
    offSeg = rd32(data, 0x04);
    nTex   = rd16(data, 0x18);
    nSeg   = rd16(data, 0x1A);

    /* segmentsBoundingBoxes: LevelModelSegmentBoundingBox = 6 x s16 each. */
    {
        int32_t offBox = rd32(data, 0x08);
        if (nSeg > 0 && offBox != 0 &&
            in_bounds((uint32_t) offBox, (uint32_t) nSeg * 12u, size)) {
            sw16_arr(data, (uint32_t) offBox, (uint32_t) nSeg * 6u);
        }
    }
    /* segmentsBspTree: BspTreeNode = 8 bytes: s16 leftNode, s16 rightNode,
     * u8 splitType, u8 segmentIndex, s16 splitValue. Node count is not stored
     * in the header; the tree is walked by index. We swap conservatively using
     * the region between the bsp-tree offset and the bounding-box offset when
     * both are present (bsp tree is laid out last). If the extent is unknown we
     * skip it (documented in notes). */
    /* (skipped: BSP node count unknown from header — see notes) */

    if (nTex > 0) {
        swap_texinfos(data, (uint32_t) offTex, (uint32_t) nTex, size);
    }

    /* --- per-segment header + nested geometry --- */
    if (offSeg != 0) {
        for (k = 0; k < nSeg; k++) {
            uint32_t s = (uint32_t) offSeg + (uint32_t) k * LEVEL_SEGMENT_SIZE;
            int32_t sVtx, sTri, sBat;
            int16_t sNVtx, sNTri, sNBat;

            if (!in_bounds(s, LEVEL_SEGMENT_SIZE, size)) {
                break;
            }
            /* On-disk fields the game reads (rest are runtime scratch): */
            sw32(data, s + 0x00); /* vertices (offset)      */
            sw32(data, s + 0x04); /* triangles (offset)     */
            sw32(data, s + 0x08); /* unk8 (read as bitfield/value elsewhere) */
            sw32(data, s + 0x0C); /* batches (offset)       */
            sw32(data, s + 0x14); /* collisionFacets (offset) */
            sw16(data, s + 0x1C); /* numberOfVertices       */
            sw16(data, s + 0x1E); /* numberOfTriangles      */
            sw16(data, s + 0x20); /* numberOfBatches        */
            sw16(data, s + 0x28); /* unk28 (segmentsBitfields index) */
            sw32(data, s + 0x3C); /* unk3C (bitfield, read as `& 2`) */

            sVtx  = rd32(data, s + 0x00);
            sTri  = rd32(data, s + 0x04);
            sBat  = rd32(data, s + 0x0C);
            sNVtx = rd16(data, s + 0x1C);
            sNTri = rd16(data, s + 0x1E);
            sNBat = rd16(data, s + 0x20);

            if (sNVtx > 0) swap_vertices(data, (uint32_t) sVtx, (uint32_t) sNVtx, size);
            if (sNTri > 0) swap_triangles(data, (uint32_t) sTri, (uint32_t) sNTri, size);
            if (sNBat > 0) swap_batches(data, (uint32_t) sBat, (uint32_t) sNBat + 1u, size);

            /* collisionFacets: one CollisionFacetPlanes per triangle = 4 x u16
             * (basePlaneIndex + edgeBisectorPlane[3]). track_init_collision() and
             * resolve_collisions() read these indices from ROM to build/query the
             * runtime collision planes; left big-endian, basePlaneIndex reads e.g.
             * 0x4200 instead of 0x0042 -> wild plane indexing -> nan/garbage planes
             * -> a racer's wheels never register a ground contact (can't move). */
            {
                int32_t sCF = rd32(data, s + 0x14); /* already byteswapped above */
                if (sNTri > 0 && sCF != 0 && in_bounds((uint32_t) sCF, (uint32_t) sNTri * 8u, size)) {
                    sw16_arr(data, (uint32_t) sCF, (uint32_t) sNTri * 4u);
                }
            }
        }
    }
}

/* ==========================================================================
 *  ASSET_LEVEL_HEADERS  (LevelHeader, fixed ~0xC4..0xC8 record)
 *  Layout: structs.h LevelHeader (wins over levelHeader.hpp on discrepancies).
 * ======================================================================== */
static void swap_level_header(void *data, uint32_t size) {
    if (size < 0xC4u) {
        return;
    }

    /*
     * TEST HOOK -- MDKR_FORCE_LAPS=N overrides the level's lap requirement.
     * No-op unless the variable is set (same contract as MDKR_FORCE_BOOST).
     *
     * Why it exists: the race-finish path (final lap -> raceFinished -> results
     * -> time saved) is the core play loop, and validating it otherwise depends
     * on an input script driving three clean laps. The open-loop fixture route
     * holds the racing line for one lap and then drifts (measured: lap 1 at clock
     * 2776, lap 2 not until 10557), so a driving script is a flaky way to reach
     * the finish. Shortening the race makes the finish reachable deterministically
     * without touching race logic.
     *
     * Done HERE, once, at the load boundary, deliberately: `laps` is read from
     * ~12 places across racer.c and game_ui.c, and overriding it per-site would
     * mean patching the whole race loop and HUD. Rewriting the single s8 in the
     * header keeps every reader consistent by construction. It is a plain byte at
     * 0x4B (no endianness involved), so this cannot perturb the swap itself.
     */
    {
        static int sForcedLaps = -2;
        if (sForcedLaps == -2) {
            const char *e = getenv("MDKR_FORCE_LAPS");
            sForcedLaps = (e != NULL && e[0] != '\0') ? atoi(e) : -1;
        }
        if (sForcedLaps > 0 && size > 0x4Bu) {
            ((int8_t *) data)[0x4B] = (int8_t) sForcedLaps; /* LevelHeader.laps */
        }
    }

    swf32(data, 0x08); /* course_height */

    sw16(data, 0x34); /* geometry */
    sw16(data, 0x36); /* collectables */
    sw16(data, 0x38); /* skybox */
    sw16(data, 0x3A); /* fogNear */
    sw16(data, 0x3C); /* fogFar */
    sw16(data, 0x3E); /* fogR */
    sw16(data, 0x40); /* fogG */
    sw16(data, 0x42); /* fogB */

    sw16(data, 0x54); /* instruments (u16) */

    /* wave parameters (s16 values interleaved with u8 fields) */
    sw16(data, 0x5A); /* waveSineHeight0 */
    sw16(data, 0x5E); /* waveSineHeight1 */
    sw16(data, 0x60); /* waveSeedSize */
    sw16(data, 0x62); /* wavePower */
    sw16(data, 0x64); /* unk64 */
    sw16(data, 0x66); /* unk66 */
    sw16(data, 0x68); /* waveTexID */
    sw16(data, 0x6E); /* waveViewDist */

    /* miscAssets[7] / unk74[7] : s32 misc-asset indices, patched via
     * get_misc_asset() at runtime. */
    sw32_arr(data, 0x74, 7);

    /* weather block */
    sw16(data, 0x90); /* weatherEnable */
    sw16(data, 0x92); /* weatherType */
    /* 0x94 intensity, 0x95 opacity : bytes */
    sw16(data, 0x96); /* weatherVelX */
    sw16(data, 0x98); /* weatherVelY */
    sw16(data, 0x9A); /* weatherVelZ */

    /* 0x9C cameraFOV, 0x9D..0x9F bgColor, 0xA0/0xA1 unkA0/unkA1 : bytes
     * (NOTE: levelHeader.hpp calls 0xA0 a single be_int16; structs.h — which
     * the game reads — splits it into two u8 fields, so no swap here). */
    sw32(data, 0xA4); /* unkA4 / specialSkyTexture (u32) */
    sw16(data, 0xA8); /* unkA8 */
    sw16(data, 0xAA); /* unkAA */
    sw32(data, 0xAC); /* pulseLightData (s32 offset/-1) */
    sw16(data, 0xB0); /* unkB0 */
    /* 0xB2..0xB9 : bytes (voidColour etc.) */
    sw16(data, 0xBA); /* unkBA / objectMap2 */
    /* 0xBC..0xC3 : bytes (gradient bg colours) */
    /* 0xC4 trailing u32 (levelHeader.hpp unkC4) exists on disk but structs.h
     * stops at 0xC4 and the game never reads it; swap if the record carries it. */
    if (size >= 0xC8u) {
        sw32(data, 0xC4);
    }
}

/* ==========================================================================
 *  ASSET_OBJECTS  (ObjectHeader, one 0x78 record + arrays it points at)
 *  Layout: structs.h ObjectHeader (== objectHeader.hpp).
 * ======================================================================== */
static void swap_object_header(void *data, uint32_t size) {
    int32_t offModelIds, offVehParts, offParticles, offUnk24;
    uint8_t nModelIds, nVehParts, nParticles;

    if (size < 0x78u) {
        return;
    }

    sw32(data, 0x00); /* unk0 */
    swf32(data, 0x04); /* shadowScale */
    swf32(data, 0x08); /* unk8 */
    swf32(data, 0x0C); /* scale */
    sw32(data, 0x10); /* modelIds (offset) */
    sw32(data, 0x14); /* vehiclePartIds (offset) */
    sw32(data, 0x18); /* vehiclePartIndices (offset) */
    sw32(data, 0x1C); /* objectParticles (offset) */
    sw32(data, 0x20); /* pad20 */
    sw32(data, 0x24); /* unk24 (offset -> ObjectHeader24) */
    swf32(data, 0x28); /* shadeAmbient */
    swf32(data, 0x2C); /* shadeDiffuse */
    sw16(data, 0x30); /* flags (u16) */
    sw16(data, 0x32); /* shadowGroup */
    sw16(data, 0x34); /* unk34 */
    sw16(data, 0x36); /* waterEffectGroup */
    sw16(data, 0x38); /* unk38 */
    /* 0x3A..0x3D : bytes */
    sw16(data, 0x3E); /* shadeAngleY */
    sw16(data, 0x40); /* shadeAngleZ */
    sw16(data, 0x42); /* unk42 */
    sw16(data, 0x44); /* unk44 */
    sw16(data, 0x46); /* unk46 */
    sw16(data, 0x48); /* shadowBottom/Top region s16 */
    sw16(data, 0x4A); /* unk4A */
    sw16(data, 0x4C); /* unk4C */
    sw16(data, 0x4E); /* drawDistance */
    sw16(data, 0x50); /* unk50 */
    /* 0x52..0x5F : bytes (counts, type, name-adjacent) */
    /* 0x60..0x6F internalName[16] : bytes */
    /* 0x70..0x77 : bytes */

    offModelIds  = rd32(data, 0x10);
    offVehParts  = rd32(data, 0x14);
    offParticles = rd32(data, 0x1C);
    offUnk24     = rd32(data, 0x24);
    nModelIds    = rd8(data, 0x55); /* numberOfModelIds */
    nVehParts    = rd8(data, 0x56); /* numberOfVehicleParts (attachPointCount) */
    nParticles   = rd8(data, 0x57); /* numberOfParticles */

    /* modelIds: s32[numberOfModelIds] */
    if (nModelIds > 0 && offModelIds != 0 &&
        in_bounds((uint32_t) offModelIds, (uint32_t) nModelIds * 4u, size)) {
        sw32_arr(data, (uint32_t) offModelIds, nModelIds);
    }
    /* vehiclePartIds: s32[numberOfVehicleParts] */
    if (nVehParts > 0 && offVehParts != 0 &&
        in_bounds((uint32_t) offVehParts, (uint32_t) nVehParts * 4u, size)) {
        sw32_arr(data, (uint32_t) offVehParts, nVehParts);
    }
    /* objectParticles: ObjHeaderParticleEntry = (s32 upper, s32 lower) each.
     * (The .hpp exposes a s32/2xs16 union; game reads the s32 form.) */
    if (nParticles > 0 && offParticles != 0 &&
        in_bounds((uint32_t) offParticles, (uint32_t) nParticles * 8u, size)) {
        sw32_arr(data, (uint32_t) offParticles, (uint32_t) nParticles * 2u);
    }
    /* unk24 -> ObjectHeader24 (0x18): u16 unk6, u32 unk8, s16 homeX/Y/Z,
     * u16 radius/unk14/unk16. */
    if (offUnk24 != 0 && in_bounds((uint32_t) offUnk24, 0x18u, size)) {
        uint32_t u = (uint32_t) offUnk24;
        sw16(data, u + 0x06); /* unk6 */
        sw32(data, u + 0x08); /* unk8 (union u32) */
        sw16(data, u + 0x0C); /* homeX */
        sw16(data, u + 0x0E); /* homeY */
        sw16(data, u + 0x10); /* homeZ */
        sw16(data, u + 0x12); /* radius */
        sw16(data, u + 0x14); /* unk14 */
        sw16(data, u + 0x16); /* unk16 */
    }
}

/* ==========================================================================
 *  ASSET_TEXTURES_2D / _3D  (TextureHeader(s); header only, texels untouched)
 *  Layout: structs.h TextureHeader (== texture.hpp), 0x20 bytes.
 * ======================================================================== */
#define TEXTURE_HEADER_SIZE 0x20u
static void swap_texture_header_at(void *data, uint32_t off) {
    sw16(data, off + 0x06); /* flags */
    sw16(data, off + 0x08); /* ciPaletteOffset */
    sw16(data, off + 0x0A); /* numberOfCommands (runtime-init; harmless) */
    sw32(data, off + 0x0C); /* cmd (runtime-init; harmless) */
    sw16(data, off + 0x12); /* numOfTextures (u16) */
    sw16(data, off + 0x14); /* frameAdvanceDelay (u16) */
    sw16(data, off + 0x16); /* textureSize (s16, incl. header) */
    /* all other fields are u8 */
}

void swap_texture_header(void *texHeader) {
    if (texHeader != NULL) {
        swap_texture_header_at(texHeader, 0);
    }
}

static void swap_texture(void *data, uint32_t size) {
    /* The game derives the frame count as `numOfTextures >> 8` on the BE u16 at
     * 0x12 — i.e. the raw high byte at offset 0x12. Read it BEFORE swapping. */
    uint32_t count;
    uint32_t cur = 0;
    uint32_t i;

    if (size < TEXTURE_HEADER_SIZE) {
        return;
    }
    count = rd8(data, 0x12);
    if (count == 0) {
        count = 1;
    }

    for (i = 0; i < count; i++) {
        int16_t texSize;
        if (!in_bounds(cur, TEXTURE_HEADER_SIZE, size)) {
            break;
        }
        swap_texture_header_at(data, cur);
        texSize = rd16(data, cur + 0x16); /* now native */
        /* Animated frames are packed header+texels of `textureSize` bytes. Some
         * textures store 0 here (write-size=false); we can only safely advance
         * when it is known. Static textures (count==1) don't need to advance. */
        if (texSize <= (int16_t) TEXTURE_HEADER_SIZE) {
            break;
        }
        cur += (uint32_t) texSize;
    }
    /* Texel payload after each header is left byte-for-byte (N64 format). */
}

/* ==========================================================================
 *  ASSET_SPRITES  (SpriteAsset header; frame offset bytes untouched)
 *  Layout: structs.h SpriteAsset (== sprite.hpp SpriteHeader, 12-byte head).
 * ======================================================================== */
static void swap_sprite(void *data, uint32_t size) {
    if (size < 0x0Cu) {
        return;
    }
    sw16(data, 0x00); /* baseTextureId / startTextureIndex */
    sw16(data, 0x02); /* numberOfFrames */
    sw16(data, 0x04); /* anchor.x (unk4) */
    sw16(data, 0x06); /* anchor.y (unk6) */
    sw32(data, 0x08); /* unused_field (unk8; 0 in ROM) */
    /* 0x0C.. frameTexOffsets : bytes */
}

/* ==========================================================================
 *  ASSET_FONTS  (u32 numberOfFonts + FontFile[0x400] each)
 *  Layout: fonts.hpp FontData/FontFile (== font.h FontData).
 * ======================================================================== */
#define FONT_FILE_SIZE 0x400u
static void swap_fonts(void *data, uint32_t size) {
    uint32_t count, i;
    if (size < 4u) {
        return;
    }
    sw32(data, 0x00); /* numberOfFonts */
    count = (uint32_t) rd32(data, 0x00);

    for (i = 0; i < count; i++) {
        uint32_t f = 4u + i * FONT_FILE_SIZE;
        if (!in_bounds(f, FONT_FILE_SIZE, size)) {
            break;
        }
        /* +0x00 name[32] : bytes */
        sw16(data, f + 0x20); /* fixedWidth */
        sw16(data, f + 0x22); /* yOffset */
        sw16(data, f + 0x24); /* specialCharacterWidth */
        sw16(data, f + 0x26); /* tabWidth */
        /* +0x28 junkText[24] : bytes */
        sw16_arr(data, f + 0x40, 32); /* textureIndices[32] (s16) */
        /* +0x80 reservedForTexturePointers[32] : runtime scratch, skip */
        /* +0x100 characters[96] : all s8/u8, no swap */
    }
}

/* ==========================================================================
 *  ASSET_TTGHOSTS  (GhostHeader + GhostNode[])
 *  Layout: ttGhost.hpp.
 * ======================================================================== */
#define GHOST_NODE_SIZE 12u
static void swap_tt_ghost(void *data, uint32_t size) {
    int16_t nodeCount;
    uint32_t i, base;

    if (size < 8u) {
        return;
    }
    /* header: 4 bytes ids + s16 time + s16 nodeCount */
    sw16(data, 0x04); /* time */
    sw16(data, 0x06); /* nodeCount */
    nodeCount = rd16(data, 0x06);
    if (nodeCount <= 0) {
        return;
    }
    base = 8u;
    for (i = 0; i < (uint32_t) nodeCount; i++) {
        uint32_t n = base + i * GHOST_NODE_SIZE;
        if (!in_bounds(n, GHOST_NODE_SIZE, size)) {
            break;
        }
        sw16_arr(data, n, 6); /* x,y,z, zRot,xRot,yRot (all s16) */
    }
}

/* ==========================================================================
 *  ASSET_TTGHOSTS_TABLE  (array of TTGhostTable, 8 bytes each)
 *
 *  NOT a plain u32 offset table. objects.h TTGhostTable is
 *      u8 mapId; u8 defaultVehicleId; [2 bytes padding]; s32 ghostOffset;
 *  so word 0 of every entry is a BYTE PAIR (+2 padding) and only word 1 is a
 *  32-bit scalar. Running the generic asset_swap_lut() over it reverses word 0
 *  and therefore relocates mapId from byte 0 to byte 3:
 *
 *    on disk   03 00 00 00 | 00 00 00 00      -> mapId=3, vehicle=0, offset=0
 *    lut-swap  00 00 00 03 | 00 00 00 00      -> mapId=0, vehicle=0
 *    terminator on disk FF FF 00 00 -> lut-swap 00 00 FF FF -> mapId=0x00
 *
 *  timetrial_load_staff_ghost() (objects.c) both matches on mapId and stops on
 *  `mapId != 0xFF`, so the whole-word swap breaks the lookup AND destroys the
 *  terminator, walking the table past its allocation. Swap only ghostOffset.
 * ======================================================================== */
#define TT_GHOST_TABLE_ENTRY_SIZE 8u
static void swap_tt_ghost_table(void *data, uint32_t size) {
    uint32_t off;
    for (off = 0; off + TT_GHOST_TABLE_ENTRY_SIZE <= size;
         off += TT_GHOST_TABLE_ENTRY_SIZE) {
        /* +0 mapId (u8), +1 defaultVehicleId (u8), +2..+3 padding: NO swap. */
        sw32(data, off + 4); /* ghostOffset */
    }
}

/* ==========================================================================
 *  ASSET_PARTICLES  (array of ParticleDescriptor, 0x18 each)
 *  Layout: particles.h ParticleDescriptor.
 * ======================================================================== */
#define PARTICLE_DESC_SIZE 0x18u
static void swap_particles(void *data, uint32_t size) {
    uint32_t count = size / PARTICLE_DESC_SIZE;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t p = i * PARTICLE_DESC_SIZE;
        /* +0 kind,movementType : bytes */
        sw16(data, p + 0x02); /* flags */
        sw16(data, p + 0x04); /* textureID */
        sw16(data, p + 0x06); /* textureFrameStep */
        sw16(data, p + 0x08); /* lifeTime */
        sw16(data, p + 0x0A); /* lifeTimeRange / bitfield union (u16) */
        /* +0x0C opacity,opacityVel : bytes */
        sw16(data, p + 0x0E); /* opacityTimer */
        swf32(data, p + 0x10); /* scale */
        /* +0x14 colour RGBA : bytes */
    }
}

/* ==========================================================================
 *  ASSET_PARTICLE_BEHAVIORS  (array of ParticleBehaviour, 0xA0 each)
 *  Layout: particles.h ParticleBehaviour.
 * ======================================================================== */
#define PARTICLE_BEHAVIOUR_SIZE 0xA0u
static void swap_particle_behaviours(void *data, uint32_t size) {
    uint32_t count = size / PARTICLE_BEHAVIOUR_SIZE;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t b = i * PARTICLE_BEHAVIOUR_SIZE;
        sw32(data, b + 0x00);        /* flags */
        sw32_arr(data, b + 0x04, 3); /* emitterPos Vec3f */
        swf32(data, b + 0x10);       /* sourceDistance */
        sw16_arr(data, b + 0x14, 3); /* sourceRotation Vec3s */
        sw16(data, b + 0x1A);        /* maxParticlesFromSamePos */
        sw16_arr(data, b + 0x1C, 3); /* sourceAngularVelocity Vec3s */
        sw16_arr(data, b + 0x22, 3); /* emissionDirection Vec3s */
        sw16(data, b + 0x28);        /* maxParticlesInSameDir */
        sw16_arr(data, b + 0x2A, 3); /* emissionDirAngularVelocity Vec3s */
        sw32_arr(data, b + 0x30, 3); /* velocityModifier Vec3f */
        swf32(data, b + 0x3C);       /* emissionSpeed */
        sw16(data, b + 0x40);        /* spawnInterval */
        sw16(data, b + 0x42);        /* burstCount */
        sw16_arr(data, b + 0x44, 3); /* rotation Vec3s */
        sw16_arr(data, b + 0x4A, 3); /* angularVelocity Vec3s */
        swf32(data, b + 0x50);       /* scale */
        swf32(data, b + 0x54);       /* scaleVelocity */
        swf32(data, b + 0x58);       /* movementParam */
        sw32(data, b + 0x5C);        /* randomizationFlags */
        sw32(data, b + 0x60);        /* sourceDistanceRange */
        sw16_arr(data, b + 0x64, 3); /* sourceDirRange Vec3s */
        sw16_arr(data, b + 0x6A, 3); /* emissionDirRange Vec3s */
        sw32(data, b + 0x70);        /* emissionSpeedRange */
        sw32_arr(data, b + 0x74, 3); /* velocityModifierRange Vec3i */
        sw16_arr(data, b + 0x80, 3); /* rotationRange Vec3s */
        sw16_arr(data, b + 0x86, 3); /* angularVelocityRange Vec3s */
        sw32(data, b + 0x8C);        /* scaleRange */
        sw32(data, b + 0x90);        /* scaleVelocityRange */
        sw32(data, b + 0x94);        /* movementParamRange */
        /* +0x98 colourRange RGBA : bytes */
        sw32(data, b + 0x9C);        /* colourLoop (offset -> misc asset / -1) */
    }
}

/* ==========================================================================
 *  ASSET_AI_BEHAVIOUR  (array of AIBehaviourTable, 0x18 each: 2x f32 + s8[16])
 * ======================================================================== */
#define AI_BEHAVIOUR_SIZE 0x18u
static void swap_ai_behaviour(void *data, uint32_t size) {
    uint32_t count = size / AI_BEHAVIOUR_SIZE;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t a = i * AI_BEHAVIOUR_SIZE;
        swf32(data, a + 0x00); /* unk0 */
        swf32(data, a + 0x04); /* unk4 */
        /* +0x08 percentages[4][4] : bytes */
    }
}

/* ==========================================================================
 *  ASSET_OBJECT_ANIMATIONS  (full walk)
 *
 *  Layout is authoritative from game/src/hasm_native/obj_animate.c (traced from
 *  the shipping asm obj_animate.s). The animation blob does NOT carry the
 *  animated-vertex count, so the full body swap needs numAnimatedVertices from
 *  the owning ObjectModel (model->numberOfAnimatedVertices) — hence the public
 *  asset_swap_object_animation() variant. The generic dispatcher can only do the
 *  leading keyframeCount.
 *
 *  Blob layout, `data`-relative (obj_animate's `animData` = data + 4, i.e. just
 *  past the leading s32 count that model_anim_init strips via `anim++`):
 *
 *    [0x00]                                  s32 keyframeCount (== animLength)
 *    animData+0x00 == data+0x04 :            12-byte frame-0 offset header
 *    animData+0x0C == data+0x10 :            frame-0 delta table, 3*nAnimVerts s16
 *                                            (indexed by animated-vertex slot;
 *                                             bound = mi->vertices[2] scratch =
 *                                             nAnimVerts*3 s16)
 *    keyframe blocks, keyframeSize = nAnimVerts*3 + 12 each, laid out so that
 *    keyframe m's trailing 12-byte header sits at animData + keyframeSize*m - 0xC
 *    (m >= 2) followed by keyframe m's nAnimVerts*3 s8 delta block. The s8
 *    deltas are single bytes and MUST NOT be swapped.
 *
 *  Each 12-byte offset header holds s16 offsetX/offsetY/offsetZ at +0/+2/+4 and
 *  s16 headTilt at +0xA (bytes +6..+9 unused). obj_animate reads keyframe
 *  headers only for m in [2, animLength] (the frame-0 header is the special case
 *  at animData+0); m == 1 would alias the frame-0 delta table, so we start at 2
 *  to avoid double-swapping those s16 values.
 * ======================================================================== */
void asset_swap_object_animation(void *data, uint32_t size, uint32_t numAnimatedVertices) {
    uint32_t keyframeSize;
    int32_t animLength;
    uint32_t hdr;
    uint32_t m;

    if (data == NULL || size < 4u) {
        return;
    }

    /* leading keyframeCount / animLength */
    sw32(data, 0x00);
    animLength = rd32(data, 0x00);

    keyframeSize = numAnimatedVertices * 3u + 12u;

    /* frame-0 offset header at data+4: s16 at +0,+2,+4,+0xA */
    if (in_bounds(0x04u, 12u, size)) {
        sw16(data, 0x04 + 0x0); /* offsetX */
        sw16(data, 0x04 + 0x2); /* offsetY */
        sw16(data, 0x04 + 0x4); /* offsetZ */
        sw16(data, 0x04 + 0xA); /* headTilt */
    }

    /* frame-0 full-precision delta table at data+0x10: 3*nAnimVerts s16 */
    if (numAnimatedVertices > 0 &&
        in_bounds(0x10u, numAnimatedVertices * 3u * 2u, size)) {
        sw16_arr(data, 0x10u, numAnimatedVertices * 3u);
    }

    /* keyframe trailing headers: header m at data + keyframeSize*m - 8
     * (= animData + keyframeSize*m - 0xC). Walk m = 2 .. animLength, bounded by
     * the buffer. s8 delta blocks between headers are left byte-for-byte. */
    if (animLength > 0 && keyframeSize > 0) {
        for (m = 2; m <= (uint32_t) animLength; m++) {
            uint32_t base = keyframeSize * m;
            if (base < 8u) {
                continue;
            }
            hdr = base - 8u; /* data-relative start of the 12-byte header */
            if (!in_bounds(hdr, 12u, size)) {
                break;
            }
            sw16(data, hdr + 0x0); /* offsetX */
            sw16(data, hdr + 0x2); /* offsetY */
            sw16(data, hdr + 0x4); /* offsetZ */
            sw16(data, hdr + 0xA); /* headTilt */
        }
    }
}

/* ==========================================================================
 *  ASSET_MISC sub-asset: LevelHeader_70 "pulsating light" data (heterogeneous
 *  MISC section is punted at load, so this record is swapped on demand at the
 *  func_8007F1E8 call sites, with a dedup guard against per-level re-swap).
 * ======================================================================== */
#define LH70_ENTRY_STRIDE 8u   /* s32 unk0 + u8 r,g,b,a */
#define LH70_HEADER_SIZE  0x18u

/* Session-long set of already-normalized blob pointers. gAssetsMiscSection is
 * loaded once and never freed, so raw pointer identity is stable and valid as a
 * dedup key. The distinct LevelHeader_70 blob count per session is tiny. */
#define LH70_SWAPPED_MAX 512
static const void *s_lh70Swapped[LH70_SWAPPED_MAX];
static uint32_t    s_lh70SwappedCount;

static int lh70_already_swapped(const void *blob) {
    uint32_t i;
    for (i = 0; i < s_lh70SwappedCount; i++) {
        if (s_lh70Swapped[i] == blob) {
            return 1;
        }
    }
    if (s_lh70SwappedCount < LH70_SWAPPED_MAX) {
        s_lh70Swapped[s_lh70SwappedCount++] = blob;
    }
    return 0;
}

void asset_swap_misc_lightdata(void *blob) {
    int32_t count;
    int32_t i;

    if (blob == NULL) {
        return;
    }
    if (lh70_already_swapped(blob)) {
        return; /* already normalized on an earlier level load */
    }

    /* header: 0x00..0x0C are s32 (0x00 is the entry count); 0x10/0x14 RGBA bytes */
    sw32(blob, 0x00);
    sw32(blob, 0x04);
    sw32(blob, 0x08);
    sw32(blob, 0x0C);
    /* 0x10, 0x14 ColourRGBA: bytes, no swap */

    count = rd32(blob, 0x00);
    if (count < 0 || count > 4096) {
        return; /* implausible — leave the rest untouched */
    }
    for (i = 0; i < count; i++) {
        uint32_t e = LH70_HEADER_SIZE + (uint32_t) i * LH70_ENTRY_STRIDE;
        sw32(blob, e + 0x00); /* LevelHeader_70_18.unk0 (s32) */
        /* +0x04 r,g,b,a bytes, no swap */
    }
}

/* ==========================================================================
 *  ASSET_MISC sub-asset: PulsatingLightData (LevelHeader.pulseLightData).
 *
 *  Same situation as LevelHeader_70 above: the record lives in the punted
 *  ASSET_MISC section and its type is only knowable at the get_misc_asset()
 *  call site (game.c level_load()). structs.h PulsatingLightData:
 *
 *      0x00 u16 numberFrames    0x02 u16 currentFrame
 *      0x04 u16 time            0x06 u16 totalTime
 *      0x08 s32 outColorValue
 *      0x0C PulsatingLightDataFrame frames[numberFrames]  (u16 value, u16 time)
 *
 *  Every field is multi-byte; nothing here is byte data. Left big-endian,
 *  init_pulsating_light_data() reads numberFrames as a byte-reversed u16 and
 *  loops that many times over `frames[]` — for us.v80 misc sub-asset 64
 *  (Spaceport Alpha / Star City) the true count is 4 but the reversed read is
 *  1024, so the totalTime accumulation runs 1020 entries past the end of a
 *  28-byte blob and `outColorValue` (the light colour fed to gDPSetPrimColor
 *  for RENDER_PULSING_LIGHTS batches) is derived from garbage.
 * ======================================================================== */
#define PULSE_HEADER_SIZE 0x0Cu
#define PULSE_FRAME_SIZE  4u

/* Same rationale as s_lh70Swapped: gAssetsMiscSection is loaded once and never
 * freed, but level_load() re-fetches the same offset on every level load. */
#define PULSE_SWAPPED_MAX 128
static const void *s_pulseSwapped[PULSE_SWAPPED_MAX];
static uint32_t    s_pulseSwappedCount;

void asset_swap_misc_pulsating(void *blob, uint32_t size) {
    uint32_t i, frames, capacity;

    if (blob == NULL || size < PULSE_HEADER_SIZE) {
        return;
    }
    for (i = 0; i < s_pulseSwappedCount; i++) {
        if (s_pulseSwapped[i] == blob) {
            return; /* already normalized on an earlier level load */
        }
    }
    if (s_pulseSwappedCount < PULSE_SWAPPED_MAX) {
        s_pulseSwapped[s_pulseSwappedCount++] = blob;
    }

    sw16(blob, 0x00); /* numberFrames */
    sw16(blob, 0x02); /* currentFrame */
    sw16(blob, 0x04); /* time         */
    sw16(blob, 0x06); /* totalTime    */
    sw32(blob, 0x08); /* outColorValue */

    frames   = (uint32_t) (uint16_t) rd16(blob, 0x00);
    capacity = (size - PULSE_HEADER_SIZE) / PULSE_FRAME_SIZE;
    if (frames > capacity) {
        frames = capacity; /* never walk past the blob */
    }
    for (i = 0; i < frames; i++) {
        uint32_t f = PULSE_HEADER_SIZE + i * PULSE_FRAME_SIZE;
        sw16(blob, f + 0); /* value */
        sw16(blob, f + 2); /* time  */
    }
}

/* ==========================================================================
 *  ASSET_MISC sub-asset 65: the magic-code (cheat) table.
 *
 *  A MIXED record: a u16 index block followed by raw ASCII string bytes, so
 *  neither "leave it alone" nor a blanket halfword swap is right — swapping the
 *  whole blob would byte-reverse every cheat string.
 *
 *  Layout (menu.c gCheatsAssetData):
 *      u16 [0]                     numberOfCheats
 *      u16 [1 .. numberOfCheats*3] three BYTE offsets per cheat (code text,
 *                                  description line 1, description line 2),
 *                                  each relative to the START of the blob
 *      bytes                       the NUL-terminated ASCII strings
 *
 *  The index block is therefore (1 + numberOfCheats*3) halfwords, and the first
 *  string offset must equal its byte length. us.v80: numberOfCheats == 29, so
 *  the block is 88 halfwords == 176 bytes, and offset[0] == 176 exactly — that
 *  identity is what pins the field map, and the swapper asserts it by refusing
 *  to run when it does not hold.
 *
 *  Must be called AFTER decrypt_magic_codes(): the count that bounds the index
 *  block is only meaningful once decrypted. Decryption permutes bits WITHIN
 *  each byte and never reorders bytes, so it commutes with this swap.
 * ======================================================================== */
void asset_swap_misc_magic_codes(void *blob, uint32_t size) {
    uint32_t count, indexHalfwords, indexBytes, firstOffset;

    if (blob == NULL || size < 2u) {
        return;
    }

    sw16(blob, 0x00);
    count = (uint32_t) (uint16_t) rd16(blob, 0x00);

    indexHalfwords = 1u + count * 3u;
    indexBytes     = indexHalfwords * 2u;
    if (count == 0u || indexBytes > size) {
        sw16(blob, 0x00); /* implausible — undo and leave the blob untouched */
        return;
    }

    /* Swap the index block, then check the self-describing invariant. */
    sw16_arr(blob, 0x02, count * 3u);
    firstOffset = (uint32_t) (uint16_t) rd16(blob, 0x02);
    if (firstOffset != indexBytes) {
        /* Field map does not hold for this ROM — revert rather than corrupt. */
        sw16_arr(blob, 0x02, count * 3u);
        sw16(blob, 0x00);
    }
    /* Everything from indexBytes on is ASCII: deliberately NOT swapped. */
}

/* ==========================================================================
 *  ASSET_MISC sub-asset 20: the boost / exhaust graphics table (Object_Boost[]).
 *
 *  Two independent defects made this decode to garbage before this converter
 *  existed (see docs/OPEN_ITEMS.md):
 *
 *   1. ASSET_MISC is punted by asset_swap_normalize (heterogeneous), so the
 *      record is still big-endian. A blanket 32-bit word swap
 *      (objects.c dkr_misc_swap_words) is WRONG here: it would scramble the
 *      packed s16 spriteId/textureId pair at 0x6C and the u8 quad at 0x70.
 *      Hence this per-field swizzle.
 *   2. The ON-DISK stride is 0x80, but sizeof(Object_Boost) on an LP64 host is
 *      0x88 — the two trailing runtime pointers (`Sprite *sprite`,
 *      `TextureHeader *tex`) are 4 bytes each on the N64 and 8 here. Indexing
 *      the raw blob as Object_Boost[] therefore walks off after entry 0
 *      regardless of endianness. Hence the host-layout copy: this writes into a
 *      caller-owned native array at the caller's (host) stride.
 *
 *  ON-DISK record, 0x80 bytes — CONFIRMED by hand-decoding the raw ROM bytes of
 *  all 10 entries (us.v80, misc word offset 178, 1280 bytes = 10 * 0x80):
 *
 *      0x00  Object_Boost_Inner carBoostData        9 * f32
 *      0x24  Object_Boost_Inner hovercraftBoostData 9 * f32
 *      0x48  Object_Boost_Inner flyingBoostData     9 * f32
 *              (Inner = Vec3f position; f32 unkC,unk10,unk14,unk18,unk1C,unk20)
 *      0x6C  s16 spriteId          entry0 = 0x002F (47)
 *      0x6E  s16 textureId         entry0 = 0x00CE (206)
 *      0x70  u8  unk70             runtime state; 0 in ROM
 *      0x71  u8  unk71             runtime state; 0 in ROM
 *      0x72  u8  unk72             runtime state; 0 in ROM
 *      0x73  s8  unk73             runtime state; 0 in ROM
 *      0x74  f32 unk74             runtime state; 0 in ROM
 *      0x78  u32 sprite            runtime host pointer; 0 in ROM
 *      0x7C  u32 tex               runtime host pointer; 0 in ROM
 *
 *  Entry 0 decoded: car pos (0, 24, 64) / 1.0 / 0.1 / 32 / 4 / 192 / 32;
 *  hovercraft pos (0, 64, 48); flying pos (0, 8, 100) — all plausible
 *  kart-relative exhaust offsets, which is what confirms the field map.
 *
 *  Everything below 0x78 has IDENTICAL host and on-disk offsets (every field is
 *  a naturally-aligned <=4-byte scalar and 0x78 is already 8-aligned, so the
 *  host adds no padding in the prefix). That is what lets this function stay
 *  struct-free per this file's contract; objects.c locks the host offsets with
 *  _Static_asserts.
 * ======================================================================== */
#define BOOST_ROM_STRIDE  0x80u
#define BOOST_ROM_PREFIX  0x78u /* bytes before the two runtime pointer fields */

uint32_t asset_swap_misc_boost(void *dst, uint32_t dstStride, uint32_t dstCapacity, const void *src,
                               uint32_t srcBytes) {
    uint32_t entries, i;

    if (dst == NULL || src == NULL || dstStride < BOOST_ROM_PREFIX) {
        return 0;
    }
    entries = srcBytes / BOOST_ROM_STRIDE;
    if (entries > dstCapacity) {
        entries = dstCapacity;
    }

    for (i = 0; i < entries; i++) {
        uint8_t *d = (uint8_t *) dst + (size_t) i * dstStride;
        const uint8_t *s = (const uint8_t *) src + (size_t) i * BOOST_ROM_STRIDE;
        uint32_t f;

        /* Host-layout copy: the 0x78-byte prefix is offset-identical, the
         * trailing host pointer fields are zeroed (they are 0 in ROM anyway and
         * are (re)populated by racerfx_alloc). */
        memcpy(d, s, BOOST_ROM_PREFIX);
        memset(d + BOOST_ROM_PREFIX, 0, dstStride - BOOST_ROM_PREFIX);

        /* 27 f32 across the three Object_Boost_Inner blocks: 0x00 .. 0x68. */
        for (f = 0; f < 27u; f++) {
            swf32(d, f * 4u);
        }
        sw16(d, 0x6Cu); /* spriteId  */
        sw16(d, 0x6Eu); /* textureId */
        /* 0x70..0x73: individual u8/s8 — deliberately NOT swapped. */
        swf32(d, 0x74u); /* unk74 */
    }
    return entries;
}

/* ==========================================================================
 *  ASSET_LEVEL_OBJECT_MAPS  (PARTIAL: file header + per-entry x/y/z)
 *  Layout: levelObjectMap.hpp header (16B) + level_object_entries.h entries.
 * ======================================================================== */
static void swap_level_object_map(void *data, uint32_t size) {
    uint32_t fileSize, pos, endEntries;

    if (size < 0x10u) {
        return;
    }
    sw32(data, 0x00); /* header.fileSize (u32) */
    fileSize = (uint32_t) rd32(data, 0x00);

    /* Entries begin after the 16-byte header. The game walks them by the
     * per-entry size byte (entry[1] & 0x3F) for `fileSize` bytes. */
    endEntries = 0x10u + fileSize;
    if (endEntries > size) {
        endEntries = size;
    }
    pos = 0x10u;
    while (pos + 8u <= endEntries) {
        uint32_t entrySize = (uint32_t) (rd8(data, pos + 1) & 0x3F);
        if (entrySize == 0) {
            break;
        }
        /* LevelObjectEntryCommon: u8 objectID, u8 size, s16 x, s16 y, s16 z */
        sw16(data, pos + 2); /* x */
        sw16(data, pos + 4); /* y */
        sw16(data, pos + 6); /* z */
        /* Per-behavior body params (>= +0x08) PUNTED — see notes. */
        pos += entrySize;
    }
}

/* ==========================================================================
 *  Dispatch
 * ======================================================================== */
void asset_swap_lut(void *data, uint32_t size) {
    if (data == NULL || size < 4u) {
        return;
    }
    sw32_arr(data, 0, size / 4u);
}

void asset_swap_normalize(int assetType, void *data, uint32_t size) {
    if (data == NULL || size == 0u) {
        return;
    }

    switch (assetType) {
        /* ---- u32 offset tables (loaded wholesale via asset_table_load) ---- */
        case ASSET_AI_BEHAVIOUR_TABLE:
        case ASSET_TEXTURES_3D_TABLE:
        case ASSET_TEXTURES_2D_TABLE:
        case ASSET_GAME_TEXT_TABLE:
        case ASSET_MENU_TEXT_TABLE:
        case ASSET_SCREENS_TABLE:
        case ASSET_SPRITES_TABLE:
        case ASSET_MISC_TABLE:
        case ASSET_LEVEL_OBJECT_MAPS_TABLE:
        case ASSET_LEVEL_HEADERS_TABLE:
        case ASSET_LEVEL_NAMES_TABLE:
        case ASSET_LEVEL_MODELS_TABLE:
        case ASSET_OBJECT_MODELS_TABLE:
        case ASSET_OBJECT_ANIMATIONS_TABLE:
        case ASSET_OBJECT_HEADERS_TABLE:
        case ASSET_AUDIO_TABLE:
        case ASSET_PARTICLES_TABLE:
        case ASSET_PARTICLE_BEHAVIORS_TABLE:
        case ASSET_WEATHER_PARTICLES: /* loaded as an s32 table (gWeatherAssetTable) */
            asset_swap_lut(data, size);
            break;

        /* NOT a u32 offset table — {u8,u8,pad2,s32} records. See swapper. */
        case ASSET_TTGHOSTS_TABLE:
            swap_tt_ghost_table(data, size);
            break;

        /* ---- s16 id lists ---- */
        case ASSET_ANIMATION_IDS:
        case ASSET_HUD_ELEMENT_IDS:
        case ASSET_MENU_ELEMENT_IDS:
        case ASSET_DUMMY_PARTICLE_IDS:
        case ASSET_LEVEL_OBJECT_TRANSLATION_TABLE:
            sw16_arr(data, 0, size / 2u);
            break;

        /* ---- structured, single-record or arrayed ---- */
        case ASSET_LEVEL_HEADERS:
            swap_level_header(data, size);
            break;
        case ASSET_OBJECT_MODELS:
            swap_object_model(data, size);
            break;
        case ASSET_LEVEL_MODELS:
            swap_level_model(data, size);
            break;
        case ASSET_OBJECTS:
            swap_object_header(data, size);
            break;
        case ASSET_TEXTURES_2D:
        case ASSET_TEXTURES_3D:
            swap_texture(data, size);
            break;
        case ASSET_SPRITES:
            swap_sprite(data, size);
            break;
        case ASSET_FONTS:
            swap_fonts(data, size);
            break;
        case ASSET_TTGHOSTS:
            swap_tt_ghost(data, size);
            break;
        case ASSET_PARTICLES:
            swap_particles(data, size);
            break;
        case ASSET_PARTICLE_BEHAVIORS:
            swap_particle_behaviours(data, size);
            break;
        case ASSET_AI_BEHAVIOUR:
            swap_ai_behaviour(data, size);
            break;
        case ASSET_OBJECT_ANIMATIONS:
            /* The generic path lacks the owning model's numberOfAnimatedVertices,
             * which the blob does not carry, so it can only normalize the leading
             * keyframeCount. The integration hook MUST instead call
             * asset_swap_object_animation(data, size, model->numberOfAnimatedVertices)
             * (from object_models.c, right after gzip_inflate) to swap the full
             * body. Do not call both — keyframeCount would double-swap. */
            if (size >= 4u) {
                sw32(data, 0x00);
            }
            break;
        case ASSET_LEVEL_OBJECT_MAPS:
            swap_level_object_map(data, size);
            break;

        /* ---- deliberately NO byteswap (byte-order-defined / string data) ---- */
        case ASSET_GAME_TEXT:    /* textbox/dialog command byte stream          */
        case ASSET_MENU_TEXT:    /* ASCII strings                               */
        case ASSET_LEVEL_NAMES:  /* ASCII strings                               */
        case ASSET_SCREENS:      /* 16-byte header + RGBA16 texels (unused)     */
        case ASSET_EMPTY_14:     /* CI4/CI8 TLUT palette texels                 */
            break;

        /* ---- punted (see docs/asset_swap_notes.md) ---- */
        case ASSET_AUDIO:              /* ALBankFile/ALSeqFile (libultra-owned) */
        case ASSET_MISC:               /* heterogeneous sub-assets              */
        case ASSET_JAPANESE_FONTS:     /* not used by us_v80 (REGION != JP)     */
        case ASSET_JAPANESE_FONTS_TABLE:
        default:
            break;
    }
}
