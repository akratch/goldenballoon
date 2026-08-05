#include "waves.h"
#ifdef NATIVE_PORT
#include "mdkr_bounds.h"
#endif
#include "camera.h"
#include "macros.h"
#include "math_util.h"
#include "memory.h"
#include "objects.h"
#include "PRinternal/viint.h"
#include "printf.h"
#include "racer.h"
#include "textures_sprites.h"
#include "tracks.h"
#include "types.h"
#ifdef NATIVE_PORT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "gfx_shadow_frame.h"
#endif
#include <ultra64.h>

/************ .data ************/

f32 *gWaveHeightTable = NULL;     // indexed by values of gWaveHeightIndices
Vec2s *gWaveHeightIndices = NULL; // holds some sort of index?
TexCoords *gWaveUVTable = NULL;
f32 *D_800E304C[9] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

Vertex *gWaveVertices[4] = { NULL, NULL, NULL, NULL };
Triangle *gWaveTriangles[4] = { NULL, NULL, NULL, NULL };
Triangle D_800E3090[4] = {
    { { { BACKFACE_DRAW, 0, 2, 1 } }, { { { 0, 0 } } }, { { { 0, 0 } } }, { { { 0, 0 } } } },
    { { { BACKFACE_DRAW, 1, 2, 3 } }, { { { 0, 0 } } }, { { { 0, 0 } } }, { { { 0, 0 } } } },
    { { { BACKFACE_DRAW, 0, 2, 1 } }, { { { 0, 0 } } }, { { { 0, 0 } } }, { { { 0, 0 } } } },
    { { { BACKFACE_DRAW, 1, 2, 3 } }, { { { 0, 0 } } }, { { { 0, 0 } } }, { { { 0, 0 } } } },
};

TextureHeader *gWaveTextureHeader = NULL;
s32 *D_800E30D4 = NULL; // indexed by gWaveModel.unkC
LevelModel_Alternate *gWaveModel = NULL;
s32 gVisibleWaveTiles = 0; // Tracks an index into gWaveBlockIDs
s16 *D_800E30E0 = NULL;    // Points to either D_800E30E8 or D_800E3110 and is used for D_800E30D4
s16 *D_800E30E4 = NULL;    // Points to either D_800E30FC or D_800E3144 and is used to index D_800E304C

s16 D_800E30E8[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };

s16 D_800E30FC[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 0 };

s16 D_800E3110[26] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0 };

s16 D_800E3144[26] = { 0, 1, 1, 1, 2, 3, 4, 4, 4, 5, 3, 4, 4, 4, 5, 3, 4, 4, 4, 5, 6, 7, 7, 7, 8, 0 };

u8 *D_800E3178 = NULL;
s32 gWaveTileTotal = 0; // some sort of count? Relative to gWaveController.subdivisions
LevelHeader_70 *D_800E3180 = NULL;
unk800E3184 *D_800E3184 = NULL; // tracks an index into gWaveGenList
s32 gWaveGenCount = 0;      // counter for something, incremented in wavegen_register, decremented in wavegen_destroy
#ifdef NATIVE_PORT
/* MDKR_WAVES_TRACE accounting -- see waves_get_y() and the [WAVES] row. */
s32 gWavesTraceHits = 0;
f32 gWavesTraceMaxY = 0.0f;
#endif
s32 gWaveTileGridCount = 0; // used in mempool_alloc_safe size calculation, multiplied with 8
WaveGen *gWaveGenList = NULL;
Object **gWaveGenObjs = NULL; // might be a length of 32
Object *gWaveGeneratorObj = NULL;

/*******************************/

/************ .rodata ************/

const char D_800E9160[] = "\nCouldn't find a block to pick wave details from.\nUsing block 0 as default.";
const char D_800E91AC[] = "\n\nBlock may be specified using 'P' on water group node.";

/*********************************/

/************ .bss ************/

Gfx *gWaveDL;
Mtx *gWaveMtx;
WaveControl gWaveController; // holds lots of control information used in this file
s32 gWaveVertexFlip;         // either 1 or 0, toggled in waves_update, used to index vertices and triangles
f32 gWaveLowerY;             // is set to lowest value of gWaveHeightTable
f32 gWaveUpperY;             // is set to highest value of gWaveHeightTable
UNUSED s32 D_8012A024;
Vertex D_8012A028[2][4]; // stores values of gWaveVertices to be used in waves_render
s32 gWavePlayerCount;    // controls whether 2 or 4 items are used in gWaveVertices / gWaveTriangles
TriangleBatchInfo *gWaveBatch;
TextureHeader *gWaveTexture;
s32 gWaveUVBaseX;      // u value for gWaveUVTable
s32 gWaveUVBaseY;      // v value for gWaveUVTable
s32 gWaveUVStepX;      // is added onto u of gWaveUVBaseX, is multiplied with texture width
s32 gWaveUVStepY;      // is added onto v of gWaveUVBaseX, is multiplied with texture height
s32 gWaveTexUVMaskX;   // bitmask / something width (related to texture)
s32 gWaveTexUVMaskY;   // bitmask / something height (related to texture)
s32 gWaveTexAnimFrame; // stores / relates to frameAdvanceDelay, used in waves_render
f32 gWaveBoundingBoxW; // copy of gWaveBoundingBoxDiffX
f32 gWaveBoundingBoxH; // copy of gWaveBoundingBoxDiffZ
s32 gWaveBoundingBoxDiffX;
s32 gWaveBoundingBoxDiffZ;
s32 gWaveBoundingBoxX1;
s32 gWaveBoundingBoxZ1;
f32 gWaveVtxStepX;      // some sort of ratio for x
f32 gWaveVtxStepZ;      // some sort of ratio for z
s32 gWaveBlockBoundsX1; // level bounding box x1
s32 gWaveBlockBoundsZ1; // level bounding box x2
s32 gWaveBlockBoundsX2; // level bounding box z1
s32 gWaveBlockBoundsZ2; // level bounding box x2
s32 gWaveBlockPosX;     // copy of gWaveBoundingBoxX1
s32 gWaveBlockPosZ;     // copy of gWaveBoundingBoxZ1
s32 gWaveTileCountX;    // used in mempool_alloc_safe size calculation
s32 gWaveTileCountZ;    // used in mempool_alloc_safe size calculation
s32 gNumberOfLevelSegments;
s32 D_8012A0E8[64];
s16 gWaveBlockIDs[512]; // used to index gWaveModel and as arg0 for func_800B92F4 and func_800B97A8
#ifdef NATIVE_PORT
/*
 * LAYOUT: on the N64 these two symbols are ONE contiguous 26-entry table, and
 * waves.c depends on that rather than merely benefiting from it.
 * waves_visibility() sentinel-clears all 26 slots — `D_8012A5E8[0..1]` *and*
 * `D_8012A600[0..23]`, spelled out as its own reset loop — and then appends every
 * visible wave block through `D_8012A5E8[var_a3]`, with var_a3 measured up to 25
 * (Whale Bay, level 8/19). That is, it indexes D_8012A5E8 straight on into
 * D_8012A600. func_800B92F4() and func_800B97A8() then walk
 * `for (k = 0; D_8012A5E8[k].blockID != -1; k++)` over the same 26 slots.
 *
 * As two separate C objects that only holds if the linker happens to place them
 * adjacently, and whether it does is luck:
 *
 *   Mach-O/arm64 starts D_8012A5E8 at an address 8 mod 16, so its 24 bytes end
 *   16-byte aligned and the 288-byte D_8012A600 (which LLVM gives a 16-byte
 *   preferred alignment) needs no padding — exactly adjacent, so it works.
 *
 *   wasm-ld starts D_8012A5E8 *at* 16-byte alignment, so its 24 bytes end at
 *   8 mod 16 and D_8012A600 is padded up: an 8-BYTE HOLE. Slots 2..25 then land
 *   8 bytes BELOW the slots the reset loop cleared, so the `blockID != -1` walk
 *   never reliably terminates, k runs past the table, and func_800B92F4()'s
 *   `D_800E3178[D_8012A5E8[k].unk8]` loads through a garbage index — the
 *   "memory access out of bounds" a player hit in the browser build.
 *
 * Measured in the exact wasm that crashed (source 63c5d32): &D_8012A5E8 = 231504,
 * so &D_8012A5E8[2] = 231528, but &D_8012A600 = 231536. Natively the two are 24
 * bytes apart, i.e. the gap is 0, which is why this was invisible on the native
 * target and in the whole regression suite.
 *
 * Back both names onto one 26-entry array so the contiguity the code relies on is
 * guaranteed by the language instead of by the linker, on every target. This also
 * removes the UB: `D_8012A5E8[0..25]` is now in bounds of a real 26-element array
 * (UBSan -fsanitize=array-bounds flagged indices 2 and 3 here before). D_8012A600
 * stays a 24-element array, so ARRAY_COUNT(D_8012A600) in the reset loop is
 * unchanged. Nothing outside this file references either symbol.
 */
#define WAVE_VISIBLE_SLOTS 26
typedef struct WaveVisibleSplit {
    unk8012A5E8 head[2];
    unk8012A5E8 tail[WAVE_VISIBLE_SLOTS - 2];
} WaveVisibleSplit;
typedef union WaveVisibleTable {
    unk8012A5E8 all[WAVE_VISIBLE_SLOTS];
    WaveVisibleSplit split;
} WaveVisibleTable;
static WaveVisibleTable gWaveVisibleTable;
#define D_8012A5E8 (gWaveVisibleTable.all)
#define D_8012A600 (gWaveVisibleTable.split.tail)
_Static_assert(offsetof(WaveVisibleSplit, tail) == 2 * sizeof(unk8012A5E8),
               "D_8012A600 must begin exactly at D_8012A5E8[2] — that adjacency IS the contract");
_Static_assert(sizeof(WaveVisibleTable) == WAVE_VISIBLE_SLOTS * sizeof(unk8012A5E8),
               "the 26 wave-visibility slots must be one contiguous run, with no padding");
/*
 * CONTAINMENT for the two walks below. The union above makes the table contiguous,
 * so `blockID != -1` terminates within WAVE_VISIBLE_SLOTS by construction — this
 * bound is never reached in normal play (measured max k = 24 of 26 across every
 * track that renders waves). It exists because the walks do not only READ past the
 * table when the invariant breaks, they WRITE: func_800B92F4() does
 * `D_8012A5E8[k].unk8++` and func_800B97A8() does `unk8 += subdivisions` for every
 * matching entry. That is how the SECOND browser crash happened. With wasm-ld's
 * 8-byte hole the walk ran to k = 1199, and at k = 50 the `unk8` store landed at
 * byte offset 12*50+8 = 608 from &D_8012A5E8 — which in the shipped module is
 * gWaveBlockIDs[8] (gWaveBlockIDs sits at +592). waves_render() then read that
 * corrupted block id (60, against 49 segments), indexed gWaveModel past its array
 * and loaded D_800E30D4[0x9a009a00] ~9.6 GB out of bounds. Reproduced natively by
 * emulating that layout: Bus error / SIGSEGV; with the hole removed, k <= 22 and
 * clean. So an unbounded walk turns a layout mistake into silent corruption of an
 * unrelated array three subsystems away. Bound it: degrade, never corrupt.
 *
 * The DETECTOR for the invariant itself is tests/check_wave_visible_table.py, which
 * fails closed on the built wasm. This is belt-and-braces behind it.
 */
#define WAVE_VISIBLE_WALK_LIVE(k) ((k) < WAVE_VISIBLE_SLOTS && D_8012A5E8[k].blockID != -1)
#else
unk8012A5E8 D_8012A5E8[2];
unk8012A5E8 D_8012A600[24];
#define WAVE_VISIBLE_WALK_LIVE(k) (D_8012A5E8[k].blockID != -1)
#endif
f32 gWavePowerBase;
f32 gWaveMagnitude;
s32 gWavePowerDivisor;

/*****************************/

#define FREE_MEM(mem)          \
    tempMem = (s32 *) mem;     \
    if (tempMem != NULL) {     \
        mempool_free(tempMem); \
        mem = NULL;            \
    }
#define FREE_TEX(tex)      \
    tempTex = tex;         \
    if (tempTex != NULL) { \
        tex_free(tempTex); \
        tex = NULL;        \
    }

/**
 * Free all Wavegen data from memory.
 * Has safety checks to ensure what it's freeing exists, first.
 */
void waves_free(void) {
    TextureHeader *tempTex;
    s32 *tempMem;
    FREE_MEM(gWaveHeightTable);
    FREE_MEM(gWaveHeightIndices);
    FREE_MEM(gWaveUVTable);
    FREE_MEM(D_800E304C[0]);
    FREE_MEM(gWaveVertices[0]);
    FREE_MEM(gWaveTriangles[0]);
    FREE_TEX(gWaveTextureHeader);
    FREE_MEM(D_800E30D4);
    FREE_MEM(gWaveModel);
    FREE_MEM(D_800E3178);
    gWaveGenList = NULL;
    gWaveGenObjs = NULL;
    D_800E3184 = NULL;
    gWaveGenCount = 0;
}

/**
 * Allocates and sets up pointers for all wave geometry and data.
 * Allocates a set for each viewport, up to 2.
 * Calls waves_free as a precautionary.
 */
void waves_alloc(void) {
    s32 temp;
    s32 allocSize;
    s32 i;

    waves_free();
    gWaveHeightTable = (f32 *) mempool_alloc_safe(gWaveController.seedSize * sizeof(f32), COLOUR_TAG_CYAN);
    /* tileCount^2 Vec2s and (subdivisions + 1)^2 TexCoords. Both are four-byte
     * elements; the pointer-sized sizeof they were written with is only the same
     * number where pointers are four bytes. */
    gWaveHeightIndices = (Vec2s *) mempool_alloc_safe(
        (gWaveController.tileCount * sizeof(Vec2s)) * gWaveController.tileCount, COLOUR_TAG_CYAN);
    gWaveUVTable = (TexCoords *) mempool_alloc_safe(((gWaveController.subdivisions + 1) * sizeof(TexCoords)) *
                                                        (gWaveController.subdivisions + 1),
                                                    COLOUR_TAG_CYAN);
    allocSize = ((gWaveController.subdivisions + 1) << 2) * (gWaveController.subdivisions + 1);
    D_800E304C[0] = mempool_alloc_safe(allocSize * ARRAY_COUNT(D_800E304C), COLOUR_TAG_CYAN);
    for (i = 1; i < ARRAY_COUNT(D_800E304C); i++) {
        D_800E304C[i] = (f32 *) (((uintptr_t) D_800E304C[0]) + (allocSize * i));
    }
    temp = (gWaveController.subdivisions + 1);
    allocSize = (temp * 250 * (gWaveController.subdivisions + 1));
    if (gWavePlayerCount != 2) { // 1 player
        /* NATIVE_PORT: (u32) truncates a 64-bit host pointer -- uintptr_t. The
         * [4][1] -> [4] flattening is upstream's (decomp commit 1301ba78); the
         * cast widening is ours and must stay on every one of these lines. */
        gWaveVertices[0] = (Vertex *) mempool_alloc_safe(allocSize << 1, COLOUR_TAG_CYAN);
        gWaveVertices[1] = (Vertex *) (((uintptr_t) gWaveVertices[0]) + allocSize);
    } else { // 2 Player
        gWaveVertices[0] = (Vertex *) mempool_alloc_safe(allocSize << 2, COLOUR_TAG_CYAN);
        gWaveVertices[1] = (Vertex *) (((uintptr_t) gWaveVertices[0]) + allocSize);
        gWaveVertices[2] = (Vertex *) (((uintptr_t) gWaveVertices[1]) + allocSize);
        gWaveVertices[3] = (Vertex *) (((uintptr_t) gWaveVertices[2]) + allocSize);
    }
    allocSize = (gWaveController.subdivisions * 32) * gWaveController.subdivisions;
    if (gWavePlayerCount != 2) { // 1 Player
        gWaveTriangles[0] = mempool_alloc_safe(allocSize << 1, COLOUR_TAG_CYAN);
        gWaveTriangles[1] = (Triangle *) (((uintptr_t) gWaveTriangles[0]) + allocSize);
    } else { // 2 Player
        gWaveTriangles[0] = (Triangle *) mempool_alloc_safe(allocSize << 2, COLOUR_TAG_CYAN);
        gWaveTriangles[1] = (Triangle *) (((uintptr_t) gWaveTriangles[0]) + allocSize);
        gWaveTriangles[2] = (Triangle *) (((uintptr_t) gWaveTriangles[1]) + allocSize);
        gWaveTriangles[3] = (Triangle *) (((uintptr_t) gWaveTriangles[2]) + allocSize);
    }
    gWaveTextureHeader = load_texture(gWaveController.textureId);
}

/**
 * Initialise wave controller variables using the level header.
 * Two player waves hardset the subdivision level to 4.
 */
