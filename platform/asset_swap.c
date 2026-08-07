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

/* VehicleSoundAsset on-disk layout: 0x4C bytes. ASSET_AUDIO is heterogeneous,
 * so this typed sub-record is normalized at its only raw-load call site. */
#define VEHICLE_SOUND_RECORD_SIZE 0x4Cu

int asset_swap_vehicle_sound(void *data, uint32_t size) {
    if (data == NULL || size < VEHICLE_SOUND_RECORD_SIZE) {
        return 0;
    }

    sw16_arr(data, 0x00, 2);  /* soundId[2] */
    sw16_arr(data, 0x18, 10); /* pitchLevels[2][5] */
    sw16_arr(data, 0x3C, 7);  /* pitch scale/velocity s16 fields */
    return 1;
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
/* ObjectModel header field offsets. Names follow structs.h / objectModel.hpp.
 * `OM_UNIDENTIFIED_*` are fields neither the decomp nor this port has ever
 * identified — they are named by offset so the swap reads as a field list
 * without asserting a meaning nobody has established. */
#define OM_TEXTURES                    0x00u
#define OM_VERTICES                    0x04u
#define OM_TRIANGLES                   0x08u
#define OM_RESERVED_C                  0x0Cu
#define OM_RESERVED_10                 0x10u
#define OM_ATTACH_POINTS               0x14u
#define OM_NUM_ATTACH_POINTS           0x18u
#define OM_UNIDENTIFIED_1A             0x1Au /* u16, unidentified */
#define OM_COLLISION_SPHERES           0x1Cu
#define OM_COLLISION_SPHERES_SIZE      0x20u
#define OM_NUM_TEXTURES                0x22u
#define OM_NUM_VERTICES                0x24u
#define OM_NUM_TRIANGLES               0x26u
#define OM_NUM_BATCHES                 0x28u
#define OM_FILE_SIZE                   0x2Cu
#define OM_RESERVED_REFERENCES         0x30u
#define OM_RESERVED_32                 0x32u
#define OM_BATCHES                     0x38u
#define OM_UNIDENTIFIED_3C             0x3Cu /* f32, unidentified */
#define OM_RESERVED_40                 0x40u
#define OM_RESERVED_ANIMATIONS         0x44u
#define OM_RESERVED_NUM_ANIMATIONS     0x48u
#define OM_NUM_ANIMATED_VERTICES       0x4Au
#define OM_ANIMATED_VERTEX_INDICES     0x4Cu
#define OM_HAS_ANIMATED_TEXTURE        0x50u
#define OM_RESERVED_52                 0x52u
#define OM_UNUSED_54                   0x54u

static void swap_object_model(void *data, uint32_t size) {
    int32_t offTex, offVtx, offTri, offBat, offAttach, offSpheres, offAnimIdx;
    int16_t nTex, nVtx, nTri, nBat, nAttach, sphSize;

    if (size < 0x58u) {
        return;
    }

    /* --- header (0x00 .. 0x54) --- */
    sw32(data, OM_TEXTURES);
    sw32(data, OM_VERTICES);
    sw32(data, OM_TRIANGLES);
    sw32(data, OM_RESERVED_C);
    sw32(data, OM_RESERVED_10);
    sw32(data, OM_ATTACH_POINTS);
    sw16(data, OM_NUM_ATTACH_POINTS);
    sw16(data, OM_UNIDENTIFIED_1A);
    sw32(data, OM_COLLISION_SPHERES);
    sw16(data, OM_COLLISION_SPHERES_SIZE);
    sw16(data, OM_NUM_TEXTURES);
    sw16(data, OM_NUM_VERTICES);
    sw16(data, OM_NUM_TRIANGLES);
    sw16(data, OM_NUM_BATCHES);
    /* 0x2A,0x2B unidentified : bytes */
    sw32(data, OM_FILE_SIZE);
    sw16(data, OM_RESERVED_REFERENCES);
    sw16(data, OM_RESERVED_32);
    /* 0x34..0x37 : bytes */
    sw32(data, OM_BATCHES);
    swf32(data, OM_UNIDENTIFIED_3C);
    sw32(data, OM_RESERVED_40);
    sw32(data, OM_RESERVED_ANIMATIONS);
    sw16(data, OM_RESERVED_NUM_ANIMATIONS);
    sw16(data, OM_NUM_ANIMATED_VERTICES);
    sw32(data, OM_ANIMATED_VERTEX_INDICES);
    sw16(data, OM_HAS_ANIMATED_TEXTURE);
    sw16(data, OM_RESERVED_52);
    sw32(data, OM_UNUSED_54);

    /* --- read now-native offsets + counts --- */
    offTex     = rd32(data, OM_TEXTURES);
    offVtx     = rd32(data, OM_VERTICES);
    offTri     = rd32(data, OM_TRIANGLES);
    offAttach  = rd32(data, OM_ATTACH_POINTS);
    offSpheres = rd32(data, OM_COLLISION_SPHERES);
    offBat     = rd32(data, OM_BATCHES);
    offAnimIdx = rd32(data, OM_ANIMATED_VERTEX_INDICES);
    nAttach    = rd16(data, OM_NUM_ATTACH_POINTS);
    sphSize    = rd16(data, OM_COLLISION_SPHERES_SIZE);
    nTex       = rd16(data, OM_NUM_TEXTURES);
    nVtx       = rd16(data, OM_NUM_VERTICES);
    nTri       = rd16(data, OM_NUM_TRIANGLES);
    nBat       = rd16(data, OM_NUM_BATCHES);

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

/* LevelModel header offsets (structs.h LevelModel). `LM_UNIDENTIFIED_*` /
 * `LMSEG_UNIDENTIFIED_*` are unidentified in the decomp; where this port has
 * observed how a field is consumed, that observation is in the trailing
 * comment and the name still records only what is actually known. */
#define LM_TEXTURES                 0x00u
#define LM_SEGMENTS                 0x04u
#define LM_SEGMENT_BOUNDING_BOXES   0x08u
#define LM_UNIDENTIFIED_0C          0x0Cu /* s32 offset, target unidentified */
#define LM_SEGMENT_BITFIELDS        0x10u
#define LM_SEGMENT_BSP_TREE         0x14u
#define LM_NUM_TEXTURES             0x18u
#define LM_NUM_SEGMENTS             0x1Au
#define LM_UNIDENTIFIED_1C          0x1Cu /* s16, unidentified */
#define LM_NUM_ANIMATED_TEXTURES    0x1Eu
#define LM_MINIMAP_SPRITE_INDEX     0x20u
#define LM_MINIMAP_ROTATION         0x24u
#define LM_UNIDENTIFIED_26          0x26u /* s16, unidentified */
#define LM_MINIMAP_X_SCALE          0x28u
#define LM_MINIMAP_Y_SCALE          0x2Cu
#define LM_MINIMAP_OFFSET_X_ADV1    0x30u
#define LM_MINIMAP_OFFSET_Y_ADV1    0x32u
#define LM_MINIMAP_OFFSET_X_ADV2    0x34u
#define LM_MINIMAP_OFFSET_Y_ADV2    0x36u
#define LM_MINIMAP_COLOR            0x38u
#define LM_LOWER_X_BOUNDS           0x3Cu
#define LM_UPPER_X_BOUNDS           0x3Eu
#define LM_LOWER_Y_BOUNDS           0x40u
#define LM_UPPER_Y_BOUNDS           0x42u
#define LM_LOWER_Z_BOUNDS           0x44u
#define LM_UPPER_Z_BOUNDS           0x46u
#define LM_MODEL_SIZE               0x48u

/* LevelModelSegment offsets, relative to the segment base. */
#define LMSEG_VERTICES              0x00u
#define LMSEG_TRIANGLES             0x04u
#define LMSEG_UNIDENTIFIED_08       0x08u /* s32; read as a bitfield/value elsewhere */
#define LMSEG_BATCHES               0x0Cu
#define LMSEG_COLLISION_FACETS      0x14u
#define LMSEG_NUM_VERTICES          0x1Cu
#define LMSEG_NUM_TRIANGLES         0x1Eu
#define LMSEG_NUM_BATCHES           0x20u
#define LMSEG_BITFIELD_INDEX        0x28u /* indexes LM_SEGMENT_BITFIELDS */
#define LMSEG_UNIDENTIFIED_3C       0x3Cu /* s32 bitfield; only ever read as `& 2` */

static void swap_level_model(void *data, uint32_t size) {
    int32_t offTex, offSeg;
    int16_t nTex, nSeg;
    int32_t k;

    if (size < 0x4Cu) {
        return;
    }

    /* --- LevelModel header (0x00 .. 0x48) --- */
    sw32(data, LM_TEXTURES);
    sw32(data, LM_SEGMENTS);
    sw32(data, LM_SEGMENT_BOUNDING_BOXES);
    sw32(data, LM_UNIDENTIFIED_0C);
    sw32(data, LM_SEGMENT_BITFIELDS);
    sw32(data, LM_SEGMENT_BSP_TREE);
    sw16(data, LM_NUM_TEXTURES);
    sw16(data, LM_NUM_SEGMENTS);
    sw16(data, LM_UNIDENTIFIED_1C);
    sw16(data, LM_NUM_ANIMATED_TEXTURES);
    sw32(data, LM_MINIMAP_SPRITE_INDEX);
    sw16(data, LM_MINIMAP_ROTATION);
    sw16(data, LM_UNIDENTIFIED_26);
    swf32(data, LM_MINIMAP_X_SCALE);
    swf32(data, LM_MINIMAP_Y_SCALE);
    sw16(data, LM_MINIMAP_OFFSET_X_ADV1);
    sw16(data, LM_MINIMAP_OFFSET_Y_ADV1);
    sw16(data, LM_MINIMAP_OFFSET_X_ADV2);
    sw16(data, LM_MINIMAP_OFFSET_Y_ADV2);
    sw32(data, LM_MINIMAP_COLOR);
    sw16(data, LM_LOWER_X_BOUNDS);
    sw16(data, LM_UPPER_X_BOUNDS);
    sw16(data, LM_LOWER_Y_BOUNDS);
    sw16(data, LM_UPPER_Y_BOUNDS);
    sw16(data, LM_LOWER_Z_BOUNDS);
    sw16(data, LM_UPPER_Z_BOUNDS);
    sw32(data, LM_MODEL_SIZE);

    offTex = rd32(data, LM_TEXTURES);
    offSeg = rd32(data, LM_SEGMENTS);
    nTex   = rd16(data, LM_NUM_TEXTURES);
    nSeg   = rd16(data, LM_NUM_SEGMENTS);

    /* segmentsBoundingBoxes: LevelModelSegmentBoundingBox = 6 x s16 each. */
    {
        int32_t offBox = rd32(data, LM_SEGMENT_BOUNDING_BOXES);
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
            sw32(data, s + LMSEG_VERTICES);
            sw32(data, s + LMSEG_TRIANGLES);
            sw32(data, s + LMSEG_UNIDENTIFIED_08);
            sw32(data, s + LMSEG_BATCHES);
            sw32(data, s + LMSEG_COLLISION_FACETS);
            sw16(data, s + LMSEG_NUM_VERTICES);
            sw16(data, s + LMSEG_NUM_TRIANGLES);
            sw16(data, s + LMSEG_NUM_BATCHES);
            sw16(data, s + LMSEG_BITFIELD_INDEX);
            sw32(data, s + LMSEG_UNIDENTIFIED_3C);

            sVtx  = rd32(data, s + LMSEG_VERTICES);
            sTri  = rd32(data, s + LMSEG_TRIANGLES);
            sBat  = rd32(data, s + LMSEG_BATCHES);
            sNVtx = rd16(data, s + LMSEG_NUM_VERTICES);
            sNTri = rd16(data, s + LMSEG_NUM_TRIANGLES);
            sNBat = rd16(data, s + LMSEG_NUM_BATCHES);

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
                int32_t sCF = rd32(data, s + LMSEG_COLLISION_FACETS); /* already byteswapped above */
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
/* LevelHeader offsets (structs.h LevelHeader). `LH_UNIDENTIFIED_*` are fields
 * the decomp still calls unkNN; where levelHeader.hpp offers a candidate name
 * that this port has not confirmed against a reader, it is recorded in the
 * trailing comment rather than adopted into the macro name. */
#define LH_COURSE_HEIGHT        0x08u
#define LH_LAPS                 0x4Bu /* plain s8, no swap */
#define LH_GEOMETRY             0x34u
#define LH_COLLECTABLES         0x36u
#define LH_SKYBOX               0x38u
#define LH_FOG_NEAR             0x3Au
#define LH_FOG_FAR              0x3Cu
#define LH_FOG_R                0x3Eu
#define LH_FOG_G                0x40u
#define LH_FOG_B                0x42u
#define LH_INSTRUMENTS          0x54u
#define LH_WAVE_SINE_HEIGHT0    0x5Au
#define LH_WAVE_SINE_HEIGHT1    0x5Eu
#define LH_WAVE_SEED_SIZE       0x60u
#define LH_WAVE_POWER           0x62u
#define LH_UNIDENTIFIED_64      0x64u /* s16, unidentified (wave block) */
#define LH_UNIDENTIFIED_66      0x66u /* s16, unidentified (wave block) */
#define LH_WAVE_TEX_ID          0x68u
#define LH_WAVE_VIEW_DIST       0x6Eu
#define LH_MISC_ASSETS          0x74u /* s32[7], decomp unk74[7] */
#define LH_MISC_ASSETS_COUNT    7u
#define LH_WEATHER_ENABLE       0x90u
#define LH_WEATHER_TYPE         0x92u
#define LH_WEATHER_VEL_X        0x96u
#define LH_WEATHER_VEL_Y        0x98u
#define LH_WEATHER_VEL_Z        0x9Au
#define LH_UNIDENTIFIED_A4      0xA4u /* u32; levelHeader.hpp calls it specialSkyTexture */
#define LH_UNIDENTIFIED_A8      0xA8u /* s16, unidentified */
#define LH_UNIDENTIFIED_AA      0xAAu /* s16, unidentified */
#define LH_PULSE_LIGHT_DATA     0xACu /* s32 offset, or -1 */
#define LH_UNIDENTIFIED_B0      0xB0u /* s16, unidentified */
#define LH_UNIDENTIFIED_BA      0xBAu /* s16; levelHeader.hpp calls it objectMap2 */
#define LH_TRAILING_C4          0xC4u /* u32 present on disk; the game never reads it */

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
        if (sForcedLaps > 0 && size > LH_LAPS) {
            ((int8_t *) data)[LH_LAPS] = (int8_t) sForcedLaps;
        }
    }

    swf32(data, LH_COURSE_HEIGHT);

    sw16(data, LH_GEOMETRY);
    sw16(data, LH_COLLECTABLES);
    sw16(data, LH_SKYBOX);
    sw16(data, LH_FOG_NEAR);
    sw16(data, LH_FOG_FAR);
    sw16(data, LH_FOG_R);
    sw16(data, LH_FOG_G);
    sw16(data, LH_FOG_B);

    sw16(data, LH_INSTRUMENTS);

    /* wave parameters (s16 values interleaved with u8 fields) */
    sw16(data, LH_WAVE_SINE_HEIGHT0);
    sw16(data, LH_WAVE_SINE_HEIGHT1);
    sw16(data, LH_WAVE_SEED_SIZE);
    sw16(data, LH_WAVE_POWER);
    sw16(data, LH_UNIDENTIFIED_64);
    sw16(data, LH_UNIDENTIFIED_66);
    sw16(data, LH_WAVE_TEX_ID);
    sw16(data, LH_WAVE_VIEW_DIST);

    /* misc-asset indices, patched via get_misc_asset() at runtime. */
    sw32_arr(data, LH_MISC_ASSETS, LH_MISC_ASSETS_COUNT);

    /* weather block */
    sw16(data, LH_WEATHER_ENABLE);
    sw16(data, LH_WEATHER_TYPE);
    /* 0x94 intensity, 0x95 opacity : bytes */
    sw16(data, LH_WEATHER_VEL_X);
    sw16(data, LH_WEATHER_VEL_Y);
    sw16(data, LH_WEATHER_VEL_Z);

    /* 0x9C cameraFOV, 0x9D..0x9F bgColor, 0xA0/0xA1 unidentified : bytes
     * (NOTE: levelHeader.hpp calls 0xA0 a single be_int16; structs.h — which
     * the game reads — splits it into two u8 fields, so no swap here). */
    sw32(data, LH_UNIDENTIFIED_A4);
    sw16(data, LH_UNIDENTIFIED_A8);
    sw16(data, LH_UNIDENTIFIED_AA);
    sw32(data, LH_PULSE_LIGHT_DATA);
    sw16(data, LH_UNIDENTIFIED_B0);
    /* 0xB2..0xB9 : bytes (voidColour etc.) */
    sw16(data, LH_UNIDENTIFIED_BA);
    /* 0xBC..0xC3 : bytes (gradient bg colours) */
    /* structs.h stops at 0xC4 and the game never reads the trailing word; swap
     * it only if the record actually carries it. */
    if (size >= 0xC8u) {
        sw32(data, LH_TRAILING_C4);
    }
}

/* ==========================================================================
 *  ASSET_OBJECTS  (ObjectHeader, one 0x78 record + arrays it points at)
 *  Layout: structs.h ObjectHeader (== objectHeader.hpp).
 * ======================================================================== */
/* ObjectHeader offsets (structs.h ObjectHeader == objectHeader.hpp).
 * `OH_UNIDENTIFIED_*` / `OH24_UNIDENTIFIED_*` are unidentified in the decomp. */
#define OH_UNIDENTIFIED_00      0x00u /* s32, unidentified */
#define OH_SHADOW_SCALE         0x04u
#define OH_UNIDENTIFIED_08      0x08u /* f32, unidentified */
#define OH_SCALE                0x0Cu
#define OH_MODEL_IDS            0x10u
#define OH_VEHICLE_PART_IDS     0x14u
#define OH_VEHICLE_PART_INDICES 0x18u
#define OH_OBJECT_PARTICLES     0x1Cu
#define OH_PAD_20               0x20u
#define OH_HEADER24             0x24u /* s32 offset -> ObjectHeader24 */
#define OH_SHADE_AMBIENT        0x28u
#define OH_SHADE_DIFFUSE        0x2Cu
#define OH_FLAGS                0x30u
#define OH_SHADOW_GROUP         0x32u
#define OH_UNIDENTIFIED_34      0x34u /* s16, unidentified */
#define OH_WATER_EFFECT_GROUP   0x36u
#define OH_UNIDENTIFIED_38      0x38u /* s16, unidentified */
#define OH_SHADE_ANGLE_Y        0x3Eu
#define OH_SHADE_ANGLE_Z        0x40u
#define OH_UNIDENTIFIED_42      0x42u /* s16, unidentified */
#define OH_UNIDENTIFIED_44      0x44u /* s16, unidentified */
#define OH_UNIDENTIFIED_46      0x46u /* s16, unidentified */
#define OH_SHADOW_REGION        0x48u /* shadowBottom/Top region s16 */
#define OH_UNIDENTIFIED_4A      0x4Au /* s16, unidentified */
#define OH_UNIDENTIFIED_4C      0x4Cu /* s16, unidentified */
#define OH_DRAW_DISTANCE        0x4Eu
#define OH_UNIDENTIFIED_50      0x50u /* s16; objects.c:3530 scales it by the
                                       * object's scale into Object::unk34, which
                                       * the frustum test adds as a bounding
                                       * radius -- so this is very likely the
                                       * model's cull radius, but the decomp has
                                       * not named it and nothing else reads it. */
#define OH_NUM_MODEL_IDS        0x55u /* u8 */
#define OH_NUM_VEHICLE_PARTS    0x56u /* u8 (attachPointCount) */
#define OH_NUM_PARTICLES        0x57u /* u8 */
#define OH_NUM_LIGHT_SOURCES    0x5Au /* s8 (structs.h ObjectHeader) */

/* ObjectHeader24 (0x18 bytes), relative to the OH_HEADER24 target. */
#define OH24_UNIDENTIFIED_06    0x06u /* u16, unidentified */
#define OH24_UNIDENTIFIED_08    0x08u /* u32 union arm, unidentified */
#define OH24_HOME_X             0x0Cu
#define OH24_HOME_Y             0x0Eu
#define OH24_HOME_Z             0x10u
#define OH24_RADIUS             0x12u
#define OH24_UNIDENTIFIED_14    0x14u /* u16, unidentified */
#define OH24_UNIDENTIFIED_16    0x16u /* u16, unidentified */
#define OH24_SIZE               0x18u

static void swap_object_header(void *data, uint32_t size) {
    int32_t offModelIds, offVehParts, offParticles, offHeader24;
    uint8_t nModelIds, nVehParts, nParticles;

    if (size < 0x78u) {
        return;
    }

    sw32(data, OH_UNIDENTIFIED_00);
    swf32(data, OH_SHADOW_SCALE);
    swf32(data, OH_UNIDENTIFIED_08);
    swf32(data, OH_SCALE);
    sw32(data, OH_MODEL_IDS);
    sw32(data, OH_VEHICLE_PART_IDS);
    sw32(data, OH_VEHICLE_PART_INDICES);
    sw32(data, OH_OBJECT_PARTICLES);
    sw32(data, OH_PAD_20);
    sw32(data, OH_HEADER24);
    swf32(data, OH_SHADE_AMBIENT);
    swf32(data, OH_SHADE_DIFFUSE);
    sw16(data, OH_FLAGS);
    sw16(data, OH_SHADOW_GROUP);
    sw16(data, OH_UNIDENTIFIED_34);
    sw16(data, OH_WATER_EFFECT_GROUP);
    sw16(data, OH_UNIDENTIFIED_38);
    /* 0x3A..0x3D : bytes */
    sw16(data, OH_SHADE_ANGLE_Y);
    sw16(data, OH_SHADE_ANGLE_Z);
    sw16(data, OH_UNIDENTIFIED_42);
    sw16(data, OH_UNIDENTIFIED_44);
    sw16(data, OH_UNIDENTIFIED_46);
    sw16(data, OH_SHADOW_REGION);
    sw16(data, OH_UNIDENTIFIED_4A);
    sw16(data, OH_UNIDENTIFIED_4C);
    sw16(data, OH_DRAW_DISTANCE);
    sw16(data, OH_UNIDENTIFIED_50);
    /* 0x52..0x5F : bytes (counts, type, name-adjacent) */
    /* 0x60..0x6F internalName[16] : bytes */
    /* 0x70..0x77 : bytes */

    offModelIds  = rd32(data, OH_MODEL_IDS);
    offVehParts  = rd32(data, OH_VEHICLE_PART_IDS);
    offParticles = rd32(data, OH_OBJECT_PARTICLES);
    offHeader24  = rd32(data, OH_HEADER24);
    nModelIds    = rd8(data, OH_NUM_MODEL_IDS);
    nVehParts    = rd8(data, OH_NUM_VEHICLE_PARTS);
    nParticles   = rd8(data, OH_NUM_PARTICLES);

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
    /* OH_HEADER24 -> ObjectHeader24[numLightSources] (0x18 bytes each).
     *
     * light_setup_light_sources() (objects.c) indexes this as an ARRAY:
     * DKR_PTR(ObjectHeader24, header->unk24)[i] for i < header->numLightSources,
     * and light_add_from_object_header() (lights.c) reads homeX/homeY/homeZ,
     * radius, unk14, unk16 and unk6 out of each record. Normalizing only record
     * 0 would leave records 1..n-1 big-endian.
     *
     * Inert on retail: every one of the 304 us.v80 object headers declares
     * numLightSources == 0 (asserted by tests/check_asset_swap_invariants.py),
     * so today `n` is 0 and this loop does not execute at all -- the swap for
     * record 0 that used to run unconditionally was itself already gated off by
     * in_bounds(), because no retail header's unk24 points at an in-bounds 0x18
     * record. This is future-proofing for a ROM or asset edit that populates the
     * array, and it keeps the pinned zero-lights gate green either way. */
    if (offHeader24 != 0) {
        int32_t nLights = (int32_t) (int8_t) rd8(data, OH_NUM_LIGHT_SOURCES);
        int32_t li;
        for (li = 0; li < nLights; li++) {
            uint32_t u = (uint32_t) offHeader24 + (uint32_t) li * OH24_SIZE;
            if (!in_bounds(u, OH24_SIZE, size)) {
                break;
            }
            sw16(data, u + OH24_UNIDENTIFIED_06);
            sw32(data, u + OH24_UNIDENTIFIED_08);
            sw16(data, u + OH24_HOME_X);
            sw16(data, u + OH24_HOME_Y);
            sw16(data, u + OH24_HOME_Z);
            sw16(data, u + OH24_RADIUS);
            sw16(data, u + OH24_UNIDENTIFIED_14);
            sw16(data, u + OH24_UNIDENTIFIED_16);
        }
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
/* SpriteAsset head offsets (structs.h SpriteAsset == sprite.hpp SpriteHeader).
 * The decomp names 0x04/0x06/0x08 unk4/unk6/unk8; structs.h resolves the first
 * two as the sprite anchor and the third as an unused field (0 in ROM). */
#define SPR_BASE_TEXTURE_ID 0x00u
#define SPR_NUM_FRAMES      0x02u
#define SPR_ANCHOR_X        0x04u
#define SPR_ANCHOR_Y        0x06u
#define SPR_UNUSED_FIELD    0x08u

static void swap_sprite(void *data, uint32_t size) {
    if (size < 0x0Cu) {
        return;
    }
    sw16(data, SPR_BASE_TEXTURE_ID);
    sw16(data, SPR_NUM_FRAMES);
    sw16(data, SPR_ANCHOR_X);
    sw16(data, SPR_ANCHOR_Y);
    sw32(data, SPR_UNUSED_FIELD);
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
        /* lifeTimeRange, aliased by the line/point particles' packed bit
         * fields. Swapping restores this halfword's numeric VALUE, which is all
         * a swapper can do: the sub-fields inside it are at N64 bit POSITIONS
         * and must be extracted with the PARTICLE_DESC_* accessors in
         * game/src/particles.h, never through host C bitfield members. */
        sw16(data, p + 0x0A);
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
/* AIBehaviourTable: two leading f32 the decomp still calls unk0/unk4, then a
 * 4x4 byte percentage table. Neither f32 has been identified. */
#define AIB_UNIDENTIFIED_00 0x00u /* f32, unidentified */
#define AIB_UNIDENTIFIED_04 0x04u /* f32, unidentified */

static void swap_ai_behaviour(void *data, uint32_t size) {
    uint32_t count = size / AI_BEHAVIOUR_SIZE;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t a = i * AI_BEHAVIOUR_SIZE;
        swf32(data, a + AIB_UNIDENTIFIED_00);
        swf32(data, a + AIB_UNIDENTIFIED_04);
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
 *  reset_colour_cycle (func_8007F1E8) call sites, with a dedup guard against per-level re-swap).
 * ======================================================================== */
#define LH70_ENTRY_STRIDE 8u   /* one unidentified s32 + u8 r,g,b,a */
#define LH70_HEADER_SIZE  0x18u
/* LevelHeader_70 header: 0x00 is the entry count, 0x04..0x0C are further s32
 * the decomp has not identified; 0x10/0x14 are RGBA byte quads. */
#define LH70_ENTRY_COUNT       0x00u
#define LH70_UNIDENTIFIED_04   0x04u /* s32, unidentified */
#define LH70_UNIDENTIFIED_08   0x08u /* s32, unidentified */
#define LH70_UNIDENTIFIED_0C   0x0Cu /* s32, unidentified */
/* Per-entry (LevelHeader_70_18), relative to the entry base. */
#define LH70E_UNIDENTIFIED_00  0x00u /* s32, decomp LevelHeader_70_18.unk0 */

/* Session-long set of already-normalized blob pointers. Raw pointer identity is
 * a valid dedup key only for as long as the MISC section the blobs live in is
 * the same allocation: if ASSET_MISC were ever reloaded it would very likely
 * land at the same arena address (boot allocation order is deterministic), and a
 * memo built against the old section would then suppress the swap of a fresh
 * big-endian record. game/src/objects.c dkr_misc_swap_claim() guards its own memo
 * exactly this way; misc_memo_check_section() below gives these two the same guard.
 *
 * The tables also fail CLOSED on overflow — a full table reports "already
 * swapped" (skip) rather than "not yet swapped" (re-swap), because re-swapping a
 * record puts it back into big-endian, which is strictly worse than leaving one
 * un-normalized record alone. */
#define LH70_SWAPPED_MAX 512
static const void *s_lh70Swapped[LH70_SWAPPED_MAX];
static uint32_t    s_lh70SwappedCount;

/* The PulsatingLightData memo (defined with its swapper further down) shares the
 * same section-identity guard. */
#define PULSE_SWAPPED_MAX 128
static const void *s_pulseSwapped[PULSE_SWAPPED_MAX];
static uint32_t    s_pulseSwappedCount;

/* Weak so the standalone unit-test binaries that link asset_swap.c without
 * game/src/objects.c still resolve it (they never load a MISC section, so the
 * guard below is a no-op there). The game binary's strong definition wins. */
__attribute__((weak)) int32_t *gAssetsMiscSection;
static const void *s_miscMemoSection;
static int s_miscMemoSectionSeen;

/* Drop both memos if the MISC section they were built against has been replaced. */
static void misc_memo_check_section(void) {
    const void *section = (const void *) gAssetsMiscSection;

    if (!s_miscMemoSectionSeen || s_miscMemoSection != section) {
        s_miscMemoSectionSeen = 1;
        s_miscMemoSection = section;
        s_lh70SwappedCount = 0;
        s_pulseSwappedCount = 0;
    }
}

/* Returns 1 if `blob` has already been normalized (or cannot be recorded, in
 * which case skipping is the safe direction), 0 if the caller should swap it. */
static int misc_memo_claim(const void **table, uint32_t *count, uint32_t max,
                           const void *blob) {
    uint32_t i;

    misc_memo_check_section();
    for (i = 0; i < *count; i++) {
        if (table[i] == blob) {
            return 1;
        }
    }
    if (*count >= max) {
        return 1; /* table full: fail closed — never swap the same record twice */
    }
    table[(*count)++] = blob;
    return 0;
}

static int lh70_already_swapped(const void *blob) {
    return misc_memo_claim(s_lh70Swapped, &s_lh70SwappedCount, LH70_SWAPPED_MAX, blob);
}

void asset_swap_misc_lightdata(void *blob) {

/* The bounded core, shared by both views of this blob shape.
 *
 * ASSET_MISC sub-asset 58 is read as a LevelHeader_70 by game_ui.c AND as a
 * ColorLoopEntry[] by particles.c (ParticleBehaviour.colourLoop), and 59/60 are
 * read only as colour loops. The two struct shapes are not in conflict over the
 * bytes each consumer actually reads:
 *
 *   LevelHeader_70  0x00 s32 unk0 (entry count)   ColorLoopEntry[0].numEntries
 *                   0x04/0x08/0x0C s32            entry[0].rgba / entry[1] words
 *                   0x10/0x14 ColourRGBA bytes    entry[2].rgba  <- read, bytes
 *                   0x18+ entry[i].unk0 s32       entry[3..].numEntries word
 *                                                 (the colour loop reads only
 *                                                  each record's +0x04 RGBA)
 *
 * The colour-loop reader needs exactly one s32 normalized -- numEntries at 0x00,
 * which is the LevelHeader_70 entry count at the same offset -- and reads
 * everything else as bytes at offsets this walk never swaps. So ONE
 * normalization satisfies both views, and both entry points share one dedup
 * registry: whichever consumer resolves the blob first normalizes it, and the
 * other cannot double-swap it back. Getting that wrong is why the colour-loop
 * swap was previously left undone (docs/asset_swap_notes.md, "Residual risks").
 *
 * `size` is the sub-asset's byte length (get_misc_asset_size) and bounds the
 * entry walk, because the entry count is authored data and nothing else limits
 * it. It changes nothing on us.v80 -- sub-assets 58/59/60 are 52 bytes and their
 * count of 4 puts the last swapped word at 0x30..0x33, exactly inside -- so this
 * is future-proofing against an asset whose count outruns its blob, the same
 * bound asset_swap_misc_pulsating() already carries. Pass 0 when the length is
 * not knowable, which keeps the previous unbounded behaviour. */
static void misc_lh70_normalize(void *blob, uint32_t size) {
    int32_t count;
    int32_t i;

    if (blob == NULL) {
        return;
    }
    if (lh70_already_swapped(blob)) {
        return; /* already normalized on an earlier level load / other view */
    }

    sw32(blob, LH70_ENTRY_COUNT);
    sw32(blob, LH70_UNIDENTIFIED_04);
    sw32(blob, LH70_UNIDENTIFIED_08);
    sw32(blob, LH70_UNIDENTIFIED_0C);
    /* 0x10, 0x14 ColourRGBA: bytes, no swap */

    count = rd32(blob, LH70_ENTRY_COUNT);
    if (count < 0 || count > 4096) {
        return; /* implausible — leave the rest untouched */
    }
    for (i = 0; i < count; i++) {
        uint32_t e = LH70_HEADER_SIZE + (uint32_t) i * LH70_ENTRY_STRIDE;
        /* Bound by the bytes this walk actually WRITES (the leading s32), not by
         * the 8-byte stride: sub-asset 58's last entry starts 4 bytes before the
         * end of the blob, and its RGBA tail is not swapped anyway. Bounding on
         * the stride would silently drop a legal swap. */
        if (size != 0u && !in_bounds(e + LH70E_UNIDENTIFIED_00, 4u, size)) {
            break; /* authored count runs past the blob — stop at the edge */
        }
        sw32(blob, e + LH70E_UNIDENTIFIED_00);
        /* +0x04 r,g,b,a bytes, no swap */
    }
}

void asset_swap_misc_lightdata(void *blob, uint32_t size) {
    misc_lh70_normalize(blob, size);
}

void asset_swap_misc_colourloop(void *blob, uint32_t size) {
    misc_lh70_normalize(blob, size);
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

/* Same rationale, and the same memo, as s_lh70Swapped: level_load() re-fetches
 * the same offset on every level load. Table declared with s_lh70Swapped above so
 * both share the MISC-section identity guard. */

void asset_swap_misc_pulsating(void *blob, uint32_t size) {
    uint32_t i, frames, capacity;

    if (blob == NULL || size < PULSE_HEADER_SIZE) {
        return;
    }
    if (misc_memo_claim(s_pulseSwapped, &s_pulseSwappedCount, PULSE_SWAPPED_MAX, blob)) {
        return; /* already normalized on an earlier level load */
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
 *  identity is what pins the field map. The swapper also bounds every offset
 *  and requires its string to terminate inside the blob; it restores the input
 *  bytes if any part of that validation fails.
 *
 *  Must be called AFTER decrypt_magic_codes(): the count and offsets that bound
 *  the index block exist only in the plaintext. The cipher transposes bit pairs
 *  across each four-byte group and does NOT commute with a halfword byte swap.
 * ======================================================================== */
int asset_swap_misc_magic_codes(void *blob, uint32_t size) {
    uint32_t count, indexHalfwords, indexBytes, firstOffset, i;
    uint16_t stringOffset;

    if (blob == NULL || size < 2u) {
        return 0;
    }

    sw16(blob, 0x00);
    count = (uint32_t) (uint16_t) rd16(blob, 0x00);

    indexHalfwords = 1u + count * 3u;
    indexBytes     = indexHalfwords * 2u;
    if (count == 0u || indexBytes > size) {
        sw16(blob, 0x00); /* implausible — undo and leave the blob untouched */
        return 0;
    }

    /* Swap the index block, then check the self-describing invariant. */
    sw16_arr(blob, 0x02, count * 3u);
    firstOffset = (uint32_t) (uint16_t) rd16(blob, 0x02);
    if (firstOffset != indexBytes) {
        /* Field map does not hold for this ROM — revert rather than corrupt. */
        sw16_arr(blob, 0x02, count * 3u);
        sw16(blob, 0x00);
        return 0;
    }

    /* Every table entry is a byte offset to a NUL-terminated string. Checking
     * only offset[0] proves the record shape, but would still let one damaged
     * entry escape into menu.c's strcmp/draw_text walks. Validate the complete
     * table while it is bounded here; fail closed by restoring the original
     * big-endian bytes if even one string is outside the payload or unterminated. */
    for (i = 0; i < count * 3u; i++) {
        stringOffset = (uint16_t) rd16(blob, 0x02 + i * 2u);
        if (stringOffset < indexBytes || stringOffset >= size ||
            memchr((uint8_t *) blob + stringOffset, '\0', size - stringOffset) == NULL) {
            sw16_arr(blob, 0x02, count * 3u);
            sw16(blob, 0x00);
            return 0;
        }
    }
    /* Everything from indexBytes on is ASCII: deliberately NOT swapped. */
    return 1;
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
/* Object_Boost on-disk offsets. The three Object_Boost_Inner blocks occupy
 * 0x00..0x68 as 27 consecutive f32. 0x70..0x73 and 0x74 are runtime state
 * (0 in ROM) that the decomp still calls unk70..unk74. */
#define BOOST_INNER_F32_COUNT 27u
#define BOOST_SPRITE_ID       0x6Cu
#define BOOST_TEXTURE_ID      0x6Eu
#define BOOST_UNIDENTIFIED_74 0x74u /* f32 runtime state, unidentified */

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
        for (f = 0; f < BOOST_INNER_F32_COUNT; f++) {
            swf32(d, f * 4u);
        }
        sw16(d, BOOST_SPRITE_ID);
        sw16(d, BOOST_TEXTURE_ID);
        /* 0x70..0x73: individual u8/s8 — deliberately NOT swapped. */
        swf32(d, BOOST_UNIDENTIFIED_74);
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
        case ASSET_AUDIO:              /* heterogeneous; typed consumers own it */
        case ASSET_MISC:               /* heterogeneous sub-assets              */
        case ASSET_JAPANESE_FONTS:     /* not used by us_v80 (REGION != JP)     */
        case ASSET_JAPANESE_FONTS_TABLE:
        default:
            break;
    }
}