void waves_init_header(LevelHeader *header) {
    if (gWavePlayerCount != 2) {
        gWaveController.subdivisions = header->waveSubdivisons;
    } else {
        gWaveController.subdivisions = 4;
    }
    gWaveController.tileCount = header->unk57;
    gWaveController.initSine[0].sineStep = header->waveSineStep0;
    gWaveController.initSine[0].height = header->waveSineHeight0 / 256.0f;
    gWaveController.initSine[0].sineBase = header->waveSineBase0 << 8;
    gWaveController.initSine[1].sineStep = header->waveSineStep1;
    gWaveController.initSine[1].height = header->waveSineHeight1 / 256.0f;
    gWaveController.initSine[1].sineBase = header->waveSineBase1 << 8;
    gWaveController.seedSize = header->waveSeedSize & ~1; // Always an even number.
    if (gWavePlayerCount != 2) {
        gWaveController.waveViewDist = header->waveViewDist;
    } else {
        gWaveController.waveViewDist = 3;
    }
    gWaveController.doubleDensity = header->waveDoubleDensity;
    gWaveController.textureId = header->waveTexID & 0xFFFF;
    gWaveController.uvScaleX = header->waveUVScaleX;
    gWaveController.uvScaleY = header->waveUVScaleY;
    gWaveController.uvScrollX = header->waveUVScrollX;
    gWaveController.uvScrollY = header->waveUVScrollY;
    gWaveController.magnitude = header->wavePower / 256.0f;
    gWaveController.unk44 = header->unk64 / 256.0f;
    gWaveController.unk48 = header->unk66 / 256.0f;
    gWaveController.xlu = header->wavesXlu;
}

void waves_init(LevelModel *model, LevelHeader *header, s32 playerCount) {
    s32 k;
    s32 sineVar2;
    s32 j_2;
    s32 var_s3;
    s32 var_s5;
    s32 sineVar1;
    s32 sineStep1;
    s32 i;
    s32 sineStep2;
    s32 j;
    s32 i_2;
    s32 var_t0;
    s32 var_v1;
    s32 var_t7;
    s32 var_a1;
    s32 var_t2;

    gWavePlayerCount = playerCount;
    waves_init_header(header);
    waves_alloc();
    func_800BBDDC(model, header);
    if (gWaveBatch == NULL) {
        /* No water batch in the level model, so there is no wave texture to
         * derive UV steps from and nothing for the tile grid to cover. The
         * caller's gWaveBlockCount test is the same condition reached another
         * way; this keeps the two in step when they disagree. */
        return;
    }
    gWaveBoundingBoxW = gWaveBoundingBoxDiffX;
    gWaveBoundingBoxH = gWaveBoundingBoxDiffZ;
    gWaveVtxStepX = gWaveBoundingBoxW / gWaveController.subdivisions;
    gWaveVtxStepZ = gWaveBoundingBoxH / gWaveController.subdivisions;
    gWaveUVBaseX = 0;
    gWaveUVBaseY = 0;
    gWaveUVStepX = ((gWaveTexture->width * gWaveController.uvScaleX) * 32) / gWaveController.subdivisions;
    gWaveUVStepY = ((gWaveTexture->height * gWaveController.uvScaleY) * 32) / gWaveController.subdivisions;
    gWaveTexUVMaskX = (gWaveTexture->width * 32) - 1;
    gWaveTexUVMaskY = (gWaveTexture->height * 32) - 1;
    gWaveTexAnimFrame = 0;
    sineVar1 = gWaveController.initSine[0].sineBase;
    sineStep1 = (gWaveController.initSine[0].sineStep << 16) / gWaveController.seedSize;
    sineVar2 = gWaveController.initSine[1].sineBase;
    sineStep2 = (gWaveController.initSine[1].sineStep << 16) / gWaveController.seedSize;
    gWaveLowerY = 10000.0f;
    gWaveUpperY = -10000.0f;
    for (i_2 = 0; i_2 < gWaveController.seedSize; i_2++) {
        gWaveHeightTable[i_2] = (sins_f(sineVar1) * gWaveController.initSine[0].height) +
                                (gWaveController.initSine[1].height * sins_f(sineVar2));
        if (gWaveController.doubleDensity) {
            gWaveHeightTable[i_2] *= 2.0f;
        }
        if (gWaveHeightTable[i_2] < gWaveLowerY) {
            gWaveLowerY = gWaveHeightTable[i_2];
        }
        if (gWaveUpperY < gWaveHeightTable[i_2]) {
            gWaveUpperY = gWaveHeightTable[i_2];
        }
        sineVar1 += sineStep1;
        sineVar2 += sineStep2;
    };
    save_rng_seed();
    /* Implementation-defined multi-character literals are not portable. */
    set_rng_seed(0x57415646);

    var_s5 = 0;
    for (i_2 = 0; i_2 < gWaveController.tileCount; i_2++) {
        for (j_2 = 0; j_2 < gWaveController.tileCount; j_2++) {
            gWaveHeightIndices[var_s5].s[0] = rand_range(0, gWaveController.seedSize - 1);
            gWaveHeightIndices[var_s5].s[1] = rand_range(0, gWaveController.seedSize - 1);
            var_s5++;
        }
    }
    var_s5 = 0;
    i_2 = 0;
    load_rng_seed();
    if (playerCount != 2) {
        playerCount = 2;
    } else {
        playerCount = 4;
    }
    for (var_s3 = 0; var_s3 < 25; var_s3++) {
        if (0 <= gWaveController.subdivisions) {
            do {
                for (j_2 = 0; gWaveController.subdivisions >= j_2; j_2++) {
                    for (k = 0; k < playerCount; k++) {
                        gWaveVertices[k][var_s5].x = (j_2 * gWaveVtxStepX) + 0.5;
                        gWaveVertices[k][var_s5].z = (i_2 * gWaveVtxStepZ) + 0.5;
                        // this is only 0 for hot top volcano
                        if (gWaveController.xlu == FALSE) {
                            gWaveVertices[k][var_s5].r = 255;
                            gWaveVertices[k][var_s5].g = 255;
                            gWaveVertices[k][var_s5].b = 255;
                        } else {
                            gWaveVertices[k][var_s5].r = 0;
                            gWaveVertices[k][var_s5].g = 0;
                            gWaveVertices[k][var_s5].b = 0;
                        }
                        gWaveVertices[k][var_s5].a = 255;
                    }
                    var_s5++;
                }
                i_2++;
            } while (gWaveController.subdivisions >= i_2);
            i_2 = 0;
        }
    }

    var_s5 = 0;
    for (i_2 = 0; i_2 < gWaveController.subdivisions; i_2++) {
        for (j_2 = 0; j_2 < gWaveController.subdivisions; j_2++) {
            for (k = 0; k < playerCount; k++) {
                gWaveTriangles[k][var_s5].flags = BACKFACE_DRAW;
                gWaveTriangles[k][var_s5].vi0 = j_2;
                gWaveTriangles[k][var_s5].vi1 = (j_2 + gWaveController.subdivisions) + 1;
                gWaveTriangles[k][var_s5].vi2 = j_2 + 1;
                var_s5++;
                gWaveTriangles[k][var_s5].flags = BACKFACE_DRAW;
                gWaveTriangles[k][var_s5].vi0 = j_2 + 1;
                gWaveTriangles[k][var_s5].vi1 = (j_2 + gWaveController.subdivisions) + 1;
                gWaveTriangles[k][var_s5].vi2 = (j_2 + gWaveController.subdivisions) + 2;
                var_s5--;
            }
            var_s5 += 2;
        }
    }
    func_800BC6C8();

    var_s5 = (gWaveController.subdivisions + 1) * gWaveController.subdivisions;
    for (i = 0; i < ARRAY_COUNT(D_8012A028); i++) {
        D_8012A028[i][0].x = gWaveVertices[i][0].x;
        D_8012A028[i][0].y = 0;
        D_8012A028[i][0].z = gWaveVertices[i][0].z;
        D_8012A028[i][0].r = gWaveVertices[i][0].r;
        D_8012A028[i][0].g = gWaveVertices[i][0].g;
        D_8012A028[i][0].b = gWaveVertices[i][0].b;
        D_8012A028[i][0].a = gWaveVertices[i][0].a;

        D_8012A028[i][1].x = gWaveVertices[i][gWaveController.subdivisions].x;
        D_8012A028[i][1].y = 0;
        D_8012A028[i][1].z = gWaveVertices[i][gWaveController.subdivisions].z;
        D_8012A028[i][1].r = gWaveVertices[i][gWaveController.subdivisions].r;
        D_8012A028[i][1].g = gWaveVertices[i][gWaveController.subdivisions].g;
        D_8012A028[i][1].b = gWaveVertices[i][gWaveController.subdivisions].b;
        D_8012A028[i][1].a = gWaveVertices[i][gWaveController.subdivisions].a;

        D_8012A028[i][2].x = gWaveVertices[i][var_s5].x;
        D_8012A028[i][2].y = 0;
        D_8012A028[i][2].z = gWaveVertices[i][var_s5].z;
        D_8012A028[i][2].r = gWaveVertices[i][var_s5].r;
        D_8012A028[i][2].g = gWaveVertices[i][var_s5].g;
        D_8012A028[i][2].b = gWaveVertices[i][var_s5].b;
        D_8012A028[i][2].a = gWaveVertices[i][var_s5].a;

        D_8012A028[i][3].x = gWaveVertices[i][var_s5 + gWaveController.subdivisions].x;
        D_8012A028[i][3].y = 0;
        D_8012A028[i][3].z = gWaveVertices[i][var_s5 + gWaveController.subdivisions].z;
        D_8012A028[i][3].r = gWaveVertices[i][var_s5 + gWaveController.subdivisions].r;
        D_8012A028[i][3].g = gWaveVertices[i][var_s5 + gWaveController.subdivisions].g;
        D_8012A028[i][3].b = gWaveVertices[i][var_s5 + gWaveController.subdivisions].b;
        D_8012A028[i][3].a = gWaveVertices[i][var_s5 + gWaveController.subdivisions].a;
    }

    func_800BCC70(model);
    if (gWaveController.waveViewDist == 3) {
        D_800E30E0 = D_800E30E8;
        D_800E30E4 = D_800E30FC;
    } else {
        gWaveController.waveViewDist = 5;
        D_800E30E0 = D_800E3110;
        D_800E30E4 = D_800E3144;
    }
    gWaveGenCount = 0;
    gWavePowerDivisor = 0;
    gWaveGeneratorObj = NULL;
    gWaveVertexFlip = 0;
}

/**
 * Set wave visiblity variables to zero.
 */
void waves_visibility_reset(void) {
    s32 i;

    gVisibleWaveTiles = 0;
    for (i = 0; i < gWaveTileCountX * gWaveTileCountZ; i++) {
        D_800E30D4[i] = 0;
    }
}

void waves_visibility(s32 xPosition, s32 yPosition, s32 zPosition, s32 currentViewport, s32 updateRate) {
    s32 xPosRatio;
    s32 zPosRatio;
    s32 tempXPosRatio;
    s32 var_v1;
    s32 var_s5;
    u32 var_t2;
    s32 var_t5;
    s32 i;
    s32 var_s4;
    s32 var_a3;
    s32 blockDist;

    xPosRatio = (xPosition - gWaveBlockPosX) / gWaveBoundingBoxDiffX;
    zPosRatio = (zPosition - gWaveBlockPosZ) / gWaveBoundingBoxDiffZ;

    if (0) {}

#ifdef NATIVE_PORT
    /* NATIVE_PORT (compile-time only, no code): the reset loop below advances by
     * FOUR and terminates on `!=`, so it only stops if the count is a multiple of
     * 4. It is -- 24 -- but that is a consequence of WAVE_VISIBLE_SLOTS being 26,
     * which is a ROM fact recorded 100 lines above and exactly the sort of number
     * a future correction moves. At 25 or 27 slots this loop walks off the end of
     * the table writing `blockID = -1` until it faults, in the same data
     * neighbourhood as the two browser crashes in docs/OPEN_ITEMS.md "wavetable".
     * Found by tools/sweep_bug_shapes.py's equality-cap sweep as the only other
     * stride > 1 equality cap in game/src. Asserting the coupling costs nothing
     * and removes the trap. */
    _Static_assert(ARRAY_COUNT(D_8012A600) % 4 == 0,
                   "the reset loop below strides 4 and stops on `!=`, so the slot count "
                   "must be a multiple of 4 or it runs past the table");
#endif
    for (var_v1 = 0; var_v1 != ARRAY_COUNT(D_8012A600); var_v1 += 4) {
        D_8012A5E8[0].blockID = -1;
        D_8012A5E8[1].blockID = -1;
        D_8012A600[var_v1].blockID = -1;
        D_8012A600[var_v1 + 1].blockID = -1;
        D_8012A600[var_v1 + 2].blockID = -1;
        D_8012A600[var_v1 + 3].blockID = -1;
    }

    if (gWaveController.doubleDensity) {
        xPosition -= (xPosRatio * gWaveBoundingBoxDiffX) + gWaveBlockPosX;
        zPosition -= (zPosRatio * gWaveBoundingBoxDiffZ) + gWaveBlockPosZ;
        if ((gWaveBoundingBoxDiffX >> 1) < xPosition) {
            var_v1 = 8;
        } else {
            var_v1 = 0;
        }

        if ((gWaveBoundingBoxDiffZ >> 1) < zPosition) {
            var_s4 = 16;
        } else {
            var_s4 = 0;
        }

        for (var_v1 -= (gWaveController.waveViewDist >> 1) * 8; var_v1 < 0; var_v1 += 16) {
            xPosRatio -= 1;
        }

        for (var_s4 -= (gWaveController.waveViewDist >> 1) * 16; var_s4 < 0; var_s4 += 32) {
            zPosRatio -= 1;
        }

        for (blockDist = 0, var_a3 = 0; blockDist < gWaveController.waveViewDist; blockDist++) {
            if (zPosRatio >= 0 && zPosRatio < gWaveTileCountZ) {
                tempXPosRatio = xPosRatio;
                var_t5 = var_v1;
                var_t2 = (zPosRatio * gWaveTileCountX) + xPosRatio;
                for (var_s5 = 0; var_s5 < gWaveController.waveViewDist; var_s5++) {
                    // clang-format off
                    if (
                        (tempXPosRatio >= 0) &&
                        (tempXPosRatio < gWaveTileCountX) &&
                        (D_8012A0E8[zPosRatio] & DKR_SHL32(1u, tempXPosRatio))
                    ) {
                        // clang-format on
                        D_800E30D4[var_t2] |= D_800E30E0[blockDist * gWaveController.waveViewDist + var_s5]
                                              << (var_t5 + var_s4);
                        for (i = 0; i < gNumberOfLevelSegments; i++) {
                            if (var_t2 == gWaveModel[i].unkC) {
                                D_8012A5E8[var_a3].blockID = i;
                                D_8012A5E8[var_a3].unk2 = (var_t5 + var_s4) >> 3;
                                D_8012A5E8[var_a3].unk8 = i * gWaveTileTotal;
                                D_8012A5E8[var_a3].unk4 = gWaveModel[i].unk12;
                                if (var_t5 != 0) {
                                    D_8012A5E8[var_a3].unk8 += gWaveController.subdivisions;
                                    D_8012A5E8[var_a3].unk4 += gWaveController.subdivisions;
                                    while (D_8012A5E8[var_a3].unk4 >= gWaveController.tileCount) {
                                        D_8012A5E8[var_a3].unk4 -= gWaveController.tileCount;
                                    }
                                }
                                D_8012A5E8[var_a3].unk6 = gWaveModel[i].unk10;
                                if (var_s4 != 0) {
                                    D_8012A5E8[var_a3].unk8 +=
                                        ((gWaveController.subdivisions * 2) + 1) * gWaveController.subdivisions;
                                    D_8012A5E8[var_a3].unk6 += gWaveController.subdivisions;
                                    while (D_8012A5E8[var_a3].unk6 >= gWaveController.tileCount) {
                                        D_8012A5E8[var_a3].unk6 -= gWaveController.tileCount;
                                    }
                                }
                                var_a3++;
                                i = 0x7FFF;
                            }
                        }
                    }
                    var_t5 += 8;
                    if (var_t5 > 8) {
                        var_t5 -= 16;
                        tempXPosRatio++;
                        var_t2++;
                    }
                }
            }
            var_s4 += 16;
            if (var_s4 > 16) {
                var_s4 -= 32;
                zPosRatio++;
            }
        }
    } else {
        xPosRatio -= gWaveController.waveViewDist >> 1;
        zPosRatio -= gWaveController.waveViewDist >> 1;
        for (blockDist = 0, var_a3 = 0; blockDist < gWaveController.waveViewDist; blockDist++, zPosRatio++) {
            if ((zPosRatio >= 0) && (zPosRatio < gWaveTileCountZ)) {
                tempXPosRatio = xPosRatio;
                var_t2 = (zPosRatio * gWaveTileCountX) + xPosRatio;
                for (var_s5 = 0; var_s5 < gWaveController.waveViewDist; var_s5++, tempXPosRatio++, var_t2++) {
                    // clang-format off
                    if (
                        (tempXPosRatio >= 0) &&
                        (tempXPosRatio < gWaveTileCountX) &&
                        (D_8012A0E8[zPosRatio] & DKR_SHL32(1u, tempXPosRatio))
                    ) {
                        // clang-format on
                        D_800E30D4[var_t2] = D_800E30E0[blockDist * gWaveController.waveViewDist + var_s5];
                        for (i = 0; i < gNumberOfLevelSegments; i++) {
                            if (var_t2 == gWaveModel[i].unkC) {
                                D_8012A5E8[var_a3].blockID = i;
                                D_8012A5E8[var_a3].unk2 = 0;
                                D_8012A5E8[var_a3].unk4 = gWaveModel[i].unk12;
                                D_8012A5E8[var_a3].unk6 = gWaveModel[i].unk10;
                                D_8012A5E8[var_a3].unk8 = i * gWaveTileTotal;
                                var_a3++;
                                i = 0x7FFF;
                            }
                        }
                    }
                }
            }
        }
    }

    func_800BA288(currentViewport, updateRate);
}

/**
 * Returns whether the current wave tile is expected to be the high quality wavegen, over the standard flat plane.
 */
s32 waves_block_hq(LevelModelSegment *block) {
    s32 indexNum = 0;
    s32 result = FALSE;
    while (indexNum < gNumberOfLevelSegments && block != gWaveModel[indexNum].block) {
        indexNum++;
    };
    if (D_800E30D4[gWaveModel[indexNum].unkC]) {
        result = TRUE;
        gWaveBlockIDs[gVisibleWaveTiles++] = indexNum;
    }
    return result;
}

void func_800B92F4(s32 blockID, s32 viewportID) {
    s32 i;
    s32 var_s2;
    s32 j;
    f32 temp_f22;
    f32 vertexY;
    s32 sp98;
    s32 var_v0;
    s32 sp90;
    s32 sp8C;
    s32 sp88;
    s32 sp84;
    s32 vertexCol;
    s32 vertexIdx;
    s32 var_s1;
    s32 k;
    Vertex *vertices;
    LevelModel_Alternate *sp6C;

    temp_f22 = (1.0f - gWaveController.unk44) / 255.0f;
    sp6C = &gWaveModel[blockID];
    sp90 = (gWaveController.subdivisions + 1) * (gWaveController.subdivisions + 1);
    for (k = 0; WAVE_VISIBLE_WALK_LIVE(k); k++) {
        if (blockID != D_8012A5E8[k].blockID) {
            continue;
        }

        if (gWaveController.doubleDensity) {
            sp98 = (((u32) D_800E30D4[sp6C->unkC] >> (D_8012A5E8[k].unk2 * 8)) & 0xFF) - 1;
            sp8C = (D_8012A5E8[k].unk2 & 1) * gWaveController.subdivisions;
            sp88 = ((D_8012A5E8[k].unk2 & 2) >> 1) * gWaveController.subdivisions;
        } else {
            sp98 = (D_800E30D4[sp6C->unkC] & 0xFF) - 1;
            sp8C = 0;
            sp88 = 0;
        }

        sp84 = D_8012A5E8[k].unk6;
        vertices = &gWaveVertices[gWaveVertexFlip + viewportID][sp90 * sp98];
        sp98 = D_800E30E4[sp98];
        vertexIdx = 0;
        for (i = 0; i <= gWaveController.subdivisions; i++) {
            var_s1 = D_8012A5E8[k].unk4;
            var_s2 = (sp84 * gWaveController.tileCount) + var_s1;
            for (j = 0; j <= gWaveController.subdivisions; j++) {
                vertexY = (gWaveHeightTable[gWaveHeightIndices[var_s2].s[0]] +
                           gWaveHeightTable[gWaveHeightIndices[var_s2].s[1]]) *
                          gWaveController.magnitude;
                if (gWaveGenCount > 0) {
                    vertexY += waves_get_y(blockID, j + sp8C, i + sp88);
                }
                vertexY *= D_800E304C[sp98][vertexIdx];
                var_v0 = D_800E3178[D_8012A5E8[k].unk8];

                // clang-format off
                var_v0 <<= 1; \
                D_8012A5E8[k].unk8++;
                // clang-format on
                if (var_v0 < 255) {
                    vertexY *= gWaveController.unk44 + ((f32) var_v0 * temp_f22);
                }
                var_v0 += (s32) (vertexY * gWaveController.unk48);
                if (var_v0 > 255) {
                    var_v0 = 255;
                } else if (var_v0 < 0) {
                    var_v0 = 0;
                }
                var_v0 += ((255 - var_v0) * sp6C->unk14[viewportID >> 1].unk0[D_8012A5E8[k].unk2]) >> 7;
                vertices[vertexIdx].y = vertexY;

                vertexCol = var_v0 < 192 ? 255 : ((255 - var_v0) * 4) & 255;
                vertices[vertexIdx].r = vertexCol;
                vertices[vertexIdx].g = vertexCol;
                vertices[vertexIdx].b = vertexCol;

                vertexCol = var_v0 < 64 ? (var_v0 * 4) & 255 : 255;
                vertices[vertexIdx].a = vertexCol;
                vertexIdx++;
                var_s1++;
                var_s2++;
                if (var_s1 >= gWaveController.tileCount) {
                    var_s1 -= gWaveController.tileCount;
                    var_s2 -= gWaveController.tileCount;
                }
            }
            sp84++;
            if (sp84 >= gWaveController.tileCount) {
                sp84 -= gWaveController.tileCount;
            }
            if (gWaveController.doubleDensity) {
                D_8012A5E8[k].unk8 += gWaveController.subdivisions;
            }
        }
    }
}

void func_800B97A8(s32 blockID, s32 arg1) {
    s32 var_v0;
    f32 temp_f26;
    s32 vertexIdx;
    s32 spA8;
    f32 y;
    s32 spA0;
    s32 sp9C;
    s32 sp98;
    s32 var_a0;
    f32 var_f28;
    s32 var_s1;
    s32 i;
    s32 var_s2;
    s32 j;
    Vertex *vertices;
    LevelModel_Alternate *sp78;
    s32 k;

    k = 0;
    temp_f26 = (gWaveUpperY * 2.0f) - (gWaveLowerY * 2.0f);
    spA0 = (gWaveController.subdivisions + 1) * (gWaveController.subdivisions + 1);
    sp78 = &gWaveModel[blockID];
    var_f28 = temp_f26 <= 0 ? 0 : 1.0f / temp_f26;

    temp_f26 = gWaveLowerY * 2.0f;
    for (; WAVE_VISIBLE_WALK_LIVE(k); k++) {
        if (blockID != D_8012A5E8[k].blockID) {
            continue;
        }

        if (gWaveController.doubleDensity) {
            spA8 = (((u32) D_800E30D4[sp78->unkC] >> (D_8012A5E8[k].unk2 * 8)) & 0xFF) - 1;
            sp9C = (D_8012A5E8[k].unk2 & 1) * gWaveController.subdivisions;
            sp98 = ((s32) (D_8012A5E8[k].unk2 & 2) >> 1) * gWaveController.subdivisions;
        } else {
            spA8 = (D_800E30D4[sp78->unkC] & 0xFF) - 1;
            sp9C = 0;
            sp98 = 0;
        }

        var_a0 = D_8012A5E8[k].unk6;
        vertices = &gWaveVertices[gWaveVertexFlip + arg1][spA0 * spA8];
        spA8 = D_800E30E4[spA8];
        for (i = 0, vertexIdx = 0; i <= gWaveController.subdivisions; i++) {
            var_s1 = D_8012A5E8[k].unk4;
            var_s2 = (var_a0 * gWaveController.tileCount) + var_s1;
            for (j = 0; j <= gWaveController.subdivisions; j++) {
                y = (gWaveHeightTable[gWaveHeightIndices[var_s2].s[0]] +
                     gWaveHeightTable[gWaveHeightIndices[var_s2].s[1]]) *
                    gWaveController.magnitude;
                if (gWaveGenCount > 0) {
                    y += waves_get_y(blockID, j + sp9C, i + sp98);
                }
                y *= D_800E304C[spA8][vertexIdx];
                vertices[vertexIdx].y = y;
                y = (y - temp_f26) * var_f28;
                if (y > 1.0f) {
                    y = 1.0f;
                } else if (y < 0) {
                    y = 0;
                }
                var_v0 = (0 * y);
                var_v0 += 0xFF;
                vertices[vertexIdx].r = var_v0;
                vertices[vertexIdx].g = var_v0;
                vertices[vertexIdx].b = var_v0;
                vertices[vertexIdx].a = 0xFF;
                vertexIdx++;
                var_s1++;
                var_s2++;
                if (var_s1 >= gWaveController.tileCount) {
                    var_s1 -= gWaveController.tileCount;
                    var_s2 -= gWaveController.tileCount;
                }
            }
            var_a0++;
            if (var_a0 >= gWaveController.tileCount) {
                var_a0 -= gWaveController.tileCount;
            }
            if (gWaveController.doubleDensity) {
                D_8012A5E8[k].unk8 += gWaveController.subdivisions;
            }
        }
    }
}

#ifdef NATIVE_PORT
/**
 * Fidelity Phase 2b (P0) -- authoritative half of waves_update.
 *
 * Census: docs/RENDER_MUTATION_CENSUS_2026-07-29.md, "Scene-tree deep census"
 * P0. `waves_update` used to run entirely inside `render_scene` (tracks.c:410),
 * yet three of the things it advances are read back by PHYSICS, not by the
 * renderer:
 *
 *   - gWaveHeightIndices[].s[0]/[1] -- the per-tile wave phase, sampled by
 *     get_level_segment_waves -> func_800BB2F4 into racer->buoyancy
 *     (racer.c:2146-2148, 4612-4613) and object Y placement
 *     (object_functions.c:5751/5791 via obj_wave_height);
 *   - gWaveGenList[].unk1A via func_800BFE98 (waves.c:2784), same readers;
 *   - gWaveController.magnitude / gWavePowerDivisor, the global wave strength
 *     those same queries scale by.
 *
 * Under high-rate presentation (N renders per authoritative tick) the render
 * path would advance the water phase N times per tick and racers would bob at
 * the frame rate. So the phase advance lives here, called once per tick from
 * thread3_main.c immediately before render_scene, under the same
 * `if (gWaveBlockCount)` guard and the same `is_game_paused() -> rate 0` gate
 * render_scene applied.
 *
 * What deliberately does NOT move (see the census's flip-parity rule):
 *   - gWaveVertexFlip (waves.c:872) is a per-RENDER double-buffer parity and is
 *     not idempotent -- it must stay render-side;
 *   - the tex-frame / UV-base advance and the UV table + triangle rebuild are
 *     presentation state with no gameplay reader (class 2). They stay in
 *     waves_update for now, consuming the unchanged tempUpdateRate, which keeps
 *     this diff behaviour-identical under the one-render-per-tick containment
 *     loop. They are a later migration.
 *
 * Under containment (exactly one render_scene per tick) this is a pure code
 * motion: the same writes happen the same number of times in the same order
 * relative to every reader.
 */
void waves_tick(s32 updateRate) {
    s32 i_2;
    s32 j_2;
    s32 k_2;

    for (i_2 = 0, j_2 = 0; i_2 < gWaveController.tileCount; i_2++) {
        for (k_2 = 0; k_2 < gWaveController.tileCount; k_2++) {
            gWaveHeightIndices[j_2].s[0] += updateRate;
            while (gWaveHeightIndices[j_2].s[0] >= gWaveController.seedSize) {
                gWaveHeightIndices[j_2].s[0] -= gWaveController.seedSize;
            }
            gWaveHeightIndices[j_2].s[1] += updateRate;
            while (gWaveHeightIndices[j_2].s[1] >= gWaveController.seedSize) {
                gWaveHeightIndices[j_2].s[1] -= gWaveController.seedSize;
            }
            j_2++;
        }
    }

    if (gWaveGenCount > 0) {
        func_800BFE98(updateRate);
    }

    if (gWavePowerDivisor <= 0) {
        return;
    }

    if (updateRate < gWavePowerDivisor) {
        gWaveController.magnitude += (f32) updateRate * gWaveMagnitude;
        gWavePowerDivisor -= updateRate;
    } else {
        gWaveController.magnitude = gWavePowerBase;
        gWavePowerDivisor = 0;
    }
}
#endif

/**
 * Updates the animations and UV mapping of the waves.
 */
void waves_update(s32 updateRate) {
    s32 i;
    s32 j;
    s32 k;
    s32 var_a2;
    s32 var_ra;
    s32 var_v1;
    s32 var_t5;
    s32 var_t2;
    s32 var_s0;
#ifndef NATIVE_PORT
    s32 k_2;
    s32 j_2;
    s32 i_2;
#endif

    gWaveVertexFlip = 1 - gWaveVertexFlip;
#ifdef NATIVE_PORT
    /* MDKR_WAVES_TRACE=1 -- per-render wave-state diagnostic. One row per
     * waves_update: the authoritative phase/magnitude the surface is built
     * from, plus how much swell actually reached it. `gens` is
     * gWaveGenCount, `swellhits` counts vertices that landed inside some
     * generator's radius since the last row, and `swellmax` is the largest
     * single contribution any vertex received.
     *
     * Those last three are the ones that matter: measured on levels 19 and 34
     * they read `gens=0 swellhits=0 swellmax=0.000` for the whole race, which
     * is how the wave-generator system was found to be inert (see the long
     * note on BHV_WAVE_GENERATOR in objects.c's mdkr_objmap_swap_entry_body).
     * Without them a screenshot of flat water is ambiguous between "the data
     * is wrong" and "the data never arrives". Off by default; one cached
     * getenv when off. */
    {
        static s32 sWavesTraceEnabled = -1;
        static u32 sWavesTraceFrame = 0;
        if (sWavesTraceEnabled < 0) {
            const char *v = getenv("MDKR_WAVES_TRACE");
            sWavesTraceEnabled = (v != NULL && v[0] == '1') ? 1 : 0;
        }
        if (sWavesTraceEnabled) {
            fprintf(stderr,
                    "[WAVES] n=%u rate=%d mag=%.4f magstep=%.5f powdiv=%d "
                    "base=%.4f h0=%d,%d tex=%d uvx=%d gens=%d swellhits=%d "
                    "swellmax=%.3f\n",
                    sWavesTraceFrame++, updateRate, gWaveController.magnitude,
                    gWaveMagnitude, gWavePowerDivisor, gWavePowerBase,
                    gWaveHeightIndices[0].s[0], gWaveHeightIndices[0].s[1],
                    gWaveTexAnimFrame, gWaveUVBaseX, gWaveGenCount,
                    gWavesTraceHits, gWavesTraceMaxY);
        }
        gWavesTraceHits = 0;
        gWavesTraceMaxY = 0.0f;
    }
#endif
#ifndef NATIVE_PORT
    /* NATIVE_PORT: moved to waves_tick() above -- authoritative wave phase. */
    for (i_2 = 0, j_2 = 0; i_2 < gWaveController.tileCount; i_2++) {
        for (k_2 = 0; k_2 < gWaveController.tileCount; k_2++) {
            gWaveHeightIndices[j_2].s[0] += updateRate;
            while (gWaveHeightIndices[j_2].s[0] >= gWaveController.seedSize) {
                gWaveHeightIndices[j_2].s[0] -= gWaveController.seedSize;
            }
            gWaveHeightIndices[j_2].s[1] += updateRate;
            while (gWaveHeightIndices[j_2].s[1] >= gWaveController.seedSize) {
                gWaveHeightIndices[j_2].s[1] -= gWaveController.seedSize;
            }
            j_2++;
        }
    }
#endif

    gWaveTexAnimFrame += gWaveTextureHeader->frameAdvanceDelay * updateRate;
    if (gWaveTexAnimFrame < 0) {
        gWaveTexAnimFrame = 0;
    } else {
        while (gWaveTexAnimFrame >= gWaveTextureHeader->numOfTextures) {
            gWaveTexAnimFrame -= gWaveTextureHeader->numOfTextures;
        }
    }

    gWaveUVBaseX = ((gWaveController.uvScrollX * updateRate) + gWaveUVBaseX) & gWaveTexUVMaskX;
    gWaveUVBaseY = ((gWaveController.uvScrollY * updateRate) + gWaveUVBaseY) & gWaveTexUVMaskY;
    var_a2 = gWaveUVBaseY;
    for (i = 0, var_s0 = 0; i <= gWaveController.subdivisions; i++) {
        var_v1 = gWaveUVBaseX;
        for (j = 0; j <= gWaveController.subdivisions; j++) {
            gWaveUVTable[var_s0].u = var_v1;
            gWaveUVTable[var_s0].v = var_a2;
            var_v1 += gWaveUVStepX;
            var_s0++;
        }
        var_a2 += gWaveUVStepY;
    }

    if (gWavePlayerCount != 2) {
        var_t2 = 1;
    } else {
        var_t2 = 2;
    }

    var_s0 = 0;
    var_t5 = 0;
    var_ra = gWaveController.subdivisions + 1;
    for (i = 0; i < gWaveController.subdivisions; i++) {
        for (j = 0; j < gWaveController.subdivisions; j++) {
            for (k = 0; k < var_t2; k++) {
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv0.u = gWaveUVTable[var_t5].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv0.v = gWaveUVTable[var_t5 + 1].v;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv1.u = gWaveUVTable[var_ra].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv1.v = gWaveUVTable[var_ra].v;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv2.u = gWaveUVTable[var_t5 + 1].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0].uv2.v = gWaveUVTable[var_t5 + 1].v;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv0.u = gWaveUVTable[var_t5 + 1].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv0.v = gWaveUVTable[var_t5 + 1].v;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv1.u = gWaveUVTable[var_ra].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv1.v = gWaveUVTable[var_ra + 1].v;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv2.u = gWaveUVTable[var_ra + 1].u;
                gWaveTriangles[gWaveVertexFlip + (k << 1)][var_s0 + 1].uv2.v = gWaveUVTable[var_ra + 1].v;
            }
            var_t5++;
            var_ra++;
            var_s0 += 2;
        }
        var_t5++;
        var_ra++;
    }

    D_800E3090[2 * gWaveVertexFlip].uv0.u = gWaveUVTable[0].u;
    D_800E3090[2 * gWaveVertexFlip].uv0.v = gWaveUVTable[0].v;
    D_800E3090[2 * gWaveVertexFlip].uv1.u =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1)].u;
    D_800E3090[2 * gWaveVertexFlip].uv1.v =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1)].v;
    D_800E3090[2 * gWaveVertexFlip].uv2.u = gWaveUVTable[gWaveController.subdivisions].u;
    D_800E3090[2 * gWaveVertexFlip].uv2.v = gWaveUVTable[gWaveController.subdivisions].v;
    D_800E3090[2 * gWaveVertexFlip + 1].uv0.u = gWaveUVTable[gWaveController.subdivisions].u;
    D_800E3090[2 * gWaveVertexFlip + 1].uv0.v = gWaveUVTable[gWaveController.subdivisions].v;
    D_800E3090[2 * gWaveVertexFlip + 1].uv1.u =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1)].u;
    D_800E3090[2 * gWaveVertexFlip + 1].uv1.v =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1)].v;
    D_800E3090[2 * gWaveVertexFlip + 1].uv2.u =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1) + gWaveController.subdivisions]
            .u;
    D_800E3090[2 * gWaveVertexFlip + 1].uv2.v =
        gWaveUVTable[gWaveController.subdivisions * (gWaveController.subdivisions + 1) + gWaveController.subdivisions]
            .v;

#ifndef NATIVE_PORT
    /* NATIVE_PORT: moved to waves_tick() -- authoritative wave generators and
     * global wave strength (both feed racer->buoyancy). */
    if (gWaveGenCount > 0) {
        func_800BFE98(updateRate);
    }

    if (gWavePowerDivisor <= 0) {
        return;
    }

    if (updateRate < gWavePowerDivisor) {
        gWaveController.magnitude += (f32) updateRate * gWaveMagnitude;
        gWavePowerDivisor -= updateRate;
    } else {
        gWaveController.magnitude = gWavePowerBase;
        gWavePowerDivisor = 0;
    }
#endif
}

void func_800BA288(s32 arg0, s32 arg1) {
    s32 i;
    s32 j;

    arg1 *= 8;
    for (i = 0; i < gNumberOfLevelSegments; i++) {
        if (D_8012A0E8[gWaveModel[i].unkB] & DKR_SHL32(1u, gWaveModel[i].unkA)) {
            if (gWaveController.doubleDensity) {
                for (j = 0; j < 4; j++) {
                    if (D_800E30D4[gWaveModel[i].unkC] & (0xFFu << ((u32)j * 8u))) {
                        if (arg1 < gWaveModel[i].unk14[arg0].unk0[j]) {
                            gWaveModel[i].unk14[arg0].unk0[j] -= arg1;
                        } else {
                            gWaveModel[i].unk14[arg0].unk0[j] = 0;
                        }
                    } else {
                        if ((gWaveModel[i].unk14[arg0].unk0[j] + arg1) < 0x80) {
                            gWaveModel[i].unk14[arg0].unk0[j] += arg1;
                        } else {
                            gWaveModel[i].unk14[arg0].unk0[j] = 0x80;
                        }
                    }
                }
            } else {
                if (D_800E30D4[gWaveModel[i].unkC]) {
                    if (arg1 < gWaveModel[i].unk14[arg0].unk0[0]) {
                        gWaveModel[i].unk14[arg0].unk0[0] -= arg1;
                    } else {
                        gWaveModel[i].unk14[arg0].unk0[0] = 0;
                    }
                } else {
                    if (gWaveModel[i].unk14[arg0].unk0[0] + arg1 < 0x80) {
                        gWaveModel[i].unk14[arg0].unk0[0] += arg1;
                    } else {
                        gWaveModel[i].unk14[arg0].unk0[0] = 0x80;
                    }
                }
            }
        }
    }
}

/**
 * Loads a texture into texture memory.
 * Can offset the texture address in bytes, since waves use multi-texturing.
 */
void wave_load_material(TextureHeader *tex, s32 rtile) {
    s32 txmask;
    s32 tmem;
    u32 texWidth;

    texWidth = tex->width;
    tmem = 0;
    if (texWidth == 16) {
        txmask = 4;
        if (rtile != G_TX_RENDERTILE) {
            tmem = 384;
        }
    } else if (texWidth == 32) {
        txmask = 5;
        if (rtile != G_TX_RENDERTILE) {
            tmem = 256;
        }
    } else {
        stubbed_printf("WAVE Error: unsupported texture width %u\n", texWidth);
        return;
    }

    // difference is G_IM_SIZ_32b vs G_IM_SIZ_16b
    if (TEX_FORMAT(tex->format) == TEX_FORMAT_RGBA32) {
        gDPLoadMultiBlock(gWaveDL++, OS_K0_TO_PHYSICAL(tex + 1), tmem, rtile, G_IM_FMT_RGBA, G_IM_SIZ_32b, texWidth,
                          texWidth, 0, 0, 0, txmask, txmask, 0, 0);
    } else {
        gDPLoadMultiBlock(gWaveDL++, OS_K0_TO_PHYSICAL(tex + 1), tmem, rtile, G_IM_FMT_RGBA, G_IM_SIZ_16b, texWidth,
                          texWidth, 0, 0, 0, txmask, txmask, 0, 0);
    }
}

/**
 * Renders waves in the current viewport.
 */
void waves_render(Gfx **dList, Mtx **mtx, s32 viewportID) {
    s32 sp11C;
    Vertex *vtx;
    Triangle *tri;
    s32 numTris;
    s32 numVerts;
    s32 j;
    s32 sp104;
    s32 var_fp;
    s32 var_t0;
    ObjectTransform transform;
    LevelModel_Alternate *spE0;
    s32 i;
    TextureHeader *tex1;
    TextureHeader *tex2;

    if (viewportID != VIEWPORT_LAYOUT_2_PLAYERS || gWavePlayerCount != 2) {
        viewportID = 0;
    } else {
        viewportID = 2;
    }

    if (gVisibleWaveTiles > 0) {
        gWaveDL = *dList;
        gWaveMtx = *mtx;
        i = 0;
        rendermode_reset(&gWaveDL);
        gSPSetGeometryMode(gWaveDL++, G_ZBUFFER);
        // Low Quality water. Sets up material data here too.
        if (gWaveController.xlu) {
            gSPClearGeometryMode(gWaveDL++, G_FOG);
            tex1 = set_animated_texture_header(gWaveTextureHeader, gWaveTexAnimFrame * (16 * 16));
            tex2 = set_animated_texture_header(gWaveTexture, gWaveBatch->texOffset * (128 * 128));
            wave_load_material(tex1, 1);
            wave_load_material(tex2, 0);
            gDPSetCombineMode(gWaveDL++, G_CC_BLENDTEX_MODULATEA_1_PRIM, G_CC_BLENDI_ENV_ALPHA_MODULATEA2);
            if (TEX_FORMAT(tex1->format) == TEX_FORMAT_RGBA32 &&
                cam_get_viewport_layout() <= VIEWPORT_LAYOUT_1_PLAYER) {
                gDPSetOtherMode(gWaveDL++, DKR_OMH_2CYC_BILERP, DKR_OML_COMMON | G_RM_AA_ZB_XLU_INTER2);
            } else {
                gDPSetOtherMode(gWaveDL++, DKR_OMH_2CYC_BILERP, DKR_OML_COMMON | G_RM_AA_ZB_OPA_SURF2);
            }
            gDPSetPrimColor(gWaveDL++, 0, 0, 255, 255, 255, 0);
            if (D_800E3180 != NULL) {
                gDPSetEnvColor(gWaveDL++, D_800E3180->rgba.r, D_800E3180->rgba.g, D_800E3180->rgba.b,
                               D_800E3180->rgba.a);
            } else {
                gDPSetEnvColor(gWaveDL++, 255, 255, 255, 0);
            }
        } else {
            gSPSetGeometryMode(gWaveDL++, G_FOG);
            tex1 = set_animated_texture_header(gWaveTexture, gWaveBatch->texOffset * (128 * 128));
            gDkrDmaDisplayList(gWaveDL++, OS_K0_TOKEN_TO_PHYSICAL(tex1->cmd),
                               tex1->numberOfCommands);
            gDPSetCombineMode(gWaveDL++, G_CC_BLENDT_ENV_ALPHA_A_PRIM, G_CC_MODULATEIDECALA2);
            gDPSetOtherMode(gWaveDL++, DKR_OMH_2CYC_BILERP, DKR_OML_COMMON | G_RM_FOG_SHADE_A | G_RM_AA_ZB_OPA_SURF2);
            gDPSetPrimColor(gWaveDL++, 0, 0, 255, 255, 255, 255);
            if (D_800E3180 != NULL) {
                gDPSetEnvColor(gWaveDL++, D_800E3180->rgba.r, D_800E3180->rgba.g, D_800E3180->rgba.b,
                               D_800E3180->rgba.a);
            } else {
                gDPSetEnvColor(gWaveDL++, 255, 255, 255, 0);
            }
        }
        if (gWaveController.doubleDensity) {
            transform.scale = 0.5f;
        } else {
            transform.scale = 1.0f;
        }

        transform.rotation.x = transform.rotation.y = transform.rotation.z = 0;
        // High Quality water
        for (; i < gVisibleWaveTiles; i++) {
            if (gWaveController.xlu) {
                func_800B92F4(gWaveBlockIDs[i], viewportID);
            } else {
                func_800B97A8(gWaveBlockIDs[i], viewportID);
            }
            spE0 = &gWaveModel[gWaveBlockIDs[i]];
            transform.x_position = spE0->originX;
            transform.y_position = spE0->originY;
            transform.z_position = spE0->originZ;
            sp104 = D_800E30D4[spE0->unkC];
            if (gWaveController.doubleDensity) {
                for (sp11C = 0; sp11C < 2; sp11C++) {
                    transform.x_position = spE0->originX;
                    for (var_fp = 0; var_fp < 2; var_fp++) {
                        mtx_cam_push(&gWaveDL, &gWaveMtx, &transform, 1.0f, 0.0f);
                        if (sp104 & 0xFF) {
                            numTris = gWaveController.subdivisions << 1;
                            numVerts = gWaveController.subdivisions + 1;
                            var_t0 = ((sp104 & 0xFF) - 1) * numVerts * numVerts;
                            for (j = 0; j < gWaveController.subdivisions; j++) {
                                vtx = &gWaveVertices[gWaveVertexFlip + viewportID][var_t0];
                                tri = &gWaveTriangles[gWaveVertexFlip + viewportID][j * (gWaveController.subdivisions << 1)];

#ifdef NATIVE_PORT
                                /* Wave surfaces are per-frame vertex-animated
                                 * water/lava; some arms draw them with an
                                 * opaque render mode (Hot Top Volcano, Trophy
                                 * Race, split-screen), and animated liquid
                                 * must never cast a map shadow. */
                                gfx_shadow_caster_exclude_mark(tri);
#endif
                                gSPVertexDKR(gWaveDL++, OS_K0_TO_PHYSICAL(vtx), numVerts << 1, 0);
                                gSPPolygon(gWaveDL++, OS_K0_TO_PHYSICAL(tri), numTris, TRIN_ENABLE_TEXTURE);

                                var_t0 += gWaveController.subdivisions + 1;
                            }
                        } else {
#ifdef NATIVE_PORT
                            gfx_shadow_caster_exclude_mark(&D_800E3090[gWaveVertexFlip << 1]);
#endif
                            gSPVertexDKR(gWaveDL++, OS_K0_TO_PHYSICAL(&D_8012A028[gWaveVertexFlip]), 4, 0);
                            gSPPolygon(gWaveDL++, OS_K0_TO_PHYSICAL(&D_800E3090[gWaveVertexFlip << 1]), 2,
                                       TRIN_ENABLE_TEXTURE);
                        }
                        mtx_pop(&gWaveDL);
                        sp104 >>= 8;
                        transform.x_position += gWaveBoundingBoxW * 0.5f;
                    }
                    transform.z_position += gWaveBoundingBoxH * 0.5f;
                }
            } else {
                mtx_cam_push(&gWaveDL, &gWaveMtx, &transform, 1.0f, 0.0f);
                numTris = gWaveController.subdivisions << 1;
                numVerts = gWaveController.subdivisions + 1;
                var_t0 = ((sp104 & 0xFF) - 1) * numVerts * numVerts;
                for (j = 0; j < gWaveController.subdivisions; j++) {
                    vtx = &gWaveVertices[gWaveVertexFlip + viewportID][var_t0];
                    tri = &gWaveTriangles[gWaveVertexFlip + viewportID][j * (gWaveController.subdivisions << 1)];

#ifdef NATIVE_PORT
                    /* See above: animated liquid never casts. */
                    gfx_shadow_caster_exclude_mark(tri);
#endif
                    gSPVertexDKR(gWaveDL++, OS_K0_TO_PHYSICAL(vtx), numVerts << 1, 0);
                    gSPPolygon(gWaveDL++, OS_K0_TO_PHYSICAL(tri), numTris, TRIN_ENABLE_TEXTURE);

                    var_t0 += gWaveController.subdivisions + 1;
                }
                mtx_pop(&gWaveDL);
            }
        }
        if (gWaveController.xlu) {
            gSPSetGeometryMode(gWaveDL++, G_FOG);
            gDPSetPrimColor(gWaveDL++, 0, 0, 255, 255, 255, 255);
        }
        *dList = gWaveDL;
        *mtx = gWaveMtx;
    }
    gVisibleWaveTiles = 0;
}

f32 func_800BB2F4(s32 arg0, f32 arg1, f32 arg2, Vec3f *arg3) {
    f32 spA4;
    f32 spA0;
    f32 var_f12;
    f32 var_f2;
    f32 sp94;
    f32 sp90;
    f32 var_f16;
    f32 arg3X;
    f32 arg3Y;
    f32 arg3Z;
    s32 var_a0;
    f32 sp78;
    s32 var_v0;
    s32 sp70;
    s32 sp6C;
    s32 var_t0;
    s32 var_a3;
    s32 sp60;
    u8 *tempD_800E3178;
    s32 sp58;
    s32 pad;

    if (arg0 < 0 || arg0 >= gNumberOfLevelSegments) {
        arg0 = 0;
    }

    sp78 = gWaveModel[arg0].originY;

    sp90 = gWaveVtxStepZ;
    sp94 = gWaveVtxStepX;
    var_f16 = (1.0f - gWaveController.unk44) / 127.0f;
    sp58 = arg0 * gWaveTileTotal;

    if (gWaveController.doubleDensity) {
        sp90 /= 2.0f;
        sp94 /= 2.0f;
        sp60 = (gWaveController.subdivisions * 2) + 1;
    } else {
        sp60 = gWaveController.subdivisions + 1;
    }

    arg1 -= gWaveModel[arg0].originX;
    if (arg1 < 0.0f) {
        arg1 = 0.0f;
    } else if (gWaveBoundingBoxW <= arg1) {
        arg1 = gWaveBoundingBoxW - 1; // needs to be 1 instead of 1.0f
    }

    arg2 -= gWaveModel[arg0].originZ;
    if (arg2 < 0.0f) {
        arg2 = 0.0f;
    } else if (gWaveBoundingBoxH <= arg2) {
        arg2 = gWaveBoundingBoxH - 1; // needs to be 1 instead of 1.0f
    }

    sp70 = arg1 / sp94;
    sp6C = arg2 / sp90;
    arg1 -= sp70 * sp94;
    arg2 -= sp6C * sp90;

    var_t0 = gWaveModel[arg0].unk12 + sp70;
    while (var_t0 >= gWaveController.tileCount) {
        var_t0 -= gWaveController.tileCount;
    }

    var_a3 = gWaveModel[arg0].unk10 + sp6C;
    while (var_a3 >= gWaveController.tileCount) {
        var_a3 -= gWaveController.tileCount;
    }

    var_v0 = 0;
    if (arg2 != sp90 && arg1 < (((sp90 - arg2) / sp90) * sp94)) {
        var_v0 = 1;
    }

    if (var_v0) {
        var_a0 = (var_a3 * gWaveController.tileCount) + var_t0;
        spA0 = (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
               gWaveController.magnitude;
        var_a0 = sp58 + sp70 + (sp6C * sp60);
        if (gWaveGenCount > 0) {
            spA0 += waves_get_y(arg0, sp70, sp6C);
        }

        var_v0 = D_800E3178[var_a0];
        if (D_800E3178[var_a0] < 0x7F) {
            spA0 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        var_a0 =
            (var_a3 + 1) >= gWaveController.tileCount ? var_t0 : ((var_a3 + 1) * gWaveController.tileCount) + var_t0;
        var_f12 =
            (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
            gWaveController.magnitude;
        if (gWaveGenCount > 0) {
            var_f12 += waves_get_y(arg0, sp70, sp6C + 1);
        }
        var_a0 = sp58 + sp70 + ((sp6C + 1) * sp60);
        var_v0 = D_800E3178[var_a0];
        if (D_800E3178[var_a0] < 0x7F) {
            var_f12 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        if ((var_t0 + 1) >= gWaveController.tileCount) {
            var_a0 = var_a3 * gWaveController.tileCount;
        } else {
            var_a0 = (var_t0 + 0) + (var_a3 * gWaveController.tileCount) + 1;
        }
        var_f2 =
            (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
            gWaveController.magnitude;
        if (gWaveGenCount > 0) {
            var_f2 += waves_get_y(arg0, sp70 + 1, sp6C);
        }
        var_a0 = sp58 + sp70 + (sp6C * sp60);
        var_a0++;
        var_v0 = D_800E3178[var_a0];
        if (D_800E3178[var_a0] < 0x7F) {
            var_f2 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        if (gWaveController.doubleDensity) {
            spA0 /= 2.0f;
            var_f12 /= 2.0f;
            var_f2 /= 2.0f;
        }

        spA4 = 0.0f;
        arg3X = (spA0 - var_f2) * sp90;
        arg3Y = sp90 * sp94;
        arg3Z = (spA0 - var_f12) * sp94;
    } else {
        if ((var_t0 + 1) >= gWaveController.tileCount) {
            var_a0 = var_a3 * gWaveController.tileCount;
        } else {
            var_a0 = (var_t0 + 0) + (var_a3 * gWaveController.tileCount) + 1;
        }
        spA0 = (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
               gWaveController.magnitude;
        if (gWaveGenCount > 0) {
            spA0 += waves_get_y(arg0, sp70 + 1, sp6C);
        }
        var_a0 = sp58 + sp70 + (sp6C * sp60);
        var_a0++;
        var_v0 = D_800E3178[var_a0];
        if (D_800E3178[var_a0] < 0x7F) {
            spA0 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        var_a0 =
            (var_a3 + 1) >= gWaveController.tileCount ? var_t0 : ((var_a3 + 1) * gWaveController.tileCount) + var_t0;
        var_f12 =
            (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
            gWaveController.magnitude;
        if (gWaveGenCount > 0) {
            var_f12 += waves_get_y(arg0, sp70, sp6C + 1);
        }
        var_a0 = sp58 + sp70 + ((sp6C + 1) * sp60);
        var_v0 = (tempD_800E3178 = D_800E3178)[var_a0];
        if (var_v0 < 0x7F) {
            var_f12 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        var_a0 = (var_t0 + 1) >= gWaveController.tileCount ? 0 : (var_t0 + 1);
        if ((var_a3 + 1) < gWaveController.tileCount) {
            var_a0 += (var_a3 + 1) * gWaveController.tileCount;
        }

        var_f2 =
            (gWaveHeightTable[gWaveHeightIndices[var_a0].s[0]] + gWaveHeightTable[gWaveHeightIndices[var_a0].s[1]]) *
            gWaveController.magnitude;
        if (gWaveGenCount > 0) {
            var_f2 += waves_get_y(arg0, sp70 + 1, sp6C + 1);
        }
        var_a0 = sp58 + sp70 + ((sp6C + 1) * sp60);
        var_a0++;
        var_v0 = D_800E3178[var_a0];
        if (var_v0 < 0x7F) {
            var_f2 *= gWaveController.unk44 + (var_v0 * var_f16);
        }

        if (gWaveController.doubleDensity) {
            spA0 /= 2.0f;
            var_f12 /= 2.0f;
            var_f2 /= 2.0f;
        }

        spA4 = sp94;
        arg3X = sp90 * (var_f12 - var_f2);
        arg3Y = sp90 * sp94;
        arg3Z = sp94 * (spA0 - var_f2);
    }

    var_f16 = sqrtf((arg3X * arg3X) + (arg3Y * arg3Y) + (arg3Z * arg3Z));
    if (var_f16 != 0.0 && arg3Y != 0.0) {
        arg3X /= var_f16;
        arg3Y /= var_f16;
        arg3Z /= var_f16;
        sp78 -= (((arg3X * arg1) + (arg3Z * arg2)) - ((spA4 * arg3X) + (spA0 * arg3Y))) / arg3Y;
    }

    if (arg3 != NULL) {
        arg3->x = arg3X;
        arg3->y = arg3Y;
        arg3->z = arg3Z;
    }

    return sp78;
}

void func_800BBDDC(LevelModel *level, LevelHeader *header) {
    func_800BBE08(level, header);
    func_800BBF78(level);
}

// determines current bounding box, batch and texture
void func_800BBE08(LevelModel *level, LevelHeader *header) {
    s16 numSegments;
    s32 j;
    TriangleBatchInfo *curBatch;
    s32 i;
    s32 colourID;
    LevelModelSegmentBoundingBox *bb;
    LevelModelSegment *segment;

    numSegments = level->numberOfSegments;
    curBatch = 0;

    for (i = 0; curBatch == 0 && i < numSegments; i++) {
        segment = &DKR_PTR(LevelModelSegment, level->segments)[i];
        for (j = 0; curBatch == 0 && j < segment->numberOfBatches; j++) {
            if ((DKR_PTR(TriangleBatchInfo, segment->batches)[j].flags & (RENDER_UNK_1000000 | RENDER_WATER | RENDER_HIDDEN)) ==
                (RENDER_UNK_1000000 | RENDER_WATER)) {
                curBatch = &DKR_PTR(TriangleBatchInfo, segment->batches)[j];
            }
        }
    }

    if (curBatch == 0) {
        i = 0;
    } else {
        i--;
    }
    bb = &DKR_PTR(LevelModelSegmentBoundingBox, level->segmentsBoundingBoxes)[i];
    gWaveBoundingBoxDiffX = bb->x2 - bb->x1;
    gWaveBoundingBoxDiffZ = bb->z2 - bb->z1;
    gWaveBoundingBoxX1 = bb->x1;
    gWaveBoundingBoxZ1 = bb->z1;
    gWaveBatch = curBatch;
    if (curBatch == 0) {
        /* No water batch in the level: there is no texture and no colour to
         * select, and gWaveBatch stays NULL for the render path to test. */
        gWaveTexture = NULL;
        D_800E3180 = NULL;
        return;
    }
    gWaveTexture = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, level->textures)[curBatch->textureIndex].texture);
    // Change these batch flags to 0, 1, 2 and 4
    colourID = (curBatch->flags & (RENDER_UNK_40000000 | RENDER_UNK_20000000 | RENDER_UNK_10000000)) >> 28;
    if (colourID > 0) {
        D_800E3180 = DKR_PTR(LevelHeader_70, (&header->unk70[0])[colourID]);
    } else {
        D_800E3180 = NULL;
    }
}

void func_800BBF78(LevelModel *model) {
    LevelModelSegmentBoundingBox *levelSegmentBoundingBoxes;
    s32 j;
    s32 temp_t1;
    s32 segmentVertexY;
    s32 temp_t2;
    s32 pad;
    s32 subdivisions;
    LevelModelSegment *levelSegments;
    s32 i;
    s32 var_v0;
    LevelModel_Alternate *otherModel;

    levelSegments = DKR_PTR(LevelModelSegment, model->segments);
    levelSegmentBoundingBoxes = DKR_PTR(LevelModelSegmentBoundingBox, model->segmentsBoundingBoxes);
    subdivisions = gWaveController.subdivisions;
    if (gWaveController.doubleDensity) {
        subdivisions *= 2;
    }
    gWaveBlockBoundsX1 = levelSegmentBoundingBoxes->x1;
    gWaveBlockBoundsX2 = levelSegmentBoundingBoxes->x2;
    gWaveBlockBoundsZ1 = levelSegmentBoundingBoxes->z1;
    gWaveBlockBoundsZ2 = levelSegmentBoundingBoxes->z2;
    for (i = 1; i < model->numberOfSegments; i++) {
        if (levelSegmentBoundingBoxes[i].x1 < gWaveBlockBoundsX1) {
            gWaveBlockBoundsX1 = levelSegmentBoundingBoxes[i].x1;
        }
        if (gWaveBlockBoundsX2 < levelSegmentBoundingBoxes[i].x2) {
            gWaveBlockBoundsX2 = levelSegmentBoundingBoxes[i].x2;
        }
        if (levelSegmentBoundingBoxes[i].z1 < gWaveBlockBoundsZ1) {
            gWaveBlockBoundsZ1 = levelSegmentBoundingBoxes[i].z1;
        }
        if (gWaveBlockBoundsZ2 < levelSegmentBoundingBoxes[i].z2) {
            gWaveBlockBoundsZ2 = levelSegmentBoundingBoxes[i].z2;
        }
    }

    gWaveBlockPosX = gWaveBoundingBoxX1;
    while (gWaveBlockBoundsX1 < gWaveBlockPosX) {
        gWaveBlockPosX -= gWaveBoundingBoxDiffX;
    }

    gWaveBlockPosZ = gWaveBoundingBoxZ1;
    while (gWaveBlockBoundsZ1 < gWaveBlockPosZ) {
        gWaveBlockPosZ -= gWaveBoundingBoxDiffZ;
    }

    gWaveTileCountX = ((gWaveBlockBoundsX2 - gWaveBlockPosX) / gWaveBoundingBoxDiffX) + 1;
    gWaveTileCountZ = ((gWaveBlockBoundsZ2 - gWaveBlockPosZ) / gWaveBoundingBoxDiffZ) + 1;
    gWaveTileGridCount = (subdivisions * gWaveTileCountX) + 1;

    if (D_800E30D4 != NULL) {
        mempool_free(D_800E30D4);
    }
    /* One s32 per tile, not one pointer. */
    D_800E30D4 = mempool_alloc_safe(gWaveTileCountX * gWaveTileCountZ * sizeof(*D_800E30D4), COLOUR_TAG_CYAN);

    if (gWaveModel != NULL) {
        mempool_free(gWaveModel);
    }

    /*
     * NATIVE_PORT: this one allocation is carved into four arrays, and both the
     * size and the carving assumed a 4-byte pointer. The tail after the model
     * segments is, in order:
     *
     *     gWaveGenList   32 * sizeof(WaveGen)   = 0x800 on every platform
     *     gWaveGenObjs   32 * sizeof(Object *)  = 0x80 on N64, 0x100 on LP64
     *     D_800E3184     gWaveTileGridCount * sizeof(unk800E3184)
     *
     * The original tail constant 0x880 is exactly 0x800 + 32 * 4, so it is only
     * correct where pointers are four bytes. Worse, the offset that finds
     * gWaveGenObjs -- `gWaveGenList + sizeof(WaveGen *) * 8` -- is pointer
     * arithmetic on WaveGen *, i.e. it advances (sizeof(WaveGen *) * 8) WHOLE
     * WaveGen structs. On N64 that is 4 * 8 = 32 structs = 0x800, landing
     * exactly right; on LP64 it is 8 * 8 = 64 structs = 0x1000, which overshoots
     * by 0x800 and lands inside (or past) D_800E3184's bucket table.
     *
     * Measured before this fix, on levels 19 and 34: gWaveGenObjs sat 0x1000
     * after gWaveGenList, its 32 "slots" read back as 0xFF-filled bucket bytes
     * rather than NULL, wavegen_register never found a free slot,
     * gWaveGenCount stayed 0, and waves_get_y returned before touching a single
     * generator -- so no swell has ever rendered in the native port. The zeroing
     * loop below was simultaneously writing 32 pointers of zeroes over
     * D_800E3184's 0xFF sentinels, corrupting the very table the register walk
     * scans.
     *
     * Note this is native-only. wasm32 pointers are four bytes, so the original
     * expression carves correctly there exactly as it does on N64 -- which is
     * why the swell system has always been live in the browser build, and why
     * the byte-swapped generator values (see BHV_WAVE_GENERATOR in objects.c)
     * were visible to players there and nowhere else.
     *
     * The fix is to say what was meant: 32 WaveGen structs, and a tail sized
     * from the host's real pointer width. The original expression is kept for
     * the N64 build, where it is correct.
     */
    // clang-format off
#ifdef NATIVE_PORT
    gWaveModel = mempool_alloc_safe(
        (model->numberOfSegments * sizeof(LevelModel_Alternate)) + (gWaveTileGridCount * 8) +
            (32 * sizeof(WaveGen)) + (32 * sizeof(Object *)),
        COLOUR_TAG_CYAN
    );
#else
    gWaveModel = mempool_alloc_safe(
        (model->numberOfSegments * sizeof(LevelModel_Alternate)) + (gWaveTileGridCount * 8) + 0x880,
        COLOUR_TAG_CYAN
    );
#endif
    // clang-format on

    gWaveGenList = (WaveGen *) ((u8 *) gWaveModel + model->numberOfSegments * sizeof(LevelModel_Alternate));
#ifdef NATIVE_PORT
    gWaveGenObjs = (Object **) (gWaveGenList + 32);
#else
    gWaveGenObjs = (Object **) (gWaveGenList + sizeof(WaveGen *) * 8);
#endif
    D_800E3184 = (unk800E3184 *) (gWaveGenObjs + 32);

#ifdef NATIVE_PORT
    /* D_800E3184 is an array of 8-byte buckets. Initialize through the owning
     * array instead of indexing unk0 past its first subobject. */
    memset(
        D_800E3184,
        0xFF,
        (size_t)gWaveTileGridCount * sizeof(*D_800E3184));
#else
    for (i = 0; i < (gWaveTileGridCount * 8); i++) {
        D_800E3184->unk0[i] = 0xFF;
    }
#endif

    // 0x20 / 32 sizeof what?
    for (i = 0; i < 32; i++) {
        gWaveGenObjs[i] = 0;
    }

    gWaveGenCount = 0;

    for (i = 0; i < (gWaveTileCountX * gWaveTileCountZ); i++) {
        D_800E30D4[i] = 0;
    }

    for (i = 0; i < ARRAY_COUNT(D_8012A0E8); i++) {
        D_8012A0E8[i] = 0;
    }

    gNumberOfLevelSegments = model->numberOfSegments;
    for (i = 0; i < gNumberOfLevelSegments; i++) {
        temp_t1 = levelSegmentBoundingBoxes[i].x1 - gWaveBlockPosX + 8;
        temp_t2 = levelSegmentBoundingBoxes[i].z1 - gWaveBlockPosZ + 8;
        temp_t1 = ((temp_t1 / gWaveBoundingBoxDiffX) * gWaveBoundingBoxDiffX) + gWaveBlockPosX;
        temp_t2 = ((temp_t2 / gWaveBoundingBoxDiffZ) * gWaveBoundingBoxDiffZ) + gWaveBlockPosZ;
        segmentVertexY = 0;
        for (j = 0; j < levelSegments[i].numberOfBatches; j++) {
            if ((DKR_PTR(TriangleBatchInfo, levelSegments[i].batches)[j].flags & RENDER_WATER) &&
                (DKR_PTR(TriangleBatchInfo, levelSegments[i].batches)[j].flags & RENDER_UNK_0400000)) {
                segmentVertexY = DKR_PTR(Vertex, levelSegments[i].vertices)[DKR_PTR(TriangleBatchInfo, levelSegments[i].batches)[j].verticesOffset].y;
            }
        }

        otherModel = &gWaveModel[i];
        otherModel->block = &levelSegments[i];
        otherModel->originX = temp_t1;
        otherModel->originY = segmentVertexY;
        otherModel->originZ = temp_t2;
        otherModel->unkA = (temp_t1 - gWaveBlockPosX) / gWaveBoundingBoxDiffX;
        otherModel->unkB = (temp_t2 - gWaveBlockPosZ) / gWaveBoundingBoxDiffZ;
        otherModel->unkC = otherModel->unkA + (otherModel->unkB * gWaveTileCountX);
        otherModel->unk12 = 0;
        otherModel->unk10 = 0;
        if (levelSegments[i].hasWaves != 0) {
            var_v0 = otherModel->unkA * subdivisions;
            while (var_v0 >= gWaveController.tileCount) {
                var_v0 -= gWaveController.tileCount;
            }
            otherModel->unk12 = var_v0;
            var_v0 = otherModel->unkB * subdivisions;
            while (var_v0 >= gWaveController.tileCount) {
                var_v0 -= gWaveController.tileCount;
            }
            otherModel->unk10 = var_v0;
            D_8012A0E8[otherModel->unkB] |= DKR_SHL32(1u, otherModel->unkA);
        }

        for (j = 0; j < 4; j++) {
            otherModel->unk14[0].unk0[j] = 0x80;
            otherModel->unk14[1].unk0[j] = 0x80;
        }
    }
}

void func_800BC6C8(void) {
    s32 i;
    s32 j;
    s32 k;
    s32 var_s0;
    s32 var_v0;
    f32 sp34[0x80];

    var_v0 = 0x4000 / gWaveController.subdivisions;
    var_s0 = 0;
    for (i = 0; i < gWaveController.subdivisions;) {
        sp34[i++] = sins_f(var_s0);
        var_s0 += var_v0;
    }
    sp34[0] = 0.0f;
    sp34[gWaveController.subdivisions] = 1.0f;

    k = 0;
    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = 0; j <= gWaveController.subdivisions; j++) {
            D_800E304C[4][k] = 1.0f;
            k++;
        }
    }

    k = 0;
    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = 0; j <= gWaveController.subdivisions; j++) {
            D_800E304C[1][k] = sp34[i];
            k++;
        }
    }

    k = 0;
    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = 0; j <= gWaveController.subdivisions;) {
            D_800E304C[3][k++] = sp34[j++];
        }
    }

    k = 0;
    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = 0; j <= gWaveController.subdivisions;) {
            D_800E304C[5][k++] = sp34[gWaveController.subdivisions - j++];
        }
    }

    k = 0;
    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = 0; j <= gWaveController.subdivisions; j++) {
            D_800E304C[7][k] = sp34[gWaveController.subdivisions - i];
            k++;
        }
    }

    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = i; j <= gWaveController.subdivisions; j++) {
            D_800E304C[0][i * (gWaveController.subdivisions + 1) + j] = sp34[i];
            D_800E304C[0][j * (gWaveController.subdivisions + 1) + i] = sp34[i];
        }
    }

    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = i; j <= gWaveController.subdivisions; j++) {
            D_800E304C[2][(i * (gWaveController.subdivisions + 1)) + gWaveController.subdivisions - j] = sp34[i];
            D_800E304C[2][(j * (gWaveController.subdivisions + 1)) + gWaveController.subdivisions - i] = sp34[i];
        }
    }

    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = i; j <= gWaveController.subdivisions; j++) {
            D_800E304C[6][(gWaveController.subdivisions * (gWaveController.subdivisions + 1)) -
                          (i * (gWaveController.subdivisions + 1)) + j] = sp34[i];
            D_800E304C[6][(gWaveController.subdivisions * (gWaveController.subdivisions + 1)) -
                          (j * (gWaveController.subdivisions + 1)) + i] = sp34[i];
        }
    }

    for (i = 0; i <= gWaveController.subdivisions; i++) {
        for (j = i; j <= gWaveController.subdivisions; j++) {
            // clang-format off
            D_800E304C[8][(gWaveController.subdivisions * (gWaveController.subdivisions + 1)) - (i * (gWaveController.subdivisions + 1)) + gWaveController.subdivisions - j] = sp34[i];
            D_800E304C[8][(gWaveController.subdivisions * (gWaveController.subdivisions + 1)) - (j * (gWaveController.subdivisions + 1)) + gWaveController.subdivisions - i] = sp34[i];
            // clang-format on
        }
    }
}

void func_800BCC70(LevelModel *model) {
    s32 i;
    u32 pad_sp188;
    s32 sp184;
    s32 k;
    s32 var_t4;
    s32 var_t8;
    f32 y;
    f32 x;
    f32 z;
    f32 stepX;
    f32 stepZ;
    s32 collisionCount;
    s32 sp140[8];
    s32 j;
    s32 var_a1;
    s32 var_a2;
    s32 var_a3;
    s32 var_s2;
    s32 subdivisions;
    s32 var_s5;
    f32 colY[30];
    f32 *spA8;
    Vec2i *spA4;
    u8 *spA0;

    subdivisions = gWaveController.subdivisions;
    if (gWaveController.doubleDensity) {
        subdivisions *= 2;
    }
    gWaveTileTotal = (subdivisions + 1) * (subdivisions + 1);
    if (D_800E3178 != NULL) {
        mempool_free(D_800E3178);
    }
    D_800E3178 = mempool_alloc_safe(model->numberOfSegments * gWaveTileTotal, COLOUR_TAG_CYAN);
    spA0 = mempool_alloc_safe(model->numberOfSegments * 4, COLOUR_TAG_CYAN);
    spA4 = mempool_alloc_safe((gWaveTileCountX * gWaveTileCountZ) * 8, COLOUR_TAG_CYAN);
    spA8 = mempool_alloc_safe((subdivisions * 4) + 4, COLOUR_TAG_CYAN);

    // temp assignment required for match
    pad_sp188 = -1;
    for (var_a3 = 0; var_a3 < (gWaveTileCountX * gWaveTileCountZ); var_a3++) {
        spA4[var_a3].i[0] = -1;
        spA4[var_a3].i[1] = pad_sp188;
    }

    for (k = 0; k <= subdivisions; k++) {
        spA8[k] = ((subdivisions - (k << 1)) / (f32) subdivisions) * 8.0f;
    }

    stepX = gWaveBoundingBoxW / subdivisions;
    stepZ = gWaveBoundingBoxH / subdivisions;
    for (i = 0, var_s5 = 0; i < model->numberOfSegments; i++) {
        if (D_8012A0E8[gWaveModel[i].unkB] & DKR_SHL32(1u, gWaveModel[i].unkA)) {
            spA4[(gWaveModel[i].unkB * gWaveTileCountX) + gWaveModel[i].unkA].i[0] = i;
            spA4[(gWaveModel[i].unkB * gWaveTileCountX) + gWaveModel[i].unkA].i[1] = var_s5;
            y = gWaveModel[i].originY;
            z = gWaveModel[i].originZ;
            for (sp184 = 0; sp184 <= subdivisions; sp184++) {
                x = gWaveModel[i].originX;
                for (k = 0; k <= subdivisions; k++) {
                    // var_v0 stores the length of spAC
#ifdef NATIVE_PORT
                    collisionCount = collision_get_y(i, spA8[k] + x, spA8[sp184] + z, colY, ARRAY_COUNT(colY));
#else
                    collisionCount = collision_get_y(i, spA8[k] + x, spA8[sp184] + z, colY);
#endif
                    if (collisionCount == 0) {
                        var_a2 = 255;
                    } else {
                        j = 0;
                        while (j < collisionCount - 1 && y <= colY[j]) {
                            j++;
                        }
                        if (y < colY[j]) {
                            var_a2 = 0;
                        } else if (y == colY[j]) {
                            var_a2 = 255;
                        } else {
                            var_a2 = (y - colY[j]) * gWaveController.unk48;
                            if (var_a2 > 511) {
                                var_a2 = 511;
                            }
                            var_a2 >>= 1;
                        }
                    }
                    D_800E3178[var_s5++] = var_a2;
                    x += stepX;
                }
                z += stepZ;
            }
        } else {
            // clang-format off
            // @note \ required here
            for (k = 0; k < gWaveTileTotal; k++) { \
                D_800E3178[var_s5++] = 255;
            }
            // clang-format on
        }
    }

    for (sp184 = 0; sp184 < gWaveTileCountZ; sp184++) {
        for (k = 0; k < gWaveTileCountX; k++) {
            var_t4 = spA4[(sp184 * gWaveTileCountX) + k].i[1];
            if (var_t4 != -1) {
                if (sp184 < (gWaveTileCountZ - 1)) {
                    collisionCount = spA4[((sp184 + 1) * gWaveTileCountX) + k].i[1];
                    if (collisionCount != -1) {
                        var_a1 = ((subdivisions + 1) * subdivisions) + var_t4 + 1;
                        j = collisionCount + 1;
                        for (var_a3 = 0; var_a3 < subdivisions - 1; var_a3++) {
                            var_a2 = (D_800E3178[var_a1] + D_800E3178[j]) >> 1;
                            D_800E3178[var_a1] = var_a2;
                            D_800E3178[j] = var_a2;
                            j++;
                            var_a1++;
                        }
                    }
                }
                if (k < (gWaveTileCountX - 1)) {
                    collisionCount = spA4[(sp184 * gWaveTileCountX) + k + 1].i[1];
                    if (collisionCount != -1) {
                        var_a1 = (subdivisions * 2) + var_t4 + 1;
                        j = collisionCount + subdivisions + 1;
                        for (var_a3 = 0; var_a3 < subdivisions - 1; var_a3++) {
                            var_a2 = (D_800E3178[var_a1] + D_800E3178[j]) >> 1;
                            D_800E3178[var_a1] = var_a2;
                            if (!j) {}
                            D_800E3178[j] = var_a2;
                            j += subdivisions;
                            j++;
                            var_a1 += subdivisions;
                            var_a1++;
                        }
                    }
                }
            }
        }
    }

    for (var_t4 = 0, i = 0; i < model->numberOfSegments; i++, var_t4 += gWaveTileTotal) {
        spA0[(i * 4) + 0] = D_800E3178[var_t4];
        spA0[(i * 4) + 1] = D_800E3178[var_t4 + (subdivisions)];
        spA0[(i * 4) + 2] = D_800E3178[var_t4 + ((subdivisions + 1) * subdivisions)];
        spA0[(i * 4) + 3] = D_800E3178[var_t4 + ((subdivisions + 1) * (subdivisions + 1)) - 1];
    }

    for (var_t4 = 0, i = 0; i < model->numberOfSegments; i++, var_t4 += gWaveTileTotal) {
        var_s5 = (gWaveModel[i].unkB * gWaveTileCountX) + gWaveModel[i].unkA;
        if (spA4[var_s5].i[0] != -1) {
            for (var_a3 = 0; var_a3 < ARRAY_COUNT(sp140); var_a3++) {
                sp140[var_a3] = -1;
            }

            if (gWaveModel[i].unkB > 0) {
                if (gWaveModel[i].unkA > 0) {
                    sp140[0] = spA4[var_s5 - gWaveTileCountX - 1].i[0];
                }
                sp140[1] = spA4[var_s5 - gWaveTileCountX].i[0];
                if (gWaveModel[i].unkA < gWaveTileCountX - 1) {
                    sp140[2] = spA4[var_s5 - gWaveTileCountX + 1].i[0];
                }
            }

            if (gWaveModel[i].unkA > 0) {
                sp140[3] = spA4[var_s5 - 1].i[0];
            }

            if (gWaveModel[i].unkA < (gWaveTileCountX - 1)) {
                sp140[4] = spA4[var_s5 + 1].i[0];
            }

            if (gWaveModel[i].unkB < (gWaveTileCountZ - 1)) {
                if (gWaveModel[i].unkA > 0) {
                    sp140[5] = spA4[var_s5 + gWaveTileCountX - 1].i[0];
                }
                sp140[6] = spA4[var_s5 + gWaveTileCountX].i[0];
                if (gWaveModel[i].unkA < gWaveTileCountX - 1) {
                    sp140[7] = spA4[var_s5 + gWaveTileCountX + 1].i[0];
                }
            }

            var_a3 = 1;
            var_a2 = spA0[i * 4];
            if (sp140[0] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[0] * 4) + 3];
            }

            if (sp140[1] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[1] * 4) + 2];
            }

            if (sp140[3] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[3] * 4) + 1];
            }
            D_800E3178[var_t4] = var_a2 / var_a3;

            var_a2 = spA0[(i * 4) + 1];
            var_a3 = 1;
            if (sp140[1] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[1] * 4) + 3];
            }

            if (sp140[2] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[2] * 4) + 2];
            }

            if (sp140[4] != -1) {
                var_a3++;
                var_a2 += spA0[sp140[4] * 4];
            }
            D_800E3178[var_t4 + subdivisions] = var_a2 / var_a3;

            var_a2 = spA0[(i * 4) + 2];
            var_a3 = 1;
            if (sp140[3] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[3] * 4) + 3];
            }

            if (sp140[5] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[5] * 4) + 1];
            }

            if (sp140[6] != -1) {
                var_a3++;
                var_a2 += spA0[sp140[6] * 4];
            }
            D_800E3178[var_t4 + ((subdivisions + 1) * subdivisions)] = var_a2 / var_a3;

            var_a2 = spA0[(i * 4) + 3];
            var_a3 = 1;
            if (sp140[4] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[4] * 4) + 2];
            }

            if (sp140[6] != -1) {
                var_a3++;
                var_a2 += spA0[(sp140[6] * 4) + 1];
            }

            if (sp140[7] != -1) {
                var_a3++;
                var_a2 += spA0[sp140[7] * 4];
            }

            D_800E3178[var_t4 + ((subdivisions + 1) * (subdivisions + 1)) - 1] = var_a2 / var_a3;
        }
    }

    mempool_free(spA8);
    mempool_free(spA4);
    mempool_free(spA0);
}

#ifdef NATIVE_PORT
/*
 * NATIVE_PORT: `maxOut` added -- the number of TRIANGLES the caller's two arrays
 * can hold. Third instance of the bare-pointer class found by
 * tools/sweep_bug_shapes.py, and the only one with two output pointers.
 *
 * `counter` walks the shadow's tile grid, emitting two triangles per cell, and
 * nothing caps it: it indexes `arg1[]` (the caller's D_8011C3B8[64]), `arg2[]`
 * (into D_8011C8B8[128] at the RUNNING offset D_8011D0B8, so the real capacity is
 * smaller than 128 and varies per call) and, in the first loop, the local
 * spD8[300]. The grid size comes from the shadow's world extent and
 * gWaveController.subdivisions -- level data -- so the ROM has no bound to give.
 *
 * The caller passes ARRAY_COUNT(D_8011C3B8) and the REMAINING room in
 * D_8011C8B8, whichever is smaller, so both outputs are covered by one number.
 * `counter` is clamped, not just its writes: the return value is the triangle
 * count the caller then loops over, so an unclamped count would send it reading
 * entries this function never wrote.
 *
 * NOT REACHED: measured peak in the commit message; 0 calls at the bound.
 */
s32 func_800BDC80(s32 arg0, unk8011C3B8 *arg1, unk8011C8B8 *arg2, f32 shadowXNegPosition, f32 shadowZNegPosition,
                  f32 shadowXPosition, f32 shadowZPosition, s32 maxOut) {
#else
s32 func_800BDC80(s32 arg0, unk8011C3B8 *arg1, unk8011C8B8 *arg2, f32 shadowXNegPosition, f32 shadowZNegPosition,
                  f32 shadowXPosition, f32 shadowZPosition) {
#endif
    s32 colCount;
    s32 startCol;
    s32 startRow;
    s32 currentRow;
    s32 currentCol;
    s32 endCol;
    s32 endRow;
    s32 colOffset;
    s32 rowOffset;
    s32 counter;
    f32 temp_fs0;
    f32 temp_fs1;
    f32 temp_fs3;
    f32 var_fs0;
    f32 var_fs1;
    s16 sp332;
    s16 spD8[300];
    s16 temp_fp;
    s16 temp_s2;
    s16 var_s3_2;
    f32 stepX;
    f32 stepZ;
    f32 temp_fs2;
    s32 var_s0;
    s32 var_s2;
    s32 var_s4;
    s32 temp_t0;

    temp_t0 = arg0 * gWaveTileTotal;
    stepX = gWaveVtxStepX;
    stepZ = gWaveVtxStepZ;
    temp_fs2 = (1.0f - gWaveController.unk44) / 127.0f;
    if (gWaveController.doubleDensity != 0) {
        var_fs1 = 0.5f;
        stepX *= 0.5;
        stepZ *= 0.5;
        colCount = gWaveController.subdivisions * 2;
    } else {
        var_fs1 = 1.0f;
        colCount = gWaveController.subdivisions;
    }

    shadowXNegPosition -= gWaveModel[arg0].originX;
    shadowZNegPosition -= gWaveModel[arg0].originZ;
    shadowXPosition -= gWaveModel[arg0].originX;
    shadowZPosition -= gWaveModel[arg0].originZ;
    if (shadowXNegPosition < 0.0f) {
        startCol = 0;
    } else {
        startCol = (s32) (shadowXNegPosition / stepX);
    }
    if (shadowZNegPosition < 0.0f) {
        startRow = 0;
    } else {
        startRow = (s32) (shadowZNegPosition / stepZ);
    }
    if (gWaveBoundingBoxW <= shadowXPosition) {
        endCol = colCount;
    } else {
        endCol = (s32) (shadowXPosition / stepX) + 1;
    }
    if (gWaveBoundingBoxH <= shadowZPosition) {
        endRow = colCount;
    } else {
        endRow = (s32) (shadowZPosition / stepX) + 1;
    }
    colOffset = gWaveModel[arg0].unk12 + startCol;
    while (colOffset >= gWaveController.tileCount) {
        colOffset -= gWaveController.tileCount;
    }
    rowOffset = gWaveModel[arg0].unk10 + startRow;
    while (rowOffset >= gWaveController.tileCount) {
        rowOffset -= gWaveController.tileCount;
    }

    for (currentRow = startRow, counter = 0; currentRow <= endRow; currentRow++) {
        var_s0 = colOffset;

        var_s2 = (rowOffset * gWaveController.tileCount) + var_s0;
        var_s4 = temp_t0 + startCol + (currentRow * colCount);
        for (currentCol = startCol; currentCol <= endCol; currentCol++) {

            var_fs0 =
                (gWaveHeightTable[gWaveHeightIndices[var_s2].x] + gWaveHeightTable[gWaveHeightIndices[var_s2].y]) *
                gWaveController.magnitude;
            if (gWaveGenCount > 0) {
                var_fs0 += waves_get_y(arg0, currentCol, currentRow);
            }

            if (D_800E3178[var_s4] < 0x7F) {
                var_fs0 *= gWaveController.unk44 + ((s32) D_800E3178[var_s4] * temp_fs2);
            }
#ifdef NATIVE_PORT
            /* NATIVE_PORT: spD8 is this function's OWN local and its fill has no
             * cap either -- the grid is (endRow-startRow+1) * (endCol-startCol+1)
             * cells, all from level data. Overflowing it smashes this frame, not
             * the caller's. Bounded, not clamped: `counter` must keep counting so
             * the second loop's indices stay consistent with the grid. */
            if (counter < (s32) ARRAY_COUNT(spD8) || mdkr_segbound_legacy()) {
                spD8[counter] = gWaveModel[arg0].originY + (s16) (var_fs0 * var_fs1);
            }
            counter++;
#else
            spD8[counter] = gWaveModel[arg0].originY + (s16) (var_fs0 * var_fs1);
            counter++;
#endif
            var_s0++;
            var_s4++;
            var_s2++;
            if (var_s0 >= gWaveController.tileCount) {
                var_s0 -= gWaveController.tileCount;
                var_s2 -= gWaveController.tileCount;
            }
        }
#ifdef NATIVE_PORT
        mdkr_bound_probe(4, counter, (s32) ARRAY_COUNT(spD8));
#endif
        rowOffset++;
        if (rowOffset >= gWaveController.tileCount) {
            rowOffset -= gWaveController.tileCount;
        }
    }

    // FAKE
    temp_fs3 = (stepX * stepZ) * (stepX * stepZ);
    colCount = (endCol - startCol) + 1;
    sp332 = gWaveModel[arg0].originZ + (s16) (startRow * stepZ);

    for (currentRow = startRow, counter = 0; currentRow < endRow; currentRow++) {
        temp_fp = gWaveModel[arg0].originZ + (s16) ((currentRow + 1) * stepZ);
        var_s3_2 = gWaveModel[arg0].originX + (s16) (startCol * stepX);

        for (currentCol = startCol; currentCol < endCol; currentCol++) {
            temp_s2 = gWaveModel[arg0].originX + (s16) ((currentCol + 1) * stepX);

#ifdef NATIVE_PORT
            /* NATIVE_PORT: the bound. Two triangles are emitted per cell, so the
             * check needs two slots of room. Stopping the whole walk (rather than
             * skipping cells) keeps the emitted run contiguous from 0, which is
             * what the caller's `for (i = 0; i < temp_v0; i++)` requires. */
            if (counter + 2 > maxOut && !mdkr_segbound_legacy()) {
                goto shadowFull;
            }
#endif
            arg1[counter].x1 = var_s3_2;
            arg1[counter].x2 = var_s3_2;
            arg1[counter].x3 = temp_s2;
            arg1[counter].y1 = spD8[(currentCol - startCol) + ((currentRow - startRow) * colCount)];
            arg1[counter].y2 = spD8[(currentCol - startCol) + (((currentRow - startRow) + 1) * colCount)];
            arg1[counter].y3 = spD8[((currentCol - startCol) + 1) + ((currentRow - startRow) * colCount)];
            arg1[counter].z1 = sp332;
            arg1[counter].z2 = temp_fp;
            arg1[counter].z3 = sp332;
            temp_fs0 = (arg1[counter].y1 - arg1[counter].y3) * stepZ;
            temp_fs3 = stepX * stepZ;
            temp_fs1 = (arg1[counter].y1 - arg1[counter].y2) * stepX;
            temp_fs2 = sqrtf((temp_fs0 * temp_fs0) + (temp_fs3 * temp_fs3) + (temp_fs1 * temp_fs1));
            if (temp_fs2 > 0.0f) {
                temp_fs2 = 1 / temp_fs2;
                arg2[counter].x = temp_fs0 * temp_fs2;
                arg2[counter].y = temp_fs3 * temp_fs2;
                arg2[counter].z = temp_fs1 * temp_fs2;

                arg2[counter].unkC_union.w =
                    -((arg1[counter].x1 * arg2[counter].x) + (arg1[counter].y1 * arg2[counter].y) +
                      (arg1[counter].z1 * arg2[counter].z));
                counter++;
            }
            arg1[counter].x1 = temp_s2;
            arg1[counter].x2 = var_s3_2;
            arg1[counter].x3 = temp_s2;
            arg1[counter].y1 = spD8[((currentCol - startCol) + 1) + ((currentRow - startRow) * colCount)];
            arg1[counter].y2 = spD8[(currentCol - startCol) + (((currentRow - startRow) + 1) * colCount)];
            arg1[counter].y3 = spD8[((currentCol - startCol) + 1) + (((currentRow - startRow) + 1) * colCount)];
            arg1[counter].z1 = sp332;
            arg1[counter].z2 = temp_fp;
            arg1[counter].z3 = temp_fp;
            temp_fs0 = (arg1[counter].y2 - arg1[counter].y3) * stepZ;
            temp_fs3 = stepX * stepZ;
            temp_fs1 = (arg1[counter].y1 - arg1[counter].y3) * stepX;
            temp_fs2 = sqrtf((temp_fs0 * temp_fs0) + (temp_fs3 * temp_fs3) + (temp_fs1 * temp_fs1));
            if (temp_fs2 > 0.0f) {
                temp_fs2 = 1 / temp_fs2;
                arg2[counter].x = temp_fs0 * temp_fs2;
                arg2[counter].y = temp_fs3 * temp_fs2;
                arg2[counter].z = temp_fs1 * temp_fs2;

                arg2[counter].unkC_union.w =
                    -((arg1[counter].x1 * arg2[counter].x) + (arg1[counter].y1 * arg2[counter].y) +
                      (arg1[counter].z1 * arg2[counter].z));
                counter++;
            }

            var_s3_2 = temp_s2;
        }
        sp332 = temp_fp;
    }

#ifdef NATIVE_PORT
shadowFull:
    mdkr_bound_probe(3, counter, maxOut);
#endif
    return counter;
}

Object_Log *obj_wave_init(s32 blockID, f32 x, f32 z) {
    f32 spE4;
    f32 temp_f;
    s32 pad;
    f32 var_f0;
    s32 pad2;
    f32 var_f12;
    s32 var_t4;
    f32 temp_f2;
    s32 spC4;
    s32 spC0;
    f32 var_f18;
    f32 var_f20;
    f32 var_f22;
    s32 spB0;
    s32 var_t1;
    s32 var_a0;
    s16 var_t0;
    s16 var_v1;
    s16 var_a3;
    s16 var_a1;
    f32 var_f2;
    s32 var_v0;
    f32 var_f14;
    s32 var_v0_2;
    s32 var_a2;
    s32 sp80[3];
    s32 sp68[6];
    s32 sp5C[3];
    f32 sp50[3];
    Object_Log *object;

    var_t0 = 128;
    object = NULL;
    if (gWaveHeightTable != NULL && blockID >= 0 && blockID < gNumberOfLevelSegments) {
#ifdef NATIVE_PORT
        object = mempool_alloc_safe(
            sizeof(Object_Log) + (gWaveController.seedSize >> 1),
            COLOUR_TAG_CYAN);
#else
        object = mempool_alloc_safe((gWaveController.seedSize >> 1) + (sizeof(Object_Log) - 2), COLOUR_TAG_CYAN);
#endif
        object->unk0 = gWaveModel[blockID].originY;
        object->unk2 = 0;
        object->unk3 = 1;
        object->unk4 = 0;
        object->unk6 = gWaveController.seedSize;
        var_f20 = gWaveVtxStepZ;
        var_f22 = gWaveVtxStepX;
        temp_f2 = (1.0f - gWaveController.unk44) / 127.0f;
        var_f2 = temp_f2;
        spB0 = (blockID * gWaveTileTotal);
        if (gWaveController.doubleDensity) {
            var_t4 = (gWaveController.subdivisions * 2) + 1;
            var_f20 /= 2.0f;
            var_f22 /= 2.0f;
        } else {
            var_t4 = gWaveController.subdivisions + 1;
        }
        // clang-format off
        x -= gWaveModel[blockID].originX;
        if (x < 0.0f) { x = 0.0f; }
        else if (x >= gWaveBoundingBoxW) { x = gWaveBoundingBoxW - 1; }
        z -= gWaveModel[blockID].originZ;
        if (z < 0.0f) { z = 0.0f; }
        else if (z >= gWaveBoundingBoxH) { z = gWaveBoundingBoxH - 1; }
        // clang-format on
        spC4 = x / var_f22;
        spC0 = z / var_f20;
        object->unk8 = spC4;
        object->unkA = spC0;
        object->blockID = blockID;
        x -= (spC4 * var_f22);
        z -= (spC0 * var_f20);
        var_a0 = gWaveModel[blockID].unk12 + spC4;
        while (var_a0 >= gWaveController.tileCount) {
            var_a0 -= gWaveController.tileCount;
        }

        var_v0 = gWaveModel[blockID].unk10 + spC0;
        while (var_v0 >= gWaveController.tileCount) {
            var_v0 -= gWaveController.tileCount;
        }

        var_t1 = FALSE;
        if (z != var_f20 && x < (((var_f20 - z) / var_f20) * var_f22)) {
            var_t1 = TRUE;
        }

        if (var_t1 != FALSE) {
            sp80[0] = (var_v0 * gWaveController.tileCount) + var_a0;

            if ((var_v0 + 1) >= gWaveController.tileCount) {
                sp80[1] = var_a0;
            } else {
                sp80[1] = ((var_v0 + 1) * gWaveController.tileCount) + var_a0;
            }

            if ((var_a0 + 1) >= gWaveController.tileCount) {
                sp80[2] = (var_v0 * gWaveController.tileCount);
            } else {
                sp80[2] = (var_v0 * gWaveController.tileCount) + var_a0 + 1;
            }

            spE4 = 0.0f;
            sp5C[0] = (spB0 + spC4) + (spC0 * var_t4);
            sp5C[1] = (spB0 + spC4) + ((spC0 + 1) * var_t4);
            sp5C[2] = (spB0 + spC4) + (spC0 * var_t4) + 1;
        } else {
            if ((var_a0 + 1) >= gWaveController.tileCount) {
                sp80[0] = var_v0 * gWaveController.tileCount;
            } else {
                sp80[0] = var_a0 + 1 + (var_v0 * gWaveController.tileCount);
            }

            if ((var_v0 + 1) >= gWaveController.tileCount) {
                sp80[1] = var_a0;
            } else {
                sp80[1] = ((var_v0 + 1) * gWaveController.tileCount) + var_a0;
            }

            if ((var_a0 + 1) >= gWaveController.tileCount) {
                sp80[2] = 0;
            } else {
                sp80[2] = var_a0 + 1;
            }

            if ((var_v0 + 1) < gWaveController.tileCount) {
                sp80[2] += (var_v0 + 1) * gWaveController.tileCount;
            }

            spE4 = var_f22;
            sp5C[0] = (spC4 + spB0) + (spC0 * var_t4) + 1;
            sp5C[1] = (spC4 + spB0) + (spC0 + 1) * var_t4;
            sp5C[2] = (spC4 + spB0) + (spC0 + 1) * var_t4 + 1;
        }

        for (var_a3 = 0; var_a3 < 3; var_a3++) {
            sp68[var_a3 * 2] = gWaveHeightIndices[sp80[var_a3]].s[0];
            sp68[var_a3 * 2 + 1] = gWaveHeightIndices[sp80[var_a3]].s[1];
            sp50[var_a3] = 1.0f;

            var_a2 = D_800E3178[sp5C[var_a3]];
            if (var_a2 < 0x7F) {
                sp50[var_a3] *= gWaveController.unk44 + (var_a2 * var_f2);
            }
        }

        if (gWaveController.doubleDensity) {
            sp50[0] /= 2.0;
            sp50[1] /= 2.0;
            sp50[2] /= 2.0;
        }

        for (var_a3 = 0; var_a3 < (gWaveController.seedSize >> 1); var_a3++) {
            temp_f = (gWaveHeightTable[sp68[0]] + gWaveHeightTable[sp68[1]]) * sp50[0];
            var_f12 = (gWaveHeightTable[sp68[2]] + gWaveHeightTable[sp68[3]]) * sp50[1];
            var_f0 = (gWaveHeightTable[sp68[4]] + gWaveHeightTable[sp68[5]]) * sp50[2];
            if (var_t1 != FALSE) {
                var_f2 = (temp_f - var_f0) * var_f20;
                var_f14 = (temp_f - var_f12) * var_f22;
            } else {
                var_f2 = (var_f12 - var_f0) * var_f20;
                var_f14 = (temp_f - var_f0) * var_f22;
            }
            var_f18 = var_f20 * var_f22;
            var_f0 = sqrtf((var_f2 * var_f2) + (var_f18 * var_f18) + (var_f14 * var_f14));
            var_f2 /= var_f0;
            var_f14 /= var_f0;
            var_f12 = var_f18 / var_f0;
            var_v0_2 =
                -(s32) (((var_f2 * x) + (var_f14 * z) - ((spE4 * var_f2) + (temp_f * var_f12))) * 16.0f / var_f12);
            if (var_v0_2 >= var_t0 || var_v0_2 < -var_t0) {
                var_v1 = 0;
                do {
                    var_t0 += var_t0;
                    var_v1++;
                } while (var_v0_2 >= var_t0 || var_v0_2 < -var_t0);

                for (var_a1 = 0; var_a1 < var_a3; var_a1++) {
                    object->unkE[var_a1] >>= var_v1;
                }
                object->unk2 += var_v1;
            }
            object->unkE[var_a3] = (var_v0_2 >> object->unk2);
            for (var_a1 = 0; var_a1 < 6; var_a1++) {
                sp68[var_a1] += 2;
                while (sp68[var_a1] >= gWaveController.seedSize) {
                    sp68[var_a1] -= gWaveController.seedSize;
                }
            }
        }
    }

    return object;
}

/**
 * Finds the wave height and returns it for the given object.
 */
f32 obj_wave_height(Object_Log *log, s32 updateRate) {
    s32 var_t0;
    f32 y;

    log->unk4 += updateRate;
    while (log->unk4 >= log->unk6) {
        log->unk4 -= log->unk6;
    }
    var_t0 = log->unkE[log->unk4 >> 1];
    if (log->unk4 & 1) {
        if (log->unk4 + 1 >= log->unk6) {
            var_t0 += log->unkE[0];
        } else {
            var_t0 += log->unkE[(log->unk4 >> 1) + 1];
        }
        if (log->unk2 > 0) {
            /* DKR_SHL32 (NATIVE_PORT): `<< (unk2 + 0x1F)` with unk2 > 0 is a shift
             * count >= 32, i.e. UB in C; MIPS `sllv` masks it to 5 bits, so this
             * means `<< (unk2 - 1)` -- one less than the even-index branch below,
             * which is what halves the sum of the two interpolated samples (and is
             * why unk2 == 0 takes the `>>= 1` path instead). Measured NOT folded in
             * today's Release build, because clang turns the if/else into a select
             * and so speculates the shift with a count range of [31, 286] that
             * includes the legal 31. Latent rather than active, but the same UB as
             * the cutscene-flag sites -- see docs/OPEN_ITEMS.md wave "keyshift". */
            var_t0 = (s32) DKR_SHL32(var_t0, log->unk2 + 0x1F);
        } else {
            var_t0 >>= 1;
        }
    } else {
        var_t0 = (s32)DKR_SHL32(var_t0, log->unk2);
    }
    y = (((f32) var_t0 / 16.0) + (f32) log->unk0);
    y *= gWaveController.magnitude;
    y += waves_get_y(log->blockID, log->unk8, log->unkA);
    return y;
}

// height related calculation?
f32 waves_get_y(s32 blockID, s32 arg1, s32 arg2) {
    f32 dist;
    f32 distSq;
    f32 diffX;
    f32 diffZ;
    f32 temp_f24;
    f32 zeroCheck;
    f32 temp_f30;
    f32 stepX;
    f32 y;
    f32 stepZ;
    u32 var_s0;
    s32 temp_a1;
    s32 i;
    s32 subdivisons;
    s32 temp_0;
    unk800E3184 *temp_a3;
    WaveGen *gen;

    y = 0.0f;
    if (gWaveGenCount <= 0) {
        return y;
    }

    subdivisons = gWaveController.subdivisions;
    stepX = gWaveVtxStepX;
    stepZ = gWaveVtxStepZ;
    if (gWaveController.doubleDensity) {
        subdivisons *= 2;
        stepX /= 2.0f;
        stepZ /= 2.0f;
    }
    temp_a1 = (gWaveModel[blockID].unkA * subdivisons) + arg1;
    temp_a3 = &D_800E3184[temp_a1];
    if (temp_a3->unk0[0] != 0xFF) {
        temp_0 = arg2 + (subdivisons * gWaveModel[blockID].unkB);
        temp_f30 = gWaveBlockPosX + (temp_a1 * stepX);
        temp_f24 = gWaveBlockPosZ + (temp_0 * stepZ);
        zeroCheck = 0;
        i = 0;
        do {
            gen = &gWaveGenList[temp_a3->unk0[i]];
            if (gen->minZ <= temp_f24 && temp_f24 <= gen->maxZ) {
                diffX = temp_f30 - gen->x_position;
                diffZ = temp_f24 - gen->z_position;
                distSq = (diffX * diffX) + (diffZ * diffZ);
                if (distSq < gen->radiusSq) {
                    dist = sqrtf(distSq);
                    var_s0 = gen->unk1A;
                    if (gen->unk30) {
                        if (diffX < zeroCheck) {
                            var_s0 -= (s32) (diffX * gen->unk20);
                        } else {
                            var_s0 += (s32) (diffX * gen->unk20);
                        }
                    } else if (gen->unk31) {
                        if (diffX < zeroCheck) {
                            var_s0 -= (s32) (diffZ * gen->unk20);
                        } else {
                            var_s0 += (s32) (diffZ * gen->unk20);
                        }
                    } else {
                        var_s0 += (s32) (dist * gen->unk20);
                    }
#ifdef NATIVE_PORT
                    /*
                     * (dist * 65536) / radius is a full-turn angle: the guard
                     * above is distSq < radiusSq, so dist < radius and the
                     * quotient lands in [0, 65536). coss_f takes s16, and
                     * converting a float above 32767 straight to short is
                     * undefined -- UBSan aborts on it with, measured on
                     * Boulder Canyon, "65188.8 is outside the range of
                     * representable values of type 'short'".
                     *
                     * This is not a latent bug that predates this branch in any
                     * observable sense: waves_get_y never reached a generator
                     * before the arena carving was fixed two commits ago, so
                     * this conversion had no live caller on a 64-bit host. The
                     * fix that made the swell real is what exposed it.
                     *
                     * Angles are modulo 65536, so the intended narrowing IS
                     * wraparound. Go through s32 first -- always exact here,
                     * since the value cannot reach 2^31 -- and let the s16
                     * conversion wrap, which is what the original relies on.
                     */
                    diffX = coss_f((s16) (s32) ((dist * 65536.0f) / gen->radius));
#else
                    diffX = coss_f((dist * 65536.0f) / gen->radius);
#endif
                    y += gen->unk24 * sins_f(var_s0) * diffX;
                }
            }
            i++;
        } while (i < 8 && temp_a3->unk0[i] != 0xFF);
    }

#ifdef NATIVE_PORT
    /* MDKR_WAVES_TRACE accounting: how much swell actually reached the surface
     * this frame. gWavesTraceHits counts vertices that fell inside some
     * generator's radius; gWavesTraceMaxY is the largest |contribution| any
     * vertex received. Together they answer the question a screenshot cannot:
     * "is the generator's amplitude reaching rendered geometry at all, or is
     * the swell simply outside the built blocks?" Both are plain counters; the
     * cost when the trace is off is two stores per wave vertex. */
    if (y != 0.0f) {
        f32 magnitude = (y < 0.0f) ? -y : y;
        gWavesTraceHits++;
        if (magnitude > gWavesTraceMaxY) {
            gWavesTraceMaxY = magnitude;
        }
    }
#endif
    return y;
}

/**
 * Removes a wave generator object from the list.
 */
void wavegen_destroy(Object *obj) {
    s32 i;
    s32 k;
    s32 j;
    s32 hasWave;
    unk800E3184 *temp_a1;

    if (gWaveGenList == NULL) {
        return;
    }

    for (i = 0, hasWave = FALSE; i < gWaveGenCount && hasWave == FALSE; i++) {
        if (obj == gWaveGenObjs[i]) {
            hasWave = -1;
        }
    }

    if (hasWave == FALSE) {
        stubbed_printf("\nError :: can not remove a wave swell object which doesn't exist !");
        return;
    }

    i--;
    for (j = 0; j < gWaveTileGridCount; j++) {
        for (k = 0, temp_a1 = &D_800E3184[j]; k < 8 && temp_a1->unk0[k] != 0xFF; k++) {
            if (i != temp_a1->unk0[k]) {
                continue;
            }

            while (k < 7) {
                temp_a1->unk0[k] = temp_a1->unk0[k + 1];
                k++;
            }
            temp_a1->unk0[k] = 0xFF;
            k++;
        }
    }

    /* `i` is the generator slot this call resolved above; `j` is the tile-grid
     * counter left over from the reference-removal loop and indexes nothing in
     * gWaveGenObjs. The slot released here has to be the one the count drop
     * accounts for, or wavegen_register() hands out a slot that is still live. */
    gWaveGenObjs[i] = NULL;
    gWaveGenCount--;
}

/**
 * Adds a wave generator point.
 */
void wavegen_add(Object *obj) {
    LevelObjectEntry_WaveGenerator *waveGen;
    s32 flags;

    waveGen = &obj->level_entry->waveGenerator;
    flags = 0;
    if (waveGen->unk10 != 0) {
        flags = 1;
    }

    if (waveGen->unk11 != 0) {
        flags |= 2;
    }

#ifdef NATIVE_PORT
    {
        /* MDKR_WAVES_TRACE=1 -- names every swell registration with the raw
         * entry fields, POST byte-swap, and reports whether the registration
         * actually took. This is the row that proved Boulder Canyon's generator
         * was being read byte-swapped (waveSize 30977 for 377, unkC 10243 for
         * 808, unkE 26882 for 617). `list=` and `reg=` are what distinguish "the
         * entry is now correct" from "the entry is now correct AND the swell
         * reaches the surface": if gWaveGenList is NULL, or the register call
         * returns NULL, this generator contributes nothing to waves_get_y no
         * matter what its fields say. */
        WaveGen *reg = wavegen_register(obj, obj->trans.x_position, obj->trans.z_position,
                                        (f32) waveGen->waveSize, waveGen->unk9 << 8,
                                        (f32) waveGen->unk8 / 16.0, (f32) waveGen->unkE,
                                        (f32) waveGen->unkC / 16.0, flags);
        if (getenv("MDKR_WAVES_TRACE") != NULL) {
            fprintf(stderr,
                    "[WAVEGEN-ADD] waveSize=%u unk8=%u unk9=%u unkC=%u unkE=%u "
                    "unk10=%u unk11=%u x=%.1f z=%.1f list=%d reg=%d count=%d\n",
                    waveGen->waveSize, waveGen->unk8, waveGen->unk9, waveGen->unkC,
                    waveGen->unkE, waveGen->unk10, waveGen->unk11,
                    obj->trans.x_position, obj->trans.z_position,
                    gWaveGenList != NULL, reg != NULL, gWaveGenCount);
        }
    }
#else
    wavegen_register(obj, obj->trans.x_position, obj->trans.z_position, (f32) waveGen->waveSize, waveGen->unk9 << 8,
                     (f32) waveGen->unk8 / 16.0, (f32) waveGen->unkE, (f32) waveGen->unkC / 16.0, flags);
#endif
}

const char gWavesOverlapErrorMsg[] = "\nError :: more than eight swells overlap on column %d.";

/**
 * Registers a new wave generator point with the given properties.
 */
WaveGen *wavegen_register(Object *obj, f32 xPos, f32 zPos, f32 waveSize, s32 arg4, f32 arg5, f32 arg6, f32 arg7,
                          s32 flags) {
    f32 stepSize;
    s32 emptyWave;
    s32 minX;
    s32 j;
    s32 maxX;
    s32 k;
    s32 i;
    unk800E3184 *temp;
    WaveGen *result;

    result = NULL;
    if (gWaveGenList != NULL) {
        for (i = 0, emptyWave = FALSE; i < 32 && emptyWave == FALSE; i++) {
            if (gWaveGenObjs[i] == NULL) {
                emptyWave = -1; // Why it is -1 is anyone's guess. Though all that's important is that it's nonzero.
            }
        }

        i--;
        if (emptyWave) {
            gWaveGenObjs[i] = obj;
            gWaveGenCount++;
            stepSize = gWaveVtxStepX;
            if (gWaveController.doubleDensity) {
                stepSize /= 2.0f;
            }

            minX = ((xPos - waveSize) - gWaveBlockPosX) / stepSize;
            if (minX >= gWaveTileGridCount) {
                stubbed_printf("\nError :: can not add another wave swell, reached limit of %d.", gWaveTileGridCount);
                return result;
            }

            maxX = ((xPos + waveSize) - gWaveBlockPosX) / stepSize;
            if (maxX < 0) {
                return result;
            }

            if (minX < 0) {
                minX = 0;
            }

            if (maxX >= gWaveTileGridCount) {
                maxX = gWaveTileGridCount - 1;
            }

            for (j = minX; j <= maxX; j++) {
                temp = &D_800E3184[j];
                if (temp->unk0[7] != 0xFF) {
                    continue;
                }

                k = 0;
                while (temp->unk0[k] != 0xFF) {
                    k++;
                }
                temp->unk0[k] = i;
            }
            result = &gWaveGenList[i];
            result->minZ = zPos - waveSize;
            result->maxZ = zPos + waveSize;
            result->x_position = xPos;
            result->z_position = zPos;
            result->radius = waveSize;
            result->index = i;
            result->radiusSq = waveSize * waveSize;
            result->unk1A = arg4;
            if (osTvType == OS_TV_TYPE_PAL) {
                result->unk1C = arg5 * 20971.52; //(f64) (0x80000 / 25.0);
            } else {
                result->unk1C = arg5 * 17476.27; //(f64) ((0x80000 / 1.2) / 25.0);
            }
            result->unk20 = 65536.0f / arg6;
            result->unk24 = arg7;
            result->unk28 = arg5;
            result->unk30 = flags & 1;
            result->unk31 = flags & 2;
            result->unk2C = arg6;
        }
    }

    return result;
}

UNUSED void func_800BF9F8(WaveGen *gen, f32 arg1, f32 arg2) {
    UNUSED s32 pad[4];
    s32 sp1C;
    f32 var_f0;
    s32 var_a1;
    s32 i;
    s32 var_a2;
    s32 j;
    s32 iteration;
    s32 index;
    u8 *var_a2_2;

    if (gen == NULL) {
        return;
    }

    var_f0 = gWaveVtxStepX;
    iteration = 0;
    if (gWaveController.doubleDensity) {
        var_f0 /= 2.0f;
    }
    index = gen->index;
    while (iteration != 2) {
        var_a1 = TRUE;
        var_a2 = ((gen->x_position - gen->radius) - gWaveBlockPosX) / var_f0;
        if (var_a2 >= gWaveTileGridCount) {
            var_a1 = FALSE;
        } else if (var_a2 < 0) {
            var_a2 = 0;
        }

        if (var_a1) {
            sp1C = ((gen->x_position + gen->radius) - gWaveBlockPosX) / var_f0;
            if (sp1C < 0) {
                var_a1 = FALSE;
            } else if (sp1C >= gWaveTileGridCount) {
                sp1C = gWaveTileGridCount - 1;
            }
        }

        if (var_a1) {
            for (i = var_a2; i <= sp1C; i++) {
                var_a2_2 = &D_800E3184[i].unk0[0];
                j = 0;
                if (iteration != 0) {
                    if (var_a2_2[7] == 0xFF) {
                        while (var_a2_2[0] != 0xFF) {
                            var_a2_2++;
                        }
                        var_a2_2[0] = index;
                    }
                } else {
                    while (j < 8) {
                        if (index == var_a2_2[j]) {
                            while (j < 7) {
                                var_a2_2[j] = var_a2_2[j + 1];
                                j++;
                            }
                            var_a2_2[j] = 0xFF;
                            j++;
                        }
                        j++;
                    }
                }
            }
        }

        if (iteration == 0) {
            gen->x_position += arg1;
            gen->z_position += arg2;
            gen->minZ += arg2;
            gen->maxZ += arg2;
        }
        iteration++;
    }
}

UNUSED void wavegen_scale(WaveGen *gen, f32 radiusAdd, f32 arg2, f32 arg3, f32 arg4) {
    if (gen == NULL) {
        return;
    }

    gen->radius += radiusAdd;
    if (gen->radius < 1.0) {
        gen->radius = 1.0f;
    }

    gen->minZ = (gen->z_position - gen->radius);
    gen->maxZ = (gen->z_position + gen->radius);
    gen->unk28 += arg2;
    if (osTvType == OS_TV_TYPE_PAL) {
        gen->unk1C = gen->unk28 * 20971.52; //(f64) (0x80000 / 25.0);
    } else {
        gen->unk1C = gen->unk28 * 17476.27; //(f64) ((0x80000 / 1.2) / 25.0);
    }

    gen->unk2C = gen->unk2C + arg3;
    if (gen->unk2C < 1.0) {
        gen->unk2C = 1.0f;
    }

    gen->unk20 = 65536.0f / gen->unk2C; // 0x10000
    gen->unk24 += arg4;
}

void func_800BFE98(s32 updateRate) {
    s32 i;

    for (i = 0; i < 32; i++) {
        if (gWaveGenObjs[i] != NULL) {
            gWaveGenList[i].unk1A += (0, gWaveGenList[i].unk1C * updateRate) >> 4;
        }
    }
}

/**
 * Wave Power loop func.
 * Waits for racers to pass through, then sets itself as the current baseline for how strong the waves should be.
 */
void obj_loop_wavepower(Object *obj) {
    LevelObjectEntry_WavePower *entry;
    s32 numRacers;
    Object *racerObj;
    Object_Racer *racer;
    Object **racers;
    s32 i;
    f32 diffY;
    f32 diffZ;
    f32 diffX;
    f32 distance;

    if (obj == gWaveGeneratorObj) {
        return;
    }

    racers = get_racer_objects(&numRacers);
    if (numRacers <= 0) {
        return;
    }

    racerObj = NULL;
    for (i = 0; i < numRacers && racerObj == NULL; i++) {
        racer = racers[i]->racer;
        if (racer->playerIndex == PLAYER_ONE) {
            racerObj = racers[i];
        }
    }

    if (racerObj == NULL) {
        return;
    }

    entry = &obj->level_entry->wavePower;
    distance = entry->radius;
    distance *= distance;
    diffX = racerObj->trans.x_position - obj->trans.x_position;
    diffY = racerObj->trans.y_position - obj->trans.y_position;
    diffZ = racerObj->trans.z_position - obj->trans.z_position;
    if ((diffX * diffX) + (diffY * diffY) + (diffZ * diffZ) < distance) {
        gWavePowerBase = entry->power / 256.0f;
        gWaveMagnitude = (gWavePowerBase - gWaveController.magnitude) / (f32) entry->divisor;
        gWavePowerDivisor = entry->divisor;
        gWaveGeneratorObj = obj;
#ifdef NATIVE_PORT
        /* MDKR_WAVES_TRACE=1 -- names the wavepower trigger's post-swap entry
         * values the moment it arms the magnitude ramp. */
        if (getenv("MDKR_WAVES_TRACE") != NULL) {
            fprintf(stderr,
                    "[WAVEPOWER-HIT] radius=%u power=%u divisor=%u -> base=%.4f\n",
                    entry->radius, entry->power, entry->divisor, gWavePowerBase);
        }
#endif
    }
}
