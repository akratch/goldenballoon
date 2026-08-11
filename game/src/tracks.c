#include "tracks.h"

#include "libultra/src/libc/rmonPrintf.h"

#include "asset_loading.h"
#ifdef NATIVE_PORT
#include <math.h>

#include "asset_swap.h"
#include "camera_obstruction_runtime.h"
#include "camera_obstruction.h"
#include "camera_obstruction_query.h"
#include "enh_draw_distance.h"
#include "fast3d/gfx_presentation_packet.h"
#include "gfx_shadow_frame.h"
#include "mdkr_bounds.h"
#include "platform_os.h"
#include "present_sched.h"
#include "presentation_snapshot.h"
#include "taj_visual.h"
#include "wizpig_visual.h"
#include "terry_visual.h"
#include "viewport_route_cache.h"
#endif
#include "camera.h"
#include "collision.h"
#include "common.h"
#include "f3ddkr.h"
#include "fade_transition.h"
#include "game.h"
#include "game_ui.h"
#include "gzip.h"
#include "macros.h"
#include "math_util.h"
#include "memory.h"
#include "menu.h"
#include "objects.h"
#include "particles.h"
#include "PR/gu.h"
#include "PRinternal/viint.h"
#include "racer.h"
#include "structs.h"
#include "textures_sprites.h"
#include "thread3_main.h"
#include "types.h"
#include "video.h"
#include "waves.h"
#include "weather.h"

#ifdef NATIVE_PORT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

// Maximum size for a level model is 522.5 KiB
#define LEVEL_MODEL_MAX_SIZE 0x82A00
#define LEVEL_SEGMENT_MAX 128
#define SHADOW_HEAP_DATA_CAPACITY 400
#define SHADOW_HEAP_TRI_CAPACITY 800
#define SHADOW_HEAP_VERTEX_CAPACITY 2000
#ifdef NATIVE_PORT
_Static_assert(SHADOW_HEAP_TRI_CAPACITY <= 32767 &&
                   SHADOW_HEAP_VERTEX_CAPACITY <= 32767,
               "ShadowHeapProperties stores geometry offsets in signed 16-bit fields");
#endif

#define FLAGS_8002E904 (RENDER_HIDDEN | RENDER_DECAL | RENDER_WATER | RENDER_NO_SHADOW)

#ifdef NATIVE_PORT
/* A composed Taj uses the donor only for simulation. Its vehicle model,
 * shadow, and wake must disappear as one presentation transaction. */
#define TAJ_DONOR_PRESENTATION_VISIBLE(obj)                               \
    (!taj_visual_suppress_donor_draw(obj) &&                              \
     !wizpig_visual_suppress_donor_draw(obj) &&                            \
     !terry_visual_suppress_donor_draw(obj))

static uint64_t shadow_topology_hash_bytes(
    uint64_t hash, const void *bytes, size_t size) {
    const uint8_t *cursor = (const uint8_t *)bytes;

    for (size_t index = 0; index < size; index++) {
        hash ^= cursor[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t shadow_batch_topology_signature(
    const TextureHeader *texture, s32 numVerts, s32 numTris,
    const Triangle *triangles) {
    uint64_t hash = UINT64_C(1469598103934665603);
    /* A host address, deliberately: this signature only has to distinguish
     * batches WITHIN one process's run so replay can pair them. It is therefore
     * never stable across runs (ASLR, different allocation order) and must
     * never be persisted, logged as an identity, or compared against a value
     * from another process. */
    uintptr_t textureIdentity = (uintptr_t)texture;

    hash = shadow_topology_hash_bytes(
        hash, &textureIdentity, sizeof(textureIdentity));
    hash = shadow_topology_hash_bytes(hash, &numVerts, sizeof(numVerts));
    hash = shadow_topology_hash_bytes(hash, &numTris, sizeof(numTris));
    if (triangles != NULL && numTris > 0) {
        for (s32 index = 0; index < numTris; index++) {
            /* UVs legitimately slide as the shadow moves across a receiver;
             * they are held discrete by replay and are not topology. */
            hash = shadow_topology_hash_bytes(
                hash, triangles[index].verticesArray,
                sizeof(triangles[index].verticesArray));
        }
    }
    return hash;
}
#endif

/************ .data ************/

s32 D_800DC870 = 0; // Currently unknown, might be a different type.
//!@bug These two transition effects are marked to not clear when done, meaning they stay active the whole time.
FadeTransition gFullFadeToBlack = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 40, FADE_STAY);
FadeTransition gCircleFadeToBlack = FADE_TRANSITION(FADE_CIRCLE, FADE_FLAG_OUT, FADE_COLOR_BLACK, 70, FADE_STAY);

f32 D_800DC884[10] = { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f };

Vec3f D_800DC8AC[3][3] = {
    { { { { 50.0f, 0.0f, 32.0f } } }, { { { -50.0f, 0.0f, 32.0f } } }, { { { -50.0f, 100.0f, 32.0f } } } },
    { { { { 0.0f, 0.0f, 32.0f } } }, { { { 130.0f, 60.0f, -68.0f } } }, { { { 130.0f, -60.0f, -68.0f } } } },
    { { { { 0.0f, 0.0f, 32.0f } } }, { { { -130.0f, -60.0f, -68.0f } } }, { { { -130.0f, 60.0f, -68.0f } } } },
};

LevelModel *gCurrentLevelModel = NULL; // Official Name: track
LevelHeader *gCurrentLevelHeader2 = NULL;

s32 D_800DC920 = -1;
#if REGION == REGION_JP
// T.T.カメラ  -  T.T. Camera
char gJpnTTCam[] = { 0x80, 0x2D, 0x80, 0x3C, 0x80, 0x2D, 0x80, 0x3C, 0x80, 0x55, 0x80, 0x71, 0x80, 0x76 };
#endif
u8 *gVoidData = NULL;
s32 D_800DC928 = 0; // Currently unknown, might be a different type.

u8 D_800DC92C[24] = {
    0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4, 5,
    6, 1, 1, 0, 5, 3, 2, 7, 7, 8, 3
    // There may or may not be extra zeroes here.
};

/*******************************/

/************ .bss ************/

Gfx *gTrackDL;
Mtx *gTrackMtxPtr;
Vertex *gTrackVtxPtr;
Triangle *gTrackTriPtr;

Camera *gSceneActiveCamera;

s32 gSceneCurrentPlayerID;
Object *gSkydomeSegment;
UNUSED s32 gIsNearCurrBBox; // Set to true if the current visible segment is close to the camera.
UNUSED s32 D_8011B0C0;      // Set to 0 then never read.
UNUSED s32 gDisableShadows; // Never not 0.
s32 gShadowHeapFlip;        // Flips between 0 and 1 to prevent incorrect access between frames.
s32 gShadowIndex;
s32 gWaterEffectIndex;
s32 gSceneStartSegment;
s32 D_8011B0D8;
s32 gSceneRenderSkyDome;
s8 gDrawLevelSegments;
u8 gVoidColourR; // R of RGB
u8 gVoidColourG; // G of RGB
u8 gVoidColourB; // B of RGB
f32 gCollisionNormalX;
f32 gCollisionNormalY;
f32 gCollisionNormalZ;
s32 gHitWall;
s32 gCollisionMode;
s32 D_8011B0F8; // gIsInCutscene?
s32 gAntiAliasing;
s32 gTTCamPlayerID;
s32 gTTCamID;
s32 gTTCamSmoothTimer;
#ifdef NATIVE_PORT
/* TT-camera spectate point. ttcam_update() used to keep this choice on the RACER
 * -- `racer->cameraIndex` -- and rewrite it for every racer, every frame, from
 * inside render_scene's fourth viewport. Object_Racer is simulation state, so
 * that made a per-frame render write into the authoritative object graph.
 *
 * The field is genuine cross-frame state, not a dead write: ttcam_update reads it
 * back itself, three lines later and again on the next frame (spectate_object(),
 * the gTTCamID comparison, and the gTTCamID store). It just belongs to the TT
 * camera, not to the racer. Deleting the write would break spectate-point
 * selection; holding it here does not, because gRacers is filled at level load
 * and never permuted (gRacersByPosition is the sorted view), so slot i names the
 * same racer for the whole level. Reset with the rest of the TT-cam state in
 * init_track(). racer.c's own `cameraIndex = 0` at racer init is left alone.
 *
 * gTTCamSmoothTimer above stays where it is and keeps its RAW, non-pause-gated
 * rate. */
s32 gTTCamSpectateIndex[10];
#endif
s32 D_8011B10C;
s32 gTrackTexAnimOffset;
u32 gTrackTexAnimFlags;
s32 D_8011B118;
s32 D_8011B11C;
unk8011B120 D_8011B120[32]; // Struct sizeof(0x10) / sizeof(16)
s32 D_8011B320[4];
#ifdef NATIVE_PORT
/*
 * func_8002FF6C addresses four 32-entry slices (index = plane*32 + slot).
 * The decomp's 120-entry declaration leaves plane 3 slots 24..31 outside the
 * C object. Preserve the matching declaration, but make the native C storage
 * match the indexing contract.
 */
unk8011B330 D_8011B330[4 * 32];
_Static_assert(ARRAY_COUNT(D_8011B330) == ARRAY_COUNT(D_8011B320) * 32,
               "shadow clipping requires four complete 32-entry slices");
#else
unk8011B330 D_8011B330[120]; // Struct sizeof(0x20) / sizeof(32)
#endif
s32 D_8011C230;
s32 D_8011C234;
unk8011C238 D_8011C238[32]; // Struct sizeof(0xC) / sizeof(12)
unk8011C3B8 D_8011C3B8[64];
unk8011C8B8 D_8011C8B8[128];
s32 D_8011D0B8;
unk8011C8B8 *D_8011D0BC;
TextureHeader *gNewShadowTexture;
Object *gNewShadowObj;
f32 D_8011D0C8;
s16 gNewShadowY1;
s16 gNewShadowY2;
s16 D_8011D0D0;
f32 gShadowOpacity;
f32 gNewShadowScale;
f32 gNewShadowWidth;
f32 gNewShadowLength;
f32 D_8011D0E4;
s32 D_8011D0E8;
s32 D_8011D0EC;
f32 D_8011D0F0;
f32 D_8011D0F4;
Vec4f D_8011D0F8[3];
#ifdef NATIVE_PORT
/*
 * The modern lens may need a wider level-geometry frustum, but DKR's logical
 * object admission still drives Object_Racer.unk201 in the fixed-step
 * visibility prepass. Keep the original N64 planes for that faithful object
 * contract; only raw track geometry uses the wider plane set.
 */
static Vec4f sFaithfulCullPlanes[3];
#endif
#ifdef NATIVE_PORT
/* The visibility prepass and rendering need to mask this flag locally, and
 * camera.c exposes only a getter and a clearing setter. */
extern s8 gCutsceneCameraActive;

#ifdef NATIVE_PORT
/* MDKR_COLLALLOC boundary control (platform/stubs_dkr.c). Declared here the same
 * way game/src/hasm/collision.c declares mdkr_coll_cap(): the hook lives on the
 * platform side and only this translation unit sizes the arrays. */
extern int  mdkr_coll_alloc_cells(int romCap);
extern int  mdkr_coll_alloc_canary_slack(void);
extern void mdkr_coll_canary_arm(void *candidates, void *surfaces, int index);
#endif
#endif
WaterProperties D_8011D128[20];
WaterProperties *gTrackWaves[20];
s8 D_8011D308;
LevelModel *gTrackModelHeap;
s32 *gLevelModelTable;
UNUSED f32 gPrevCameraX;          // Set but never read
UNUSED f32 gPrevCameraY;          // Set but never read
UNUSED f32 gPrevCameraZ;          // Set but never read
Triangle *gShadowHeapTris[2 + 2]; // Triangle Data for shadows
Triangle *gCurrShadowTris;
UNUSED s32 D_8011D334;
Vertex *gShadowHeapVerts[2 + 2]; // Vertex Data for shadows
Vertex *gCurrShadowVerts;
UNUSED s32 D_8011D34C;
ShadowHeapProperties *gShadowHeapData[2 + 2]; // General data for shadows. Texture and geometry size.
ShadowHeapProperties *gCurrShadowHeapData;
s32 gShadowTail;           // Position in the heap the shadow data ends at.
s32 gNewShadowTriCount;    // xOffset?
s32 gNewShadowVtxCount;    // yOffset?
#ifdef NATIVE_PORT
static s32 gShadowBuildOverflow;
static s32 gShadowPeakDataCount;
static s32 gShadowPeakTriCount;
static s32 gShadowPeakVtxCount;
static s32 gShadowOverflowDrops;
static s32 gShadowEmptyMeshes;
static s32 gShadowDrawGroups;
static s32 gShadowNonDecalDrawGroups;
static s32 gShadowDataCap = SHADOW_HEAP_DATA_CAPACITY;
static s32 gShadowTriCap = SHADOW_HEAP_TRI_CAPACITY;
static s32 gShadowVtxCap = SHADOW_HEAP_VERTEX_CAPACITY;
#endif
s32 *gCollisionCandidates; // Allocated 0x7D0
s8 *gCollisionSurfaces;
s32 gNumCollisionCandidates;
s32 gScenePlayerViewports;
UNUSED f32 gCurrBBoxDistanceToCamera; // Used in a comparison check, but functionally unused.
u32 gWaveBlockCount;
FogData gFogData[4];
Vec3i gScenePerspectivePos;
VoidMesh *gVoidMesh;     // 0x10 bytes struct?
unk8011D478 *D_8011D478; // 0xC bytes struct?
s8 *D_8011D47C;
Vertex *gVoidVerts[2];
Vertex *gVoidCurrVerts;
s32 D_8011D48C;
Triangle *gVoidTris[2];
Triangle *gVoidCurrTris;
s16 D_8011D49C;
s16 D_8011D49E;
f32 D_8011D4A0; // something x coordinate related
f32 D_8011D4A4; // something z coordinate related
f32 D_8011D4A8;
f32 D_8011D4AC; // something x coordinate related
f32 D_8011D4B0; // something z coordinate related
s8 gVoidVertexFlip;
s16 gVoidVertCount;
s16 gVoidPrimCount;
s16 D_8011D4BA;
s16 gVoidPrimLimit;

/******************************/

#ifdef NATIVE_PORT
static void shadow_refresh_caps(void) {
    gShadowDataCap = mdkr_shadow_cap(0, SHADOW_HEAP_DATA_CAPACITY);
    gShadowTriCap = mdkr_shadow_cap(1, SHADOW_HEAP_TRI_CAPACITY);
    gShadowVtxCap = mdkr_shadow_cap(2, SHADOW_HEAP_VERTEX_CAPACITY);
}

static void shadow_note_high_water(void) {
    if (gShadowTail > gShadowPeakDataCount) {
        gShadowPeakDataCount = gShadowTail;
    }
    if (gNewShadowTriCount > gShadowPeakTriCount) {
        gShadowPeakTriCount = gNewShadowTriCount;
    }
    if (gNewShadowVtxCount > gShadowPeakVtxCount) {
        gShadowPeakVtxCount = gNewShadowVtxCount;
    }
}

static s32 shadow_mesh_range_valid(s32 start, s32 end) {
    /*
     * end is the terminal descriptor index, so it may equal dataCap-1 and every
     * group dereference of i+1 remains inside the allocation.
     */
    return start >= 0 && end > start && end < gShadowDataCap;
}

void mdkr_shadow_stats(s32 *dataPeak, s32 *triPeak, s32 *vtxPeak,
                       s32 *overflowDrops, s32 *emptyMeshes,
                       s32 *drawGroups, s32 *nonDecalDrawGroups,
                       s32 *dataCap, s32 *triCap, s32 *vtxCap) {
    if (dataPeak != NULL) *dataPeak = gShadowPeakDataCount;
    if (triPeak != NULL) *triPeak = gShadowPeakTriCount;
    if (vtxPeak != NULL) *vtxPeak = gShadowPeakVtxCount;
    if (overflowDrops != NULL) *overflowDrops = gShadowOverflowDrops;
    if (emptyMeshes != NULL) *emptyMeshes = gShadowEmptyMeshes;
    if (drawGroups != NULL) *drawGroups = gShadowDrawGroups;
    if (nonDecalDrawGroups != NULL) *nonDecalDrawGroups = gShadowNonDecalDrawGroups;
    if (dataCap != NULL) *dataCap = gShadowDataCap;
    if (triCap != NULL) *triCap = gShadowTriCap;
    if (vtxCap != NULL) *vtxCap = gShadowVtxCap;
}

/*
 * CAM-03 static visual-occlusion cache.
 *
 * This is intentionally separate from collision.c's shared candidate list:
 * camera booms may span arbitrary level segments, so a first-N segment list or
 * shared scratch array is not a valid broad phase.  Geometry is copied from the
 * finalized level model once, in segment/batch/face asset order, then never
 * mutated.  The copied vertices also allow the cache to outlive harmless model
 * bookkeeping changes during a loaded level without giving the query a route to
 * gameplay state.
 */
typedef struct MdkrTrackOcclusionSegment {
    MdkrCameraVec3 minimum;
    MdkrCameraVec3 maximum;
    size_t triangle_offset;
    size_t triangle_count;
    size_t chunk_offset;
    size_t chunk_count;
    uint8_t valid;
} MdkrTrackOcclusionSegment;

typedef struct MdkrTrackOcclusionChunk {
    MdkrCameraVec3 minimum;
    MdkrCameraVec3 maximum;
    size_t triangle_offset;
    size_t triangle_count;
    uint8_t valid;
} MdkrTrackOcclusionChunk;

#define MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES 1U

typedef struct MdkrTrackOcclusionCache {
    MdkrCameraOcclusionWorld world;
    MdkrCameraVec3 *vertices;
    MdkrTrackOcclusionSegment *segments;
    MdkrTrackOcclusionChunk *chunks;
    uint32_t *indices;
    MdkrCameraOcclusionTriangle *triangles;
    MdkrTrackOcclusionTelemetry telemetry;
    uint8_t built;
} MdkrTrackOcclusionCache;

static MdkrTrackOcclusionCache sTrackOcclusionCache;

static int mdkr_track_occlusion_size_mul(size_t count, size_t item_size, size_t *out) {
    if (out == NULL || (count != 0 && item_size > SIZE_MAX / count)) {
        return 0;
    }
    *out = count * item_size;
    return 1;
}

static int mdkr_track_occlusion_triangle_degenerate(
    MdkrCameraVec3 a, MdkrCameraVec3 b, MdkrCameraVec3 c) {
    const double abx = (double)b.x - a.x;
    const double aby = (double)b.y - a.y;
    const double abz = (double)b.z - a.z;
    const double acx = (double)c.x - a.x;
    const double acy = (double)c.y - a.y;
    const double acz = (double)c.z - a.z;
    const double bcx = (double)c.x - b.x;
    const double bcy = (double)c.y - b.y;
    const double bcz = (double)c.z - b.z;
    const double cross_x = aby * acz - abz * acy;
    const double cross_y = abz * acx - abx * acz;
    const double cross_z = abx * acy - aby * acx;
    const double ab2 = abx * abx + aby * aby + abz * abz;
    const double ac2 = acx * acx + acy * acy + acz * acz;
    const double bc2 = bcx * bcx + bcy * bcy + bcz * bcz;
    const double max_edge2 = fmax(ab2, fmax(ac2, bc2));
    const double cross2 = cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;

    return !isfinite(max_edge2) || !isfinite(cross2) || max_edge2 <= 0.0 ||
           cross2 <= 1.0e-24 * max_edge2 * max_edge2;
}

static void mdkr_track_occlusion_expand_aabb(
    MdkrTrackOcclusionSegment *segment, MdkrCameraVec3 point) {
    if (!segment->valid) {
        segment->minimum = point;
        segment->maximum = point;
        segment->valid = TRUE;
        return;
    }
    if (point.x < segment->minimum.x) segment->minimum.x = point.x;
    if (point.y < segment->minimum.y) segment->minimum.y = point.y;
    if (point.z < segment->minimum.z) segment->minimum.z = point.z;
    if (point.x > segment->maximum.x) segment->maximum.x = point.x;
    if (point.y > segment->maximum.y) segment->maximum.y = point.y;
    if (point.z > segment->maximum.z) segment->maximum.z = point.z;
}

static void mdkr_track_occlusion_expand_chunk_aabb(
    MdkrTrackOcclusionChunk *chunk, MdkrCameraVec3 point) {
    if (!chunk->valid) {
        chunk->minimum = point;
        chunk->maximum = point;
        chunk->valid = TRUE;
        return;
    }
    if (point.x < chunk->minimum.x) chunk->minimum.x = point.x;
    if (point.y < chunk->minimum.y) chunk->minimum.y = point.y;
    if (point.z < chunk->minimum.z) chunk->minimum.z = point.z;
    if (point.x > chunk->maximum.x) chunk->maximum.x = point.x;
    if (point.y > chunk->maximum.y) chunk->maximum.y = point.y;
    if (point.z > chunk->maximum.z) chunk->maximum.z = point.z;
}

/* Return nonzero only for material evidence that unambiguously means a camera
 * should pass through. Cutout and vertex-alpha content deliberately remain hard
 * until CAM-08 gives individual assets an explicit soft-occluder policy. */
static int mdkr_track_occlusion_batch_nonblocking(
    const TriangleBatchInfo *batch,
    const TextureInfo *textures,
    s32 texture_count,
    MdkrTrackOcclusionTelemetry *telemetry) {
    const u32 flags = batch->flags;

    if (flags & (RENDER_HIDDEN | RENDER_WATER | RENDER_DECAL | RENDER_SEMI_TRANSPARENT)) {
        return TRUE;
    }
    if (batch->textureIndex != 0xFF) {
        if (batch->textureIndex >= texture_count) {
            /* The renderer cannot classify this safely; keep it hard and make
             * it visible in telemetry instead of guessing that it is air. */
            telemetry->unknown_policy_triangle_count++;
            return FALSE;
        }
        if (DKR_PTR(TextureHeader, textures[batch->textureIndex].texture)->flags &
            RENDER_SEMI_TRANSPARENT) {
            return TRUE;
        }
    }
    if (flags & (RENDER_CUTOUT | RENDER_VTX_ALPHA)) {
        telemetry->unknown_policy_triangle_count++;
    }
    return FALSE;
}

static void mdkr_track_occlusion_cache_free(void) {
    free(sTrackOcclusionCache.vertices);
    free(sTrackOcclusionCache.indices);
    free(sTrackOcclusionCache.triangles);
    free(sTrackOcclusionCache.segments);
    free(sTrackOcclusionCache.chunks);
    memset(&sTrackOcclusionCache, 0, sizeof(sTrackOcclusionCache));
}

static void mdkr_track_occlusion_cache_fail(const char *reason) {
    fprintf(stderr, "[FATAL] camera track-occlusion cache: %s\n", reason);
    mdkr_track_occlusion_cache_free();
    abort();
}

static void mdkr_track_occlusion_cache_build(void) {
    LevelModelSegment *level_segments;
    TextureInfo *textures;
    size_t vertex_count = 0;
    size_t triangle_capacity = 0;
    size_t vertex_bytes = 0;
    size_t index_bytes = 0;
    size_t triangle_bytes = 0;
    size_t segment_bytes = 0;
    size_t chunk_capacity = 0;
    size_t chunk_bytes = 0;
    size_t chunk_count = 0;
    size_t triangle_count = 0;
    size_t vertex_base = 0;
    uint32_t source_stable_id = 0;
    s32 segment_index;
    uint64_t build_started;

    mdkr_track_occlusion_cache_free();
    build_started = 0U;
    {
        const char *perf_value = getenv("MDKR_CAMERA_PERF");
        if (perf_value != NULL && perf_value[0] != '\0' && perf_value[0] != '0') {
            build_started = platform_perf_monotonic_ns();
        }
    }
    if (gCurrentLevelModel == NULL || gCurrentLevelModel->numberOfSegments <= 0) {
        mdkr_track_occlusion_cache_fail("no finalized level model");
    }

    level_segments = DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments);
    textures = DKR_PTR(TextureInfo, gCurrentLevelModel->textures);
    for (segment_index = 0; segment_index < gCurrentLevelModel->numberOfSegments; segment_index++) {
        const LevelModelSegment *segment = &level_segments[segment_index];
        if (segment->numberOfVertices < 0 || segment->numberOfTriangles < 0 ||
            segment->numberOfBatches < 0 ||
            (size_t)segment->numberOfVertices > SIZE_MAX - vertex_count ||
            (size_t)segment->numberOfTriangles > SIZE_MAX - triangle_capacity) {
            mdkr_track_occlusion_cache_fail("invalid segment count");
        }
        vertex_count += (size_t)segment->numberOfVertices;
        triangle_capacity += (size_t)segment->numberOfTriangles;
    }
    if (vertex_count > UINT32_MAX) {
        mdkr_track_occlusion_cache_fail("vertex index space exhausted");
    }
    if (!mdkr_track_occlusion_size_mul(vertex_count, sizeof(MdkrCameraVec3), &vertex_bytes) ||
        !mdkr_track_occlusion_size_mul(triangle_capacity, 3U * sizeof(uint32_t), &index_bytes) ||
        !mdkr_track_occlusion_size_mul(triangle_capacity, sizeof(MdkrCameraOcclusionTriangle), &triangle_bytes) ||
        !mdkr_track_occlusion_size_mul((size_t)gCurrentLevelModel->numberOfSegments,
                                       sizeof(MdkrTrackOcclusionSegment), &segment_bytes) ||
        triangle_capacity > SIZE_MAX - (MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES - 1U) ||
        (triangle_capacity + MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES - 1U) /
                MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES >
            SIZE_MAX - (size_t)gCurrentLevelModel->numberOfSegments) {
        mdkr_track_occlusion_cache_fail("allocation size overflow");
    }
    chunk_capacity =
        (triangle_capacity + MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES - 1U) /
            MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES +
        (size_t)gCurrentLevelModel->numberOfSegments;
    if (!mdkr_track_occlusion_size_mul(
            chunk_capacity, sizeof(MdkrTrackOcclusionChunk), &chunk_bytes)) {
        mdkr_track_occlusion_cache_fail("chunk allocation size overflow");
    }
    if (vertex_bytes > SIZE_MAX - index_bytes ||
        vertex_bytes + index_bytes > SIZE_MAX - triangle_bytes ||
        vertex_bytes + index_bytes + triangle_bytes > SIZE_MAX - segment_bytes ||
        vertex_bytes + index_bytes + triangle_bytes + segment_bytes > SIZE_MAX - chunk_bytes) {
        mdkr_track_occlusion_cache_fail("telemetry byte count overflow");
    }

    sTrackOcclusionCache.vertices = malloc(vertex_bytes);
    sTrackOcclusionCache.indices = malloc(index_bytes);
    sTrackOcclusionCache.triangles = malloc(triangle_bytes);
    sTrackOcclusionCache.segments = calloc(1, segment_bytes);
    sTrackOcclusionCache.chunks = calloc(1, chunk_bytes);
    if ((vertex_bytes != 0 && sTrackOcclusionCache.vertices == NULL) ||
        (index_bytes != 0 && sTrackOcclusionCache.indices == NULL) ||
        (triangle_bytes != 0 && sTrackOcclusionCache.triangles == NULL) ||
        sTrackOcclusionCache.segments == NULL ||
        (chunk_bytes != 0 && sTrackOcclusionCache.chunks == NULL)) {
        mdkr_track_occlusion_cache_fail("allocation failed");
    }

    for (segment_index = 0; segment_index < gCurrentLevelModel->numberOfSegments; segment_index++) {
        const LevelModelSegment *segment = &level_segments[segment_index];
        const Vertex *source_vertices = DKR_PTR(Vertex, segment->vertices);
        const Triangle *source_triangles = DKR_PTR(Triangle, segment->triangles);
        const TriangleBatchInfo *batches = DKR_PTR(TriangleBatchInfo, segment->batches);
        MdkrTrackOcclusionSegment *out_segment = &sTrackOcclusionCache.segments[segment_index];
        s32 vertex_index;
        s32 batch_index;

        out_segment->triangle_offset = triangle_count;
        for (vertex_index = 0; vertex_index < segment->numberOfVertices; vertex_index++) {
            const Vertex *source = &source_vertices[vertex_index];
            sTrackOcclusionCache.vertices[vertex_base + (size_t)vertex_index] =
                (MdkrCameraVec3) { (float)source->x, (float)source->y, (float)source->z };
        }
        for (batch_index = 0; batch_index < segment->numberOfBatches; batch_index++) {
            const TriangleBatchInfo *batch = &batches[batch_index];
            const TriangleBatchInfo *next_batch = &batches[batch_index + 1];
            const s32 face_start = batch->facesOffset;
            const s32 face_end = next_batch->facesOffset;
            const s32 vertex_start = batch->verticesOffset;
            const s32 vertex_end = next_batch->verticesOffset;
            s32 face_index;

            if (face_start < 0 || face_end < face_start || face_end > segment->numberOfTriangles ||
                vertex_start < 0 || vertex_end < vertex_start || vertex_end > segment->numberOfVertices) {
                sTrackOcclusionCache.telemetry.malformed_batch_count++;
                continue;
            }
            for (face_index = face_start; face_index < face_end; face_index++) {
                const Triangle *source = &source_triangles[face_index];
                const size_t index_offset = triangle_count * 3U;
                uint32_t local_indices[3];
                MdkrCameraVec3 a;
                MdkrCameraVec3 b;
                MdkrCameraVec3 c;

                if (source_stable_id == UINT32_MAX) {
                    mdkr_track_occlusion_cache_fail("stable-ID space exhausted");
                }
                source_stable_id++;
                local_indices[0] = (uint32_t)vertex_start + source->vi0;
                local_indices[1] = (uint32_t)vertex_start + source->vi1;
                local_indices[2] = (uint32_t)vertex_start + source->vi2;
                if (local_indices[0] >= (uint32_t)vertex_end ||
                    local_indices[1] >= (uint32_t)vertex_end ||
                    local_indices[2] >= (uint32_t)vertex_end) {
                    sTrackOcclusionCache.telemetry.rejected_triangle_count++;
                    continue;
                }
                a = sTrackOcclusionCache.vertices[vertex_base + local_indices[0]];
                b = sTrackOcclusionCache.vertices[vertex_base + local_indices[1]];
                c = sTrackOcclusionCache.vertices[vertex_base + local_indices[2]];
                if (mdkr_track_occlusion_triangle_degenerate(a, b, c)) {
                    sTrackOcclusionCache.telemetry.rejected_triangle_count++;
                    continue;
                }
                /* Validate every source face before applying material policy:
                 * a translucent/decal tag cannot turn malformed geometry into
                 * harmless input that silently escapes the telemetry census. */
                if (mdkr_track_occlusion_batch_nonblocking(
                        batch, textures, gCurrentLevelModel->numberOfTextures,
                        &sTrackOcclusionCache.telemetry)) {
                    sTrackOcclusionCache.telemetry.nonblocking_triangle_count++;
                    continue;
                }
                /* Gameplay collision is deliberately not our authority: an
                 * opaque visual batch can opt out of vehicle collision and
                 * still be a wall the camera must not enter. */
                if (batch->flags & RENDER_NO_COLLISION) {
                    sTrackOcclusionCache.telemetry.visual_no_collision_hard_triangle_count++;
                }
                sTrackOcclusionCache.indices[index_offset] = (uint32_t)(vertex_base + local_indices[0]);
                sTrackOcclusionCache.indices[index_offset + 1U] = (uint32_t)(vertex_base + local_indices[1]);
                sTrackOcclusionCache.indices[index_offset + 2U] = (uint32_t)(vertex_base + local_indices[2]);
                sTrackOcclusionCache.triangles[triangle_count] =
                    (MdkrCameraOcclusionTriangle) {
                        source_stable_id,
                        MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK,
                        MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK,
                        0,
                    };
                mdkr_track_occlusion_expand_aabb(out_segment, a);
                mdkr_track_occlusion_expand_aabb(out_segment, b);
                mdkr_track_occlusion_expand_aabb(out_segment, c);
                triangle_count++;
            }
        }
        out_segment->triangle_count = triangle_count - out_segment->triangle_offset;
        out_segment->chunk_offset = chunk_count;
        if (out_segment->triangle_count != 0U) {
            size_t chunk_triangle_offset;
            const size_t segment_triangle_end =
                out_segment->triangle_offset + out_segment->triangle_count;

            for (chunk_triangle_offset = out_segment->triangle_offset;
                 chunk_triangle_offset < segment_triangle_end;
                 chunk_triangle_offset += MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES) {
                MdkrTrackOcclusionChunk *chunk;
                size_t chunk_triangle_end =
                    chunk_triangle_offset + MDKR_TRACK_OCCLUSION_CHUNK_TRIANGLES;
                size_t chunk_triangle_index;

                if (chunk_count >= chunk_capacity) {
                    mdkr_track_occlusion_cache_fail("chunk capacity exhausted");
                }
                if (chunk_triangle_end > segment_triangle_end) {
                    chunk_triangle_end = segment_triangle_end;
                }
                chunk = &sTrackOcclusionCache.chunks[chunk_count++];
                chunk->triangle_offset = chunk_triangle_offset;
                chunk->triangle_count = chunk_triangle_end - chunk_triangle_offset;
                for (chunk_triangle_index = chunk_triangle_offset;
                     chunk_triangle_index < chunk_triangle_end;
                     chunk_triangle_index++) {
                    const size_t index_offset = chunk_triangle_index * 3U;
                    mdkr_track_occlusion_expand_chunk_aabb(
                        chunk, sTrackOcclusionCache.vertices[
                            sTrackOcclusionCache.indices[index_offset]]);
                    mdkr_track_occlusion_expand_chunk_aabb(
                        chunk, sTrackOcclusionCache.vertices[
                            sTrackOcclusionCache.indices[index_offset + 1U]]);
                    mdkr_track_occlusion_expand_chunk_aabb(
                        chunk, sTrackOcclusionCache.vertices[
                            sTrackOcclusionCache.indices[index_offset + 2U]]);
                }
            }
        }
        out_segment->chunk_count = chunk_count - out_segment->chunk_offset;
        vertex_base += (size_t)segment->numberOfVertices;
    }

    sTrackOcclusionCache.world.vertices = sTrackOcclusionCache.vertices;
    sTrackOcclusionCache.world.vertex_count = vertex_count;
    sTrackOcclusionCache.world.indices = sTrackOcclusionCache.indices;
    sTrackOcclusionCache.world.triangles = sTrackOcclusionCache.triangles;
    sTrackOcclusionCache.world.triangle_count = triangle_count;
    sTrackOcclusionCache.telemetry.segment_count = (size_t)gCurrentLevelModel->numberOfSegments;
    sTrackOcclusionCache.telemetry.broadphase_chunk_count = chunk_count;
    sTrackOcclusionCache.telemetry.vertex_count = vertex_count;
    sTrackOcclusionCache.telemetry.hard_triangle_count = triangle_count;
    sTrackOcclusionCache.telemetry.bytes =
        vertex_bytes + index_bytes + triangle_bytes + segment_bytes + chunk_bytes;
    if (build_started != 0U) {
        const uint64_t build_finished = platform_perf_monotonic_ns();
        sTrackOcclusionCache.telemetry.build_ns =
            build_finished >= build_started ? build_finished - build_started : 0U;
    }
    sTrackOcclusionCache.built = TRUE;
    fprintf(stderr,
            "[CAM-OCCLUSION] segments=%zu chunks=%zu vertices=%zu hard=%zu no-collision-hard=%zu nonblocking=%zu rejected=%zu unknown=%zu malformed=%zu bytes=%zu build_ns=%llu\n",
            sTrackOcclusionCache.telemetry.segment_count,
            sTrackOcclusionCache.telemetry.broadphase_chunk_count,
            sTrackOcclusionCache.telemetry.vertex_count,
            sTrackOcclusionCache.telemetry.hard_triangle_count,
            sTrackOcclusionCache.telemetry.visual_no_collision_hard_triangle_count,
            sTrackOcclusionCache.telemetry.nonblocking_triangle_count,
            sTrackOcclusionCache.telemetry.rejected_triangle_count,
            sTrackOcclusionCache.telemetry.unknown_policy_triangle_count,
            sTrackOcclusionCache.telemetry.malformed_batch_count,
            sTrackOcclusionCache.telemetry.bytes,
            (unsigned long long)sTrackOcclusionCache.telemetry.build_ns);
}

static int mdkr_track_occlusion_bounds_overlap_sweep(
    MdkrCameraVec3 bounds_minimum,
    MdkrCameraVec3 bounds_maximum,
    int bounds_valid,
    MdkrCameraVec3 start_eye,
    MdkrCameraVec3 desired_eye,
    double broadphase_radius) {
    const double start[3] = { start_eye.x, start_eye.y, start_eye.z };
    const double end[3] = { desired_eye.x, desired_eye.y, desired_eye.z };
    const double minimum[3] = { (double)bounds_minimum.x - broadphase_radius,
                                (double)bounds_minimum.y - broadphase_radius,
                                (double)bounds_minimum.z - broadphase_radius };
    const double maximum[3] = { (double)bounds_maximum.x + broadphase_radius,
                                (double)bounds_maximum.y + broadphase_radius,
                                (double)bounds_maximum.z + broadphase_radius };
    double enter = 0.0;
    double exit = 1.0;
    int axis;

    if (!bounds_valid || !isfinite(broadphase_radius) || broadphase_radius < 0.0f) {
        return FALSE;
    }
    for (axis = 0; axis < 3; axis++) {
        const double delta = (double)end[axis] - start[axis];
        if (fabs(delta) <= 1.0e-12) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) {
                return FALSE;
            }
        } else {
            double a = ((double)minimum[axis] - start[axis]) / delta;
            double b = ((double)maximum[axis] - start[axis]) / delta;
            if (a > b) {
                const double temp = a;
                a = b;
                b = temp;
            }
            if (a > enter) enter = a;
            if (b < exit) exit = b;
            if (enter > exit) {
                return FALSE;
            }
        }
    }
    return exit >= 0.0 && enter <= 1.0;
}

MdkrCameraSweepStatus mdkr_track_occlusion_sweep(
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepHit best;
    s32 segment_index;
    int have_hit = FALSE;

    if (out_hit == NULL || input == NULL || !sTrackOcclusionCache.built) {
        if (out_hit != NULL) {
            memset(out_hit, 0, sizeof(*out_hit));
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (input->mask != 0U &&
        (input->mask & MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK) == 0U) {
        return mdkr_camera_sweep(&sTrackOcclusionCache.world, input, out_hit);
    }
    memset(&best, 0, sizeof(best));
    for (segment_index = 0; segment_index < (s32)sTrackOcclusionCache.telemetry.segment_count;
         segment_index++) {
        const MdkrTrackOcclusionSegment *segment = &sTrackOcclusionCache.segments[segment_index];
        size_t chunk_index;

        if (segment->triangle_count == 0U ||
            !mdkr_track_occlusion_bounds_overlap_sweep(
                segment->minimum, segment->maximum, segment->valid, input->start_eye,
                input->desired_eye, input->guard.radius)) {
            continue;
        }
        for (chunk_index = segment->chunk_offset;
             chunk_index < segment->chunk_offset + segment->chunk_count;
             chunk_index++) {
            const MdkrTrackOcclusionChunk *chunk = &sTrackOcclusionCache.chunks[chunk_index];
            MdkrCameraOcclusionWorld local_world;
            MdkrCameraSweepHit candidate;
            MdkrCameraSweepStatus status;

            if (!mdkr_track_occlusion_bounds_overlap_sweep(
                    chunk->minimum, chunk->maximum, chunk->valid, input->start_eye,
                    input->desired_eye, input->guard.radius)) {
                continue;
            }
            local_world = sTrackOcclusionCache.world;
            local_world.indices += chunk->triangle_offset * 3U;
            local_world.triangles += chunk->triangle_offset;
            local_world.triangle_count = chunk->triangle_count;
            status = mdkr_camera_sweep(&local_world, input, &candidate);
            if (status == MDKR_CAMERA_SWEEP_INVALID) {
                memset(out_hit, 0, sizeof(*out_hit));
                return status;
            }
            if (status == MDKR_CAMERA_SWEEP_HIT &&
                (!have_hit || candidate.fraction < best.fraction ||
                 (candidate.fraction == best.fraction &&
                  candidate.stable_id < best.stable_id))) {
                best = candidate;
                have_hit = TRUE;
            }
        }
    }
    if (have_hit) {
        *out_hit = best;
        return MDKR_CAMERA_SWEEP_HIT;
    }
    return mdkr_camera_sweep(&(MdkrCameraOcclusionWorld) { 0 }, input, out_hit);
}

static int mdkr_track_occlusion_rounded_candidate_better(
    const MdkrCameraSweepHit *candidate,
    const MdkrCameraSweepHit *best) {
    if ((double)candidate->fraction < (double)best->fraction -
            MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) {
        return TRUE;
    }
    if (fabs((double)candidate->fraction - (double)best->fraction) <=
        MDKR_CAMERA_OBSTRUCTION_QUERY_TIME_TIE_EPSILON) {
        if (candidate->stable_id != best->stable_id) {
            return candidate->stable_id < best->stable_id;
        }
        if (candidate->feature != best->feature) {
            return candidate->feature < best->feature;
        }
    }
    return FALSE;
}

static void mdkr_track_occlusion_counter_add(uint64_t *counter, uint64_t value) {
    if (UINT64_MAX - *counter < value) {
        *counter = UINT64_MAX;
    } else {
        *counter += value;
    }
}

static void mdkr_track_occlusion_record_exact_work(
    const MdkrCameraRoundedLensSweepTelemetry *work,
    uint64_t segment_candidates,
    uint64_t triangle_candidates,
    int invalid) {
    MdkrTrackOcclusionTelemetry *telemetry = &sTrackOcclusionCache.telemetry;

    mdkr_track_occlusion_counter_add(
        &telemetry->exact_segment_candidate_count, segment_candidates);
    mdkr_track_occlusion_counter_add(
        &telemetry->exact_triangle_candidate_count, triangle_candidates);
    if (work != NULL) {
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_analytic_sat_count,
            work->analytic_swept_sat_tests);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_analytic_revalidation_miss_count,
            work->analytic_revalidation_misses);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_bounded_interval_test_count,
            work->bounded_interval_tests);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_bounded_interval_exhaustion_count,
            work->bounded_interval_exhaustions);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_stationary_test_count, work->stationary_tests);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_advance_iteration_count,
            work->conservative_advance_iterations);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_refinement_test_count,
            work->contact_refinement_tests);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_interval_fallback_count, work->interval_fallbacks);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_interval_sample_count, work->interval_samples);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_ambiguous_interval_count, work->ambiguous_intervals);
        mdkr_track_occlusion_counter_add(
            &telemetry->exact_publication_revalidation_count,
            work->publication_revalidations);
        if (work->stationary_tests > telemetry->exact_max_stationary_tests_per_sweep) {
            telemetry->exact_max_stationary_tests_per_sweep = work->stationary_tests;
        }
    }
    if (triangle_candidates > telemetry->exact_max_triangle_candidates_per_sweep) {
        telemetry->exact_max_triangle_candidates_per_sweep = triangle_candidates;
    }
    if (invalid) {
        mdkr_track_occlusion_counter_add(&telemetry->exact_invalid_sweep_count, 1U);
    }
}

static void mdkr_track_occlusion_accumulate_exact_work(
    MdkrCameraRoundedLensSweepTelemetry *total,
    const MdkrCameraRoundedLensSweepTelemetry *part) {
#define MDKR_TRACK_ADD_EXACT_FIELD(field) \
    mdkr_track_occlusion_counter_add(&total->field, part->field)
    MDKR_TRACK_ADD_EXACT_FIELD(triangles_seen);
    MDKR_TRACK_ADD_EXACT_FIELD(triangles_filtered);
    MDKR_TRACK_ADD_EXACT_FIELD(triangles_aabb_rejected);
    MDKR_TRACK_ADD_EXACT_FIELD(triangles_narrowed);
    MDKR_TRACK_ADD_EXACT_FIELD(analytic_swept_sat_tests);
    MDKR_TRACK_ADD_EXACT_FIELD(analytic_revalidation_misses);
    MDKR_TRACK_ADD_EXACT_FIELD(bounded_interval_tests);
    MDKR_TRACK_ADD_EXACT_FIELD(bounded_interval_exhaustions);
    MDKR_TRACK_ADD_EXACT_FIELD(stationary_tests);
    MDKR_TRACK_ADD_EXACT_FIELD(conservative_advance_iterations);
    MDKR_TRACK_ADD_EXACT_FIELD(contact_refinement_tests);
    MDKR_TRACK_ADD_EXACT_FIELD(interval_fallbacks);
    MDKR_TRACK_ADD_EXACT_FIELD(interval_samples);
    MDKR_TRACK_ADD_EXACT_FIELD(ambiguous_intervals);
    MDKR_TRACK_ADD_EXACT_FIELD(publication_revalidations);
#undef MDKR_TRACK_ADD_EXACT_FIELD
}

MdkrCameraSweepStatus mdkr_track_occlusion_rounded_lens_sweep(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit) {
    MdkrCameraSweepHit best;
    MdkrCameraRoundedLensSweepTelemetry total_work;
    double broadphase_radius;
    uint64_t segment_candidates = 0U;
    uint64_t triangle_candidates = 0U;
    s32 segment_index;
    int have_hit = FALSE;

    memset(&total_work, 0, sizeof(total_work));
    if (sTrackOcclusionCache.built) {
        mdkr_track_occlusion_counter_add(
            &sTrackOcclusionCache.telemetry.exact_sweep_count, 1U);
    }
    if (out_hit == NULL || input == NULL || !sTrackOcclusionCache.built) {
        if (out_hit != NULL) {
            memset(out_hit, 0, sizeof(*out_hit));
        }
        if (sTrackOcclusionCache.built) {
            mdkr_track_occlusion_record_exact_work(&total_work, 0U, 0U, TRUE);
        }
        return MDKR_CAMERA_SWEEP_INVALID;
    }
    if (input->mask != 0U &&
        (input->mask & MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK) == 0U) {
        MdkrCameraSweepStatus status = mdkr_camera_rounded_lens_sweep_profiled(
            &sTrackOcclusionCache.world, input, out_hit, &total_work);
        mdkr_track_occlusion_record_exact_work(
            &total_work, 0U, total_work.triangles_narrowed,
            status == MDKR_CAMERA_SWEEP_INVALID);
        return status;
    }
    if (!mdkr_camera_rounded_lens_guard_conservative_radius(
            &input->guard, &broadphase_radius)) {
        MdkrCameraSweepStatus status = mdkr_camera_rounded_lens_sweep_profiled(
            &(MdkrCameraOcclusionWorld) { 0 }, input, out_hit, &total_work);
        mdkr_track_occlusion_record_exact_work(&total_work, 0U, 0U, TRUE);
        return status;
    }
    memset(&best, 0, sizeof(best));
    for (segment_index = 0; segment_index < (s32)sTrackOcclusionCache.telemetry.segment_count;
         segment_index++) {
        const MdkrTrackOcclusionSegment *segment = &sTrackOcclusionCache.segments[segment_index];
        size_t chunk_index;

        if (segment->triangle_count == 0U ||
            !mdkr_track_occlusion_bounds_overlap_sweep(
                segment->minimum, segment->maximum, segment->valid, input->start_eye,
                input->desired_eye, broadphase_radius)) {
            continue;
        }
        segment_candidates++;
        for (chunk_index = segment->chunk_offset;
             chunk_index < segment->chunk_offset + segment->chunk_count;
             chunk_index++) {
            const MdkrTrackOcclusionChunk *chunk = &sTrackOcclusionCache.chunks[chunk_index];
            MdkrCameraOcclusionWorld local_world;
            MdkrCameraSweepHit candidate;
            MdkrCameraSweepStatus status;
            MdkrCameraRoundedLensSweepTelemetry chunk_work;

            if (!mdkr_track_occlusion_bounds_overlap_sweep(
                    chunk->minimum, chunk->maximum, chunk->valid, input->start_eye,
                    input->desired_eye, broadphase_radius)) {
                continue;
            }
            local_world = sTrackOcclusionCache.world;
            local_world.indices += chunk->triangle_offset * 3U;
            local_world.triangles += chunk->triangle_offset;
            local_world.triangle_count = chunk->triangle_count;
            triangle_candidates += chunk->triangle_count;
            status = mdkr_camera_rounded_lens_sweep_profiled(
                &local_world, input, &candidate, &chunk_work);
            mdkr_track_occlusion_accumulate_exact_work(&total_work, &chunk_work);
            if (status == MDKR_CAMERA_SWEEP_INVALID) {
                memset(out_hit, 0, sizeof(*out_hit));
                mdkr_track_occlusion_record_exact_work(
                    &total_work, segment_candidates, triangle_candidates, TRUE);
                return status;
            }
            if (status == MDKR_CAMERA_SWEEP_HIT &&
                (!have_hit || mdkr_track_occlusion_rounded_candidate_better(&candidate, &best))) {
                best = candidate;
                have_hit = TRUE;
            }
        }
    }
    if (have_hit) {
        *out_hit = best;
        mdkr_track_occlusion_record_exact_work(
            &total_work, segment_candidates, triangle_candidates, FALSE);
        return MDKR_CAMERA_SWEEP_HIT;
    }
    {
        MdkrCameraSweepStatus status = mdkr_camera_rounded_lens_sweep_profiled(
            &(MdkrCameraOcclusionWorld) { 0 }, input, out_hit, NULL);
        mdkr_track_occlusion_record_exact_work(
            &total_work, segment_candidates, triangle_candidates,
            status == MDKR_CAMERA_SWEEP_INVALID);
        return status;
    }
}

void mdkr_track_occlusion_get_telemetry(MdkrTrackOcclusionTelemetry *out) {
    if (out != NULL) {
        *out = sTrackOcclusionCache.telemetry;
    }
}
#endif

/**
 * Sets the number of expected viewports in the scene.
 * Like most other viewport vars, it's 0-3 rather than 1-4.
 * Set as an s32 for some reason.
 */
s32 set_scene_viewport_num(s32 numPorts) {
    gScenePlayerViewports = numPorts;
    return 0;
}

/**
 * Initialises the level.
 * Allocates RAM to load generate the level geometry, spawn objects and generate shadows.
 */
void init_track(u32 geometry, u32 skybox, s32 numberOfPlayers, Vehicle vehicle, u32 entranceId, u32 collectables,
                u32 arg6) {
    s32 i;

    gCurrentLevelHeader2 = level_header();
    D_8011B0F8 = FALSE;
    gTTCamPlayerID = 0;
    gTTCamID = 0;
    gTTCamSmoothTimer = 0;
#ifdef NATIVE_PORT
    for (i = 0; i < (s32) ARRAY_COUNT(gTTCamSpectateIndex); i++) {
        gTTCamSpectateIndex[i] = 0;
    }
#endif
    D_8011B10C = 0;

    if (gCurrentLevelHeader2->race_type == RACETYPE_CUTSCENE_1 ||
        gCurrentLevelHeader2->race_type == RACETYPE_CUTSCENE_2) {
        D_8011B0F8 = TRUE;
    }

    generate_track(geometry);

    gWaveBlockCount = 0;

    if (numberOfPlayers < 2) {
        for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
            if (DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].hasWaves != 0) {
                gWaveBlockCount++;
                DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].hasWaves = 1;
            }
        }
    }

    if (is_in_two_player_adventure() &&
        (gCurrentLevelHeader2->race_type == RACETYPE_DEFAULT || gCurrentLevelHeader2->race_type & RACETYPE_CHALLENGE)) {
        i = 2;
    } else {
        i = numberOfPlayers + 1;
    }

    if (gWaveBlockCount) {
        waves_init(gCurrentLevelModel, gCurrentLevelHeader2, i);
    }

    cam_set_layout(numberOfPlayers);
    skydome_spawn(skybox);
    gTrackTexAnimOffset = 0;
    gTrackTexAnimFlags = RENDER_TEX_ANIM;
    path_enable();
    track_spawn_objects(arg6, 0);
    track_spawn_objects(collectables, 1);
    gScenePlayerViewports = numberOfPlayers;
    track_setup_racers(vehicle, entranceId, numberOfPlayers);
    racerfx_alloc(72, 64);

    if (geometry == 0 && entranceId == 0) {
        transition_begin(&gCircleFadeToBlack);
    } else {
        transition_begin(&gFullFadeToBlack);
    }
    cam_set_layout(gScenePlayerViewports);

    numberOfPlayers = gScenePlayerViewports;
    gAntiAliasing = FALSE;
    // All shadow groups are double buffered. Environment shadows never change, so they could've single buffered them.
    for (i = 0; i < ARRAY_COUNT(gShadowHeapData); i++) {
        gShadowHeapData[i] =
            (ShadowHeapProperties *) mempool_alloc_safe(
                sizeof(ShadowHeapProperties) * SHADOW_HEAP_DATA_CAPACITY, COLOUR_TAG_YELLOW);
        gShadowHeapTris[i] = (Triangle *) mempool_alloc_safe(
            sizeof(Triangle) * SHADOW_HEAP_TRI_CAPACITY, COLOUR_TAG_YELLOW);
        gShadowHeapVerts[i] = (Vertex *) mempool_alloc_safe(
            sizeof(Vertex) * SHADOW_HEAP_VERTEX_CAPACITY, COLOUR_TAG_YELLOW);
    }

    gShadowHeapFlip = 0;
    shadow_update(SHADOW_SCENERY, SHADOW_SCENERY, LOGIC_NULL);
    shadow_update(SHADOW_ACTORS, SHADOW_ACTORS, LOGIC_NULL);
    gShadowHeapFlip = 1;
    shadow_update(SHADOW_SCENERY, SHADOW_SCENERY, LOGIC_NULL);
    shadow_update(SHADOW_ACTORS, SHADOW_ACTORS, LOGIC_NULL);
    gShadowHeapFlip = 0;
    if (gCurrentLevelHeader2->useVoid) {
        gVoidColourR = gCurrentLevelHeader2->voidColour.red;
        gVoidColourG = gCurrentLevelHeader2->voidColour.green;
        gVoidColourB = gCurrentLevelHeader2->voidColour.blue;
        void_init(numberOfPlayers + 1);
    }
}

/**
 * The root function for rendering the entire scene.
 * Handles drawing the track, objects and the majority of the HUD in single player.
 */
void render_scene(Gfx **dList, Mtx **mtx, Vertex **vtx, Triangle **tris, s32 updateRate) {
    s32 i;
    s32 numViewports;
    s32 tempUpdateRate;
    s8 flip;
    s32 posX;
    s32 posY;
    s32 j;
#ifdef NATIVE_PORT
    /* Test-only render-starvation seam. Authoritative tick work has already
     * completed before this function; skipping here must therefore leave the
     * raw simulation state byte-identical. */
    {
        extern int mdkr_test_render_skip_this_tick(void);
        if (mdkr_test_render_skip_this_tick()) {
            return;
        }
        {
            extern void mdkr_test_render_impurity_inject(void);
            mdkr_test_render_impurity_inject();
        }
    }
#endif
#ifdef NATIVE_PORT
    s32 savedCutsceneCamera;
    camera_obstruction_presentation_begin();
    /* Latch Enhancements.DrawDistance and Enhancements.LodBias for this whole
     * drawn frame. Reading them once here, rather than per object, is what
     * stops a setting changed mid-frame from culling half the scene at one
     * radius and half at another. */
    mdkr_enh_draw_distance_begin_frame();
#endif

    gTrackDL = *dList;
    gTrackMtxPtr = *mtx;
    gTrackVtxPtr = *vtx;
    gTrackTriPtr = *tris;
    gSceneRenderSkyDome = TRUE;
    gDisableShadows = FALSE;
    D_8011B0C0 = 0;
    gIsNearCurrBBox = FALSE;
    numViewports = cam_set_layout(gScenePlayerViewports);
    if (is_game_paused()) {
        tempUpdateRate = 0;
    } else {
        tempUpdateRate = updateRate;
    }
    if (gWaveBlockCount) {
        waves_update(tempUpdateRate);
    }
    shadow_update(SHADOW_ACTORS, SHADOW_ACTORS, updateRate);
#ifndef NATIVE_PORT
    for (i = 0; i < 7; i++) {
        if ((s32) gCurrentLevelHeader2->unk74[i] != -1) {
            update_colour_cycle(DKR_PTR(LevelHeader_70, gCurrentLevelHeader2->unk74[i]), tempUpdateRate);
        }
    }
    if ((s32) gCurrentLevelHeader2->pulseLightData != -1) {
        update_pulsating_light_data(DKR_PTR(PulsatingLightData, gCurrentLevelHeader2->pulseLightData),
                                    tempUpdateRate);
    }
#endif
    gDrawLevelSegments = TRUE;
    if (gCurrentLevelHeader2->race_type == RACETYPE_CUTSCENE_2) {
        gDrawLevelSegments = FALSE;
        gAntiAliasing = TRUE;
    }
    if (gCurrentLevelHeader2->race_type == RACETYPE_CUTSCENE_1 || gCurrentLevelHeader2->unkBD) {
        gAntiAliasing = TRUE;
    }
#ifndef NATIVE_PORT
    if (gCurrentLevelHeader2->skyDome == -1) {
        TextureHeader *skyTex = DKR_PTR(TextureHeader, gCurrentLevelHeader2->unkA4);
        i = (skyTex->width << 9) - 1;
        gCurrentLevelHeader2->unkA8 =
            (gCurrentLevelHeader2->unkA8 + (gCurrentLevelHeader2->unkA2 * tempUpdateRate)) & i;
        i = (skyTex->height << 9) - 1;
        gCurrentLevelHeader2->unkAA =
            (gCurrentLevelHeader2->unkAA + (gCurrentLevelHeader2->unkA3 * tempUpdateRate)) & i;
        tex_animate_texture(skyTex, &gTrackTexAnimFlags, &gTrackTexAnimOffset, tempUpdateRate);
    }
#endif
    flip = FALSE;
    if (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) {
        flip = TRUE;
    }
#ifdef ANTI_TAMPER
    // Antipiracy measure
    if (IO_READ(0x200) != 0xAC290000) {
        flip = TRUE;
    }
#endif
    rendermode_reset(&gTrackDL);
    gDkrDisableBillboard(gTrackDL++);
    gSPClearGeometryMode(gTrackDL++, G_CULL_FRONT);
    gDPSetBlendColor(gTrackDL++, 0, 0, 0, 100);
    gDPSetPrimColor(gTrackDL++, 0, 0, 255, 255, 255, 255);
    gDPSetEnvColor(gTrackDL++, 255, 255, 255, 0);
#ifndef NATIVE_PORT
    rain_fog();
    update_fog(numViewports, tempUpdateRate);
#endif
#ifndef NATIVE_PORT
    scroll_particle_textures(tempUpdateRate);
#endif
    if (gCurrentLevelModel->numberOfAnimatedTextures > 0) {
        track_tex_anim(tempUpdateRate);
    }
    for (j = gSceneCurrentPlayerID = 0; j < numViewports; gSceneCurrentPlayerID++, j = gSceneCurrentPlayerID) {
        if (gCurrentLevelHeader2 && !gCurrentLevelHeader2 && !gCurrentLevelHeader2) {} // Fakematch
        if (j == 0) {
            if (is_player_two_in_control() && numViewports == 1) {
                gSceneCurrentPlayerID = PLAYER_TWO;
            }
        }
        if (flip) {
            gSPSetGeometryMode(gTrackDL++, G_CULL_FRONT);
        }
        apply_fog(gSceneCurrentPlayerID);
        gDPPipeSync(gTrackDL++);
        set_active_camera(gSceneCurrentPlayerID);
        viewport_main(&gTrackDL, &gTrackMtxPtr);
        func_8002A31C();
        // Show detailed skydome in single player.
        if (numViewports < 2) {
            mtx_world_origin(&gTrackDL, &gTrackMtxPtr);
            if (gCurrentLevelHeader2->skyDome == -1) {
                trackbg_render_flashy();
            } else {
                skydome_render();
            }
        } else {
            mtx_perspective(&gTrackDL, &gTrackMtxPtr);
            trackbg_render_gradient();
            camSetProjMtx(&gTrackDL, &gTrackMtxPtr);
            mtx_world_origin(&gTrackDL, &gTrackMtxPtr);
        }
        gDPPipeSync(gTrackDL++);
        initialise_player_viewport_vars(updateRate);
        weather_clip_planes(-1, -512);
        // Show weather effects in single player.
        if (gCurrentLevelHeader2->weatherEnable > 0 && numViewports < 2) {
#ifdef NATIVE_PORT
            /* The weather renderer still uses the ROM RNG internally for
             * particle placement. Keep those draws invisible to gameplay RNG;
             * weather_tick owns the authoritative weather integration.
             * weather_update consumes RNG at weather.c:955/956/1000/1001/1068
             * (rain/snow particle placement and the lightning roll). */
            save_rng_seed();
#endif
            weather_update(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, &gTrackTriPtr, tempUpdateRate);
#ifdef NATIVE_PORT
            load_rng_seed();
#endif
        }
        lensflare_override(cam_get_active_camera());
        lensflare_render(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, cam_get_active_camera());
        hud_render_player(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, get_racer_object_by_port(gSceneCurrentPlayerID),
                          updateRate);
    }
    // Show TT Cam toggle for the fourth viewport when playing 3 player.
    if (numViewports == 3 && level_type() != RACETYPE_CHALLENGE_EGGS && level_type() != RACETYPE_CHALLENGE_BATTLE &&
        level_type() != RACETYPE_CHALLENGE_BANANAS) {
        if (hud_setting() == 0) {
            if (flip) {
                gSPSetGeometryMode(gTrackDL++, G_CULL_FRONT);
            }
            apply_fog(PLAYER_FOUR);
            gDPPipeSync(gTrackDL++);
            set_active_camera(PLAYER_FOUR);
#ifdef NATIVE_PORT
            /* Render used to call disable_cutscene_camera() here: a GLOBAL,
             * unrestored clear of
             * gCutsceneCameraActive performed from the draw. That flag gates
             * racer input (racer.c:4513), audio, and active-camera selection
             * (camera.c:1984), and thread3_main.c's own clear is gated on
             * !gIsPaused -- so on a PAUSED three-player TT-camera frame the
             * render-side clear was the only one in the frame, and whether the
             * flag survived the frame depended on whether the scene was drawn.
             * The matching, narrowly conditioned clear now lives at the end of
             * mode_game (the simulation step). What is left here is a
             * viewport-local mask, the same save/restore
             * scene_visibility_prepare_viewport already uses for this same
             * viewport. */
            savedCutsceneCamera = gCutsceneCameraActive;
            gCutsceneCameraActive = 0;
#else
            disable_cutscene_camera();
#endif
#ifndef NATIVE_PORT
            ttcam_update(updateRate);
#endif
            viewport_main(&gTrackDL, &gTrackMtxPtr);
            func_8002A31C();
            mtx_perspective(&gTrackDL, &gTrackMtxPtr);
            trackbg_render_gradient();
            camSetProjMtx(&gTrackDL, &gTrackMtxPtr);
            mtx_world_origin(&gTrackDL, &gTrackMtxPtr);
            gDPPipeSync(gTrackDL++);
            initialise_player_viewport_vars(updateRate);
            weather_clip_planes(-1, -512);
            lensflare_override(cam_get_active_camera());
            lensflare_render(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, cam_get_active_camera());
            set_text_font(FONT_COLOURFUL);
            if (osTvType == OS_TV_TYPE_PAL) {
                posX = SCREEN_WIDTH_HALF + 6;
                posY = SCREEN_HEIGHT_HALF_PAL + 6;
            } else {
                posX = SCREEN_WIDTH_HALF + 10;
                posY = SCREEN_HEIGHT_HALF + 5;
            }
#if REGION == REGION_JP
            draw_text(&gTrackDL, posX, posY, gJpnTTCam, ALIGN_TOP_LEFT);
#else
            draw_text(&gTrackDL, posX, posY, "TT CAM", ALIGN_TOP_LEFT);
#endif
#ifdef NATIVE_PORT
            /* End of the masked viewport; render leaves the flag as it found it. */
            gCutsceneCameraActive = savedCutsceneCamera;
#endif
        } else {
            set_active_camera(PLAYER_FOUR);
#ifndef NATIVE_PORT
            ttcam_update(updateRate);
#endif
        }
    }
    viewport_reset(&gTrackDL);
    gDPPipeSync(gTrackDL++);
    gDkrDisableBillboard(gTrackDL++);
    gShadowHeapFlip = 1 - gShadowHeapFlip;
    *dList = gTrackDL;
    *mtx = gTrackMtxPtr;
    *vtx = gTrackVtxPtr;
    *tris = gTrackTriPtr;
#ifdef NATIVE_PORT
    camera_obstruction_presentation_end();
#endif
}

/************ .rodata ************/
UNUSED const char gTrackClippingErrorString[] = "Solid Clipping x0=x1 Error!!!\n";
UNUSED const char gTrackHeightOverflowString[] = "TrackGetHeight() - Overflow!!!\n";

/**
 * Allocates control data and geometry for the void.
 * The void refers to the flat coloured background that's shaped in real time to prevent
 * being able to see through level geometry, breaking immersion.
 */
void void_init(s32 viewportCount) {
    s32 i;
    s32 sp30;
    s32 sp2C;
    s32 vtxLimit;
    s32 triLimit;
    u8 *ptr;

    D_8011D4BA = 175;
    gVoidPrimLimit = 45;
    // Halve the primitive limit for multiplayer.
    if (viewportCount >= 2) {
        gVoidPrimLimit >>= 1;
    }

    sp30 = (D_8011D4BA + 6) * sizeof(unk8011D478);
    sp2C = D_8011D4BA + 5;
    vtxLimit = (gVoidPrimLimit + 5) * 4 * sizeof(Vertex);
    triLimit = (gVoidPrimLimit + 5) * 2 * sizeof(Triangle);

    gVoidMesh = mempool_alloc_safe(viewportCount * sizeof(VoidMesh), COLOUR_TAG_CYAN);
    gVoidData = mempool_alloc_safe(sp30 + sp2C + (vtxLimit + triLimit) * 2 * viewportCount, COLOUR_TAG_CYAN);

    ptr = gVoidData;

    if (ptr != NULL) {
        D_8011D478 = (unk8011D478 *) ptr;
        ptr += sp30;

        D_8011D47C = (s8 *) ptr;
        // Align by 8
#ifdef NATIVE_PORT
        /* LP64: the original (s32) cast truncates the 64-bit arena pointer and,
         * being signed, sign-extends it when bit 31 is set -> every gVoidMesh
         * verts/tris pointer becomes 0xffffffff........, which void_check hands to
         * gTrackVtxPtr and void_generate_primitive then faults writing through.
         * Align through uintptr_t so the high bits survive (cf. align16). */
        ptr = (u8 *) ((uintptr_t) (ptr + sp2C + 8) & ~(uintptr_t) 7);
#else
        ptr = (u8 *) ((s32) (ptr + sp2C + 8) & ~7);
#endif

        for (i = 0; i < viewportCount; i++) {
            gVoidMesh[i].tris[0] = (Triangle *) ptr;
            ptr += triLimit;

            gVoidMesh[i].tris[1] = (Triangle *) ptr;
            ptr += triLimit;

            gVoidMesh[i].verts[0] = (Vertex *) ptr;
            ptr += vtxLimit;

            gVoidMesh[i].verts[1] = (Vertex *) ptr;
            ptr += vtxLimit;
        }
    }
    gVoidVertexFlip = 0;
}

/**
 * Free the void mesh and control data if it exists.
 */
void void_free(void) {
    if (gVoidData != NULL) {
        mempool_free(gVoidMesh);
        mempool_free(gVoidData);
        gVoidData = NULL;
    }
}

// root func for the out of bounds void rendering
void void_check(u8 *segmentIds, s32 numberOfSegments, s32 viewportIndex) {
    s16 i;
    s16 j;
    f32 yCameraSins;
    f32 yCameraCoss;
    f32 temp_f22;
    Vertex *vtx;
    Triangle *tri;
    s16 temp_s3;
    s16 var_s0;
    s16 var_s4;
    s16 breakLoop;
    s32 *ptr2;
    LevelModelSegmentBoundingBox *bbox;
    s16 sum;
#ifdef NATIVE_PORT
    // Triage sweep (BUG_CLASS_SWEEP_REPORT.md #14, tracks.c void_check): the
    // ROM-era array is sized as a guess at the N64 frame layout ("real size is
    // unknown"), with no bound on the push below. D_8011D49E can hold up to
    // D_8011D4BA (175) paired entries -- two per void edge -- so up to 87
    // edges could theoretically be open at once; sized here for that worst
    // case rather than the guessed 24. Measured peak across the hub tour and
    // all 20 main + boss tracks is 10 (14-element headroom at the old size),
    // so this has not been observed to fire, but the fix is cheap and the
    // capacity guard below makes an over-long run degrade (dropped edge)
    // instead of corrupting the stack frame if it ever does.
    s8 sp7C[88];
#else
    s8 sp7C[24]; // possible UB here, real size is unknown
#endif
    s32 pad;

    gVoidTris[0] = gVoidMesh[viewportIndex].tris[0];
    gVoidTris[1] = gVoidMesh[viewportIndex].tris[1];
    gVoidVerts[0] = gVoidMesh[viewportIndex].verts[0];
    gVoidVerts[1] = gVoidMesh[viewportIndex].verts[1];
    material_set_no_tex_offset(&gTrackDL, NULL, RENDER_ANTI_ALIASING | RENDER_Z_COMPARE);
    D_8011D49C = 0;
    D_8011D49E = 0;

    yCameraSins = sins_f(-gSceneActiveCamera->trans.rotation.y_rotation);
    yCameraCoss = coss_f(-gSceneActiveCamera->trans.rotation.y_rotation);

    D_8011D4AC = gSceneActiveCamera->trans.x_position + yCameraSins * 250.0;
    D_8011D4B0 = gSceneActiveCamera->trans.z_position + yCameraCoss * 250.0;

    temp_f22 = -(yCameraSins * D_8011D4AC + yCameraCoss * D_8011D4B0);

    D_8011D4A0 = -yCameraCoss;
    D_8011D4A4 = yCameraSins;
    D_8011D4A8 = -(D_8011D4A0 * D_8011D4AC + D_8011D4A4 * D_8011D4B0);

    i = 0;
    for (; i < numberOfSegments; i++) {
        bbox = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[segmentIds[i]];
        breakLoop = 0;
        breakLoop += bbox->x1 * yCameraSins + yCameraCoss * bbox->z1 + temp_f22 <= 0.0;
        breakLoop += yCameraSins * bbox->x2 + yCameraCoss * bbox->z1 + temp_f22 <= 0.0;
        breakLoop += bbox->x1 * yCameraSins + yCameraCoss * bbox->z2 + temp_f22 <= 0.0;
        breakLoop += yCameraSins * bbox->x2 + yCameraCoss * bbox->z2 + temp_f22 <= 0.0;
        if (breakLoop & 3) {
            func_80026430(&DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentIds[i]], yCameraSins, yCameraCoss, temp_f22);
            if (DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentIds[i]].unk3C & 2) {
                func_80026070(bbox, yCameraSins, yCameraCoss, temp_f22);
            }
        }
    }

    func_80026C14(300, gCurrentLevelModel->lowerYBounds - 195, 1);
    func_80026C14(-300, gCurrentLevelModel->lowerYBounds - 195, 1);
    func_80026C14(300, gCurrentLevelModel->upperYBounds + 195, 0);
    func_80026C14(-300, gCurrentLevelModel->upperYBounds + 195, 0);

    if (D_8011D49E >= D_8011D4BA || D_8011D49E == 0) {
        return;
    }

    do {
        breakLoop = TRUE;
        ptr2 = (s32 *) D_8011D478;
        for (i = 0; i < D_8011D49E - 1; i++, ptr2 += 2) {
            if (D_8011D478[i + 1].unk0 < D_8011D478[i].unk0) {
                s32 tmp;

                tmp = ptr2[0];
                ptr2[0] = ptr2[2];
                ptr2[2] = tmp;

                tmp = ptr2[1];
                ptr2[1] = ptr2[3];
                ptr2[3] = tmp;

                breakLoop = FALSE;
            }
        }
    } while (!breakLoop);

    var_s0 = 0;

    for (i = 0; i < D_8011D49E; i++) {
        j = D_8011D478[i].unk7 * 2;
        if (D_8011D47C[j] == -1) {
            D_8011D478[i].unk6 |= 2;
            D_8011D47C[j] = i;
        } else {
            D_8011D47C[j + 1] = i;
        }
    }
    var_s4 = temp_s3 = D_8011D478[0].unk0;

    vtx = gTrackVtxPtr;
    tri = gTrackTriPtr;
    gTrackVtxPtr = gVoidVerts[gVoidVertexFlip];
    gTrackTriPtr = gVoidTris[gVoidVertexFlip];

    gVoidVertexFlip = 1 - gVoidVertexFlip;
    gVoidCurrVerts = gTrackVtxPtr;
    gVoidCurrTris = gTrackTriPtr;
    gVoidVertCount = 0;
    gVoidPrimCount = 0;

    i = 0;
    while (i < D_8011D49E) {
        while (i < D_8011D49E && var_s4 == D_8011D478[i].unk0) {
            if (D_8011D478[i].unk6 & 2) {
#ifdef NATIVE_PORT
                if (var_s0 < (s32) ARRAY_COUNT(sp7C)) {
                    sp7C[var_s0] = D_8011D478[i].unk7;
                    var_s0++;
                }
#else
                sp7C[var_s0] = D_8011D478[i].unk7;
                var_s0++;
#endif
            } else {
                for (j = 0; j < var_s0; j++) {
                    if (sp7C[j] == D_8011D478[i].unk7) {
                        var_s0--;
                        while (j < var_s0) {
                            sp7C[j] = sp7C[j + 1];
                            j++;
                        }
                    }
                }
            }
            i++;
        }
        if (i < D_8011D49E) {
            temp_s3 = D_8011D478[i].unk0;
            if (var_s4 != temp_s3) {
                func_80026E54(var_s0, sp7C, temp_s3, var_s4);
                var_s4 = temp_s3;
            }
        }
    }
    if (gVoidVertCount != 0) {
#ifdef NATIVE_PORT
        /* The void curtain is a camera-relative rendering trick rewritten in
         * place every frame; it must never enter the shadow caster feed (the
         * address-keyed static cache would freeze its first frames as a
         * permanent phantom wall). */
        gfx_shadow_caster_exclude_mark(gVoidCurrTris);
#endif
        gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(gVoidCurrVerts), gVoidVertCount, 0);
        gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(gVoidCurrTris), gVoidVertCount >> 1, TRIN_DISABLE_TEXTURE);
    }
    gTrackVtxPtr = vtx;
    gTrackTriPtr = tri;
}

void func_80026070(LevelModelSegmentBoundingBox *arg0, f32 arg1, f32 arg2, f32 arg3) {
    f32 sp80[4];
    f32 sp70[4];
    f32 sp60[4];
    f32 temp;
    f32 sp54[2];
    f32 sp4C[2];
    s16 index;
    s16 nextIndex;
    s16 sp40[4];
    s16 var_t0;
    s16 temp2;

    sp80[0] = arg0->x1;
    sp70[0] = arg0->z1;
    sp80[1] = arg0->x2;
    sp70[1] = arg0->z1;
    sp80[2] = arg0->x2;
    sp70[2] = arg0->z2;
    sp80[3] = arg0->x1;
    sp70[3] = arg0->z2;

    temp2 = 0;
    for (index = 0; index < 4; index++) {
        sp60[index] = (arg1 * sp80[index]) + (sp70[index] * arg2) + arg3;
        sp40[index] = sp60[index] <= 0.0;
        temp2 += sp40[index];
    }
    if (temp2 != 0) {
        var_t0 = 0;
        if (temp2 == 4) {
            return;
        }
        for (index = 0; index < 4; index++) {
            nextIndex = index + 1;
            if (nextIndex >= 4) {
                nextIndex = 0;
            }
            if ((sp40[nextIndex] != sp40[index]) != 0U) {
                temp = sp60[index] / (sp60[index] - sp60[nextIndex]);
                sp54[var_t0] = sp80[index] + ((sp80[nextIndex] - sp80[index]) * temp);
                sp4C[var_t0] = sp70[index] + ((sp70[nextIndex] - sp70[index]) * temp);
                var_t0++;
            }
        }
        if (var_t0 != 2) {
            return;
        }

        sp60[0] = (D_8011D4A0 * sp54[0]) + (D_8011D4A4 * sp4C[0]) + D_8011D4A8;
        sp60[1] = (D_8011D4A0 * sp54[1]) + (D_8011D4A4 * sp4C[1]) + D_8011D4A8;

        if (sp60[1] < sp60[0]) {
            temp = sp60[0];
            sp60[0] = sp60[1];
            sp60[1] = temp;
        }

        // Returns must be on the same line.
        // clang-format off
        if (-300.0 > sp60[1]) { return; }
        if (sp60[0] > 300.0) { return; }
        // clang-format on

        if (sp60[0] < -300.0) {
            sp60[0] = -300.0f;
        }
        if (sp60[1] > 300.0) {
            sp60[1] = 300.0f;
        }
        func_80026C14(sp60[1], gCurrentLevelModel->lowerYBounds - 195, 0);
        func_80026C14(sp60[0], gCurrentLevelModel->lowerYBounds - 195, 0);
    }
}

void func_80026430(LevelModelSegment *segment, f32 arg1, f32 arg2, f32 arg3) {
    s16 i;
    s16 index;
    s16 verticesOffset;
    s16 nextFaceOffset;
    s16 nextIndex;
    s16 currFaceOffset;
    s16 j;
    Vertex *vert;
    s8 spF8[3];
    f32 temp;
    s16 var_s0;
    s16 var_t0;
#ifdef AVOID_UB
    f32 spE8[3]; // This really should be size of 3, but something is keeping it from matching that way.
#else
    f32 spE8[2];
#endif
    f32 spDC[3];
    f32 spD0[3];
    f32 spC4[3];
    f32 spB8[3];
    f32 spB0[2];
    f32 spA8[2];
    f32 spA0[2];

    if (D_8011D49E >= D_8011D4BA) {
        return;
    }

    for (i = 0; i < segment->numberOfBatches; i++) {
        currFaceOffset = DKR_PTR(TriangleBatchInfo, segment->batches)[i].facesOffset;
        verticesOffset = DKR_PTR(TriangleBatchInfo, segment->batches)[i].verticesOffset;
        nextFaceOffset = DKR_PTR(TriangleBatchInfo, segment->batches)[i + 1].facesOffset;
        if (DKR_PTR(TriangleBatchInfo, segment->batches)[i].flags & (RENDER_HIDDEN | RENDER_NO_COLLISION)) {
            currFaceOffset = nextFaceOffset;
        }
        for (j = currFaceOffset; j < nextFaceOffset; j++) {
            if ((DKR_PTR(Triangle, segment->triangles)[j].flags & BACKFACE_DRAW)) {
                continue;
            }
            var_t0 = 0;
            for (index = 0; index < 3; index++) {
                vert = &(DKR_PTR(Triangle, segment->triangles)[j].verticesArray[index + 1] + verticesOffset)[DKR_PTR(Vertex, segment->vertices)];
                spE8[index] = vert->x;
                spDC[index] = vert->y;
                spD0[index] = vert->z;
                spC4[index] = (arg1 * spE8[index]) + (arg2 * spD0[index]) + arg3;

                spF8[index] = (spC4[index] <= 0.0);
                var_t0 += (spF8[index] <= 0.0);
            }
            if ((var_t0 == 1) || (var_t0 == 2)) {
                for (var_s0 = 0, index = 0; index < 3; index++) {
                    nextIndex = index + 1;
                    if (nextIndex >= 3) {
                        nextIndex = 0;
                    }
                    if ((spF8[nextIndex] != spF8[index]) != 0) {
                        temp = spC4[index] / (spC4[index] - spC4[nextIndex]);
                        spB0[var_s0] = spE8[index] + ((spE8[nextIndex] - spE8[index]) * temp);
                        spB8[var_s0] = spDC[index] + ((spDC[nextIndex] - spDC[index]) * temp);
                        spA0[var_s0] = spB8[var_s0];
                        spA8[var_s0] = spD0[index] + ((spD0[nextIndex] - spD0[index]) * temp);
                        var_s0++;
                    }
                }

                var_s0 = 0;
                spF8[0] = 0;
                spF8[1] = 0;
                spC4[0] = (D_8011D4A0 * spB0[0]) + (D_8011D4A4 * spA8[0]) + D_8011D4A8;
                spC4[1] = (D_8011D4A0 * spB0[1]) + (D_8011D4A4 * spA8[1]) + D_8011D4A8;
                if (spC4[0] < -300.0) {
                    spF8[0] = 1;
                }
                if (spC4[0] > 300.0) {
                    spF8[0] |= 2;
                }
                if (spC4[1] < -300.0) {
                    spF8[1] = 1;
                }
                if (spC4[1] > 300.0) {
                    spF8[1] |= 2;
                }
                // clang-format off
                if ((spF8[0] | spF8[1]) == 0) {  var_s0 = 1; }
                // clang-format on
                else if ((spF8[1] != spF8[0]) != 0) {
                    index = 0;
                    if (spC4[1] < spC4[0]) {
                        index = 1;
                    }
                    nextIndex = 1 - index;
                    if (spF8[index] == 1) {
                        temp = ((-spC4[index] - 300.0) / (spC4[nextIndex] - spC4[index]));
                        spB8[index] = spB8[index] + ((spB8[nextIndex] - spB8[index]) * temp);
                        spC4[index] = -300.0f;
                    }
                    if (spF8[nextIndex] == 2) {
                        temp = ((spC4[nextIndex] - 300.0) / (spC4[nextIndex] - spC4[index]));
                        spB8[nextIndex] = spB8[nextIndex] + ((spB8[index] - spB8[nextIndex]) * temp);
                        spC4[nextIndex] = 300.0f;
                    }
                    var_s0 = 1;
                }
                if (var_s0 != 0) {
                    var_t0 = (DKR_PTR(CollisionFacetPlanes, segment->collisionFacets)[j].basePlaneIndex << 2);
                    temp = (spB0[0] + D_8011D4A0) * DKR_PTR(f32, segment->collisionPlanes)[var_t0];
                    temp += spB8[0] * DKR_PTR(f32, segment->collisionPlanes)[var_t0 + 1];
                    temp += (spA8[0] + D_8011D4A4) * DKR_PTR(f32, segment->collisionPlanes)[var_t0 + 2];
                    temp += DKR_PTR(f32, segment->collisionPlanes)[var_t0 + 3];
                    var_s0 = (temp > 0.0) << 2;
                    if (DKR_PTR(f32, segment->collisionPlanes)[var_t0 + 1] < 0.0f) {
                        var_s0 |= 1;
                    }
                    if (spC4[0] == spC4[1]) {
                        var_s0 |= 8;
                    }
                    func_80026C14(spC4[0], spB8[0], var_s0);
                    func_80026C14(spC4[1], spB8[1], var_s0);
                }
            }
        }
    }
}

void func_80026C14(s16 arg0, s16 arg1, s32 arg2) {
    s16 i;
    s16 j;

    if (D_8011D49E < D_8011D4BA) {
        i = 0;
        while (i < D_8011D49E && D_8011D478[i].unk0 < arg0) {
            i++;
        }
        while (i < D_8011D49E && arg0 == D_8011D478[i].unk0 && D_8011D478[i].unk2 < arg1) {
            i++;
        }
        j = D_8011D49E;
        while (i < j) {
            D_8011D478[j].unk0 = D_8011D478[j - 1].unk0;
            D_8011D478[j].unk2 = D_8011D478[j - 1].unk2;
            D_8011D478[j].unk7 = D_8011D478[j - 1].unk7;
            D_8011D478[j].unk6 = D_8011D478[j - 1].unk6;
            j--;
        }
        D_8011D478[i].unk0 = arg0;
        D_8011D478[i].unk2 = arg1;
        D_8011D478[i].unk4 = 0;
        D_8011D478[i].unk7 = D_8011D49C;
        D_8011D478[i].unk6 = arg2;
        D_8011D47C[D_8011D49E] = -1;
        if (D_8011D49E & 1) {
            D_8011D49C++;
        }
        D_8011D49E++;
    }
}

void func_80026E54(s16 arg0, s8 *arg1, f32 arg2, f32 arg3) {
    unk8011D478 *next;
    unk8011D478 *curr;
    s16 temp3;
    s16 temp4;
    f32 curr_unk0;
    f32 curr_unk2;
    f32 next_unk0;
    f32 next_unk2;
    s32 noSwap;
    s16 i;
    s16 j;
    f32 sp94[20];
    f32 sp6C[10];
    s8 sp60[10];
    s8 temp;
    s8 temp0;
    s8 temp1;
    s8 swapByte;

    if (arg0 >= 10 || arg0 == 0) {
        return;
    }

    for (j = 0, i = 0; i < arg0;) {
        temp = arg1[i];
        curr = &D_8011D478[D_8011D47C[(s16) (temp * 2)]];
        next = &D_8011D478[D_8011D47C[((s16) (temp * 2)) + 1]];
        curr_unk0 = (f32) curr->unk0;
        curr_unk2 = (f32) curr->unk2;
        next_unk0 = (f32) next->unk0;
        next_unk2 = (f32) next->unk2;
        if (curr_unk0 == next_unk0) {
            return;
        }
        sp94[j++] = ((next_unk2 - curr_unk2) * ((curr_unk0 - arg3) / ((curr_unk0 - next_unk0)))) + curr_unk2;
        sp6C[i] = sp94[j - 1];
        sp60[i] = i;
        sp94[j++] = ((curr_unk2 - next_unk2) * ((arg2 - next_unk0) / ((curr_unk0 - next_unk0)))) + next_unk2;
        sp6C[i] += sp94[j - 1];
        i++;
    }

    do {
        noSwap = TRUE;
        for (i = 0; i < arg0 - 1; i++) {
            if (sp6C[sp60[i + 1]] < sp6C[sp60[i]]) {
                swapByte = arg1[i];
                arg1[i] = arg1[i + 1];
                arg1[i + 1] = swapByte;
                swapByte = sp60[i];
                sp60[i] = sp60[i + 1];
                sp60[i + 1] = swapByte;
                noSwap = FALSE;
            }
        }
    } while (noSwap == FALSE);

    for (i = 0; i < arg0 - 1; i++) {
        temp0 = D_8011D478[D_8011D47C[arg1[i] * 2]].unk6 & 1;
        temp1 = D_8011D478[D_8011D47C[arg1[i + 1] * 2]].unk6 & 1;
        if (temp0 && !temp1) {
            temp3 = (sp60[i] * 2);
            temp4 = (sp60[i + 1] * 2);
            void_generate_primitive(&sp94[temp3], &sp94[temp4], arg3, arg2);
        }
    }
}

s32 void_generate_primitive(f32 *arg0, f32 *arg1, f32 arg2, f32 arg3) {
    Vertex *verts;
    Triangle *tris;
    s32 triIndex;
    s16 vertZ1;
    s16 vertX2;
    s16 vertZ2;
    s16 vertX1;
    u8 colour_r;
    u8 colour_g;
    u8 colour_b;
    u8 colour_a;

    if (gVoidPrimCount >= gVoidPrimLimit) {
        return 0;
    }

    if (gVoidVertCount == 24) {
#ifdef NATIVE_PORT
        /* See the flush site: void geometry never casts. */
        gfx_shadow_caster_exclude_mark(gVoidCurrTris);
#endif
        gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(gVoidCurrVerts), gVoidVertCount, 0);
        gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(gVoidCurrTris), (gVoidVertCount >> 1), TRIN_DISABLE_TEXTURE);
        gVoidCurrVerts = gTrackVtxPtr;
        gVoidVertCount = 0;
        gVoidCurrTris = gTrackTriPtr;
    }

    vertX1 = arg2 * D_8011D4A0 + D_8011D4AC;
    vertZ1 = arg2 * D_8011D4A4 + D_8011D4B0;
    vertX2 = arg3 * D_8011D4A0 + D_8011D4AC;
    vertZ2 = arg3 * D_8011D4A4 + D_8011D4B0;

    colour_r = gVoidColourR;
    colour_g = gVoidColourG;
    colour_b = gVoidColourB;
    colour_a = 0xFF;

    verts = gTrackVtxPtr;
    verts[0].x = vertX1;
    verts[0].y = arg0[0] + 2.0f;
    verts[0].z = vertZ1;
    verts[0].r = colour_r;
    verts[0].g = colour_g;
    verts[0].b = colour_b;
    verts[0].a = colour_a;

    verts[1].x = vertX2;
    verts[1].y = arg0[1] + 2.0f;
    verts[1].z = vertZ2;
    verts[1].r = colour_r;
    verts[1].g = colour_g;
    verts[1].b = colour_b;
    verts[1].a = colour_a;

    verts[2].x = vertX1;
    verts[2].y = arg1[0] - 2.0f;
    verts[2].z = vertZ1;
    verts[2].r = colour_r;
    verts[2].g = colour_g;
    verts[2].b = colour_b;
    verts[2].a = colour_a;

    verts[3].x = vertX2;
    verts[3].y = arg1[1] - 2.0f;
    verts[3].z = vertZ2;
    verts[3].r = colour_r;
    verts[3].g = colour_g;
    verts[3].b = colour_b;
    verts[3].a = colour_a;
    // @fake
    if (1) {}
    verts += 4;
    gTrackVtxPtr = verts;

    tris = gTrackTriPtr;
    triIndex = (u8) gVoidVertCount;

    // @fake, using index 0 directly doesn't match
    vertX2 = 0;
    tris[vertX2].flags = BACKFACE_DRAW;
    tris[vertX2].vi0 = triIndex + 2;
    tris[vertX2].vi1 = triIndex + 1;
    tris[vertX2].vi2 = triIndex;
    tris[vertX2].uv0.u = 1024 - 32;
    tris[vertX2].uv0.v = 1024 - 32;
    tris[vertX2].uv1.u = 1024 - 32;
    tris[vertX2].uv1.v = 0;
    tris[vertX2].uv2.u = 1;
    tris[vertX2].uv2.v = 0;

    tris[1].flags = BACKFACE_DRAW;
    tris[1].vi0 = triIndex + 3;
    tris[1].vi1 = triIndex + 1;
    tris[1].vi2 = triIndex + 2;
    tris[1].uv0.u = 1;
    tris[1].uv0.v = 1024 - 32;
    tris[1].uv1.u = 1024 - 32;
    tris[1].uv1.v = 1024 - 32;
    tris[1].uv2.u = 1;
    tris[1].uv2.v = 0;
    tris += 2;
    gTrackTriPtr = tris;

    gVoidVertCount += 4;
    gVoidPrimCount++;
    return 0;
}

s32 func_80027568(void) {
    LevelModelSegment *segment = NULL; // spE4
    s32 ret;
    s32 var_t4;
    f32 playerDist;
    f32 camDist;
    f32 scalingFactor;
    s32 curViewport;
    s32 flipSide;
    s32 numRacers; // spC4
    s32 i;
    s32 j;
    f32 var_f18;
    f32 A1, B1, C1, D1;
    f32 var_f20;
    f32 var_f22;
    f32 var_f24;
    CollisionNode *colNode;
    Object_Racer *racer;
    f32 A, B, C, D;
    Object **racerObjects; // sp80
    Object *racerObj;      // sp7C
    f32 *planes;

    racerObjects = get_racer_objects(&numRacers);
    if (numRacers == 0) {
        return FALSE;
    }
    if ((check_if_showing_cutscene_camera() != 0) || (gSceneActiveCamera->mode >= 5) ||
        (gSceneActiveCamera->mode == 3)) {
        return FALSE;
    }
    curViewport = get_current_viewport();
    racerObj = NULL;
    for (i = 0; i < numRacers; i++) {
        racer = racerObjects[i]->racer;
        if (curViewport == racer->playerIndex) {
            racerObj = racerObjects[i];
            i = numRacers; // Come on! Just use break!
        }
    }
    if (racerObj == NULL) {
        return FALSE;
    }
    generate_collision_candidates(1, &racerObj->trans.position, &gSceneActiveCamera->trans.position,
                                  VEHICLE_NO_OVERRIDE);
    ret = FALSE;
    for (var_t4 = 0; var_t4 < gNumCollisionCandidates && ret == FALSE; var_t4++) {
        flipSide = gCollisionCandidates[var_t4];
        if (flipSide > 0) {
            // this is segment Entry
#ifdef NATIVE_PORT
            segment = (LevelModelSegment *) DKR_COLL_DECODE_SEG(flipSide);
#else
            segment = (LevelModelSegment *) PHYS_TO_K0(flipSide);
#endif
        } else {
            if (segment == NULL) {
                /* Candidate facets are meaningful only after a segment anchor. */
                continue;
            }
#ifdef NATIVE_PORT
            colNode = (CollisionNode *) DKR_COLL_DECODE_FACET(flipSide);
#else
            colNode = (CollisionNode *) flipSide;
#endif
            curViewport = colNode->colPlaneIndex << 2;
            planes = &DKR_PTR(f32, segment->collisionPlanes)[curViewport];
            A = planes[0];
            B = planes[1];
            C = planes[2];
            D = planes[3];

            camDist = A * gSceneActiveCamera->trans.x_position + B * gSceneActiveCamera->trans.y_position +
                      C * gSceneActiveCamera->trans.z_position + D - 14.0;
            if (camDist < -0.1) {
                playerDist = A * racerObj->trans.x_position + B * racerObj->trans.y_position +
                             C * racerObj->trans.z_position + D;
                if (playerDist >= -0.1) {
                    var_f20 = (gSceneActiveCamera->trans.x_position - racerObj->trans.x_position);
                    var_f22 = (gSceneActiveCamera->trans.y_position - racerObj->trans.y_position);
                    var_f24 = (gSceneActiveCamera->trans.z_position - racerObj->trans.z_position);

                    if (playerDist != camDist) {
                        scalingFactor = playerDist / (playerDist - camDist);
                    } else {
                        scalingFactor = 0.0f;
                    }

                    var_f20 = racerObj->trans.x_position + (var_f20 * scalingFactor);
                    var_f22 = racerObj->trans.y_position + (var_f22 * scalingFactor);
                    var_f24 = racerObj->trans.z_position + (var_f24 * scalingFactor);

                    for (j = 0, ret = TRUE; j < 3 && ret == TRUE; j++) {
                        flipSide = FALSE;
                        curViewport = colNode->closestTri[j];
                        if (curViewport & 0x8000) {
                            curViewport &= 0x7FFF;
                            flipSide = TRUE;
                        }
                        curViewport = curViewport << 2;
                        planes = &DKR_PTR(f32, segment->collisionPlanes)[curViewport];
                        A1 = planes[0];
                        B1 = planes[1];
                        C1 = planes[2];
                        D1 = planes[3];
                        var_f18 = A1 * var_f20 + B1 * var_f22 + C1 * var_f24 + D1;
                        if (flipSide) {
                            var_f18 = -var_f18;
                        }
                        if (var_f18 > 4.0f) {
                            ret = FALSE;
                        }
                    }
                }
            }
        }
    }
    return ret;
}

/**
 * Sets up the camera placement for the 4th viewport when using T.T Cam in 3 player.
 * It utilises spectate points then points at the race leader.
 * Uses lookat smoothing when changing which player, otherwise, snaps if the camera itself changes.
 */
void ttcam_update(s32 updateRate) {
    s16 angleDiff;
    f32 xDelta;
    f32 yDelta;
    f32 zDelta;
    f32 xzSqr;
    Camera *camera;
    Object *thisObject;
    Object **racerGroup;
    Object *lastObject = NULL;
    Object *objectFirstPlace = NULL;
    Object_Racer *currentRacer;
    Object_Racer *lastRacer;
    Object_Racer *racerFirstPlace = NULL;
    s32 numRacers;
    s32 i;
    s32 cameraId;
    Object *camObj;
#ifdef NATIVE_PORT
    s32 lastRacerIndex = -1;
    s32 firstPlaceIndex = -1;
    s32 selectedIndex;
    s32 spectateIndex;
#endif

    racerGroup = get_racer_objects(&numRacers);
    lastRacer = NULL;
    for (i = 0; i < numRacers; i++) {
        if (1) {} // fake
        if (racerGroup[i] != NULL) {
            currentRacer = racerGroup[i]->racer;
#ifdef NATIVE_PORT
            cameraId = (i < (s32) ARRAY_COUNT(gTTCamSpectateIndex)) ? gTTCamSpectateIndex[i] : 0;
#else
            cameraId = currentRacer->cameraIndex;
#endif
            spectate_nearest(racerGroup[i], &cameraId);
#ifdef NATIVE_PORT
            if (i < (s32) ARRAY_COUNT(gTTCamSpectateIndex)) {
                gTTCamSpectateIndex[i] = cameraId;
            }
#else
            currentRacer->cameraIndex = cameraId;
#endif
            if (currentRacer->raceFinished) {
                if (currentRacer->finishPosition == 1) {
                    racerFirstPlace = currentRacer;
                    objectFirstPlace = racerGroup[i];
#ifdef NATIVE_PORT
                    firstPlaceIndex = i;
#endif
                }
            } else if (lastRacer == NULL) {
                lastRacer = currentRacer;
                lastObject = racerGroup[i];
#ifdef NATIVE_PORT
                lastRacerIndex = i;
#endif
            } else if (currentRacer->racePosition < lastRacer->racePosition) {
                lastRacer = currentRacer;
                lastObject = racerGroup[i];
#ifdef NATIVE_PORT
                lastRacerIndex = i;
#endif
            }
        }
    }
    if (lastRacer != NULL) {
        currentRacer = lastRacer;
        thisObject = lastObject;
#ifdef NATIVE_PORT
        selectedIndex = lastRacerIndex;
#endif
    } else if (racerFirstPlace != NULL) {
        currentRacer = racerFirstPlace;
        thisObject = objectFirstPlace;
#ifdef NATIVE_PORT
        selectedIndex = firstPlaceIndex;
#endif
    } else {
        /* No racers means there is no valid spectator target this frame. */
        return;
    }
#ifdef NATIVE_PORT
    spectateIndex = (selectedIndex >= 0 && selectedIndex < (s32) ARRAY_COUNT(gTTCamSpectateIndex))
                        ? gTTCamSpectateIndex[selectedIndex]
                        : 0;
#else
#define spectateIndex (currentRacer->cameraIndex)
#endif
    camObj = spectate_object(spectateIndex);
    if (gTTCamID != spectateIndex) {
        gTTCamSmoothTimer = 0;
#ifdef NATIVE_PORT
        /* The zeroed smooth timer IS the snap: below, the spectator's
         * orientation is written outright rather than eased, and its position
         * is written to a different camera object every tick regardless. Two
         * adjacent spectate points on the same track sit far inside the
         * snapshot's teleport threshold and share one camera slot, so nothing
         * about the pose says "cut" — this does.
         *
         * The argument is the VIEWPORT index; PLAYER_FOUR is this spectator's
         * viewport, and the gCameras[] slot it records may be that plus four
         * while a cutscene camera owns the viewport. */
        presentation_snapshot_note_camera_cut(PLAYER_FOUR);
#endif
    } else if (gTTCamPlayerID != currentRacer->playerIndex) {
        gTTCamSmoothTimer = 180;
        gTTCamPlayerID = currentRacer->playerIndex;
    }
    if (camObj != NULL) {
        camera = cam_get_active_camera_no_cutscenes();
        camera->trans.x_position = camObj->trans.x_position;
        camera->trans.y_position = camObj->trans.y_position;
        camera->trans.z_position = camObj->trans.z_position;
        xDelta = camera->trans.x_position - thisObject->trans.x_position;
        yDelta = camera->trans.y_position - thisObject->trans.y_position;
        zDelta = camera->trans.z_position - thisObject->trans.z_position;
        xzSqr = sqrtf((xDelta * xDelta) + (zDelta * zDelta));
        if (gTTCamSmoothTimer != 0) {
            angleDiff = ((s32) (-atan2s(xDelta, zDelta) - camera->trans.rotation.y_rotation) + 0x8000);
            //!@bug Never true, since angleDiff is signed. Should be >=.
            if (angleDiff > 0x8000) {
                angleDiff = -(0xFFFF - angleDiff);
            }
            camera->trans.rotation.y_rotation += ((s32) (angleDiff / (16.0f * (gTTCamSmoothTimer / 180.0f)))) & 0xFFFF;
            angleDiff = atan2s(yDelta, xzSqr) - camera->trans.rotation.x_rotation;
            //!@bug Never true, since angleDiff is signed. Should be >=.
            if (angleDiff > 0x8000) {
                angleDiff = -(0xFFFF - angleDiff);
            }
            camera->trans.rotation.x_rotation += ((s32) (angleDiff / (16.0f * (gTTCamSmoothTimer / 180.0f)))) & 0xFFFF;
            gTTCamSmoothTimer -= updateRate;
            if (gTTCamSmoothTimer < 0) {
                gTTCamSmoothTimer = 0;
            }
        } else {
            camera->trans.rotation.y_rotation = 0x8000 - atan2s(xDelta, zDelta);
            camera->trans.rotation.x_rotation = atan2s(yDelta, xzSqr);
        }
        camera->trans.rotation.z_rotation = 0;
        camera->cameraSegmentID = get_level_segment_index_from_position(camera->trans.x_position, currentRacer->oy1,
                                                                        camera->trans.z_position);
        gTTCamID = spectateIndex;
#ifdef NATIVE_PORT
        {
            MdkrCameraIntent intent;

            memset(&intent, 0, sizeof(intent));
            intent.camera_id = PLAYER_FOUR;
            intent.authored_mode = 0;
            intent.family = MDKR_CAMERA_INTENT_FAMILY_TT_SPECTATE;
            intent.desired_eye.x = camera->trans.x_position;
            intent.desired_eye.y = camera->trans.y_position;
            intent.desired_eye.z = camera->trans.z_position;
            /* T.T.'s exact post-selection target is the racer it just chose. */
            intent.pivot.x = thisObject->trans.x_position;
            intent.pivot.y = thisObject->trans.y_position;
            intent.pivot.z = thisObject->trans.z_position;
            intent.target = intent.pivot;
            /* The selected racer's transform is at road contact; target the
             * chassis center while preserving the authored orbit pivot. */
            intent.target.y += 20.0f;
            intent.target_valid = TRUE;
            camera_obstruction_intent_capture(&intent);
        }
#endif
    }
#ifndef NATIVE_PORT
#undef spectateIndex
#endif
}

#ifdef NATIVE_PORT
/**
 * Advance the authoritative three-player spectator camera once per fixed tick.
 *
 * The original renderer called ttcam_update() once from its optional fourth
 * viewport. Native presentation may elide or replay that viewport, so leaving
 * the update there made gCameras[PLAYER_FOUR], the TT target selection, and
 * next-tick sort/LOD/visibility depend on whether a frame happened to draw.
 * This predicate is the union of render_scene's two TT-camera branches: the
 * spectator camera advances for every ordinary three-player race whether its
 * viewport is visible (HUD setting 0) or hidden.
 */
void scene_tt_camera_tick(s32 updateRate) {
    s32 savedCamera;
    s32 viewports = scene_visibility_viewport_count();

    if (viewports != 3 || level_type() == RACETYPE_CHALLENGE_EGGS ||
        level_type() == RACETYPE_CHALLENGE_BATTLE ||
        level_type() == RACETYPE_CHALLENGE_BANANAS) {
        return;
    }

    savedCamera = get_current_viewport();
    set_active_camera(PLAYER_FOUR);
    ttcam_update(updateRate);
    set_active_camera(savedCamera);
}
#endif

/**
 * Handle the flipbook effect for level geometry textures.
 */
void track_tex_anim(s32 updateRate) {
    s32 segmentNumber, batchNumber;
    LevelModelSegment *segment;
    TextureHeader *texture;
    TriangleBatchInfo *batch;
    s32 temp;

    segment = DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments);
    for (segmentNumber = 0; segmentNumber < gCurrentLevelModel->numberOfSegments; segmentNumber++) {
        batch = DKR_PTR(TriangleBatchInfo, segment[segmentNumber].batches);
        for (batchNumber = 0; batchNumber < segment[segmentNumber].numberOfBatches; batchNumber++) {
            if (batch[batchNumber].flags & RENDER_TEX_ANIM) {
                if (batch[batchNumber].textureIndex != 0xFF) {
                    texture = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[batch[batchNumber].textureIndex].texture);
                    if (texture->numOfTextures != 0x100 && texture->frameAdvanceDelay) {
                        temp = batch[batchNumber].texOffset << 6;
                        if (batch[batchNumber].flags & RENDER_UNK_80000000) {
                            temp |= batch[batchNumber].miscData;
                            tex_animate_texture(texture, &batch[batchNumber].flags, &temp, updateRate);
                            batch[batchNumber].miscData = temp & 0x3F;
                        } else {
                            tex_animate_texture(texture, &batch[batchNumber].flags, &temp, updateRate);
                        }
                        batch[batchNumber].texOffset = (temp >> 6) & 0xFF;
                    }
                }
            }
        }
    }
}

/**
 * Spawns and initialises the skydome object seen ingame.
 * Skipped if the object ID doesn't exist.
 * Also compares a checksum which can potentially trigger anti-tamper measures.
 */
void skydome_spawn(s32 objectID) {
    LevelObjectEntryCommon spawnObject;

#ifdef ANTI_TAMPER
    // Antipiracy measure
    drm_checksum_balloon();
#endif
    if (objectID == -1) {
        gSkydomeSegment = NULL;
        return;
    }
    spawnObject.x = 0;
    spawnObject.y = 0;
    spawnObject.z = 0;
    spawnObject.size = sizeof(LevelObjectEntryCommon);
    spawnObject.objectID = objectID;
    gSkydomeSegment = spawn_object(&spawnObject, OBJECT_SPAWN_UNK02);
    if (gSkydomeSegment != NULL) {
        gSkydomeSegment->level_entry = NULL;
        gSkydomeSegment->objectID = -1;
    }
}

/**
 * Sets the status to render the sky.
 * If set to false, will skip the background and skydome rendering.
 */
void set_skydome_visbility(s32 renderSky) {
    gSceneRenderSkyDome = renderSky;
}

// This function creates the flashy sky effect in the wizpig 2 race.
void trackbg_render_flashy(void) {
    Triangle *tris;
    Vertex *verts;
    s32 vCoordMask;
    s32 uCoordMask;
    f32 scaledXSin;
    f32 scaledXCos;
    f32 var_f16;
    s16 uCoords[9];
    s16 vCoords[9];
    f32 xCos;
    f32 xSin;
    f32 pad_sp108;
    Camera *camera;
    f32 pad_sp100;
    f32 xPositions[9];
    f32 zPositions[9];
    Vec3f pos;
    s32 i;
    s32 var_v0;
    s32 var_v1;
    s32 var_a1;
    s32 var_a2;
    s32 var_a3;
    u8 *var_v0_3;
    f32 var_f14;
    s16 vertY;
    s16 vTempCoord;
    s16 uTempCoord;
    LevelHeader_70 *pad2;
    LevelHeader_70 *var_t2;
    LevelHeader_70 *levelHeader;
    TextureHeader *texHeader;
    s32 pad[4];

    verts = gTrackVtxPtr;
    tris = gTrackTriPtr;

    camera = cam_get_active_camera();
    texHeader = DKR_PTR(TextureHeader, gCurrentLevelHeader2->unkA4);
    uCoordMask = (texHeader->width << 5) - 1;
    vCoordMask = (texHeader->height << 5) - 1;
    xSin = sins_f(-camera->trans.rotation.x);
    xCos = coss_f(-camera->trans.rotation.x);

    scaledXSin = xSin * 1280.0f;
    scaledXCos = xCos * 1280.0f;
    pad_sp100 = 2.0f * scaledXSin;
    xPositions[0] = -scaledXCos - (xSin * 1280.0f);
    zPositions[0] = -scaledXCos + (xSin * 1280.0f);
    xPositions[1] = scaledXCos - (xSin * 1280.0f);
    zPositions[1] = -scaledXCos - (xSin * 1280.0f);
    xPositions[2] = scaledXCos + scaledXSin;
    zPositions[2] = scaledXCos - (xSin * 1280.0f);
    xPositions[3] = -scaledXCos + (xSin * 1280.0f);
    zPositions[3] = scaledXCos + (xSin * 1280.0f);
    xPositions[4] = 0.0f;
    zPositions[4] = 0.0f;

    xPositions[5] = -(xCos * 1280.0f) - (2.0f * scaledXSin);
    zPositions[5] = scaledXSin + -(2.0f * (xCos * 1280.0f));
    xPositions[6] = (xCos * 1280.0f) - (2.0f * scaledXSin);
    zPositions[6] = -(2.0f * (xCos * 1280.0f)) - scaledXSin;
    xPositions[7] = (xCos * 1280.0f) + (2.0f * scaledXSin);
    zPositions[7] = (2.0f * (xCos * 1280.0f)) - scaledXSin;
    xPositions[8] = -(xCos * 1280.0f) + (2.0f * scaledXSin);
    zPositions[8] = (2.0f * (xCos * 1280.0f)) + scaledXSin;

    scaledXCos = 1280.0f;
    var_f14 = scaledXCos * 0.25f;

    var_a1 = texHeader->width * 16 * gCurrentLevelHeader2->unkA0;
    var_a2 = texHeader->height * 16 * gCurrentLevelHeader2->unkA1;

    var_v0 = ((s32) (camera->trans.x_position * ((scaledXCos * 0.25f) / var_a1)) + (gCurrentLevelHeader2->unkA8 >> 4)) &
             uCoordMask;
    var_v1 = ((s32) (camera->trans.z_position * ((scaledXCos * 0.25f) / var_a2)) + (gCurrentLevelHeader2->unkAA >> 4)) &
             vCoordMask;

    var_f14 = var_a1 * xCos;
    pos.z = var_a1 * xCos;
    pos.x = var_a1 * xCos;
    var_f16 = var_a2 * xSin;
    xCos = var_f16;
    pad_sp108 = var_f16;

    // @fake
    var_a2 = texHeader->height * 16 * gCurrentLevelHeader2->unkA1;

    uCoords[0] = (s16) (-var_f14 - pad_sp108) + var_v0;
    vCoords[0] = (s16) (var_f16 - var_f14) + var_v1;
    uCoords[1] = (s16) (var_f14 - pad_sp108) + var_v0;
    vCoords[1] = (s16) (-var_f14 - var_f16) + var_v1;
    uCoords[2] = (s16) (var_f14 + var_f16) + var_v0;
    vCoords[2] = (s16) (var_f14 - var_f16) + var_v1;
    uCoords[3] = (s16) (var_f16 - var_f14) + var_v0;
    vCoords[3] = (s16) (var_f14 + var_f16) + var_v1;

    uCoords[4] = var_v0;
    vCoords[4] = var_v1;

    uCoords[5] = (s16) (-var_f14 - (2.0f * xCos)) + var_v0;
    vCoords[5] = (s16) (var_f16 - (2.0f * var_f14)) + var_v1;
    uCoords[6] = (s16) (var_f14 - (2.0f * xCos)) + var_v0;
    vCoords[6] = (s16) ((-(2.0f * var_f14)) - var_f16) + var_v1;
    uCoords[7] = (s16) (pos.f[2] + (2.0f * xCos)) + var_v0;
    vCoords[7] = (s16) ((2.0f * pos.x) - var_f16) + var_v1;
    uCoords[8] = (s16) ((2.0f * xCos) - pos.z) + var_v0;
    vCoords[8] = (s16) ((2.0f * pos.x) + var_f16) + var_v1;

    mtx_world_origin(&gTrackDL, &gTrackMtxPtr);

    var_a2 = -1;

    if ((u32) gCurrentLevelHeader2->unk74[0] != UINT32_MAX) {
        var_t2 =
            DKR_PTR(LevelHeader_70, gCurrentLevelHeader2->unk74[0]);
        if ((u32) gCurrentLevelHeader2->unk74[1] == UINT32_MAX) {
            levelHeader = var_t2;
        } else {
            levelHeader =
                DKR_PTR(LevelHeader_70, gCurrentLevelHeader2->unk74[1]);
        }
    } else {
        var_t2 = NULL;
    }

    var_a3 = COLOUR_RGBA32(255, 255, 255, 0);
    if (var_t2 != NULL) {
        var_a2 = var_t2->rgba.word;
        var_a3 = levelHeader->rgba.word & (~0xFF);
    }

    gfx_init_basic_xlu(&gTrackDL, 1, var_a2, var_a3);
    texHeader = set_animated_texture_header(texHeader, gTrackTexAnimOffset << 8);
    gDkrDmaDisplayList(gTrackDL++, OS_K0_TOKEN_TO_PHYSICAL(texHeader->cmd),
                       texHeader->numberOfCommands);
    gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(gTrackVtxPtr), 9, 0);
    gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(gTrackTriPtr), 8, 1);
    gDPPipeSync(gTrackDL++);
    if (var_t2 != NULL) {
        gDPSetPrimColor(gTrackDL++, 0, 0, 255, 255, 255, 255);
        gDPSetEnvColor(gTrackDL++, 255, 255, 255, 0);
    }
    rendermode_reset(&gTrackDL);

    vertY = camera->trans.y_position + 192.0f;
    for (i = 0; i < 9; i++) {
        verts->x = xPositions[i] + camera->trans.x_position;
        verts->y = vertY;
        verts->z = zPositions[i] + camera->trans.z_position;
        verts->r = 0xFF;
        verts->g = 0xFF;
        verts->b = 0xFF;
        verts->a = (i <= 4) ? (255) : (0);
        verts++;
    }

    var_v0_3 = D_800DC92C;
    for (i = 0; i < 8; i++) {
        tris->flags = BACKFACE_DRAW;
        tris->vi0 = *var_v0_3;
        tris->uv0.u = uCoords[*var_v0_3];
        tris->uv0.v = vCoords[*var_v0_3];
        var_v0_3 += 1;
        tris->vi1 = *var_v0_3;
        tris->uv1.u = uCoords[*var_v0_3];
        tris->uv1.v = vCoords[*var_v0_3];
        var_v0_3 += 1;
        tris->vi2 = *var_v0_3;
        tris->uv2.u = uCoords[*var_v0_3];
        tris->uv2.v = vCoords[*var_v0_3];
        var_v0_3 += 1;
        tris++;
    }

    gTrackVtxPtr = verts;
    gTrackTriPtr = tris;
}

/**
 * Instead of drawing the skydome with textures, draw a solid coloured background.
 * Using different colours set in the level header, the vertices are coloured and
 * it gives the background a gradient effect.
 */
void trackbg_render_gradient(void) {
    s16 z;
    UNUSED s16 pad;
    s16 y0;
    s16 y1;
    u8 headerRed0;
    u8 headerGreen0;
    u8 headerBlue0;
    u8 headerRed1;
    u8 headerGreen1;
    u8 headerBlue1;
    Vertex *verts;
    Triangle *tris;

    verts = (Vertex *) gTrackVtxPtr;
    tris = (Triangle *) gTrackTriPtr;

    headerRed0 = gCurrentLevelHeader2->BGColourTopR;
    headerGreen0 = gCurrentLevelHeader2->BGColourTopG;
    headerBlue0 = gCurrentLevelHeader2->BGColourTopB;
    headerRed1 = gCurrentLevelHeader2->BGColourBottomR;
    headerGreen1 = gCurrentLevelHeader2->BGColourBottomG;
    headerBlue1 = gCurrentLevelHeader2->BGColourBottomB;
    rendermode_reset(&gTrackDL);
    material_set_no_tex_offset(&gTrackDL, NULL, RENDER_FOG_ACTIVE);
    gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(verts), 4, 0);
    gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(tris), 2, 0);

    z = 20;
    if (osTvType == OS_TV_TYPE_PAL) {
        y0 = -180;
        y1 = 180;
    } else {
        y0 = -150;
        y1 = 150;
    }
    if (cam_get_viewport_layout() == TWO_PLAYERS) {
        y0 >>= 1;
        y1 >>= 1;
    }
    verts->x = -200;
    verts->y = y0;
    verts->z = z;
    verts->r = headerRed0;
    verts->g = headerGreen0;
    verts->b = headerBlue0;
    verts->a = 255;
    verts++;

    verts->x = 200;
    verts->y = y0;
    verts->z = z;
    verts->r = headerRed0;
    verts->g = headerGreen0;
    verts->b = headerBlue0;
    verts->a = 255;
    verts++;

    verts->x = -200;
    verts->y = y1;
    verts->z = z;
    verts->r = headerRed1;
    verts->g = headerGreen1;
    verts->b = headerBlue1;
    verts->a = 255;
    verts++;

    verts->x = 200;
    verts->y = y1;
    verts->z = z;
    verts->r = headerRed1;
    verts->g = headerGreen1;
    verts->b = headerBlue1;
    verts->a = 255;
    verts++;

    tris->flags = BACKFACE_DRAW;
    tris->vi0 = 2;
    tris->vi1 = 1;
    tris->vi2 = 0;
    tris->uv0.u = 0;
    tris->uv0.v = 0;
    tris->uv1.u = 0;
    tris->uv1.v = 0;
    tris->uv2.u = 0;
    tris->uv2.v = 0;
    tris++;

    tris->flags = BACKFACE_DRAW;
    tris->vi0 = 3;
    tris->vi1 = 2;
    tris->vi2 = 1;
    tris->uv0.u = 0;
    tris->uv0.v = 0;
    tris->uv1.u = 0;
    tris->uv1.v = 0;
    tris->uv2.u = 0;
    tris->uv2.v = 0;
    tris++;

    gTrackVtxPtr = verts;
    gTrackTriPtr = tris;
}

/**
 * Sets the position to the current camera's position then renders the skydome if set to be visible.
 */
void skydome_render(void) {
    Camera *cam;
    if (gSkydomeSegment == NULL) {
        return;
    }

    cam = cam_get_active_camera();
#ifdef NATIVE_PORT
    /* Render purity: the camera-follow is a temporary draw transform, not object
     * state. The skydome is a live Object in the authoritative hash, so an
     * in-place write would make the hash depend on whether this tick rendered.
     * Write for the draw, restore exactly afterwards; each viewport still draws
     * the dome at its own camera. */
    {
        f32 savedX = gSkydomeSegment->trans.x_position;
        f32 savedY = gSkydomeSegment->trans.y_position;
        f32 savedZ = gSkydomeSegment->trans.z_position;
        if (gCurrentLevelHeader2->skyDome == 0) {
            gSkydomeSegment->trans.x_position = cam->trans.x_position;
            gSkydomeSegment->trans.y_position = cam->trans.y_position;
            gSkydomeSegment->trans.z_position = cam->trans.z_position;
        }
        mtx_world_origin(&gTrackDL, &gTrackMtxPtr);
        if (gSceneRenderSkyDome) {
            /* Only when the transform above actually copied the camera's
             * position: a skyDome != 0 track's dome is level-authored
             * geometry at its own fixed position, not a camera follower, and
             * must never have its world translation substituted at replay.
             * Set AFTER mtx_world_origin (itself a registration) so the note
             * survives to the dome's own mtx_cam_push, inside render_object,
             * and not to the origin push. */
            if (gCurrentLevelHeader2->skyDome == 0) {
                mdkr_presentation_note_camera_locked();
            }
            render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, gSkydomeSegment);
        }
        gSkydomeSegment->trans.x_position = savedX;
        gSkydomeSegment->trans.y_position = savedY;
        gSkydomeSegment->trans.z_position = savedZ;
    }
#else
    if (gCurrentLevelHeader2->skyDome == 0) {
        gSkydomeSegment->trans.x_position = cam->trans.x_position;
        gSkydomeSegment->trans.y_position = cam->trans.y_position;
        gSkydomeSegment->trans.z_position = cam->trans.z_position;
    }

    mtx_world_origin(&gTrackDL, &gTrackMtxPtr);
    if (gSceneRenderSkyDome) {
        render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, gSkydomeSegment);
    }
#endif
}

/**
 * Sets up all of the required variables for the player's view perspective.
 * This includes setting up the camera index, viewport and
 */
void initialise_player_viewport_vars(s32 updateRate) {
    s32 i;
    s32 numRacers;
    s32 viewportID;
    Object **racers;
    s32 segmentIndex;
    Object_Racer *racer;

    gSceneActiveCamera = cam_get_active_camera();
    viewportID = get_current_viewport();
    compute_scene_camera_transform_matrix();
    update_envmap_position(gScenePerspectivePos.x / 65536.0f, gScenePerspectivePos.y / 65536.0f,
                           gScenePerspectivePos.z / 65536.0f);
    segmentIndex = gSceneActiveCamera->cameraSegmentID;
    if (segmentIndex > -1 && (segmentIndex < gCurrentLevelModel->numberOfSegments)) {
        gSceneStartSegment = DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentIndex].unk28;
    } else {
        gSceneStartSegment = -1;
    }
#ifndef NATIVE_PORT
    /* Dead render writes. All three are declared UNUSED "Set but never read" and
     * a grep of game/src + game/include finds no reader anywhere -- only these
     * three stores. */
    gPrevCameraX = gSceneActiveCamera->trans.x_position;
    gPrevCameraY = gSceneActiveCamera->trans.y_position;
    gPrevCameraZ = gSceneActiveCamera->trans.z_position;
#endif
    if (gWaveBlockCount != 0) {
        waves_visibility_reset();
        racers = get_racer_objects(&numRacers);
        if (gSceneActiveCamera->mode != CAMERA_FINISH_RACE && numRacers > 0 && !check_if_showing_cutscene_camera()) {
            i = -1;
            do {
                i++;
                racer = racers[i]->racer;
            } while (i < numRacers - 1 && viewportID != racer->playerIndex);
            waves_visibility(racers[i]->trans.x_position, racers[i]->trans.y_position, racers[i]->trans.z_position,
                             get_current_viewport(), updateRate);
        } else {
            waves_visibility(gSceneActiveCamera->trans.x_position, gSceneActiveCamera->trans.y_position,
                             gSceneActiveCamera->trans.z_position, get_current_viewport(), updateRate);
        }
    }
#ifndef NATIVE_PORT
    /* The other dead render write. This is the
     * only access to LevelHeader.unk3 (structs.h:447) in the tree -- grep finds
     * no reader. (game.c:181's `unk3 = 1` is LevelGlobalData's own, unrelated,
     * field.) Render was stamping a byte of the level header per viewport. */
    level_header()->unk3 = 1;
#endif
    render_level_geometry_and_objects();
}

/**
 * Enable or disable anti aliasing.
 * Improves visual quality at the cost of performance.
 */
void set_anti_aliasing(s32 setting) {
    gAntiAliasing = setting;
}

/**
 * Find which segments can and should be rendered, then render their opaque geometry.
 * Render all objects inside visible segments then render the level's semitransparent geometry.
 * Afterwards, render particles.
 */
#ifdef NATIVE_PORT
/* camera.c owns these; they have no accessor pair wide enough for what the tick
 * needs (gCutsceneCameraActive has only a getter and a clearing setter; the
 * extern is at the top of this file). */
/* objects.c: OBJECT_SLOT_COUNT, the gObjPtrList allocation size (objects.c:271). */
#define SCENE_DRAW_ORDER_MAX 512

/**
 * Select the camera the LAST render_level_geometry_and_objects call of this frame
 * will use, and build its view basis, without drawing anything.
 *
 * That is the sort obj_sort_tick has to reproduce: render sorted gObjPtrList once
 * per viewport and it is the last one that survived into the next tick, so the last
 * viewport's basis is the ordering the simulation actually observed.
 *
 * The camera choice mirrors render_scene exactly: the viewport loop promotes
 * viewport 0 to PLAYER_TWO for a single viewport under player-two control (and
 * viewport_main applies the same promotion again against gViewportLayout), and for
 * 3P with the HUD enabled the TT camera (PLAYER_FOUR) draws a fourth viewport after
 * the loop. That TT pass calls disable_cutscene_camera() first; that clear is a
 * simulation-visible write of its own and is NOT moved here, so the flag is
 * masked locally for the basis and put straight back.
 *
 * The reconstruction is measured, not assumed: a throwaway probe comparing the
 * gViewMatrixF built here against the one camSetProjMtx builds later in the same
 * frame reported 6000/6000 checks bit-identical on track 5 and on level 33.
 * Nothing between the tick and render_scene writes gCameras.
 */
void scene_build_last_viewport_basis(void) {
    s32 numViewports;
    s32 ttCam;
    s32 savedCutscene;
    s32 lastViewport;

    numViewports = cam_set_layout(gScenePlayerViewports);
    ttCam = numViewports == 3 && level_type() != RACETYPE_CHALLENGE_EGGS &&
            level_type() != RACETYPE_CHALLENGE_BATTLE && level_type() != RACETYPE_CHALLENGE_BANANAS &&
            hud_setting() == 0;

    if (ttCam) {
        set_active_camera(PLAYER_FOUR);
    } else {
        lastViewport = numViewports - 1;
        if (lastViewport < 0) {
            lastViewport = 0;
        }
        if (lastViewport == 0 && is_player_two_in_control() && numViewports == 1) {
            set_active_camera(PLAYER_TWO);
        } else {
            set_active_camera(lastViewport);
        }
        if (is_player_two_in_control() && cam_get_viewport_layout() == VIEWPORT_LAYOUT_1_PLAYER) {
            set_active_camera(PLAYER_TWO);
        }
    }

    savedCutscene = gCutsceneCameraActive;
    if (ttCam) {
        gCutsceneCameraActive = 0;
    }
    cam_build_view_basis();
    gCutsceneCameraActive = savedCutscene;
}

/* render_scene's own `cam_set_layout(gScenePlayerViewports)` (tracks.c:403), so the
 * tick sees the viewport count render is about to use rather than last frame's.
 * cam_set_layout is idempotent -- it only recomputes gViewportLayout/gNumCameras. */
s32 scene_visibility_viewport_count(void) {
    return cam_set_layout(gScenePlayerViewports);
}

/* The visible-segment set and the per-viewport invisibility mask that
 * scene_object_admitted() tests against, for the viewport most recently prepared
 * by scene_visibility_prepare_viewport(). */
static u8 sTickObjectsVisible[LEVEL_SEGMENT_MAX + 1];
static s32 sTickVisibleFlags;

static s32 scene_viewport_invisible_flag(void) {
    return (get_current_viewport() & 1) ? OBJ_FLAGS_INVIS_PLAYER2 :
                                          OBJ_FLAGS_INVIS_PLAYER1;
}

/**
 * The non-drawing half of render_scene's viewport prologue, so the fixed step can
 * answer "would viewport N admit this object?".
 *
 * `objRacer->unk201 = 30` is written in set_temp_model_transforms, which only runs
 * for objects the RENDER path admitted, and it gates AI RNG
 * (racer.c:9088 -> racer_AI_pathing_inputs, racer.c:382 roll_percent_chance ->
 * rand_range) and particle emission (racer.c:2247/3452). "Was drawn" must not be a
 * simulation input, so the same predicate is evaluated in the tick from the
 * ORIGINAL logical frusta and OR-ed across viewports, which is what this
 * reproduces: the camera basis (cam_build_view_basis), the cull planes
 * (func_8002A31C -- which keeps sFaithfulCullPlanes for object admission, see
 * check_if_in_draw_range), the BSP segment traversal, and then the identical
 * per-object test render_level_geometry_and_objects applies.
 *
 * `ttCam` selects the 3P TT-camera pass, render_scene's fourth viewport.
 */
void scene_visibility_prepare_viewport(s32 viewportIndex, s32 numViewports, s32 ttCam) {
    u8 segmentIds[LEVEL_SEGMENT_MAX];
    s32 numberOfSegments;
    s32 segmentIndex;
    s32 savedCutscene;
    s32 drawSegments;
    s32 i;

    if (ttCam) {
        set_active_camera(PLAYER_FOUR);
    } else {
        if (viewportIndex == 0 && is_player_two_in_control() && numViewports == 1) {
            set_active_camera(PLAYER_TWO);
        } else {
            set_active_camera(viewportIndex);
        }
        if (is_player_two_in_control() && cam_get_viewport_layout() == VIEWPORT_LAYOUT_1_PLAYER) {
            set_active_camera(PLAYER_TWO);
        }
    }

    savedCutscene = gCutsceneCameraActive;
    if (ttCam) {
        gCutsceneCameraActive = 0;
    }
    cam_build_view_basis();
    gCutsceneCameraActive = savedCutscene;

    func_8002A31C();

    gSceneActiveCamera = cam_get_active_camera();
    segmentIndex = gSceneActiveCamera->cameraSegmentID;
    if (segmentIndex > -1 && segmentIndex < gCurrentLevelModel->numberOfSegments) {
        gSceneStartSegment = DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentIndex].unk28;
    } else {
        gSceneStartSegment = -1;
    }

    if (gCurrentLevelModel->numberOfSegments > 1) {
        numberOfSegments = 0;
        traverse_segments_bsp_tree(0, 0, gCurrentLevelModel->numberOfSegments - 1, segmentIds, &numberOfSegments);
    } else {
        numberOfSegments = 1;
        segmentIds[0] = 0;
    }

    for (i = 1; i <= gCurrentLevelModel->numberOfSegments; i++) {
        sTickObjectsVisible[i] = FALSE;
    }
    sTickObjectsVisible[0] = TRUE;

    /* render_scene decides gDrawLevelSegments before its viewport loop (tracks.c:422);
     * recompute the same condition rather than reading last frame's value. */
    drawSegments = gCurrentLevelHeader2->race_type != RACETYPE_CUTSCENE_2;
    if (drawSegments) {
        for (i = 0; i < numberOfSegments; i++) {
            sTickObjectsVisible[segmentIds[i] + 1] = TRUE;
        }
    }
    if (gCurrentLevelModel->numberOfSegments < 2) {
        sTickObjectsVisible[1] = TRUE;
    }

    sTickVisibleFlags = scene_viewport_invisible_flag();
}

/**
 * "Would this viewport have called set_temp_model_transforms on this object?"
 *
 * That is the question obj_visibility_tick has to answer, because
 * set_temp_model_transforms is where `unk201 = 30` used to be written, and
 * render_object reaches it from THREE different object loops in
 * render_level_geometry_and_objects, not one.
 *
 * The union of TWO of render's three object loops, not loop 1 alone:
 *
 *   loop 1 (:2509-2551, the opaque pass) admits `visible == 255`;
 *   loop 3 (:2588-2632, the transparency + racer-FX pass) admits
 *          `0 < visible < 255`, and a translucent racer routes there ALWAYS --
 *          loop 1 cannot take it, by construction, since its own condition is
 *          the complement.
 *
 * Reproducing loop 1 alone would make every racer with transparency < 255
 * invisible to the tick: a time-trial ghost (whose opacity is racer->transparency
 * verbatim, check_if_in_draw_range's BHV_TIMETRIAL_GHOST case) and any racer
 * mid-fade after a hit. Losing unk201 there is not cosmetic -- it stops the AI
 * steering (racer.c:9088 gates racer_AI_pathing_inputs on it), stops the
 * balloon-upgrade roll_percent_chance -> rand_range (racer.c:382) and forces
 * particleEmittersEnabled to none (racer.c:2247/3452). Render gives those racers
 * the timer from loop 3, so the tick must too.
 *
 * Loop 2 (:2543-2578) is deliberately NOT in the union: its gate is
 * `objFlags & OBJ_FLAGS_UNK_0100`, and that flag is set in exactly one place --
 * particles.c (ten sites, all on Particle objects). A racer or a ghost can never
 * carry it, and obj_visibility_tick skips OBJ_FLAGS_PARTICLE objects anyway.
 *
 * The two passes are reproduced separately rather than collapsed because their
 * `visible` computations genuinely differ in two places: OBJ_FLAGS_UNK_0080
 * yields 0 in loop 1 and 1 in loop 3, and loop 3 additionally clamps a
 * `obj->behaviorId == BHV_RACER` object at >= 255 back to 0 (which is what keeps
 * an opaque racer out of the transparency pass -- and notably does NOT apply to
 * a ghost, whose obj->behaviorId is BHV_TIMETRIAL_GHOST).
 *
 * The fixed tick commits the authoritative opacity. Render evaluates the same
 * predicate into a viewport-local value and never writes back to the object.
 */
s32 scene_object_admitted(Object *obj) {
    s32 objFlags;
    s32 opaqueVisible;
    s32 blendVisible;

    if (obj == NULL) {
        return FALSE;
    }
    objFlags = obj->trans.flags;
    /* render_object's own early-out. */
    if (objFlags & (OBJ_FLAGS_INVISIBLE | OBJ_FLAGS_SHADOW_ONLY)) {
        return FALSE;
    }
    /* This viewport's per-player invisibility mask zeroes `visible` in BOTH
     * loops, so it rejects outright. */
    if (objFlags & sTickVisibleFlags) {
        return FALSE;
    }

    /* Loop 1's `visible`. */
    opaqueVisible = 255;
    if (objFlags & OBJ_FLAGS_UNK_0080) {
        opaqueVisible = 0;
    } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
        opaqueVisible = obj->opacity;
    }

    /* Loop 3's `visible`, including its racer clamp. */
    blendVisible = 255;
    if (objFlags & OBJ_FLAGS_UNK_0080) {
        blendVisible = 1;
    } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
        blendVisible = obj->opacity;
    }
    if (obj->behaviorId == BHV_RACER && blendVisible >= 255) {
        blendVisible = 0;
    }

    if (opaqueVisible != 255 && !(blendVisible > 0 && blendVisible < 255)) {
        return FALSE;
    }
    if (!check_if_in_draw_range(obj)) {
        return FALSE;
    }
    if (sTickObjectsVisible[obj->segmentID + 1]) {
        return TRUE;
    }
    /* The `unk34 > 1000` escape belongs to loop 1 alone; loop 3 requires the
     * segment to be visible outright. */
    return opaqueVisible == 255 && obj->unk34 > 1000.0;
}

/* Private, per-viewport back-to-front draw order. gObjPtrList and
 * obj->distanceToCamera belong to the tick now; these are render's own copies, so a
 * second or third viewport still draws its transparencies in ITS camera's order
 * without permuting the array the simulation iterates. */
static Object *sSceneDrawOrder[SCENE_DRAW_ORDER_MAX];
static f32 sSceneDrawDistance[SCENE_DRAW_ORDER_MAX];
static MdkrViewportRouteCache sSceneViewportRoutes;
static Object *sSceneViewportTestTarget;
static s32 sSceneRouteLastViewport;

static s32 scene_viewport_test_flag(
    const char *name, s32 *initialized, s32 *enabled) {
    if (!*initialized) {
        const char *value = getenv(name);
        *enabled = value != NULL && value[0] != '\0' && value[0] != '0';
        *initialized = TRUE;
    }
    return *enabled;
}

static s32 scene_viewport_test_fade_last(void) {
    static s32 initialized;
    static s32 enabled;
    return scene_viewport_test_flag(
        "MDKR_TEST_VIEWPORT_FADE_LAST", &initialized, &enabled);
}

static s32 scene_viewport_test_shared_routes(void) {
    static s32 initialized;
    static s32 enabled;
    return scene_viewport_test_flag(
        "MDKR_TEST_SHARED_VIEWPORT_ROUTES", &initialized, &enabled);
}

static s32 scene_viewport_test_trace_tick(void) {
    static s32 initialized;
    static s32 tick;
    if (!initialized) {
        const char *value = getenv("MDKR_TEST_VIEWPORT_ROUTE_TRACE_TICK");
        tick = value != NULL ? atoi(value) : -1;
        initialized = TRUE;
    }
    return tick;
}

static const char *scene_viewport_route_pass_name(
    MdkrViewportRoutePass pass) {
    static const char *const names[] = { "opaque", "special", "blend" };
    return (unsigned)pass < ARRAY_COUNT(names) ? names[pass] : "invalid";
}

/* This viewport's private distance for the object currently being drawn, and
 * whether one is in scope. obj->distanceToCamera is the TICK's value now (the last
 * viewport's), but two render-side consumers legitimately want THIS viewport's:
 * the racer LOD in set_temp_model_transforms (objects.c:5121 -- which writes
 * obj->modelIndex, and modelIndex also selects the collision model, so collapsing
 * it to one viewport would change split-screen simulation) and the shadow
 * mesh-detail ramp below. Handing them the private value keeps this migration
 * behaviour-identical at every player count. Both become dead when the LOD choice
 * itself moves into the tick. */
f32 gSceneDrawDistance;
s32 gSceneDrawDistanceValid;
static const Object *sSceneRenderOpacityObject;
static s32 sSceneRenderOpacity;

s32 scene_object_render_opacity(const Object *obj) {
    if (sSceneRenderOpacityObject == obj) {
        return sSceneRenderOpacity;
    }
    return obj->opacity;
}

static void scene_render_opacity_begin(const Object *obj, s32 opacity) {
    sSceneRenderOpacityObject = obj;
    sSceneRenderOpacity = opacity;
}

static void scene_render_opacity_end(const Object *obj) {
    if (sSceneRenderOpacityObject == obj) {
        sSceneRenderOpacityObject = NULL;
    }
}

/* The camera's emergency fade on the racer it is pushing through, applied to
 * whichever opacity the caller has. Split out from the reader below so the
 * widened-draw-distance path can re-apply it to an opacity it recomputed:
 * that enhancement widens the DISTANCE fade and must not un-fade a racer the
 * camera is inside. */
static s32 scene_camera_obstruction_clamp(const Object *obj, s32 opacity) {
    const s32 viewport = get_current_viewport();

    if (obj->behaviorId == BHV_RACER &&
        obj == get_racer_object_by_port(viewport)) {
        const s32 emergencyOpacity =
            camera_obstruction_racer_opacity_for_viewport(viewport);
        if (emergencyOpacity < opacity) {
            opacity = emergencyOpacity;
        }
    }
    return opacity;
}

static s32 scene_camera_obstruction_opacity(const Object *obj) {
    return scene_camera_obstruction_clamp(obj, obj->opacity);
}

static void scene_viewport_route_store(
    Object *obj, MdkrViewportRoutePass pass, s32 opacity, s32 visible) {
    s32 viewport = get_current_viewport();
    s32 stored = viewport >= 0 &&
        mdkr_viewport_route_cache_store(
            &sSceneViewportRoutes, obj, (unsigned)viewport, pass,
            opacity, visible);
    /* OBJECT_SLOT_COUNT and the cache both cap at 512. Reaching this assertion
     * means an invalid camera index or duplicate-capacity contract drift; a
     * release build fails closed by leaving the route absent. */
    assert(stored);
    if (stored && obj == sSceneViewportTestTarget &&
        g_simTickCounter == scene_viewport_test_trace_tick()) {
        fprintf(stderr,
                "[VIEWPORT-ROUTE] tick=%d viewport=%d pass=%s opacity=%d visible=%d shadow=%d water=%d\n",
                g_simTickCounter, viewport,
                scene_viewport_route_pass_name(pass), opacity, visible,
                obj->shadow != NULL,
                obj->waterEffect != NULL &&
                    (obj->header->flags & HEADER_FLAGS_WATER_EFFECT) != 0);
    }
}

static s32 scene_viewport_route_load(
    Object *obj, unsigned requestedViewport, unsigned sourceViewport,
    MdkrViewportRoutePass pass, MdkrViewportRoute *route) {
    s32 admitted = mdkr_viewport_route_cache_load(
        &sSceneViewportRoutes, obj, sourceViewport, pass, route);
    if (obj == sSceneViewportTestTarget &&
        g_simTickCounter == scene_viewport_test_trace_tick()) {
        fprintf(stderr,
                "[VIEWPORT-DRAW] tick=%d requested=%u source=%u pass=%s admitted=%d opacity=%d visible=%d shadow=%d water=%d\n",
                g_simTickCounter, requestedViewport, sourceViewport,
                scene_viewport_route_pass_name(pass), admitted,
                admitted ? route->opacity : -1,
                admitted ? route->visible : -1,
                obj->shadow != NULL,
                obj->waterEffect != NULL &&
                    (obj->header->flags & HEADER_FLAGS_WATER_EFFECT) != 0);
    }
    return admitted;
}

/* The distance key sort_objects_by_dist (objects.c:6045) would have written for
 * this object from the ACTIVE camera -- computed, not stored on the object. */
f32 scene_object_view_distance(Object *obj) {
    if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && (obj->header->flags & HEADER_FLAGS_UNK_0080)) {
        /* The "draw me last" sentinel; see the NATIVE_PORT note in
         * sort_objects_by_dist about why it is no longer accumulated. */
        return -16000.0f;
    }
    return -get_distance_to_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
}

/**
 * The key computation and the bubble sort of sort_objects_by_dist (objects.c:6045),
 * applied to a private array. Same keys and the same algorithm, so for a single
 * viewport this reproduces the order the tick already put gObjPtrList in.
 */
s32 scene_build_private_draw_order(s32 startIndex, s32 lastIndex) {
    s32 count;
    s32 i;
    s32 didNotSwap;
    Object *obj;
    f32 tempDist;
    extern s32 gObjectCount; /* objects.c */

    /*
     * render_level_geometry_and_objects reuses obj_sort_tick's partition
     * (gObjSortFirstActive/gObjSortObjCount) instead of re-deriving it, on the
     * premise that nothing between the tick and the draw creates or destroys an
     * object. gObjSortObjCount is just gObjectCount as of the tick
     * (get_first_active_object, objects.c), so that premise is exactly
     * `gObjSortObjCount == gObjectCount` and it is cheap to state.
     *
     * If it ever stops holding, the failure is silent and nasty rather than
     * loud: this walks get_object(i) over a range that no longer describes the
     * list, drawing stale or missing objects, and every symptom shows up as a
     * rendering oddity far from the object that was spawned. Plain assert():
     * Release compiles with NDEBUG, so it costs a shipping build nothing.
     */
    assert(gObjSortObjCount == gObjectCount);

    count = 0;
    if (lastIndex < startIndex) {
        return 0;
    }
    for (i = startIndex; i <= lastIndex && count < SCENE_DRAW_ORDER_MAX; i++) {
        obj = get_object(i);
        if (obj == NULL) {
            continue;
        }
        sSceneDrawDistance[count] = scene_object_view_distance(obj);
        sSceneDrawOrder[count] = obj;
        count++;
    }

    do {
        didNotSwap = TRUE;
        for (i = 0; i < count - 1; i++) {
            if (sSceneDrawDistance[i] < sSceneDrawDistance[i + 1]) {
                obj = sSceneDrawOrder[i];
                sSceneDrawOrder[i] = sSceneDrawOrder[i + 1];
                sSceneDrawOrder[i + 1] = obj;
                tempDist = sSceneDrawDistance[i];
                sSceneDrawDistance[i] = sSceneDrawDistance[i + 1];
                sSceneDrawDistance[i + 1] = tempDist;
                didNotSwap = FALSE;
            }
        }
    } while (!didNotSwap);

    return count;
}

/**
 * Execute the authoritative side of render_scene's object/weather/HUD
 * traversal once per fixed tick. This preserves DKR's authored RNG ordering:
 * ordinary objects first, then single-player weather, then that viewport's
 * HUD. render_level_geometry_and_objects() stays read-only over simulation
 * state.
 */
static s32 scene_weather_rng_early_for_test(void) {
    static s32 initialized;
    static s32 enabled;

    if (!initialized) {
        const char *value = getenv("MDKR_TEST_WEATHER_RNG_EARLY");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
        initialized = TRUE;
    }
    return enabled;
}

void scene_authoritative_render_tick(s32 updateRate) {
    s32 numViewports;
    s32 savedCamera;
    s32 ttCam;
    s32 pass;
    s32 privateCount;
    s32 visibleFlags;
    s32 visible;
    s32 objFlags;
    s32 i;
    s32 weatherUpdateRate;
    s32 weatherRanEarly;
    s32 viewportFadeTest;
    s32 objectOpacity;
    Object *obj;

    mdkr_viewport_route_cache_reset(&sSceneViewportRoutes);
    sSceneViewportTestTarget = NULL;
    sSceneRouteLastViewport = -1;
    viewportFadeTest = scene_viewport_test_fade_last();

    /* A deliberately wrong-order test arm. It is never active in production;
     * the weather oracle uses it to prove that its digest detects the audited
     * object/weather RNG-order regression rather than merely checking rerun
     * determinism. */
    weatherRanEarly = scene_weather_rng_early_for_test();
    weatherUpdateRate = is_game_paused() ? 0 : updateRate;
    if (weatherRanEarly) {
        scene_weather_tick(weatherUpdateRate);
    }
    if (gCurrentLevelModel == NULL) {
        if (!weatherRanEarly) {
            scene_weather_tick(weatherUpdateRate);
        }
        return;
    }
    savedCamera = get_current_viewport();
    numViewports = scene_visibility_viewport_count();
    ttCam = numViewports == 3 && level_type() != RACETYPE_CHALLENGE_EGGS &&
            level_type() != RACETYPE_CHALLENGE_BATTLE &&
            level_type() != RACETYPE_CHALLENGE_BANANAS && hud_setting() == 0;

    for (pass = 0; pass < numViewports + (ttCam ? 1 : 0); pass++) {
        scene_visibility_prepare_viewport(pass, numViewports,
                                          pass >= numViewports);
        if (pass == numViewports - 1) {
            sSceneRouteLastViewport = get_current_viewport();
        }
        privateCount = scene_build_private_draw_order(
            gObjSortFirstActive, gObjSortObjCount - 1);
        visibleFlags = scene_viewport_invisible_flag();

        /* Opaque pass, front to back. */
        for (i = 0; i < privateCount; i++) {
            obj = sSceneDrawOrder[i];
            if (viewportFadeTest && pass == 0 &&
                sSceneViewportTestTarget == NULL &&
                obj->behaviorId == BHV_RACER && obj->racer != NULL &&
                obj->racer->playerIndex == PLAYER_ONE) {
                sSceneViewportTestTarget = obj;
                obj->opacity = 255;
            }
            objectOpacity = scene_camera_obstruction_opacity(obj);
            objFlags = obj->trans.flags;
            visible = 255;
            if (objFlags & OBJ_FLAGS_UNK_0080) {
                visible = 0;
            } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
                visible = objectOpacity;
            }
            if (objFlags & visibleFlags) {
                visible = 0;
            }
            if (viewportFadeTest &&
                obj == sSceneViewportTestTarget &&
                pass == numViewports - 1) {
                visible = 0;
            }
            if (visible == 255 && check_if_in_draw_range(obj) &&
                (sTickObjectsVisible[obj->segmentID + 1] ||
                 obj->unk34 > 1000.0)) {
                scene_viewport_route_store(
                    obj, MDKR_VIEWPORT_ROUTE_OPAQUE,
                    objectOpacity, visible);
                obj_authoritative_texture_tick(
                    obj, updateRate, sSceneDrawDistance[i]);
            }
        }

        /* OBJ_FLAGS_UNK_0100 pass, back to front. */
        for (i = privateCount - 1; i >= 0; i--) {
            obj = sSceneDrawOrder[i];
            objectOpacity = scene_camera_obstruction_opacity(obj);
            objFlags = obj->trans.flags;
            visible = !(objFlags & visibleFlags);
            if (visible && (objFlags & OBJ_FLAGS_UNK_0100) &&
                sTickObjectsVisible[obj->segmentID + 1] &&
                check_if_in_draw_range(obj)) {
                scene_viewport_route_store(
                    obj, MDKR_VIEWPORT_ROUTE_SPECIAL,
                    objectOpacity, visible);
                obj_authoritative_texture_tick(
                    obj, updateRate, sSceneDrawDistance[i]);
            }
        }

        /* Transparent/racer-FX pass, back to front. */
        for (i = privateCount - 1; i >= 0; i--) {
            obj = sSceneDrawOrder[i];
            objectOpacity = scene_camera_obstruction_opacity(obj);
            objFlags = obj->trans.flags;
            visible = 255;
            if (objFlags & OBJ_FLAGS_UNK_0080) {
                visible = 1;
            } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
                visible = objectOpacity;
            }
            if (objFlags & visibleFlags) {
                visible = 0;
            }
            if (obj->behaviorId == BHV_RACER && visible >= 255) {
                visible = 0;
            }
            if (obj->behaviorId == BHV_RACER && objectOpacity < 255) {
                visible = objectOpacity;
            }
            if (viewportFadeTest &&
                obj == sSceneViewportTestTarget &&
                pass == numViewports - 1) {
                visible = 64;
            }
            if (visible < 255 &&
                sTickObjectsVisible[obj->segmentID + 1] &&
                check_if_in_draw_range(obj)) {
                if (viewportFadeTest &&
                    obj == sSceneViewportTestTarget &&
                    pass == numViewports - 1) {
                    /* Deliberately leave the old shared field holding the last
                     * viewport's fade. Correct rendering must still use each
                     * earlier viewport's retained route. */
                    obj->opacity = 64;
                    objectOpacity = 64;
                }
                scene_viewport_route_store(
                    obj, MDKR_VIEWPORT_ROUTE_BLEND,
                    objectOpacity, visible);
                if (visible > 0) {
                    obj_authoritative_texture_tick(
                        obj, updateRate, sSceneDrawDistance[i]);
                }
            }
        }

        /* Canonical render_scene order for one-player weather is objects ->
         * weather -> HUD. Weather's own viewport gate makes this a no-op in
         * multiplayer; run it only once even for the optional 3P TT camera. */
        if (pass == 0 && !weatherRanEarly) {
            scene_weather_tick(weatherUpdateRate);
        }

        /* The optional fourth 3P TT-camera pass has no HUD. */
        if (pass < numViewports) {
            hud_authoritative_rng_tick_viewport(updateRate);
        }
    }

    gSceneDrawDistanceValid = FALSE;
    set_active_camera(savedCamera);
}

/* Defined with check_if_in_draw_range below, next to the predicate it wraps. */
static s32 scene_object_wide_draw_range(Object *obj, s32 *outOpacity,
                                        s32 *outAuthoredOpacity,
                                        s32 *outAuthored);

/* Enhancements.DrawDistance, applied where it belongs: to the DRAW.
 *
 * `routeAdmitted`, `*visible` and `*renderOpacity` arrive holding whatever the
 * fixed tick's route table said. At the authored setting they are returned
 * untouched without the predicate being evaluated at all, so a default frame is
 * the frame this port drew before the enhancement existed.
 *
 * Above the authored setting, two read-only things happen:
 *
 *   - an object the tick ROUTED keeps its route, and therefore its pass, its
 *     ordering and its per-viewport identity. Only its opacity is revisited,
 *     and only if the widened fade band actually changes it -- which happens
 *     exactly for the ring of objects that were fading out at the authored
 *     radius, and is the pop-in the setting exists to remove. The camera
 *     obstruction fade the route was carrying is re-applied on top.
 *
 *   - an object the tick did NOT route because the authored distance rejected
 *     it has this pass's admission re-derived: the same three conditions
 *     render_level_geometry_and_objects applies, against the same per-viewport
 *     segment table. An object the authored distance ADMITS is never added
 *     here, so nothing can be drawn twice and the route table is never
 *     second-guessed -- only extended.
 *
 * Nothing in here writes to an Object, to obj->opacity or to the route cache,
 * and it calls no *_tick. That is the whole claim: the set of objects that
 * EXIST, and every value the simulation reads back, are identical at 100% and
 * at 400%; only the set that is DRAWN moves.
 */
static s32 scene_wide_draw_route(Object *obj, MdkrViewportRoutePass pass,
                                 const u8 *segmentVisible, s32 visibleFlags,
                                 s32 routeAdmitted, s32 *visible,
                                 s32 *renderOpacity) {
    s32 extended = FALSE;
    s32 wideOpacity;
    s32 authoredOpacity;
    s32 authored;
    s32 objFlags;
    s32 wideVisible;

    /* Particles are exempt from the authored distance test entirely
     * (check_if_in_draw_range_impl's OBJ_FLAGS_PARTICLE guard), so there is no
     * threshold here for the setting to widen. */
    if (obj == NULL || mdkr_enh_draw_distance_scale() == 1.0f ||
        (obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
        goto tally;
    }
    if (!scene_object_wide_draw_range(obj, &wideOpacity, &authoredOpacity,
                                      &authored)) {
        /* Outside even the widened radius, or outside the frustum. A routed
         * object cannot reach here: the widened radius contains the authored
         * one and the frustum test is the same one the tick applied. */
        goto tally;
    }
    if (routeAdmitted) {
        if (wideOpacity != authoredOpacity) {
            *renderOpacity = scene_camera_obstruction_clamp(obj, wideOpacity);
        }
        goto tally;
    }
    if (authored) {
        goto tally;
    }

    objFlags = obj->trans.flags;
    switch (pass) {
        case MDKR_VIEWPORT_ROUTE_OPAQUE:
            if (objFlags & OBJ_FLAGS_UNK_0080) {
                wideVisible = 0;
            } else {
                wideVisible = wideOpacity;
            }
            if (objFlags & visibleFlags) {
                wideVisible = 0;
            }
            if (wideVisible != 255 ||
                !(segmentVisible[obj->segmentID + 1] || obj->unk34 > 1000.0)) {
                goto tally;
            }
            break;
        case MDKR_VIEWPORT_ROUTE_SPECIAL:
            if ((objFlags & visibleFlags) || !(objFlags & OBJ_FLAGS_UNK_0100) ||
                !segmentVisible[obj->segmentID + 1]) {
                goto tally;
            }
            wideVisible = TRUE;
            break;
        default: /* MDKR_VIEWPORT_ROUTE_BLEND */
            if (objFlags & OBJ_FLAGS_UNK_0080) {
                wideVisible = 1;
            } else {
                wideVisible = wideOpacity;
            }
            if (objFlags & visibleFlags) {
                wideVisible = 0;
            }
            if (obj->behaviorId == BHV_RACER && wideVisible >= 255) {
                wideVisible = 0;
            }
            if (wideVisible >= 255 || !segmentVisible[obj->segmentID + 1]) {
                goto tally;
            }
            break;
    }
    *visible = wideVisible;
    *renderOpacity = scene_camera_obstruction_clamp(obj, wideOpacity);
    routeAdmitted = TRUE;
    extended = TRUE;

tally:
    /* Silent unless MDKR_DRAWDIST_TRACE=1, and tallied at the authored setting
     * too, so a test has a baseline drawn-count to compare the widened one
     * against. */
    if (routeAdmitted) {
        mdkr_enh_draw_distance_count(extended ? 0 : 1, extended ? 1 : 0);
    }
    return routeAdmitted;
}
#endif

void render_level_geometry_and_objects(void) {
    s32 objCount;
    s32 numberOfSegments;
    s32 objFlags;
    s32 sp160;
    s32 i;
    s32 visibleFlags;
    u8 segmentIds[LEVEL_SEGMENT_MAX];
    /* Indexed as segmentID + 1; slot zero represents objects without a segment. */
    u8 objectsVisible[LEVEL_SEGMENT_MAX + 1];
    s32 visible;
    s32 routeAdmitted;
#ifdef NATIVE_PORT
    s32 renderOpacity;
#else
#define CHECK_DRAW_RANGE_RENDER(o) check_if_in_draw_range(o)
#endif
    Object *obj;
#ifdef NATIVE_PORT
    s32 privateCount;
    unsigned routeViewport;
    unsigned routeSourceViewport;
    MdkrViewportRoute route;
#endif

    func_80012C30();

    if (get_settings()->courseId == ASSET_LEVEL_OPENINGSEQUENCE) {
        gAntiAliasing = TRUE;
    }

#ifdef NATIVE_PORT
    /* obj_sort_tick() partitioned the list this tick; reuse its answer instead of
     * re-deriving it (get_first_active_object writes gFirstActiveObjectId, which the
     * simulation also maintains at objects.c:3869-3870). Nothing between the tick and
     * here creates or destroys objects: gParticlePtrList_flush() runs before the tick
     * block in thread3_main.c, and render only queues particles for the next flush. */
    sp160 = gObjSortFirstActive;
    objCount = gObjSortObjCount;
#else
    sp160 = get_first_active_object(&objCount);
#endif

    if (gCurrentLevelModel->numberOfSegments > 1) {
        numberOfSegments = 0;
        traverse_segments_bsp_tree(0, 0, gCurrentLevelModel->numberOfSegments - 1, segmentIds, &numberOfSegments);
    } else {
        numberOfSegments = 1;
        segmentIds[0] = 0;
    }

    for (i = 1; i <= gCurrentLevelModel->numberOfSegments; i++) {
        objectsVisible[i] = FALSE; // why not a bzero?
    }

    objectsVisible[0] = TRUE;

    if (gDrawLevelSegments) {
        for (i = 0; i < numberOfSegments; i++) {
            render_level_segment(segmentIds[i], FALSE); // Render opaque segments
            objectsVisible[segmentIds[i] + 1] = TRUE;
        }
    }

    if (gCurrentLevelModel->numberOfSegments < 2) {
        objectsVisible[1] = TRUE;
    }

    rendermode_reset(&gTrackDL);
#ifdef NATIVE_PORT
    /* obj_sort_tick() owns gObjPtrList's order and obj->distanceToCamera. This
     * viewport still needs ITS OWN back-to-front order, so build it in a private
     * array with private distances -- same keys, same bubble sort, nothing written
     * back to the simulation's array or to the objects. */
    privateCount = scene_build_private_draw_order(sp160, objCount - 1);
#else
    sort_objects_by_dist(sp160, objCount - 1);
#endif
#ifdef NATIVE_PORT
    visibleFlags = scene_viewport_invisible_flag();
    routeViewport = (unsigned)get_current_viewport();
    routeSourceViewport = scene_viewport_test_shared_routes() &&
            sSceneRouteLastViewport >= 0 ?
        (unsigned)sSceneRouteLastViewport : routeViewport;
#else
    visibleFlags = OBJ_FLAGS_INVIS_PLAYER1 << (get_current_viewport() & 1);
#endif

#ifdef NATIVE_PORT
    for (i = 0; i < privateCount; i++) {
        obj = sSceneDrawOrder[i];
        gSceneDrawDistance = sSceneDrawDistance[i];
        gSceneDrawDistanceValid = TRUE;
#else
    for (i = sp160; i < objCount; i++) {
        obj = get_object(i);
#endif
#ifdef NATIVE_PORT
        routeAdmitted = scene_viewport_route_load(
            obj, routeViewport, routeSourceViewport,
            MDKR_VIEWPORT_ROUTE_OPAQUE, &route);
        if (routeAdmitted) {
            visible = route.visible;
            renderOpacity = route.opacity;
        }
        routeAdmitted = scene_wide_draw_route(
            obj, MDKR_VIEWPORT_ROUTE_OPAQUE, objectsVisible, visibleFlags,
            routeAdmitted, &visible, &renderOpacity);
#else
        visible = 255;
        objFlags = obj->trans.flags;
        if (objFlags & OBJ_FLAGS_UNK_0080) {
            visible = 0;
        } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
            visible = obj->opacity;
        }
        if (objFlags & visibleFlags) {
            visible = 0;
        }
        routeAdmitted = obj != NULL && visible == 255 &&
            CHECK_DRAW_RANGE_RENDER(obj) &&
            (objectsVisible[obj->segmentID + 1] || obj->unk34 > 1000.0);
#endif
        if (routeAdmitted) {
#ifdef NATIVE_PORT
            scene_render_opacity_begin(obj, renderOpacity);
#endif
            if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
#ifdef NATIVE_PORT
                scene_render_opacity_end(obj);
#endif
                continue;
#ifdef NATIVE_PORT
            } else if (obj->shadow != NULL && TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
            } else if (obj->shadow != NULL) {
#endif
                shadow_render(obj, obj->shadow);
            }
            render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
#ifdef NATIVE_PORT
            if (obj->waterEffect != NULL && obj->header->flags & HEADER_FLAGS_WATER_EFFECT &&
                TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
            if (obj->waterEffect != NULL && obj->header->flags & HEADER_FLAGS_WATER_EFFECT) {
#endif
                watereffect_render(obj, obj->waterEffect);
            }
#ifdef NATIVE_PORT
            scene_render_opacity_end(obj);
#endif
        }
    }

#ifdef NATIVE_PORT
    for (i = privateCount - 1; i >= 0; i--) {
        obj = sSceneDrawOrder[i];
        gSceneDrawDistance = sSceneDrawDistance[i];
        gSceneDrawDistanceValid = TRUE;
#else
    for (i = objCount - 1; i >= sp160; i--) {
        obj = get_object(i);
#endif
#ifdef NATIVE_PORT
        routeAdmitted = scene_viewport_route_load(
            obj, routeViewport, routeSourceViewport,
            MDKR_VIEWPORT_ROUTE_SPECIAL, &route);
        if (routeAdmitted) {
            visible = route.visible;
            renderOpacity = route.opacity;
        }
        routeAdmitted = scene_wide_draw_route(
            obj, MDKR_VIEWPORT_ROUTE_SPECIAL, objectsVisible, visibleFlags,
            routeAdmitted, &visible, &renderOpacity);
#else
        objFlags = obj->trans.flags;
        if (objFlags & visibleFlags) {
            visible = FALSE;
        } else {
            visible = TRUE;
        }
        routeAdmitted = obj != NULL && visible &&
            objFlags & OBJ_FLAGS_UNK_0100 &&
            objectsVisible[obj->segmentID + 1] &&
            CHECK_DRAW_RANGE_RENDER(obj);
#endif
        if (routeAdmitted) {
#ifdef NATIVE_PORT
            scene_render_opacity_begin(obj, renderOpacity);
#endif
            if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
#ifdef NATIVE_PORT
                scene_render_opacity_end(obj);
#endif
                continue;
#ifdef NATIVE_PORT
            } else if (obj->shadow != NULL && TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
            } else if (obj->shadow != NULL) {
#endif
                shadow_render(obj, obj->shadow);
            }
            render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
#ifdef NATIVE_PORT
            if (obj->waterEffect != NULL && obj->header->flags & HEADER_FLAGS_WATER_EFFECT &&
                TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
            if (obj->waterEffect != NULL && obj->header->flags & HEADER_FLAGS_WATER_EFFECT) {
#endif
                watereffect_render(obj, obj->waterEffect);
            }
#ifdef NATIVE_PORT
            scene_render_opacity_end(obj);
#endif
        }
    }

    if (gDrawLevelSegments) {
        for (i = numberOfSegments - 1; i >= 0; i--) {
            render_level_segment(segmentIds[i], TRUE); // Render transparent segments
        }
    }

    if (gWaveBlockCount != 0) {
        waves_render(&gTrackDL, &gTrackMtxPtr, get_current_viewport());
    }

    rendermode_reset(&gTrackDL);
    material_set_no_tex_offset(&gTrackDL, NULL, RENDER_FOG_ACTIVE | RENDER_Z_COMPARE);
    func_80012C3C(&gTrackDL);

    // Particles and FX
#ifdef NATIVE_PORT
    for (i = privateCount - 1; i >= 0; i--) {
        obj = sSceneDrawOrder[i];
        gSceneDrawDistance = sSceneDrawDistance[i];
        gSceneDrawDistanceValid = TRUE;
#else
    for (i = objCount - 1; i >= sp160; i--) {
        obj = get_object(i);
#endif
#ifdef NATIVE_PORT
        routeAdmitted = scene_viewport_route_load(
            obj, routeViewport, routeSourceViewport,
            MDKR_VIEWPORT_ROUTE_BLEND, &route);
        if (routeAdmitted) {
            visible = route.visible;
            renderOpacity = route.opacity;
        }
        routeAdmitted = scene_wide_draw_route(
            obj, MDKR_VIEWPORT_ROUTE_BLEND, objectsVisible, visibleFlags,
            routeAdmitted, &visible, &renderOpacity);
#else
        visible = 255;
        objFlags = obj->trans.flags;
        if (objFlags & OBJ_FLAGS_UNK_0080) {
            visible = 1;
        } else if (!(objFlags & OBJ_FLAGS_PARTICLE)) {
            visible = obj->opacity;
        }
        if (objFlags & visibleFlags) {
            visible = 0;
        }
        if (obj->behaviorId == BHV_RACER && visible >= 255) {
            visible = 0;
        }
        routeAdmitted = obj != NULL && visible < 255 &&
            objectsVisible[obj->segmentID + 1] &&
            CHECK_DRAW_RANGE_RENDER(obj);
#endif
        if (routeAdmitted) {
#ifdef NATIVE_PORT
            scene_render_opacity_begin(obj, renderOpacity);
#endif
            if (visible > 0) {
                if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                    render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
                    goto skip;
#ifdef NATIVE_PORT
                } else if (obj->shadow != NULL && TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
                } else if (obj->shadow != NULL) {
#endif
                    shadow_render(obj, obj->shadow);
                }
                render_object(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
#ifdef NATIVE_PORT
                if ((obj->waterEffect != 0) && (obj->header->flags & HEADER_FLAGS_WATER_EFFECT) &&
                    TAJ_DONOR_PRESENTATION_VISIBLE(obj)) {
#else
                if ((obj->waterEffect != 0) && (obj->header->flags & HEADER_FLAGS_WATER_EFFECT)) {
#endif
                    watereffect_render(obj, obj->waterEffect);
                }
            }
        skip:
            if (obj->behaviorId == BHV_RACER) {
                render_racer_shield(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
                render_racer_magnet(&gTrackDL, &gTrackMtxPtr, &gTrackVtxPtr, obj);
            }
#ifdef NATIVE_PORT
            scene_render_opacity_end(obj);
#endif
        }
    }

#ifdef NATIVE_PORT
    /* Out of scope: render_object callers outside these loops (the skydome, menu
     * props) fall back to obj->distanceToCamera. */
    gSceneDrawDistanceValid = FALSE;
#endif

    if (gVoidData != NULL && func_80027568()) {
        void_check(segmentIds, numberOfSegments, get_current_viewport());
    }
    gAntiAliasing = FALSE;
#ifndef NATIVE_PORT
#undef CHECK_DRAW_RANGE_RENDER
#endif
}

/**
 * Render a batch of level geometry.
 * Since opaque and transparent are done in two separate runs, it will skip over the other.
 * Has a special case for the flashing lights in Spaceport Alpha, too.
 */
void render_level_segment(s32 segmentId, s32 nonOpaque) {
    LevelModelSegment *segment;
    s32 i;
    TriangleBatchInfo *batchInfo;
    TextureHeader *texture;
    s32 renderBatch;
    s32 numberVertices;
    s32 numberTriangles;
#ifdef NATIVE_PORT
    /* LP64: hold the full 64-bit host arena pointer, matching every other
     * gSPVertexDKR / gSPPolygon call site (game_ui.c, menu.c, weather.c). The
     * original decomp declares these s32 and casts the address with `(s32)&...`
     * below; on a 64-bit host that truncates the reconstructed arena pointer and,
     * because s32 is signed, sign-extends any pointer whose low-32 has bit 31 set
     * (ASLR-dependent) into a wild 0xffffffff.. value. OS_K0_TO_PHYSICAL then
     * registers that wild pointer, so dkr_resolve hands it straight back and
     * dkr_sp_vertex faults. Keeping the real pointer lets OS_K0_TO_PHYSICAL
     * tokenize the arena address correctly. (docs/OPEN_ITEMS.md: intermittent
     * char-select SIGSEGV.) */
    Vertex *vertices;
    Triangle *triangles;
#else
    s32 vertices;
    s32 triangles;
#endif
    s32 color;
    s32 isInvisible;
    s32 levelHeaderIndex;
    s32 texOffset;
    s32 sp78;
    s32 startPos;
    s32 endPos;
    s32 batchFlags;
    s32 textureFlags;
    //! @bug: batchInfo is uninitalized
#ifdef NATIVE_PORT
    /* The read below dereferences `batchInfo` before it is assigned. On N64 the
     * value is dead (recomputed identically at line ~1950 inside the loop before
     * any use — both numberVertices reads are loop-internal) and the stray read
     * from an uninitialised RDRAM address never faults; on the host the garbage
     * pointer segfaults. Elide the dead pre-read. */
    numberVertices = 0;
    (void) batchInfo;
#else
    numberVertices = (batchInfo + 1)->verticesOffset - batchInfo->verticesOffset;
#endif
    segment = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentId];
    sp78 = (nonOpaque && gWaveBlockCount) ? waves_block_hq(segment) : 0;
    if (nonOpaque) {
        startPos = segment->numberofOpaqueBatches;
        endPos = segment->numberOfBatches;
    } else {
        startPos = 0;
        endPos = segment->numberofOpaqueBatches;
    }
    for (i = startPos; i < endPos; i++) {
        batchInfo = &DKR_PTR(TriangleBatchInfo, segment->batches)[i];
        textureFlags = RENDER_NONE;
        isInvisible = batchInfo->flags & RENDER_HIDDEN;
        if (isInvisible) {
            continue;
        }
        batchFlags = batchInfo->flags;
        renderBatch = 0;
        if (batchInfo->textureIndex == 0xFF) {
            texture = FALSE;
        } else {
            texture = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[batchInfo->textureIndex].texture);
            textureFlags = texture->flags;
        }
        batchFlags |= RENDER_FOG_ACTIVE | RENDER_Z_COMPARE;
        if (!(batchFlags & RENDER_CUTOUT) && !(batchFlags & RENDER_DECAL)) {
            batchFlags |= gAntiAliasing;
        }
        if ((!(textureFlags & RENDER_SEMI_TRANSPARENT) && !(batchFlags & RENDER_WATER)) || batchFlags & RENDER_DECAL) {
            renderBatch = TRUE;
        }
        if (nonOpaque) {
            renderBatch = (renderBatch + 1) & 1;
        }
        if (sp78 && batchFlags & RENDER_WATER) {
            renderBatch = FALSE;
        }
        if (!renderBatch) {
            continue;
        }
        numberTriangles = batchInfo->facesOffset;
        do { // Fakematch
            numberVertices = (batchInfo + 1)->verticesOffset - batchInfo->verticesOffset;
            numberTriangles = (batchInfo + 1)->facesOffset - numberTriangles;
#ifdef NATIVE_PORT
            vertices = &DKR_PTR(Vertex, segment->vertices)[batchInfo->verticesOffset];
#else
            vertices = (s32) &DKR_PTR(Vertex, segment->vertices)[batchInfo->verticesOffset];
#endif
        } while (0);
#ifdef NATIVE_PORT
        triangles = &DKR_PTR(Triangle, segment->triangles)[batchInfo->facesOffset];
        /* Honor the ROM's authored FLAG_FORCE_NO_SHADOWS: these batches
         * (scrolling rivers, ice sheets, other animated surfaces the artists
         * excluded) must not enter the shadow caster feed even when their
         * render mode is opaque. */
        if (batchFlags & RENDER_NO_SHADOW) {
            gfx_shadow_caster_exclude_mark(triangles);
        }
#else
        triangles = (s32) &DKR_PTR(Triangle, segment->triangles)[batchInfo->facesOffset];
#endif
        texOffset = batchInfo->texOffset << 14;
        levelHeaderIndex = (batchFlags >> 28) & 7;
        if (levelHeaderIndex != (batchInfo->verticesOffset * 0)) {
#ifdef NATIVE_PORT
            /* The N64 expression reads the 4-byte slot at LevelHeader+0x70 +
             * index*4 (unk70[0] then unk74[0..6], contiguous dkrptr32 slots) as a
             * LevelHeader_70* and derefs ->rgba. On LP64 that pointer arithmetic
             * is width-dependent, so index the contiguous slot array directly and
             * reconstruct the arena pointer. */
            LevelHeader_70 *lh70 =
                DKR_PTR(LevelHeader_70, (&gCurrentLevelHeader2->unk70[0])[levelHeaderIndex]);
            gDPSetEnvColor(gTrackDL++, lh70->rgba.r, lh70->rgba.g, lh70->rgba.b, lh70->rgba.a);
#else
            gDPSetEnvColor(
                gTrackDL++,
                ((LevelHeader_70 *) ((u8 **) (&((LevelHeader **) gCurrentLevelHeader2)[levelHeaderIndex]))[28])->rgba.r,
                ((LevelHeader_70 *) ((u8 **) (&((LevelHeader **) gCurrentLevelHeader2)[levelHeaderIndex]))[28])->rgba.g,
                ((LevelHeader_70 *) ((u8 **) (&((LevelHeader **) gCurrentLevelHeader2)[levelHeaderIndex]))[28])->rgba.b,
                ((LevelHeader_70 *) ((u8 **) (&((LevelHeader **) gCurrentLevelHeader2)[levelHeaderIndex]))[28])
                    ->rgba.a);
#endif
        } else {
            gDPSetEnvColor(gTrackDL++, 255, 255, 255, 0);
        }
        if (batchFlags & RENDER_PULSING_LIGHTS) {
            color = DKR_PTR(PulsatingLightData, gCurrentLevelHeader2->pulseLightData)->outColorValue;
            gDPSetPrimColor(gTrackDL++, 0, 0, color, color, color, color);
            material_set_blinking_lights(&gTrackDL, texture, batchFlags, texOffset);
            gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(vertices), numberVertices, 0);
            gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(triangles), numberTriangles, TRIN_ENABLE_TEXTURE);
            gDPSetPrimColor(gTrackDL++, 0, 0, 255, 255, 255, 255);
        } else {
            material_set(&gTrackDL, texture, batchFlags, texOffset);
            batchFlags = TRUE;
            if (texture == NULL) {
                batchFlags = FALSE;
            }
            gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(vertices), numberVertices, 0);
            gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(triangles), numberTriangles, batchFlags);
        }
    }
}

/**
 * Parse through applicable segments in the level.
 * Uses function recursion to ensure adjacent segments remain next to each other in the list.
 */
void traverse_segments_bsp_tree(s32 nodeIndex, s32 segmentIndex, s32 segmentIndex2, u8 *segmentsOrder,
                                s32 *segmentsOrderIndex) {
    BspTreeNode *curNode;
    s32 camValue;

    curNode = &DKR_PTR(BspTreeNode, gCurrentLevelModel->segmentsBspTree)[nodeIndex];
    if (curNode->splitType == 0) {
        camValue = gSceneActiveCamera->trans.x_position; // Camera X
    } else if (curNode->splitType == 1) {
        camValue = gSceneActiveCamera->trans.y_position; // Camera Y
    } else {
        camValue = gSceneActiveCamera->trans.z_position; // Camera Z
    }

    if (camValue < curNode->splitValue) {
        if (curNode->leftNode != -1) {
            traverse_segments_bsp_tree(curNode->leftNode, segmentIndex, curNode->segmentIndex - 1, segmentsOrder,
                                       segmentsOrderIndex);
        } else {
            add_segment_to_order(segmentIndex, segmentsOrderIndex, segmentsOrder);
        }

        if (curNode->rightNode != -1) {
            traverse_segments_bsp_tree(curNode->rightNode, curNode->segmentIndex, segmentIndex2, segmentsOrder,
                                       segmentsOrderIndex);
        } else {
            add_segment_to_order(segmentIndex2, segmentsOrderIndex, segmentsOrder);
        }
    } else {
        if (curNode->rightNode != -1) {
            traverse_segments_bsp_tree(curNode->rightNode, curNode->segmentIndex, segmentIndex2, segmentsOrder,
                                       segmentsOrderIndex);
        } else {
            add_segment_to_order(segmentIndex2, segmentsOrderIndex, segmentsOrder);
        }

        if (curNode->leftNode != -1) {
            traverse_segments_bsp_tree(curNode->leftNode, segmentIndex, curNode->segmentIndex - 1, segmentsOrder,
                                       segmentsOrderIndex);
        } else {
            add_segment_to_order(segmentIndex, segmentsOrderIndex, segmentsOrder);
        }
    }
}

/**
 * Add this segment index to the specified segment ordering table if the segment in question is in view of the camera.
 */
void add_segment_to_order(s32 segmentIndex, s32 *segmentsOrderIndex, u8 *segmentsOrder) {
    u32 temp;
    if (segmentIndex < gCurrentLevelModel->numberOfSegments) {
        if (gSceneStartSegment != -1) {
            temp = DKR_PTR(u8, gCurrentLevelModel->segmentsBitfields)[gSceneStartSegment + (segmentIndex >> 3)];
            temp >>= segmentIndex & 7;
            temp &= 0xFF;
        } else {
            temp = 1;
        }
        if (temp & 1 && block_visible(&DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[segmentIndex])) {
            segmentsOrder[(*segmentsOrderIndex)++] = segmentIndex;
        }
    }
}

/**
 * Checks if the active camera is currently inside this segment.
 * Has a small inner margin where it doesn't consider the camera inside.
 * Goes unused.
 */
UNUSED s32 check_if_inside_segment(Object *obj, s32 segmentIndex) {
    LevelModelSegmentBoundingBox *bb;
    s32 x, y, z;
    if (segmentIndex >= gCurrentLevelModel->numberOfSegments) {
        return FALSE;
    }
    bb = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[segmentIndex];
    x = obj->trans.x_position;
    y = obj->trans.y_position;
    z = obj->trans.z_position;
    if ((x < (bb->x2 + 25)) && ((bb->x1 - 25) < x) && (z < (bb->z2 + 25)) && ((bb->z1 - 25) < z) &&
        (y < (bb->y2 + 25)) && ((bb->y1 - 25) < y)) {
        return TRUE;
    }

    return FALSE;
}

/**
 * Iterates through every existing segment to see which one the active camera is inside.
 * Uses mainly a two dimensional axis check here, instead of the function above.
 * Returns the segment currently inside.
 * Official Name: trackGetBlock
 */
s32 get_level_segment_index_from_position(f32 xPos, f32 yPos, f32 zPos) {
    LevelModelSegmentBoundingBox *bb;
    s32 i;
    s32 z = zPos;
    s32 x = xPos;
    s32 y = yPos;
    s32 minVal;
    s32 result;
    s32 heightDiff;

    if (gCurrentLevelModel == NULL) {
        return -1;
    }

    minVal = 1000000;
    result = -1;

    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        bb = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i];
        if (x < bb->x2 && bb->x1 < x && z < bb->z2 && bb->z1 < z) {
            heightDiff = (bb->y2 + bb->y1) >> 1; // Couldn't get / 2 to match, but >> 1 does.
            heightDiff = y - heightDiff;
            if (heightDiff < 0) {
                heightDiff = -heightDiff;
            }
            if (heightDiff < minVal) {
                minVal = heightDiff;
                result = i;
            }
        }
    }

    return result;
}

/**
 * Iterates through every existing segment to see which one the active camera is inside.
 * Uses mainly a two dimensional axis check here, instead of the function above.
 * Increments a counter based on if it's got a camera inside.
 * Because there's a tiny margin, multiple segments can be considered populated,
 * meaning that sometimes it will 2 instead of 1.
 */
#ifdef NATIVE_PORT
/*
 * NATIVE_PORT: `maxOut` is an ADDED parameter -- the capacity of the caller's
 * array. The ROM's signature has no such thing, so this is a host adaptation and
 * is gated; the ROM writes one s32 per overlapping segment through a bare pointer
 * and simply cannot stop.
 *
 * Its one caller, get_level_segment_waves(), holds `s32 segmentsInside[8]` and
 * checks `segmentCount >= 8` only AFTER the call returns, so with 8 or more
 * overlapping segments the ROM has already written past the array before anything
 * looks at the count. No instrument can see it: UBSan array-bounds needs an
 * indexed array type and this callee has only a pointer, which is why
 * tests/check_array_bounds_sweep.py records it as its own blind spot.
 *
 * WHAT THE CALLER SHOULD DO WHEN THE BOUND IS HIT IS WHAT IT ALREADY DOES.
 * `cnt` is deliberately NOT clamped, so the return value stays byte-identical to
 * the ROM's on every input, and the caller's existing `segmentCount >= 8` test --
 * the ROM's own reference behaviour -- keeps rejecting exactly the overflowing
 * case by returning 0 waves. The only thing this changes is that the writes stop.
 * Clamping `cnt` to 8 would also satisfy that test, but it would make the return
 * value diverge for no gain, and clamping to 7 would silently turn an overflow
 * into "7 valid segments", which is a behaviour change.
 *
 * NOT REACHED on any measured route: peak 4 of 8 (track 4), 0 calls at the
 * bound, across all 20 main tracks, all 10 boss tracks, the Adventure hub, the
 * attract demo and the menu routes -- 6500 to 13000 frames each. It is in because
 * the failure mode is a silent stack smash, and because the neighbouring
 * `inSegs[28]` peak is also 4, i.e. the margin here is the smallest of the class.
 * MDKR_SEGMARGIN=<n> widens the 4-unit acceptance margin so a check can drive the
 * count past the bound; MDKR_SEGBOUND=legacy restores the unbounded write.
 */
s32 get_inside_segment_count_xz(s32 x, s32 z, s32 *arg2, s32 maxOut) {
#else
s32 get_inside_segment_count_xz(s32 x, s32 z, s32 *arg2) {
#endif
    s32 i;
    s32 cnt = 0;
    LevelModelSegmentBoundingBox *bb;
#ifdef NATIVE_PORT
    /* TEST HOOKS, both no-ops unless set. `m` is the ROM's 4. */
    s32 m = mdkr_seg_margin();
    s32 legacy = mdkr_segbound_legacy();
#else
    const s32 m = 4;
#endif
    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        bb = DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes) + i;
        if (x < bb->x2 + m && bb->x1 - m < x && z < bb->z2 + m && bb->z1 - m < z) {
#ifdef NATIVE_PORT
            if (cnt < maxOut || legacy) {
                *arg2 = i;
                arg2++;
            }
            cnt++;
#else
            *arg2 = i;
            cnt++;
            arg2++;
#endif
        }
    }
#ifdef NATIVE_PORT
    mdkr_bound_probe(0, cnt, maxOut);
#endif
    return cnt;
}

/**
 * Carbon copy of the above function, but takes into account the Y axis, too.
 * Official name: trackGetCubeBlockList
 */
#ifdef NATIVE_PORT
/* NATIVE_PORT: `maxOut` added, exactly as on get_inside_segment_count_xz() above
 * and for the same reason -- one s32 per overlapping segment through a bare
 * pointer with no bound. Its one caller (func_8002F440) holds `s32 inSegs[28]`
 * and, unlike the xz caller, has NO count check at all: it loops `i < segs`
 * straight over whatever came back. Measured peak 4 of 28, 0 calls at the bound,
 * over the same 33 runs -- the widest margin of the three, and left bounded
 * anyway so the class has no unbounded member left. `cnt` is not clamped, so the
 * return value is unchanged. */
s32 get_inside_segment_count_xyz(s32 *arg0, s16 xPos1, s16 yPos1, s16 zPos1, s16 xPos2, s16 yPos2, s16 zPos2,
                                 s32 maxOut) {
#else
s32 get_inside_segment_count_xyz(s32 *arg0, s16 xPos1, s16 yPos1, s16 zPos1, s16 xPos2, s16 yPos2, s16 zPos2) {
#endif
    s32 cnt;
    s32 i;
    LevelModelSegmentBoundingBox *bb;

    xPos1 -= 4;
    yPos1 -= 4;
    zPos1 -= 4;
    xPos2 += 4;
    yPos2 += 4;
    zPos2 += 4;

    i = 0;
    cnt = 0;

    while (i < gCurrentLevelModel->numberOfSegments) {
        bb = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[i];
        if ((bb->x2 >= xPos1) && (xPos2 >= bb->x1) && (bb->z2 >= zPos1) && (zPos2 >= bb->z1) && (bb->y2 >= yPos1) &&
            (yPos2 >= bb->y1)) {
#ifdef NATIVE_PORT
            cnt++;
            if (cnt <= maxOut || mdkr_segbound_legacy()) {
                *arg0++ = i;
            }
#else
            cnt++;
            *arg0++ = i;
#endif
        }
        i++;
    }
#ifdef NATIVE_PORT
    mdkr_bound_probe(1, cnt, maxOut);
#endif
    return cnt;
}

/**
 * Returns this block data.
 */
LevelModelSegment *block_get(s32 segmentID) {
    if (segmentID < 0 || gCurrentLevelModel->numberOfSegments < segmentID) {
        return NULL;
    }

    return &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentID];
}

/**
 * Returns the bounding box data of this block.
 * Official name: trackBlockDim
 */
LevelModelSegmentBoundingBox *block_boundbox(s32 segmentID) {
    if (segmentID < 0 || gCurrentLevelModel->numberOfSegments < segmentID) {
        return NULL;
    }

    return &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[segmentID];
}

/*
 * Build one of DKR's three camera-local cull triangles as a normalized
 * world-space plane. sideX only affects the two horizontal planes.
 */
#ifdef NATIVE_PORT
static void mdkr_build_cull_plane(MtxF *cameraMatrix, s32 planeIndex,
                                  f32 sideX, Vec4f *out) {
    f32 ox1;
    f32 oy1;
    f32 oz1;
    f32 ox2;
    f32 oy2;
    f32 oz2;
    f32 ox3;
    f32 oy3;
    f32 oz3;
    f32 inverseMagnitude;
    f32 x;
    f32 y;
    f32 z;
    f32 w;

    x = D_800DC8AC[planeIndex][0].x;
    y = D_800DC8AC[planeIndex][0].y;
    z = D_800DC8AC[planeIndex][0].z;
    ox1 = x;
    oy1 = y;
    oz1 = z;
    mtxf_transform_point(*cameraMatrix, x, y, z, &ox1, &oy1, &oz1);

    x = D_800DC8AC[planeIndex][1].x;
    if (planeIndex == 1) x = sideX;
    if (planeIndex == 2) x = -sideX;
    y = D_800DC8AC[planeIndex][1].y;
    z = D_800DC8AC[planeIndex][1].z;
    ox2 = x;
    oy2 = y;
    oz2 = z;
    mtxf_transform_point(*cameraMatrix, x, y, z, &ox2, &oy2, &oz2);

    x = D_800DC8AC[planeIndex][2].x;
    if (planeIndex == 1) x = sideX;
    if (planeIndex == 2) x = -sideX;
    y = D_800DC8AC[planeIndex][2].y;
    z = D_800DC8AC[planeIndex][2].z;
    ox3 = x;
    oy3 = y;
    oz3 = z;
    mtxf_transform_point(*cameraMatrix, x, y, z, &ox3, &oy3, &oz3);

    x = ((oz2 - oz3) * oy1) + (oy2 * (oz3 - oz1)) + (oy3 * (oz1 - oz2));
    y = ((ox2 - ox3) * oz1) + (oz2 * (ox3 - ox1)) + (oz3 * (ox1 - ox2));
    z = ((oy2 - oy3) * ox1) + (ox2 * (oy3 - oy1)) + (ox3 * (oy1 - oy2));
    inverseMagnitude = (1.0 / sqrtf((x * x) + (y * y) + (z * z)));
    if (inverseMagnitude > 0.0) {
        x *= inverseMagnitude;
        y *= inverseMagnitude;
        z *= inverseMagnitude;
    }
    w = -((ox1 * x) + (oy1 * y) + (oz1 * z));
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
}

#endif

void func_8002A31C(void) {
    f32 ox1;
    f32 oy1;
    f32 oz1;
    f32 ox2;
    f32 oy2;
    f32 oz2;
    f32 ox3;
    f32 oy3;
    f32 oz3;
    MtxF *cameraMatrix;
    f32 inverseMagnitude;
    f32 x;
    f32 y;
    f32 z;
    s32 i;
    f32 w;
#ifdef NATIVE_PORT
    f32 cullSideX;
    f32 halfHorizontalFov;
    f32 projectedSlope;

    /*
     * The original side planes use |x|/|dz| = 130/100 (about 104.9 degrees
     * total), deliberately wider than the original 75.2-degree 4:3 lens. Keep
     * that exact guard band until the real host lens exceeds it, then expand in
     * lockstep with the projection plus 5% hysteresis. This prevents ultrawide
     * edge level geometry from popping without narrowing any legacy route.
     * Object admission deliberately keeps sFaithfulCullPlanes; see
     * check_if_in_draw_range().
     */
    cullSideX = 130.0f;
    halfHorizontalFov = cam_get_effective_horizontal_fov() * 0.5f;
    if (halfHorizontalFov > 0.0f && halfHorizontalFov < 87.0f) {
        projectedSlope = tanf(halfHorizontalFov * (3.14159265358979323846f / 180.0f)) * 1.05f;
        if (projectedSlope > 1.3f) {
            cullSideX = projectedSlope * 100.0f;
            if (cullSideX > 2000.0f) {
                cullSideX = 2000.0f;
            }
        }
    }
#endif

    cameraMatrix = get_projection_matrix_f32();
#ifdef NATIVE_PORT
    for (i = 0; i < ARRAY_COUNT(D_8011D0F8); i++) {
        mdkr_build_cull_plane(cameraMatrix, i, 130.0f, &sFaithfulCullPlanes[i]);
        mdkr_build_cull_plane(cameraMatrix, i, cullSideX, &D_8011D0F8[i]);
    }
#else
    for (i = 0; i < ARRAY_COUNT(D_8011D0F8);) {
        x = D_800DC8AC[i][0].x;
        y = D_800DC8AC[i][0].y;
        z = D_800DC8AC[i][0].z;
        ox1 = x;
        oy1 = y;
        oz1 = z;
        mtxf_transform_point(*cameraMatrix, x, y, z, &ox1, &oy1, &oz1);
        x = D_800DC8AC[i][1].x;
#ifdef NATIVE_PORT
        if (i == 1) x = cullSideX;
        if (i == 2) x = -cullSideX;
#endif
        y = D_800DC8AC[i][1].y;
        z = D_800DC8AC[i][1].z;
        ox2 = x;
        oy2 = y;
        oz2 = z;
        mtxf_transform_point(*cameraMatrix, x, y, z, &ox2, &oy2, &oz2);
        x = D_800DC8AC[i][2].x;
#ifdef NATIVE_PORT
        if (i == 1) x = cullSideX;
        if (i == 2) x = -cullSideX;
#endif
        y = D_800DC8AC[i][2].y;
        z = D_800DC8AC[i][2].z;
        ox3 = x;
        oy3 = y;
        oz3 = z;
        mtxf_transform_point(*cameraMatrix, x, y, z, &ox3, &oy3, &oz3);
        x = ((oz2 - oz3) * oy1) + (oy2 * (oz3 - oz1)) + (oy3 * (oz1 - oz2));
        y = ((ox2 - ox3) * oz1) + (oz2 * (ox3 - ox1)) + (oz3 * (ox1 - ox2));
        z = ((oy2 - oy3) * ox1) + (ox2 * (oy3 - oy1)) + (ox3 * (oy1 - oy2));
        inverseMagnitude = (1.0 / sqrtf((x * x) + (y * y) + (z * z)));
        if (inverseMagnitude > 0.0) {
            x *= inverseMagnitude;
            y *= inverseMagnitude;
            z *= inverseMagnitude;
        }
        w = -((ox1 * x) + (oy1 * y) + (oz1 * z));
        D_8011D0F8[i].x = x;
        D_8011D0F8[i].y = y;
        D_8011D0F8[i].z = z;
        D_8011D0F8[i].w = w;
        i++;
    }
#endif
}

/**
 * Takes a normalised (0-1) face direction of the active camera, then adds together a magnitude
 * to a total figure to determine whether or not a segment should be visible.
 * There's a large unused portion at the bottom writing to two vars, that are never later read.
 */
s32 block_visible(LevelModelSegmentBoundingBox *bb) {
    UNUSED u8 unknown[0x28];
    s64 sp48;
    s32 i, j;
    s32 isVisible;
    f32 dirX, dirY, dirZ, dirW;
    f32 x, y, z;

    for (j = 0; j < 3; j++) {
        dirX = D_8011D0F8[j].x;
        dirY = D_8011D0F8[j].y;
        dirZ = D_8011D0F8[j].z;
        dirW = D_8011D0F8[j].w;

        for (i = 0, isVisible = FALSE; i < 8 && !isVisible; i++) {
            if (i & 1) {
                sp48 = bb->x1 * dirX;
            } else {
                sp48 = bb->x2 * dirX;
            }
            if (i & 2) {
                sp48 += bb->y1 * dirY;
            } else {
                sp48 += bb->y2 * dirY;
            }
            if (i & 4) {
                sp48 += bb->z1 * dirZ;
            } else {
                sp48 += bb->z2 * dirZ;
            }
            sp48 += dirW;
            if (sp48 > 0) {
                isVisible = TRUE;
            }
        }
        if (i == 8 && !isVisible) {
            return FALSE;
        }
    }
    // From here until the "return TRUE" goes completely unused, functionally.
    x = (bb->x2 + bb->x1) >> 1;
    y = (bb->y2 + bb->y1) >> 1;
    z = (bb->z2 + bb->z1) >> 1;
    gCurrBBoxDistanceToCamera = get_distance_to_active_camera(x, y, z);
    if (gCurrBBoxDistanceToCamera < 1000.0) {
        gIsNearCurrBBox = TRUE;
    } else {
        gIsNearCurrBBox = FALSE;
    }
    return TRUE;
}

/**
 * Get the draw distance of the object, then compare it to the active camera position.
 * At the edge of its view distance, it will set its alpha based on distance, giving it a fade in or out effect.
 * Objects in range return true, objects out of range return false.
 */
#ifdef NATIVE_PORT
static s32 check_if_in_draw_range_impl(Object *obj, s32 *outOpacity, f32 distanceScale) {
#else
static s32 check_if_in_draw_range_impl(Object *obj, s32 *outOpacity) {
#endif
    f32 w;
    f32 y;
    f32 fadeDist;
    f32 z;
    f32 x;
    s32 viewDistance;
    s32 alpha;
    s32 i;
    Object_AnimatedObject *animatedObj;
    f32 accum;
    s32 temp2;
    f32 dist;
    Object_Racer *racer;

    if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
        alpha = 255;
        viewDistance = obj->header->drawDistance;
        if (obj->header->drawDistance) {
            if (gScenePlayerViewports == 3) {
                viewDistance *= 0.5;
            }
#ifdef NATIVE_PORT
            /* The player's Draw distance setting, and the ONLY thing it
             * touches. The fixed tick passes 1.0f and takes neither the branch
             * nor the multiply, so the authoritative arithmetic below is the
             * authored arithmetic instruction for instruction. Only the draw
             * passes anything else, and only through
             * scene_object_wide_draw_range(), which writes nothing back.
             *
             * Scaling viewDistance here rather than the comparison keeps the
             * fade band (viewDistance * 0.8) proportional, so the enhancement
             * moves the whole fade outward instead of leaving a ring of
             * half-faded scenery at the authored radius. */
            if (distanceScale != 1.0f) {
                viewDistance = (s32) ((f32) viewDistance * distanceScale);
            }
#endif

            dist = get_distance_to_active_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);

            if (viewDistance < dist) {
                return FALSE;
            }

            fadeDist = viewDistance * 0.8;
            if (fadeDist < dist) {
                temp2 = viewDistance - fadeDist;
                if (temp2 > 0) {
                    fadeDist = dist - fadeDist;
                    alpha = ((f32) (1.0 - ((fadeDist) / temp2)) * 255.0);
                }
                if (alpha == 0) {
                    alpha = 1;
                }
            }
        }
        switch (obj->behaviorId) {
            case BHV_RACER:
                racer = obj->racer;
                *outOpacity = ((racer->transparency + 1) * alpha) >> 8;
                break;
            case BHV_TIMETRIAL_GHOST: // Ghost Object?
                racer = obj->racer;
                *outOpacity = racer->transparency;
                break;
            case BHV_ANIMATED_OBJECT: // Cutscene object?
            case BHV_CAMERA_ANIMATION:
            case BHV_CAR_ANIMATION:
            case BHV_CHARACTER_SELECT:
            case BHV_VEHICLE_ANIMATION: // Title screen actor
            case BHV_HIT_TESTER:        // hittester
            case BHV_HIT_TESTER_2:      // animated objects?
            case BHV_ANIMATED_OBJECT_2: // space ships
                animatedObj = obj->animatedObject;
                *outOpacity = animatedObj->unk42;
                break;
            case BHV_PARK_WARDEN:
            case BHV_GOLDEN_BALLOON:
            case BHV_PARK_WARDEN_2: // GBParkwarden
                *outOpacity = obj->opacity;
                break;
            default:
                *outOpacity = alpha;
                break;
        }
        for (i = 0; i < 3; i++) {
#ifdef NATIVE_PORT
            /*
             * Keep object admission on the original guard-band frustum. The
             * fixed-step visibility prepass uses the same predicate to preserve
             * the authored AI "onscreen" contract, while the draw itself retains
             * a viewport-local opacity/LOD result.
             */
            x = sFaithfulCullPlanes[i].x;
            z = sFaithfulCullPlanes[i].z;
            w = sFaithfulCullPlanes[i].w;
            y = sFaithfulCullPlanes[i].y;
#else
            x = D_8011D0F8[i].x;
            z = D_8011D0F8[i].z;
            w = D_8011D0F8[i].w;
            y = D_8011D0F8[i].y;
#endif
            accum = (x * obj->trans.x_position) + (y * obj->trans.y_position) + (z * obj->trans.z_position) + w +
                    obj->unk34;
            if (accum < 0.0f) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

/* The AUTHORITATIVE admission test: it writes obj->opacity, which the [SIMHASH]
 * v3 stream hashes. It always asks at the authored distance -- the player's
 * Draw distance setting must not reach anything that writes back here, or "how
 * far you can see" becomes a simulation input. */
s32 check_if_in_draw_range(Object *obj) {
    s32 opacity = obj->opacity;
#ifdef NATIVE_PORT
    s32 admitted = check_if_in_draw_range_impl(obj, &opacity, 1.0f);
#else
    s32 admitted = check_if_in_draw_range_impl(obj, &opacity);
#endif
    if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
        obj->opacity = opacity;
    }
    return admitted;
}

#ifdef NATIVE_PORT
/* The same predicate at the player's widened distance, WITHOUT the write-back
 * above. Read-only over every Object field; the opacity it computes is handed
 * back by value for the draw to use and is never committed.
 *
 * Objects the AUTHORED distance already admits are reported through
 * `*outAuthored` rather than rejected, because the two callers want opposite
 * things from them: an object the tick routed needs its widened opacity (the
 * fade band moved outward with the cull radius, so it must not still be fading
 * at the old radius), while an object the tick did not route is a genuinely new
 * draw and needs a route invented for it. */
static s32 scene_object_wide_draw_range(Object *obj, s32 *outOpacity,
                                        s32 *outAuthoredOpacity,
                                        s32 *outAuthored) {
    *outAuthoredOpacity = obj->opacity;
    *outAuthored = check_if_in_draw_range_impl(obj, outAuthoredOpacity, 1.0f);
    *outOpacity = obj->opacity;
    return check_if_in_draw_range_impl(obj, outOpacity,
                                       mdkr_enh_draw_distance_scale());
}
#endif

UNUSED void func_8002AC00(s32 arg0, s32 arg1, s32 arg2) {
    s32 index;
    s32 index2;
    u8 temp;

    if (arg0 < gCurrentLevelModel->numberOfSegments && arg1 < gCurrentLevelModel->numberOfSegments) {
        index = DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[arg0].unk28;
        index2 = arg1 >> 3;
        temp = 1 << (arg1 & 7);
        if (arg2 != 0) {
            (&DKR_PTR(u8, gCurrentLevelModel->segmentsBitfields)[index])[index2] |= temp;
        } else {
            (&DKR_PTR(u8, gCurrentLevelModel->segmentsBitfields)[index])[index2] &= ~temp;
        }
    }
}

/**
 * Writes back the track collision data pointers.
 */
UNUSED void get_collision_candidate_data(s32 *numCollsionCandidates, s32 **collisionCandidates,
                                         s8 **collisionSurfaces) {
    *numCollsionCandidates = gNumCollisionCandidates;
    *collisionCandidates = gCollisionCandidates;
    *collisionSurfaces = gCollisionSurfaces;
}

/**
 * Change the collision response method.
 * For instance, can choose to ignore wall response.
 */
void set_collision_mode(s32 mode) {
    gCollisionMode = mode;
}

/**
 * Returns the surface normals of the current collision point.
 */
s32 get_collision_normal(f32 *outX, f32 *outY, f32 *outZ) {
    *outX = gCollisionNormalX;
    *outY = gCollisionNormalY;
    *outZ = gCollisionNormalZ;
    return gHitWall;
}

/**
 * Iterates through active waves on the track, then saves its rotation vector and height if found.
 * Returns zero if no waves are found, or if too high up.
 * Two types exist: calm, which have no means of displacement, and wavy, which do.
 */
s32 get_wave_properties(f32 yPos, f32 *waterHeight, Vec3f *rotation) {
    s32 var_a0;
    WaterProperties **wave;
    s32 i;
    s32 index;
    s32 len;
    f32 height;

    len = D_8011D308;
    if (rotation != NULL) {
        rotation->f[0] = 0.0f;
        rotation->f[2] = 0.0f;
        rotation->f[1] = 1.0f;
    }
    wave = gTrackWaves;
    for (var_a0 = i = 0; i < len; i++) {
        if (wave[i]->type == SURFACE_WATER_CALM || wave[i]->type == SURFACE_WATER_WAVY) {
            var_a0++;
        }
    }
    if (var_a0 == 0) {
        return SURFACE_DEFAULT;
    }
    wave = gTrackWaves;
    index = -1;
    for (i = 0; i < len; i++) {
        height = wave[i]->waveHeight;
        if (wave[i]->type == SURFACE_WATER_CALM || wave[i]->type == SURFACE_WATER_WAVY) {
            if (yPos < height + 25.0 && (wave[i]->rot.y > 0.5 || var_a0 == 1)) {
                index = i;
            }
        } else if (index >= 0 && var_a0 >= 2 && yPos < height - 20.0) {
            index = -1;
        }
    }
    if (index < 0) {
        return SURFACE_DEFAULT;
    }
    *waterHeight = gTrackWaves[index]->waveHeight;
    if (rotation != NULL) {
        rotation->f[0] = gTrackWaves[index]->rot.x;
        rotation->f[1] = gTrackWaves[index]->rot.y;
        rotation->f[2] = gTrackWaves[index]->rot.z;
    }
    return gTrackWaves[index]->type;
}

/**
 * Finds the waves in the current level segment, sorts them by height, outputs the sorted list into
 * waterProps and returns the number of waves.
 */
s32 get_level_segment_waves(s32 levelSegmentIndex, f32 xIn, f32 zIn, WaterProperties ***waterProps) {
    LevelModelSegmentBoundingBox *currentBoundingBox;
    Triangle *tri;
    Vertex *vert;
    s16 temp_a2;
    s32 currentVerticesOffset;
    LevelModelSegment *currentSegment;
    s32 j;
    s32 segmentCount;
    s16 vert1X;
    s16 vert1Z;
    s16 vert2X;
    s16 vert2Z;
    s16 vert3X;
    s16 vert3Z;
    s32 nextFaceOffset;
    u16 var_a1;
    s32 currentFaceOffset;
    s32 faceNum;
    u16 var_s1;
    s16 var_t0;
    s16 var_t1;
    s32 XInInt;
    s32 ZInInt;
    s32 side1;
    s32 side2;
    s32 side3;
    s32 segmentsInside[8];
    s32 yOutCount;
    s32 batchNum;
    s32 i;
    s32 stopSorting;
    f32 A, B, C, D;
    s32 temp;
    s32 surfaceType;
    s32 unused_bool;
    s32 pad;
    WaterProperties *wave;
    WaterProperties **waves;

    unused_bool = FALSE;
    D_8011D308 = 0;
    *waterProps = NULL;
    XInInt = xIn;
    ZInInt = zIn;
#ifdef NATIVE_PORT
    /* NATIVE_PORT: the bound the callee was missing. The `segmentCount >= 8`
     * test below is the ROM's own behaviour for the overflowing case and needs
     * no change -- see the comment on get_inside_segment_count_xz(). */
    segmentCount = get_inside_segment_count_xz(XInInt, ZInInt, segmentsInside,
                                               ARRAY_COUNT(segmentsInside));
#else
    segmentCount = get_inside_segment_count_xz(XInInt, ZInInt, segmentsInside);
#endif
    if (segmentCount == 0 || segmentCount >= 8) {
        return 0;
    }

    yOutCount = 0;
    for (i = 0; i < segmentCount; i++) {
        currentSegment = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[segmentsInside[i]];
        currentBoundingBox = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[segmentsInside[i]];

        var_a1 = 1;
        var_s1 = 0;

        temp_a2 = ((currentBoundingBox->x2 - currentBoundingBox->x1) >> 3) + 1;
        var_t0 = temp_a2 + currentBoundingBox->x1;
        var_t1 = currentBoundingBox->x1;

        for (j = 0; j < 8; j++) {
            if (var_t0 >= XInInt && XInInt >= var_t1) {
                var_s1 |= var_a1;
            }
            var_t0 += temp_a2;
            var_t1 += temp_a2;
            var_a1 <<= 1;
        }

        temp_a2 = ((currentBoundingBox->z2 - currentBoundingBox->z1) >> 3) + 1;
        var_t0 = temp_a2 + currentBoundingBox->z1;
        var_t1 = currentBoundingBox->z1;

        for (j = 0; j < 8; j++) {
            if (var_t0 >= ZInInt && ZInInt >= var_t1) {
                var_s1 |= var_a1;
            }
            var_t0 += temp_a2;
            var_t1 += temp_a2;
            var_a1 <<= 1;
        }

        for (batchNum = 0; batchNum < currentSegment->numberOfBatches; batchNum++) {
            surfaceType = DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].textureIndex].surfaceType;
            currentFaceOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].facesOffset;
            currentVerticesOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].verticesOffset;
            nextFaceOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum + 1].facesOffset;
            if ((surfaceType != SURFACE_WATER_CALM && surfaceType != SURFACE_WATER_UNK_F) &&
                (DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].flags & (RENDER_HIDDEN | RENDER_NO_COLLISION))) {
                currentFaceOffset = nextFaceOffset;
            }
            for (faceNum = currentFaceOffset; faceNum < nextFaceOffset; faceNum++) {
                if (var_s1 == (DKR_PTR(s16, currentSegment->unk10)[faceNum] & var_s1)) {
                    tri = &DKR_PTR(Triangle, currentSegment->triangles)[faceNum];
                    vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[1] + currentVerticesOffset];
                    vert1X = vert->x;
                    vert1Z = vert->z;
                    vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[2] + currentVerticesOffset];
                    vert2X = vert->x;
                    vert2Z = vert->z;
                    vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[3] + currentVerticesOffset];
                    vert3X = vert->x;
                    vert3Z = vert->z;
                    side1 = (((XInInt - vert2X) * (vert3Z - vert2Z)) - ((vert3X - vert2X) * (ZInInt - vert2Z))) >= 0;
                    side2 = (((XInInt - vert1X) * (vert2Z - vert1Z)) - ((vert2X - vert1X) * (ZInInt - vert1Z))) >= 0;
                    side3 = (((XInInt - vert1X) * (vert3Z - vert1Z)) - ((vert3X - vert1X) * (ZInInt - vert1Z))) >= 0;
                    if (side1 == side2 && side2 != side3) {
                        temp = DKR_PTR(CollisionFacetPlanes, currentSegment->collisionFacets)[faceNum].basePlaneIndex;
                        A = DKR_PTR(f32, currentSegment->collisionPlanes)[(temp * 4) + 0];
                        B = DKR_PTR(f32, currentSegment->collisionPlanes)[(temp * 4) + 1];
                        C = DKR_PTR(f32, currentSegment->collisionPlanes)[(temp * 4) + 2];
                        D = DKR_PTR(f32, currentSegment->collisionPlanes)[(temp * 4) + 3];
                        if (B != 0.0) {
                            D_8011D128[yOutCount].type = surfaceType;
                            D_8011D128[yOutCount].waveHeight = -((((A * xIn) + (C * zIn)) + D) / B);
                            D_8011D128[yOutCount].rot.x = A;
                            D_8011D128[yOutCount].rot.y = B;
                            D_8011D128[yOutCount].rot.z = C;
                            yOutCount++;
                            /* One slot stays reserved for the level segment's
                             * own water entry, which is appended after these
                             * loops without a further bound test. Saturating at
                             * ARRAY_COUNT here would let that append write
                             * D_8011D128[20] / gTrackWaves[20] and publish a
                             * count of 21. */
                            if (yOutCount >= (s32) ARRAY_COUNT(D_8011D128) - 1) {
                                batchNum = currentSegment->numberOfBatches;
                                faceNum = nextFaceOffset;
                                i = segmentCount;
                            } else if (unused_bool) {
                                // Fake to set to this to basically any variable in currentSegment as a break for
                                // optimization. This code should never run.
                                currentSegment->numberOfBatches = 1;
                            }
                        }
                    }
                }
            }
        }
    }

    if (levelSegmentIndex >= 0 && levelSegmentIndex < gCurrentLevelModel->numberOfSegments) {
        currentSegment = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[levelSegmentIndex];
        D_8011D128[yOutCount].type = SURFACE_WATER_WAVY;
        if (currentSegment->hasWaves && gWaveBlockCount != 0) {
            wave = yOutCount + D_8011D128; // fake?
            D_8011D128[yOutCount].waveHeight = func_800BB2F4(levelSegmentIndex, xIn, zIn, &wave->rot);
        } else {
            D_8011D128[yOutCount].waveHeight = currentSegment->unk38;
            D_8011D128[yOutCount].rot.x = 0.0f;
            D_8011D128[yOutCount].rot.y = 1.0f;
            D_8011D128[yOutCount].rot.z = 0.0f;
        }
        yOutCount++;
    }

    waves = gTrackWaves;
    wave = D_8011D128;

    // clang-format off
    for (j = 0; j < yOutCount; j++) { waves[j] = &wave[j]; }
    // clang-format on

    do {
        stopSorting = TRUE;

        for (j = 0; j < (yOutCount - 1); j++) {
            if (waves[j]->waveHeight < waves[j + 1]->waveHeight) {
                stopSorting = FALSE;
                wave = waves[j];
                waves[j] = waves[j + 1];
                waves[j + 1] = wave;
            }
        }

    } while (!stopSorting);

    *waterProps = gTrackWaves;
    D_8011D308 = yOutCount;

    return yOutCount;
}

#ifdef NATIVE_PORT
/**
 * Protect the water-query cache across a query made for a point the simulation
 * does not care about.
 *
 * get_level_segment_waves() above has no private output. It publishes the
 * result of the LAST query anyone made into three globals -- D_8011D128 (the
 * WaterProperties storage), gTrackWaves (the height-sorted pointer order into
 * it) and D_8011D308 (how many entries are live) -- and get_wave_properties()
 * reads them without re-querying. Its gameplay consumers are
 * update_player_racer (racer.c, water surface + buoyancy) and the octobomb Y
 * placement (object_functions.c), both of which rely on some earlier query
 * having been made for a position near THEM.
 *
 * The rain-splash emitter (weather.c) queries a RANDOM point up to 500 units
 * from player one, from inside render, and so silently overwrote that cache
 * every frame it fired. These two functions let it put the cache back.
 *
 * Only the live prefix is copied: gTrackWaves[i] always points into
 * D_8011D128, and both arrays are file-static storage at fixed addresses, so
 * restoring the raw pointers is exact.
 */
void wave_query_cache_save(WaveQueryCache *cache) {
    s32 i;

    cache->count = D_8011D308;
    for (i = 0; i < cache->count; i++) {
        cache->props[i] = D_8011D128[i];
        cache->order[i] = gTrackWaves[i];
    }
}

void wave_query_cache_restore(const WaveQueryCache *cache) {
    s32 i;

    for (i = 0; i < cache->count; i++) {
        D_8011D128[i] = cache->props[i];
        gTrackWaves[i] = cache->order[i];
    }
    D_8011D308 = cache->count;
}
#endif

s32 func_8002B9BC(Object *obj, f32 *arg1, Vec3f *arg2, s32 arg3) {
    LevelModelSegment *seg;

    if (arg2 != NULL) {
        arg2->x = 0.0f;
        arg2->z = 0.0f;
        arg2->y = 1.0f;
    }
    if ((obj->segmentID < 0) || (obj->segmentID >= gCurrentLevelModel->numberOfSegments)) {
        return FALSE;
    }
    seg = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[obj->segmentID];
    if ((seg->hasWaves != 0) && (gWaveBlockCount != 0) && (arg3 == 1)) {
        *arg1 = func_800BB2F4(obj->segmentID, obj->trans.x_position, obj->trans.z_position, arg2);
        return TRUE;
    } else {
        *arg1 = seg->unk38;
        return TRUE;
    }
}

/**
 * Searches for intersecting surfaces, then returns the Y values of all the intersecting points, in order.
 * There is no limit for surfaces returned, so not feeding a large enough yOut array could cause problems.
 */
#ifdef NATIVE_PORT
/*
 * NATIVE_PORT: `maxOut` added. The decomp's own comment above states the defect --
 * "there is no limit for surfaces returned" -- and this is the worst instance of
 * the class in the tree, because the count is per TRIANGLE (every collision facet
 * of every batch of the segment whose X/Z footprint contains the query point),
 * not per segment, and because it has five callers. Four ordinary queries now
 * share COLLISION_Y_QUERY_CAPACITY (16), while the wave builder retains its
 * larger colY[30]. Found by
 * tools/sweep_bug_shapes.py, not by the report that started this work.
 *
 * The original bounded adaptation measured a minimum per-call slack of four:
 * the old 8-element callers reached four, while the 10/30-element callers
 * reached seven on boss levels 40 and 53. Widening the ordinary local arrays to
 * 16 preserves behavior while making a future content/layout change much less
 * likely to drop a valid intersecting surface.
 *
 * Unlike get_inside_segment_count_xz() the count IS clamped here, and it has to
 * be: callers read yOut[0..count-1], so returning a count larger than the number
 * of slots actually written would hand them uninitialised stack. Clamping keeps
 * every caller reading only values this function wrote. That is a real divergence
 * from the ROM -- the ROM would return all of them -- but only in the state where
 * the ROM has already smashed the caller's frame, and the values are sorted after
 * the clamp so the ordering contract still holds over what is returned.
 */
s32 collision_get_y(s32 levelSegmentIndex, f32 xIn, f32 zIn, f32 *yOut, s32 maxOut) {
#else
s32 collision_get_y(s32 levelSegmentIndex, f32 xIn, f32 zIn, f32 *yOut) {
#endif
    LevelModelSegment *currentSegment;
    LevelModelSegmentBoundingBox *currentBoundingBox;
    Triangle *tri;
    Vertex *vert;
    f32 temp_f2_2;
    s16 vert2X;
    s16 vert2Z;
    s16 temp_a2;
    s16 vert3X;
    s16 vert3Z;
    s32 currentVerticesOffset;
    s16 nextFaceOffset;
    s16 vert1X;
    s16 currentFaceOffset;
    s16 vert1Z;
    s16 var_a1;
    s32 faceNum;
    s16 var_s1;
    s16 var_t0;
    s16 var_t1;
    s32 XInInt;
    s32 ZInInt;
    s32 temp_ra_1;
    s32 temp_ra_2;
    s32 temp_ra_3;
    s32 yOutCount;
    s32 batchNum;
    s32 i;
    s32 var_v0;
    s32 stopSorting;
    TriangleBatchInfo *currentBatch;
    f32 *temp_v1_4;
    Vec4f tempVec4f;
    u16 temp;

    if (levelSegmentIndex < 0 || levelSegmentIndex >= gCurrentLevelModel->numberOfSegments) {
        return 0;
    }

    vert = NULL; // fake?

    currentSegment = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[levelSegmentIndex];
    currentBoundingBox = &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[levelSegmentIndex];
    var_a1 = 1;
    var_s1 = 0;
    XInInt = xIn;
    ZInInt = zIn;

    temp_a2 = ((currentBoundingBox->x2 - currentBoundingBox->x1) >> 3) + 1;
    var_t0 = temp_a2 + currentBoundingBox->x1;
    var_t1 = currentBoundingBox->x1;
    for (i = 0; i < 8; i++) {
        if (var_t0 >= XInInt && XInInt >= var_t1) {
            var_s1 |= var_a1;
        }
        var_t0 += temp_a2;
        var_t1 += temp_a2;
        var_a1 *= 2;
    }

    // Same as above, but for Z
    temp_a2 = ((currentBoundingBox->z2 - currentBoundingBox->z1) >> 3) + 1;
    var_t0 = temp_a2 + currentBoundingBox->z1;
    var_t1 = currentBoundingBox->z1;
    for (i = 0; i < 8; i++) {
        if (var_t0 >= ZInInt && ZInInt >= var_t1) {
            var_s1 |= var_a1;
        }
        var_t0 += temp_a2;
        var_t1 += temp_a2;
        var_a1 *= 2;
    }

    yOutCount = 0;
    for (batchNum = 0; batchNum < currentSegment->numberOfBatches; batchNum++) {
        do {
        } while (0); // fakematch
        currentFaceOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].facesOffset;
        nextFaceOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum + 1].facesOffset;
        currentVerticesOffset = DKR_PTR(TriangleBatchInfo, currentSegment->batches)[batchNum].verticesOffset;
        for (faceNum = currentFaceOffset; faceNum < nextFaceOffset; faceNum++) {
            if (var_s1 == (DKR_PTR(s16, currentSegment->unk10)[faceNum] & var_s1)) {
                tri = &DKR_PTR(Triangle, currentSegment->triangles)[faceNum];

                vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[1] + currentVerticesOffset];
                vert1X = vert->x;
                vert1Z = vert->z;

                vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[2] + currentVerticesOffset];
                vert2X = vert->x;
                vert2Z = vert->z;

                vert = &DKR_PTR(Vertex, currentSegment->vertices)[tri->verticesArray[3] + currentVerticesOffset];
                vert3X = vert->x;
                vert3Z = vert->z;

                temp_ra_1 = (((XInInt - vert2X) * (vert3Z - vert2Z)) - ((vert3X - vert2X) * (ZInInt - vert2Z))) >= 0;
                temp_ra_2 = (((XInInt - vert1X) * (vert2Z - vert1Z)) - ((vert2X - vert1X) * (ZInInt - vert1Z))) >= 0;
                temp_ra_3 = (((XInInt - vert1X) * (vert3Z - vert1Z)) - ((vert3X - vert1X) * (ZInInt - vert1Z))) >= 0;
                var_v0 = faceNum; // fake?
                if (temp_ra_1 == temp_ra_2 && temp_ra_2 != temp_ra_3) {
                    temp = DKR_PTR(CollisionFacetPlanes, currentSegment->collisionFacets)[faceNum].basePlaneIndex;
                    tempVec4f.x = DKR_PTR(f32, currentSegment->collisionPlanes)[4 * temp + 0];
                    tempVec4f.y = DKR_PTR(f32, currentSegment->collisionPlanes)[4 * temp + 1];
                    tempVec4f.z = DKR_PTR(f32, currentSegment->collisionPlanes)[4 * temp + 2];
                    tempVec4f.w = DKR_PTR(f32, currentSegment->collisionPlanes)[4 * temp + 3];
                    if (tempVec4f.y != 0.0) {
#ifdef NATIVE_PORT
                        if (yOutCount >= maxOut && !mdkr_segbound_legacy()) {
                            continue; /* full: stop writing AND stop counting */
                        }
#endif
                        yOut[yOutCount] = -(((tempVec4f.x * xIn) + (tempVec4f.z * zIn) + tempVec4f.w) / tempVec4f.y);
                        yOutCount++;
                    }
                }
            }
        }
    }

    do {
        stopSorting = TRUE;
        for (var_v0 = 0; var_v0 < yOutCount - 1; var_v0++) {
            if (yOut[var_v0] > yOut[var_v0 + 1]) {
                stopSorting = FALSE;
                temp_f2_2 = yOut[var_v0];
                yOut[var_v0] = yOut[var_v0 + 1];
                yOut[var_v0 + 1] = temp_f2_2;
            }
        }
    } while (!stopSorting);

#ifdef NATIVE_PORT
    mdkr_bound_probe(2, yOutCount, maxOut);
#endif
    return yOutCount;
}

// Loads a level track from the index in the models table.
void generate_track(s32 modelId) {
    s32 i, j, k;
    s32 temp_s4;
    s32 modelOffset;
    u8 *compressedData;
    u8 *mdl;
    u8 *collisionCursor;

#ifdef NATIVE_PORT
    /* A direct level reload is allowed in diagnostics; never retain copied
     * geometry from the previous model while its heap is being replaced. */
    mdkr_track_occlusion_cache_free();
#endif
    set_texture_colour_tag(COLOUR_TAG_GREEN);
    gTrackModelHeap = mempool_alloc_safe(LEVEL_MODEL_MAX_SIZE, COLOUR_TAG_YELLOW);
    gCurrentLevelModel = gTrackModelHeap;
#ifdef NATIVE_PORT
    /* MDKR_COLLALLOC=1 lowers the candidate ALLOCATION to the effective
     * MDKR_COLLCAP boundary and adds one canary element just past it, so that a
     * store at index == cap becomes observable. MDKR_COLLCAP on its own only
     * moves the boundary INSIDE a full-size 500-entry block, which is why it
     * could not see the facet insert's guard sitting below its own store. See
     * platform/stubs_dkr.c (MDKR_COLLALLOC). Unset: cells == 500 and slack == 0,
     * i.e. byte-for-byte the ROM-side allocation. */
    {
        s32 cells = mdkr_coll_alloc_cells(MAX_COLLISION_CANDIDATES);
        s32 slack = mdkr_coll_alloc_canary_slack();
        gCollisionCandidates = mempool_alloc_safe((cells + slack) * 4, COLOUR_TAG_YELLOW);
        gCollisionSurfaces = mempool_alloc_safe(cells + slack, COLOUR_TAG_YELLOW);
        mdkr_coll_canary_arm(gCollisionCandidates, gCollisionSurfaces, cells);
    }
#else
    gCollisionCandidates = mempool_alloc_safe(MAX_COLLISION_CANDIDATES * 4, COLOUR_TAG_YELLOW);
    gCollisionSurfaces = mempool_alloc_safe(MAX_COLLISION_CANDIDATES, COLOUR_TAG_YELLOW);
#endif
    gNumCollisionCandidates = 0;
    gLevelModelTable = (s32 *) asset_table_load(ASSET_LEVEL_MODELS_TABLE);

    for (i = 0; gLevelModelTable[i] != -1; i++) {}
    i--;
    if (modelId >= i) {
        modelId = 0;
    }

    modelOffset = gLevelModelTable[modelId];
    temp_s4 = gLevelModelTable[modelId + 1] - modelOffset;

#ifdef NATIVE_PORT
    if (modelOffset < 0 || temp_s4 <= 0 ||
        temp_s4 > LEVEL_MODEL_MAX_SIZE ||
        modelOffset > asset_table_size(ASSET_LEVEL_MODELS) - temp_s4) {
        fprintf(
            stderr,
            "[FATAL] level model %d compressed asset span is invalid\n",
            modelId);
        abort();
    }
#endif
    compressedData = (u8 *)((uintptr_t)((u8 *)gCurrentLevelModel + LEVEL_MODEL_MAX_SIZE - temp_s4) &
                            ~(uintptr_t)15);

#ifdef NATIVE_PORT
    if (compressedData < (u8 *)gCurrentLevelModel ||
        compressedData + temp_s4 >
            (u8 *)gCurrentLevelModel + LEVEL_MODEL_MAX_SIZE) {
        fprintf(
            stderr,
            "[FATAL] level model %d compressed destination is invalid\n",
            modelId);
        abort();
    }
#endif
    asset_load(ASSET_LEVEL_MODELS, (uintptr_t)compressedData, modelOffset, temp_s4);
#ifdef NATIVE_PORT
    /* temp_s4 is the level model's compressed span, staged inside the decoded
     * model's own LEVEL_MODEL_MAX_SIZE block (checked just above). */
    gzip_inflate_sized(compressedData, (u8 *) gCurrentLevelModel, temp_s4);
#else
    gzip_inflate(compressedData, (u8 *) gCurrentLevelModel);
#endif
#ifdef NATIVE_PORT
    if (gzip_inflate_output < (u8 *)gCurrentLevelModel ||
        gzip_inflate_output >
            (u8 *)gCurrentLevelModel + LEVEL_MODEL_MAX_SIZE) {
        fprintf(
            stderr,
            "[FATAL] level model %d exceeded its decoded allocation\n",
            modelId);
        abort();
    }
    /* Compressed sections are not normalized at load; the decompressed level
     * model is still big-endian. Swap it before any field is read. */
    asset_swap_normalize(ASSET_LEVEL_MODELS, gCurrentLevelModel,
                         (u32) (gzip_inflate_output - (u8 *) gCurrentLevelModel));
    if (gCurrentLevelModel->numberOfSegments <= 0 ||
        gCurrentLevelModel->numberOfSegments > LEVEL_SEGMENT_MAX) {
        fprintf(
            stderr,
            "[FATAL] level model %d has invalid segment count %d\n",
            modelId, gCurrentLevelModel->numberOfSegments);
        abort();
    }
    if (gCurrentLevelModel->modelSize < (s32)sizeof(LevelModel) ||
        gCurrentLevelModel->modelSize > LEVEL_MODEL_MAX_SIZE) {
        fprintf(
            stderr,
            "[FATAL] level model %d has invalid modelSize %d\n",
            modelId, gCurrentLevelModel->modelSize);
        abort();
    }
#endif
    mempool_free(gLevelModelTable); // Done with the level models table, so free it.

    /* LOCAL_OFFSET_TO_RAM_ADDRESS captures this base by name. */
    mdl = (u8 *)gCurrentLevelModel;

    LOCAL_OFFSET_TO_RAM_ADDRESS(TextureInfo *, gCurrentLevelModel->textures);
    LOCAL_OFFSET_TO_RAM_ADDRESS(LevelModelSegment *, gCurrentLevelModel->segments);
    LOCAL_OFFSET_TO_RAM_ADDRESS(LevelModelSegmentBoundingBox *, gCurrentLevelModel->segmentsBoundingBoxes);
    LOCAL_OFFSET_TO_RAM_ADDRESS(u8 *, gCurrentLevelModel->unkC);
    LOCAL_OFFSET_TO_RAM_ADDRESS(u8 *, gCurrentLevelModel->segmentsBitfields);
    LOCAL_OFFSET_TO_RAM_ADDRESS(BspTreeNode *, gCurrentLevelModel->segmentsBspTree);

#ifdef NATIVE_PORT
    /* The BSP-tree node array (segmentsBspTree) is NOT covered by
     * asset_swap_normalize — its length is absent from the LevelModel header, so
     * its big-endian s16 fields survive to here. A binary tree that sorts N
     * segments front-to-back has N-1 internal split nodes; each 8-byte
     * BspTreeNode needs leftNode/rightNode/splitValue (s16) byte-swapped
     * (splitType/segmentIndex are single bytes). Without this,
     * traverse_segments_bsp_tree() reads garbage child indices and recurses
     * until the stack overflows. (OPEN_ITEMS: BSP-tree node array not swapped.) */
    {
        s32 bspNodeCount = gCurrentLevelModel->numberOfSegments - 1;
        BspTreeNode *bsp = DKR_PTR(BspTreeNode, gCurrentLevelModel->segmentsBspTree);
        for (k = 0; k < bspNodeCount; k++) {
            bsp[k].leftNode   = (s16) (((u16) bsp[k].leftNode   << 8) | ((u16) bsp[k].leftNode   >> 8));
            bsp[k].rightNode  = (s16) (((u16) bsp[k].rightNode  << 8) | ((u16) bsp[k].rightNode  >> 8));
            bsp[k].splitValue = (s16) (((u16) bsp[k].splitValue << 8) | ((u16) bsp[k].splitValue >> 8));
        }
    }
#endif

    for (k = 0; k < gCurrentLevelModel->numberOfSegments; k++) {
        LOCAL_OFFSET_TO_RAM_ADDRESS(Vertex *, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].vertices);
        LOCAL_OFFSET_TO_RAM_ADDRESS(Triangle *, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].triangles);
        LOCAL_OFFSET_TO_RAM_ADDRESS(TriangleBatchInfo *, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].batches);
        LOCAL_OFFSET_TO_RAM_ADDRESS(CollisionFacetPlanes *, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].collisionFacets);
    }
    for (k = 0; k < gCurrentLevelModel->numberOfTextures; k++) {
        DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[k].texture =
            DKR_TOK(load_texture(((s32) DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[k].texture) | 0x8000));
    }
    collisionCursor =
        (u8 *)gCurrentLevelModel + gCurrentLevelModel->modelSize;
#ifdef NATIVE_PORT
    for (k = 0; k < gCurrentLevelModel->numberOfSegments; k++) {
        LevelModelSegment *segment =
            &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k];
        u8 *trackEnd =
            (u8 *)gCurrentLevelModel + LEVEL_MODEL_MAX_SIZE;
        size_t triangleIndexBytes;
        size_t collisionMaxBytes;
        size_t collisionBytes;
        size_t unknownMaxBytes;
        size_t unknownIndexBytes;

        if (segment->numberOfTriangles < 0 ||
            segment->numberOfBatches < 0) {
            fprintf(
                stderr,
                "[FATAL] level model %d segment %d has negative counts\n",
                modelId, k);
            abort();
        }
        triangleIndexBytes =
            (size_t)segment->numberOfTriangles * sizeof(s16);
        if (collisionCursor > trackEnd ||
            triangleIndexBytes >
                (size_t)(trackEnd - collisionCursor)) {
            fprintf(
                stderr,
                "[FATAL] level model %d collision index overflow\n",
                modelId);
            abort();
        }
        segment->unk10 = DKR_TOK(collisionCursor);
        collisionCursor =
            align16(collisionCursor + triangleIndexBytes);
        if (collisionCursor > trackEnd) {
            fprintf(
                stderr,
                "[FATAL] level model %d collision index alignment overflow\n",
                modelId);
            abort();
        }

        segment->collisionPlanes = DKR_TOK(collisionCursor);
        /*
         * The builder emits at most one face plane plus three edge planes per
         * triangle; each plane is four floats. Reserve that worst case before
         * allowing the legacy writer to touch the model heap.
         */
        collisionMaxBytes =
            (size_t)segment->numberOfTriangles * 4u *
            4u * sizeof(f32);
        if (collisionMaxBytes >
            (size_t)(trackEnd - collisionCursor)) {
            fprintf(
                stderr,
                "[FATAL] level model %d collision plane capacity overflow\n",
                modelId);
            abort();
        }
        collisionBytes = (size_t)track_init_collision(segment);
        if (collisionBytes > collisionMaxBytes ||
            collisionBytes >
                (size_t)(trackEnd - collisionCursor)) {
            fprintf(
                stderr,
                "[FATAL] level model %d collision plane overflow\n",
                modelId);
            abort();
        }
        collisionCursor += collisionBytes;

        func_8002C954(
            segment,
            &DKR_PTR(
                LevelModelSegmentBoundingBox,
                gCurrentLevelModel->segmentsBoundingBoxes)[k],
            k);
        segment->unk30 = 0;
        segment->unk34 = DKR_TOK(collisionCursor);
        unknownMaxBytes =
            (size_t)segment->numberOfBatches * sizeof(s16);
        if (unknownMaxBytes >
            (size_t)(trackEnd - collisionCursor)) {
            fprintf(
                stderr,
                "[FATAL] level model %d segment index capacity overflow\n",
                modelId);
            abort();
        }
        func_8002C71C(segment);
        if (segment->unk32 < 0 ||
            segment->unk32 > segment->numberOfBatches) {
            fprintf(
                stderr,
                "[FATAL] level model %d segment %d produced invalid index count\n",
                modelId, k);
            abort();
        }
        unknownIndexBytes =
            (size_t)segment->unk32 * sizeof(s16);
        if (unknownIndexBytes >
            (size_t)(trackEnd - collisionCursor)) {
            fprintf(
                stderr,
                "[FATAL] level model %d segment index overflow\n",
                modelId);
            abort();
        }
        collisionCursor =
            align16(collisionCursor + unknownIndexBytes);
        if (collisionCursor > trackEnd) {
            fprintf(
                stderr,
                "[FATAL] level model %d segment alignment overflow\n",
                modelId);
            abort();
        }
    }
#else
    for (k = 0; k < gCurrentLevelModel->numberOfSegments; k++) {
        DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].unk10 = DKR_TOK(collisionCursor);
        collisionCursor = align16(collisionCursor +
                                  DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].numberOfTriangles * 2);
        DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].collisionPlanes = DKR_TOK(collisionCursor);
        collisionCursor += track_init_collision(&DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k]);
        func_8002C954(&DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k], &DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[k], k);
        DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].unk30 = 0;
        DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].unk34 = DKR_TOK(collisionCursor);
        func_8002C71C(&DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k]);
        collisionCursor = align16(collisionCursor +
                                  DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[k].unk32 * 2);
    }
#endif
    temp_s4 = (s32)(collisionCursor - (u8 *)gCurrentLevelModel);
    if (temp_s4 > LEVEL_MODEL_MAX_SIZE) {
        rmonPrintf("ERROR!! TrackMem overflow .. %d\n", temp_s4);
    }
    mempool_free_timer(0);
    mempool_free(gTrackModelHeap);
    mempool_alloc_fixed(temp_s4, (u8 *) gTrackModelHeap, COLOUR_TAG_YELLOW);
    mempool_free_timer(2);
    minimap_init(gCurrentLevelModel);

    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        for (temp_s4 = 0; temp_s4 < DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].numberOfBatches; temp_s4++) {
            for (k = DKR_PTR(TriangleBatchInfo, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].batches)[temp_s4].verticesOffset;
                 k < DKR_PTR(TriangleBatchInfo, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].batches)[temp_s4 + 1].verticesOffset; k++) {
                // Why do this? Why not just set the vertex colors in the model itself?
                if (DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].r == 1 &&
                    DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].g == 1) {
                    DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].a = DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].b;
                    DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].r = 0x80;
                    DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].g = 0x80;
                    DKR_PTR(Vertex, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].vertices)[k].b = 0x80;
                    DKR_PTR(TriangleBatchInfo, DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[i].batches)[temp_s4].flags |= 0x08000000;
                }
            }
        }
    }
#ifdef NATIVE_PORT
    /* This is after every load-time pointer fixup, collision build, fixed heap
     * compaction, and vertex/batch finalization. The cache never observes a
     * half-generated model. */
    mdkr_track_occlusion_cache_build();
#endif
    set_texture_colour_tag(COLOUR_TAG_MAGENTA);
}

void func_8002C71C(LevelModelSegment *segment) {
    s32 curVertY;
    s32 numVerts;
    s32 i;

    segment->unk38 = -10000;
    numVerts = 0;
    for (i = 0; i < segment->numberOfBatches; i++) {
        if (DKR_PTR(TriangleBatchInfo, segment->batches)[i].flags & 0x2000) {
            DKR_PTR(s16, segment->unk34)[numVerts++] = i;
            curVertY = DKR_PTR(Vertex, segment->vertices)[DKR_PTR(TriangleBatchInfo, segment->batches)[i].verticesOffset].y;
            if (segment->unk38 == -10000 || segment->unk38 < curVertY) {
                segment->unk38 = curVertY;
            }
        }
    }
    segment->unk32 = numVerts;
}

/**
 * Returns the current loaded level geometry
 * Official Name: trackGetTrack
 */
LevelModel *get_current_level_model(void) {
    return gCurrentLevelModel;
}

/**
 * Frees and unloads level data from RAM.
 */
void free_track(void) {
    s32 i;

    racerfx_free();
#ifdef NATIVE_PORT
    /* Free caller-owned copies before the level-model heap and its textures are
     * released. Queries after this point fail closed rather than dereferencing
     * stale geometry. */
    mdkr_track_occlusion_cache_free();
#endif
    if (gWaveBlockCount != 0) {
        waves_free();
    }
    for (i = 0; i < gCurrentLevelModel->numberOfTextures; i++) {
        tex_free(DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, gCurrentLevelModel->textures)[i].texture));
    }
    mempool_free(gTrackModelHeap);
#ifdef NATIVE_PORT
    /* Disarm before the blocks go back to the pool: the canary pointers would
     * otherwise outlive their allocation until the next level load re-arms them.
     * No-op unless MDKR_COLLALLOC is set. */
    mdkr_coll_canary_arm(NULL, NULL, -1);
#endif
    mempool_free(gCollisionCandidates);
    mempool_free(gCollisionSurfaces);
    sprite_free(DKR_PTR(Sprite, gCurrentLevelModel->minimapSpriteIndex));
    for (i = 0; i < ARRAY_COUNT(gShadowHeapData); i++) {
        mempool_free(gShadowHeapData[i]);
        mempool_free(gShadowHeapTris[i]);
        mempool_free(gShadowHeapVerts[i]);
    }
    void_free();
    if (gSkydomeSegment != NULL) {
        free_object(gSkydomeSegment);
        gParticlePtrList_flush();
    }
    free_all_objects();
    gCurrentLevelModel = NULL;
    gCurrentLevelHeader2 = NULL;
}

void func_8002C954(LevelModelSegment *segment, LevelModelSegmentBoundingBox *bbox, UNUSED s32 arg2) {
    Vertex *vert;
    s16 boxDelta;
    s32 vertZ;
    s32 vertX;
    s16 boxMin;
    u16 bit;
    s16 maxX;
    s32 j;
    s16 minX;
    u16 val;
    s16 maxZ;
    s16 minZ;
    s16 k;
    s16 boxMax;
    s32 i;
    s32 l;
    s32 startTri;
    s32 endTri;
    s32 vertsOffset;

    for (i = 0; i < segment->numberOfBatches; i++) {
        startTri = DKR_PTR(TriangleBatchInfo, segment->batches)[i].facesOffset;
        endTri = DKR_PTR(TriangleBatchInfo, segment->batches)[i + 1].facesOffset;
        vertsOffset = DKR_PTR(TriangleBatchInfo, segment->batches)[i].verticesOffset;
        for (j = startTri; j < endTri; j++) {
            if (DKR_PTR(Triangle, segment->triangles)[j].flags & TRI_FLAG_80) {
                DKR_PTR(s16, segment->unk10)[j] = 0;
            } else {
                maxX = -32000;
                maxZ = -32000;
                minZ = 32000;
                minX = 32000;

                for (l = 0; l < 3; l++) {
                    vert = &DKR_PTR(Vertex, segment->vertices)[DKR_PTR(Triangle, segment->triangles)[j].verticesArray[l + 1] + vertsOffset];
                    k = vert->x;
                    vertX = k; // This is probably fake, but it matches.
                    vertZ = vert->z;
                    if (maxX < vertX) {
                        maxX = vertX;
                    }
                    if (vertX < minX) {
                        minX = vertX;
                    }
                    if (maxZ < vertZ) {
                        maxZ = vertZ;
                    }
                    if (vertZ < minZ) {
                        minZ = vertZ;
                    }
                }
                boxDelta = ((bbox->x2 - bbox->x1) >> 3) + 1;
                bit = 1;
                boxMax = boxDelta + bbox->x1;
                boxMin = bbox->x1;
                val = 0;
                for (k = 0; k < 8; k++) {
                    if (boxMax >= minX && maxX >= boxMin) {
                        val |= bit;
                    }
                    boxMax += boxDelta;
                    boxMin += boxDelta;
                    bit <<= 1;
                }
                boxDelta = ((bbox->z2 - bbox->z1) >> 3) + 1;
                boxMax = boxDelta + bbox->z1;
                boxMin = bbox->z1;
                for (k = 0; k < 8; k++) {
                    if (boxMax >= minZ && maxZ >= boxMin) {
                        val |= bit;
                    }
                    boxMax += boxDelta;
                    boxMin += boxDelta;
                    bit <<= 1;
                }
                DKR_PTR(s16, segment->unk10)[j] = dkr_s16_from_bits(val);
            }
        }
    }
}

s32 track_init_collision(LevelModelSegment *block) {
    s32 facesOffset;
    s32 verticesOffset;
    s32 nextFacesOffset;
    Vertex *v;
    s32 batchIndex;
    s32 triIndex;
    s32 i;
    f32 mag;
    s32 counter;
    s32 colPlaneIndex;
    s32 next;
    s32 numColPlanes;
    s32 vertIndex, nextVertIndex;
    f32 x1, y1, z1;
    f32 x2, y2, z2;
    f32 x3, y3, z3;
    f32 nx, ny, nz;
    f32 x5, y5, z5;
    s32 j;

    counter = 0;

    for (batchIndex = 0; batchIndex < block->numberOfBatches; batchIndex++) {
        facesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex].facesOffset;
        verticesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex].verticesOffset;
        nextFacesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex + 1].facesOffset;

        for (triIndex = facesOffset; triIndex < nextFacesOffset; triIndex++) {
            if (DKR_PTR(Triangle, block->triangles)[triIndex].flags & TRI_FLAG_80) {
                continue;
            }

            v = &DKR_PTR(Vertex, block->vertices)[DKR_PTR(Triangle, block->triangles)[triIndex].vi0 + verticesOffset];
            x1 = v->x;
            y1 = v->y;
            z1 = v->z;

            v = &DKR_PTR(Vertex, block->vertices)[DKR_PTR(Triangle, block->triangles)[triIndex].vi1 + verticesOffset];
            x2 = v->x;
            y2 = v->y;
            z2 = v->z;

            v = &DKR_PTR(Vertex, block->vertices)[DKR_PTR(Triangle, block->triangles)[triIndex].vi2 + verticesOffset];
            x3 = v->x;
            y3 = v->y;
            z3 = v->z;

            nx = y1 * (z2 - z3) + y2 * (z3 - z1) + y3 * (z1 - z2);
            ny = z1 * (x2 - x3) + z2 * (x3 - x1) + z3 * (x1 - x2);
            nz = x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2);

            mag = sqrtf(nx * nx + ny * ny + nz * nz);
            if (mag > 0.0) {
                nx /= mag;
                ny /= mag;
                nz /= mag;
            }

            // looks like a macro
            {
                s32 temp = counter++;
                DKR_PTR(f32, block->collisionPlanes)[4 * temp + 0] = nx;
                DKR_PTR(f32, block->collisionPlanes)[4 * temp + 1] = ny;
                DKR_PTR(f32, block->collisionPlanes)[4 * temp + 2] = nz;
                DKR_PTR(f32, block->collisionPlanes)[4 * temp + 3] = -(x1 * nx + y1 * ny + z1 * nz);
            }
        }
    }

    numColPlanes = counter;

    if (D_8011B0F8) {
        return counter * 0x10;
    }

    for (batchIndex = 0; batchIndex < block->numberOfBatches; batchIndex++) {
        facesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex].facesOffset;
        verticesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex].verticesOffset;
        nextFacesOffset = DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex + 1].facesOffset;

        if (DKR_PTR(TriangleBatchInfo, block->batches)[batchIndex].flags & RENDER_NO_COLLISION) {
            facesOffset = nextFacesOffset;
        }

        for (triIndex = facesOffset; triIndex < nextFacesOffset; triIndex++) {
            if (DKR_PTR(Triangle, block->triangles)[triIndex].flags & TRI_FLAG_80) {
                continue;
            }

            colPlaneIndex = DKR_PTR(CollisionFacetPlanes, block->collisionFacets)[triIndex].basePlaneIndex;
            nx = DKR_PTR(f32, block->collisionPlanes)[4 * colPlaneIndex + 0];
            ny = DKR_PTR(f32, block->collisionPlanes)[4 * colPlaneIndex + 1];
            nz = DKR_PTR(f32, block->collisionPlanes)[4 * colPlaneIndex + 2];

            for (i = 0; i < 3; i++) {
                next = i + 1;
                if (next > 2) {
                    next = 0;
                }

                vertIndex = DKR_PTR(Triangle, block->triangles)[triIndex].verticesArray[1 + i] + verticesOffset;
                nextVertIndex = DKR_PTR(Triangle, block->triangles)[triIndex].verticesArray[1 + next] + verticesOffset;

                next = DKR_PTR(CollisionFacetPlanes, block->collisionFacets)[triIndex].edgeBisectorPlane[i];
                if (next < numColPlanes) {
                    {
                        s32 temp = next;
                        x5 = nx + DKR_PTR(f32, block->collisionPlanes)[4 * temp + 0];
                        y5 = ny + DKR_PTR(f32, block->collisionPlanes)[4 * temp + 1];
                        z5 = nz + DKR_PTR(f32, block->collisionPlanes)[4 * temp + 2];
                    }

                    v = &DKR_PTR(Vertex, block->vertices)[vertIndex];
                    x1 = v->x;
                    y1 = v->y;
                    z1 = v->z;

                    v = &DKR_PTR(Vertex, block->vertices)[nextVertIndex];
                    x2 = v->x;
                    y2 = v->y;
                    z2 = v->z;

                    x3 = x5 * 10.0f + x1;
                    y3 = y5 * 10.0f + y1;
                    z3 = z5 * 10.0f + z1;

                    x5 = y1 * (z2 - z3) + y2 * (z3 - z1) + y3 * (z1 - z2);
                    y5 = z1 * (x2 - x3) + z2 * (x3 - x1) + z3 * (x1 - x2);
                    z5 = x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2);

                    mag = sqrtf(x5 * x5 + y5 * y5 + z5 * z5);
                    if (mag > 0.0) {
                        x5 /= mag;
                        y5 /= mag;
                        z5 /= mag;
                    }

                    if (next != colPlaneIndex) {
                        for (j = 0; j < 3; j++) {
                            if (colPlaneIndex == DKR_PTR(CollisionFacetPlanes, block->collisionFacets)[next].edgeBisectorPlane[j]) {
                                DKR_PTR(CollisionFacetPlanes, block->collisionFacets)[next].edgeBisectorPlane[j] = counter | 0x8000;
                            }
                        }
                    }

                    DKR_PTR(CollisionFacetPlanes, block->collisionFacets)[triIndex].edgeBisectorPlane[i] = counter;

                    {
                        s32 temp = counter++;
                        DKR_PTR(f32, block->collisionPlanes)[4 * temp + 0] = x5;
                        DKR_PTR(f32, block->collisionPlanes)[4 * temp + 1] = y5;
                        DKR_PTR(f32, block->collisionPlanes)[4 * temp + 2] = z5;
                        DKR_PTR(f32, block->collisionPlanes)[4 * temp + 3] = -(x1 * x5 + y1 * y5 + z1 * z5);
                    }
                }
            }
        }
    }
    return counter * 0x10;
}

#ifndef NATIVE_PORT
typedef struct unk8002D30C_a0 {
    u8 pad00[0x04];
    struct unk8002D30C_a0 *unk04;
    struct unk8002D30C_a0 *unk08;
} unk8002D30C_a0;

void trackMakeAbsolute(unk8002D30C_a0 *arg0, s32 arg1) {
    while (1) {
        if (!arg0) {
            return;
        }
        if (arg0->unk04) {
            arg0->unk04 =
                (unk8002D30C_a0 *) ((uintptr_t) arg0->unk04 + (intptr_t) arg1);
        }
        if (arg0->unk08) {
            arg0->unk08 =
                (unk8002D30C_a0 *) ((uintptr_t) arg0->unk08 + (intptr_t) arg1);
        }

        trackMakeAbsolute(arg0->unk04, arg1);
        arg0 = arg0->unk08;
    }
}
#endif

/**
 * Render the shadow of an object on the ground as a decal.
 * Can subdivide itself to wrap around the terrain properly, as the N64 lacks stencil buffering.
 */
void shadow_render(Object *obj, ShadowData *shadow) {
    s32 i;
    s32 numVerts;
    s32 numTris;
    Vertex *vtx;
    Triangle *tri;
    s32 flags;
    UNUSED s32 tri2;
    s32 vtx2;
    s32 vtxCount;
    s32 triCount;
    s32 alpha;
#ifdef NATIVE_PORT
    s32 objectOpacity = scene_object_render_opacity(obj);
    uint32_t presentationBatch = 0u;
#else
    s32 objectOpacity = obj->opacity;
#endif

    if (obj->header->shadowGroup) {
        if (shadow->meshStart != -1 && gDisableShadows == FALSE) {
#ifdef NATIVE_PORT
            if (!shadow_mesh_range_valid(shadow->meshStart, shadow->meshEnd)) {
                return;
            }
#endif
            gShadowIndex = gShadowHeapFlip;
            if (obj->header->shadowGroup == SHADOW_SCENERY) {
                gShadowIndex += 2;
            }
            i = shadow->meshStart;
            gCurrShadowHeapData = gShadowHeapData[gShadowIndex];
            gCurrShadowTris = gShadowHeapTris[gShadowIndex];
            gCurrShadowVerts = gShadowHeapVerts[gShadowIndex];
#ifdef NATIVE_PORT
            if (gCurrShadowHeapData[i].vtxCount < 0 ||
                gCurrShadowHeapData[i].vtxCount >= gShadowVtxCap) {
                return;
            }
#endif
            alpha = gCurrShadowVerts[gCurrShadowHeapData[i].vtxCount].a;
            /*
             * These polygons are generated directly on the receiver surface.
             * RENDER_DECAL selects ZMODE_DEC, which every native backend maps to
             * a negative depth bias. The prior ordinary Z_COMPARE path made the
             * shadow and road win alternating depth samples as the camera/racer
             * moved — the observed random flicker.
             */
#ifdef NATIVE_PORT
            flags = RENDER_FOG_ACTIVE | RENDER_Z_COMPARE;
            if (mdkr_shadow_decal_enabled()) {
                flags |= RENDER_DECAL;
            }
#else
            flags = RENDER_FOG_ACTIVE | RENDER_Z_COMPARE;
#endif
            if (alpha == 0 || objectOpacity == 0) {
                i = shadow->meshEnd; // It'd be easier to just return...
            } else if (alpha != 255 || objectOpacity != 255) {
                flags = RENDER_FOG_ACTIVE | RENDER_SEMI_TRANSPARENT | RENDER_Z_COMPARE;
#ifdef NATIVE_PORT
                if (mdkr_shadow_decal_enabled()) {
                    flags |= RENDER_DECAL;
                }
#endif
                alpha = (objectOpacity * alpha) >> 8;
                gDPSetPrimColor(gTrackDL++, 0, 0, 255, 255, 255, alpha);
            }
            while (i < shadow->meshEnd) {
#ifdef NATIVE_PORT
                if (i < 0 || i + 1 >= gShadowDataCap ||
                    gCurrShadowHeapData[i].triCount < 0 ||
                    gCurrShadowHeapData[i].vtxCount < 0 ||
                    gCurrShadowHeapData[i + 1].triCount < gCurrShadowHeapData[i].triCount ||
                    gCurrShadowHeapData[i + 1].vtxCount < gCurrShadowHeapData[i].vtxCount ||
                    gCurrShadowHeapData[i + 1].triCount > gShadowTriCap ||
                    gCurrShadowHeapData[i + 1].vtxCount > gShadowVtxCap) {
                    break;
                }
                gShadowDrawGroups++;
                if (!(flags & RENDER_DECAL)) {
                    gShadowNonDecalDrawGroups++;
                }
#endif
                material_set_no_tex_offset(&gTrackDL, gCurrShadowHeapData[i].texture, flags);
                // I hope we can clean this part up.
                tri2 = triCount = gCurrShadowHeapData[i].triCount; // Fakematch
                vtx2 = vtxCount = gCurrShadowHeapData[i].vtxCount;
                numTris = gCurrShadowHeapData[i + 1].triCount - triCount;
                numVerts = gCurrShadowHeapData[i + 1].vtxCount - vtxCount;
                tri = &gCurrShadowTris[triCount];
                vtx = &gCurrShadowVerts[vtx2];
#ifdef NATIVE_PORT
                {
                    GfxPresentationMatrixOwner owner = {0};
                    uint64_t generation = 0u;

                    if (presentation_snapshot_identity_generation(
                            obj, &generation)) {
                        owner.address = obj;
                        owner.generation = generation;
                        owner.matrix_class =
                            GFX_PRESENTATION_MATRIX_PROJECTED_SHADOW_VERTICES;
                        owner.surface_class = MDKR_SURF_PROJECTED_SHADOW;
                        owner.geometry_signature =
                            shadow_batch_topology_signature(
                                gCurrShadowHeapData[i].texture, numVerts,
                                numTris, tri);
                        owner.valid = true;
                        /* The mesh is authored once in world space and reused
                         * unchanged by every viewport. Keep one shared stream,
                         * just like direct world-space particle vertices. */
                        (void)gfx_presentation_packet_register_projected_shadow_vertex(
                            vtx, 0, presentationBatch,
                            &owner);
                    }
                    presentationBatch++;
                }
                /*
                 * Mark the exact source batch rather than deciding here
                 * whether to omit it. The backend creates this frame's real
                 * map only after the game has built the display list; the HLE
                 * therefore owns the truthful, current-frame fallback choice.
                 * Scenery decals remain authored fallback for masked cards,
                 * which the safe caster policy deliberately excludes.
                 * Billboard-sprite actors (bananas, balloons, most pickups)
                 * are likewise excluded from the caster feed, so suppressing
                 * their decal would leave them floating with no grounding at
                 * all: only 3D-model actors trade their decal for a real map
                 * shadow. A fading/translucent actor (the semi-transparent
                 * flags arm above) is alpha-blended in the world pass and
                 * therefore never captured either — it keeps its decal too.
                 */
                if (obj->header->shadowGroup != SHADOW_SCENERY &&
                    obj->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL &&
                    !(flags & RENDER_SEMI_TRANSPARENT)) {
                    gfx_shadow_projected_range_mark(tri);
                }
#endif
                gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(vtx), numVerts, 0);
                gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(tri), numTris, 1);
                i++;
            }

            if (flags != RENDER_Z_COMPARE) {
                gDPSetPrimColor(gTrackDL++, 0, 0, 255, 255, 255, 255);
            }
        }
    }
}

/**
 * Used only by cars, render a texture on the surface of the water where the car is
 * to give the wave effect. Works almost identically to shadows, since water can be wavy.
 */
void watereffect_render(Object *obj, WaterEffect *effect) {
    s32 i;
    s32 numVerts;
    s32 numTris;
    Vertex *vtx;
    Triangle *tri;
    s32 flags;
    UNUSED s32 triCount;
    UNUSED s32 vtxCount;

    if (obj->header->waterEffectGroup) {
        if (effect->meshStart != -1 && gDisableShadows == FALSE) {
#ifdef NATIVE_PORT
            if (!shadow_mesh_range_valid(effect->meshStart, effect->meshEnd)) {
                return;
            }
#endif
            gWaterEffectIndex = gShadowHeapFlip;
            i = effect->meshStart;
            if (obj->header->waterEffectGroup == SHADOW_SCENERY) {
                gWaterEffectIndex += 2;
                if (get_distance_to_active_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position) >
                    768.0f) {
                    i = effect->meshEnd; // Just return.
                }
            }
            flags = RENDER_FOG_ACTIVE | RENDER_Z_COMPARE;
            gCurrShadowHeapData = gShadowHeapData[gWaterEffectIndex];
            gCurrShadowTris = gShadowHeapTris[gWaterEffectIndex];
            gCurrShadowVerts = gShadowHeapVerts[gWaterEffectIndex];
            while (i < effect->meshEnd) {
#ifdef NATIVE_PORT
                if (i < 0 || i + 1 >= gShadowDataCap ||
                    gCurrShadowHeapData[i].triCount < 0 ||
                    gCurrShadowHeapData[i].vtxCount < 0 ||
                    gCurrShadowHeapData[i + 1].triCount < gCurrShadowHeapData[i].triCount ||
                    gCurrShadowHeapData[i + 1].vtxCount < gCurrShadowHeapData[i].vtxCount ||
                    gCurrShadowHeapData[i + 1].triCount > gShadowTriCap ||
                    gCurrShadowHeapData[i + 1].vtxCount > gShadowVtxCap) {
                    break;
                }
#endif
                material_set_no_tex_offset(&gTrackDL, gCurrShadowHeapData[i].texture, flags);
                triCount = gCurrShadowHeapData[i].triCount; // Fakematch
                vtxCount = gCurrShadowHeapData[i].vtxCount; // Fakematch
                numTris = gCurrShadowHeapData[i + 1].triCount - gCurrShadowHeapData[i].triCount;
                numVerts = gCurrShadowHeapData[i + 1].vtxCount - gCurrShadowHeapData[i].vtxCount;
                tri = &gCurrShadowTris[gCurrShadowHeapData[i].triCount];
                vtx = &gCurrShadowVerts[gCurrShadowHeapData[i].vtxCount];
                gSPVertexDKR(gTrackDL++, OS_K0_TO_PHYSICAL(vtx), numVerts, 0);
                gSPPolygon(gTrackDL++, OS_K0_TO_PHYSICAL(tri), numTris, 1);
                i++;
            }
        }
    }
}

/**
 * Updates shadow and water effect properties for each relevant object in the scene.
 * The first argument decides whether to update shadows for static objects or moving objects.
 * In multiplayer, only important objects get shadows.
 */
void shadow_update(s32 group, s32 waterGroup, s32 updateRate) {
    s32 objIndex;
    s32 objectCount;
    Object *obj;
    ObjectHeader *objHeader;
    f32 dist;
    s32 radius;
    s32 numViewports;
    Object **objects;
    s32 skipShading;
    TextureHeader *waterTex;
    ShadowData *shadow;
    WaterEffect *waterEffect;
    s32 playerIndex;

#ifdef NATIVE_PORT
    shadow_refresh_caps();
#endif
    gShadowIndex = gShadowHeapFlip;
    if (group == SHADOW_SCENERY) {
        gShadowIndex += 2;
    }
    gCurrShadowTris = (Triangle *) gShadowHeapTris[gShadowIndex];
    gCurrShadowVerts = (Vertex *) gShadowHeapVerts[gShadowIndex];
    gCurrShadowHeapData = (ShadowHeapProperties *) gShadowHeapData[gShadowIndex];
    gShadowTail = 0;
    gNewShadowTriCount = 0;
    gNewShadowVtxCount = 0;
    numViewports = cam_get_viewport_layout();
    objects = objGetObjList(&objIndex, &objectCount);
    while (objIndex < objectCount) {
        obj = objects[objIndex];
        objHeader = obj->header;
        waterEffect = obj->waterEffect;
        shadow = obj->shadow;
        objIndex += 1;
        if ((obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            continue;
        }
        if (shadow != NULL && shadow->scale > 0.0f && group == objHeader->shadowGroup) {
            shadow->meshStart = -1;
        }
        if (obj->trans.flags & OBJ_FLAGS_INVISIBLE) {
            shadow = NULL;
        }
#ifdef NATIVE_PORT
        /* Taj is a visible carpet/rider composition. Both objects occupy the
         * same grounding footprint, so retaining the rider's actor shadow
         * would double-darken the single-player decal. */
        if (shadow != NULL &&
            (taj_visual_suppress_companion_shadow(obj) ||
             wizpig_visual_suppress_shadow(obj) ||
             terry_visual_suppress_shadow(obj))) {
            shadow = NULL;
        }
#endif
        if ((shadow != NULL && objHeader->shadowGroup == SHADOW_ACTORS) ||
            (waterEffect != NULL && objHeader->waterEffectGroup == SHADOW_ACTORS)) {
            dist = get_distance_to_active_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
        } else {
            dist = 0;
        }
        if (shadow != NULL && shadow->scale > 0.0f && group == objHeader->shadowGroup) {
            gShadowOpacity = 1.0f;
            shadow->meshStart = -1;
            skipShading = FALSE;
            // Multiplayer
            if (objHeader->shadowGroup == SHADOW_ACTORS && numViewports >= TWO_PLAYERS &&
                numViewports <= FOUR_PLAYERS) {
                if (obj->behaviorId == BHV_RACER) {
                    playerIndex = obj->racer->playerIndex;
                    if (playerIndex != PLAYER_COMPUTER) {
                        shadow_generate(obj, FALSE);
                        skipShading = TRUE;
                    }
                } else if (obj->behaviorId == BHV_WEAPON ||
                           taj_visual_multiplayer_shadow_object(obj) ||
                           wizpig_visual_multiplayer_shadow_object(obj) ||
                           terry_visual_multiplayer_shadow_object(obj)) {
                    shadow_generate(obj, FALSE);
                    skipShading = TRUE;
                }
            } else { // Single Player
                radius = objHeader->shadowFadeMin;
                if (dist < radius) {
                    if (objHeader->shadowFadeMax < dist) {
                        gShadowOpacity = (radius - dist) / (radius - objHeader->shadowFadeMax);
                    }
                    shadow_generate(obj, FALSE);
                    skipShading = TRUE;
                }
            }
            if (skipShading == FALSE && obj->shading != NULL) {
                func_8002DE30(obj);
            }
        }
        if (waterEffect != NULL && waterEffect->scale > 0.0f && waterGroup == objHeader->waterEffectGroup) {
            waterEffect->meshStart = -1;
            gShadowOpacity = 1.0f;
            waterTex = waterEffect->texture;
            if (waterTex != NULL && updateRate && waterTex->numOfTextures != 0x100) {
                waterEffect->textureFrame += waterEffect->animationSpeed;
                while (waterTex->numOfTextures < waterEffect->textureFrame) {
                    waterEffect->textureFrame -= waterTex->numOfTextures;
                }
            }

            // Multiplayer
            if (objHeader->shadowGroup == SHADOW_ACTORS && numViewports >= TWO_PLAYERS &&
                numViewports <= FOUR_PLAYERS) {
                if (obj->behaviorId == BHV_RACER) {
                    playerIndex = obj->racer->playerIndex;
                    if (playerIndex != PLAYER_COMPUTER) {
                        shadow_generate(obj, TRUE);
                    }
                } else if (obj->behaviorId == BHV_WEAPON) {
                    shadow_generate(obj, TRUE);
                }
            } else { // Single Player
                if (dist < objHeader->shadowFadeMin) {
                    if (objHeader->shadowFadeMax < dist) {
                        gShadowOpacity =
                            (objHeader->shadowFadeMin - dist) / (objHeader->shadowFadeMin - objHeader->shadowFadeMax);
                    }
                    shadow_generate(obj, TRUE);
                }
            }
        }
    }
#ifdef NATIVE_PORT
    if (gShadowTail >= 0 && gShadowTail < gShadowDataCap) {
        gCurrShadowHeapData[gShadowTail].texture = NULL;
        gCurrShadowHeapData[gShadowTail].triCount = gNewShadowTriCount;
        gCurrShadowHeapData[gShadowTail].vtxCount = gNewShadowVtxCount;
    }
    shadow_note_high_water();
#else
    gCurrShadowHeapData[gShadowTail].triCount = gNewShadowTriCount;
    gCurrShadowHeapData[gShadowTail].vtxCount = gNewShadowVtxCount;
#endif
}

void func_8002DE30(Object *obj) {
    s32 sp94;
    s32 sp90;
    s32 blockId;
    s32 var_t3;
    s32 k;
    u32 batchFlags;
    s32 foundResult;
    s32 i;
    LevelModelSegment *block;
    Triangle *triangle;
    Vertex *vertices;
    s32 minYPos;
    s32 maxYPos;
    s32 j;

    sp94 = (s32) obj->trans.y_position + obj->header->unk44;
    sp90 = (s32) obj->trans.y_position + obj->header->unk42;
    blockId = obj->segmentID;
    foundResult = FALSE;
    if (blockId != -1) {
        var_t3 = compute_grid_overlap_mask(&DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[blockId],
                                           obj->trans.x_position - 16.0f, obj->trans.z_position - 16.0f,
                                           obj->trans.x_position + 16.0f, obj->trans.z_position + 16.0f);
        block = &DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[blockId];
        for (i = 0; i < block->numberOfBatches && !foundResult; i++) {
            if (!(DKR_PTR(TriangleBatchInfo, block->batches)[i].flags & (RENDER_HIDDEN | RENDER_DECAL | RENDER_WATER | RENDER_NO_SHADOW))) {
                batchFlags = (DKR_PTR(TriangleBatchInfo, block->batches)[i].flags >> 19) & 7;
                vertices = &DKR_PTR(Vertex, block->vertices)[DKR_PTR(TriangleBatchInfo, block->batches)[i].verticesOffset];
                for (j = DKR_PTR(TriangleBatchInfo, block->batches)[i].facesOffset; j < DKR_PTR(TriangleBatchInfo, block->batches)[i + 1].facesOffset && !foundResult; j++) {
                    blockId = DKR_PTR(s16, block->unk10)[j] & var_t3;
                    if (blockId) {}
                    if (((DKR_PTR(s16, block->unk10)[j] & var_t3) & 0xFF) && ((DKR_PTR(s16, block->unk10)[j] & var_t3) & 0xFF00)) {
                        triangle = &DKR_PTR(Triangle, block->triangles)[j];
                        minYPos = vertices[triangle->verticesArray[1]].y;
                        maxYPos = minYPos;
                        for (k = 1; k < 3; k++) {
                            if (vertices[triangle->verticesArray[k + 1]].y < minYPos) {
                                minYPos = vertices[triangle->verticesArray[k + 1]].y;
                            } else if (maxYPos < vertices[triangle->verticesArray[k + 1]].y) {
                                maxYPos = vertices[triangle->verticesArray[k + 1]].y;
                            }
                        }
                        if (maxYPos >= sp90 && sp94 >= minYPos) {
                            if (tri2d_xz_contains_point(obj->trans.x_position, obj->trans.z_position,
                                                        (Vec3s *) &vertices[triangle->verticesArray[1]],
                                                        (Vec3s *) &vertices[triangle->verticesArray[2]],
                                                        (Vec3s *) &vertices[triangle->verticesArray[3]])) {
                                foundResult = TRUE;
                                obj->shading->unk0 += (((1.0f - D_800DC884[batchFlags]) - obj->shading->unk0) * 0.2);
                            }
                        }
                    }
                }
            }
        }
    }
}

/**
 * Generate shadow geometry for an object.
 * Handles water effects too, isWater is true.
 */
void shadow_generate(Object *obj, s32 isWater) {
    f32 dist;
    s32 yPos;
    f32 xPos;
    f32 zPos;
    UNUSED s32 *pad;
    s32 cheats;
    s32 inSegs[28];
    s32 i;
    s32 segs;
    f32 character_scale;
    s32 test;
#ifdef NATIVE_PORT
    s32 buildDataStart;
    s32 buildTriStart;
    s32 buildVtxStart;
#endif

    yPos = obj->trans.y_position;
    character_scale = 1.0f;
    if (obj->behaviorId == BHV_RACER) {
        cheats = get_filtered_cheats();
        if (cheats & CHEAT_BIG_CHARACTERS) {
            character_scale = 1.4f;
        } else if (cheats & CHEAT_SMALL_CHARACTERS) {
            character_scale = 0.714f;
        }
    }

    gNewShadowObj = obj;
    D_8011D0C8 = 2.0f;
#ifdef NATIVE_PORT
    buildDataStart = gShadowTail;
    buildTriStart = gNewShadowTriCount;
    buildVtxStart = gNewShadowVtxCount;
    gShadowBuildOverflow = FALSE;
#endif

    if (isWater) {
        D_8011D0B8 = 0;
        obj->waterEffect->meshStart = gShadowTail;
        gNewShadowTexture = set_animated_texture_header(obj->waterEffect->texture, obj->waterEffect->textureFrame << 8);
        gNewShadowY2 = obj->header->shadowTop + yPos;
        gNewShadowY1 = obj->header->shadowBottom + yPos;
        if (gWaveBlockCount == 0 || cam_get_viewport_layout() < VIEWPORT_LAYOUT_2_PLAYERS) {
            D_8011D0C8 = 0;
        }
        gNewShadowScale = (obj->waterEffect->scale * character_scale);
        gNewShadowWidth = gNewShadowScale * 10.0f;
        gNewShadowLength = gNewShadowScale * 10.0f;
        D_8011D0F0 = -1.0f;
    } else {
        obj->shadow->meshStart = gShadowTail;
        gNewShadowTexture = obj->shadow->texture;
        gNewShadowY2 = obj->header->unk44 + yPos;
        gNewShadowY1 = obj->header->unk42 + yPos;
        if (obj->behaviorId != BHV_RACER) {
#ifdef NATIVE_PORT
            dist = gSceneDrawDistanceValid ? gSceneDrawDistance : obj->distanceToCamera;
#else
            dist = obj->distanceToCamera;
#endif
            if (dist < 0.0) {
                dist = -dist;
            }
            dist -= 512.0;
            if (dist < 0.0) {
                dist = 0.0;
            }
            if (dist > 1024.0) {
                dist = 1024.0;
            }
            D_8011D0C8 += (dist * 0.005f);
        }
        gNewShadowScale = (obj->shadow->scale * character_scale);
        gNewShadowWidth = gNewShadowScale * 10.0f;
        gNewShadowLength = gNewShadowScale * 10.0f;
        D_8011D0E4 = 4.0f * gNewShadowWidth * gNewShadowLength;
        D_8011D0F0 = (obj->header->unk42 * 0.125f);
        if (D_8011D0F0 < 0.0f) {
            D_8011D0F0 = -D_8011D0F0;
        }
        D_8011D0F4 = (7.0f * D_8011D0F0);
        D_8011D0D0 = -0x8000;
    }
    gNewShadowScale = 144.0f / gNewShadowScale;
    xPos = gNewShadowObj->trans.x_position;
    zPos = gNewShadowObj->trans.z_position;
#ifdef NATIVE_PORT
    segs = get_inside_segment_count_xyz(inSegs, (xPos - gNewShadowWidth), gNewShadowY1, (zPos - gNewShadowLength),
                                        (xPos + gNewShadowWidth), gNewShadowY2, (zPos + gNewShadowLength),
                                        ARRAY_COUNT(inSegs));
#else
    segs = get_inside_segment_count_xyz(inSegs, (xPos - gNewShadowWidth), gNewShadowY1, (zPos - gNewShadowLength),
                                        (xPos + gNewShadowWidth), gNewShadowY2, (zPos + gNewShadowLength));
#endif
    D_8011C230 = 0;
    D_8011B118 = 0;
    for (i = 0; i < ARRAY_COUNT(D_8011B320); i++) {
        D_8011B320[i] = 0;
    }
    D_8011D0E8 = -1;
    D_8011D0EC = -1;
    for (i = 0; i < segs; i++) {
#ifdef NATIVE_PORT
        if (gShadowBuildOverflow) {
            break;
        }
#endif
        if (inSegs[i] >= 0) {
            if (isWater && (DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[inSegs[i]].hasWaves != 0) && (gWaveBlockCount != 0)) {
                func_8002EEEC(inSegs[i]);
            } else {
                test = compute_grid_overlap_mask(&DKR_PTR(LevelModelSegmentBoundingBox, gCurrentLevelModel->segmentsBoundingBoxes)[inSegs[i]],
                                                 (obj->trans.x_position - gNewShadowWidth),  // x1
                                                 (obj->trans.z_position - gNewShadowLength), // z1
                                                 (obj->trans.x_position + gNewShadowWidth),  // x2
                                                 (obj->trans.z_position + gNewShadowLength)  // z2
                );
                func_8002E904(&DKR_PTR(LevelModelSegment, gCurrentLevelModel->segments)[inSegs[i]], test, isWater);
            }
        }
    }
#ifdef NATIVE_PORT
    if (D_8011C230 < 0 || D_8011C230 > ARRAY_COUNT(D_8011C238) ||
        D_8011B118 < 0 || D_8011B118 > ARRAY_COUNT(D_8011B120)) {
        gShadowBuildOverflow = TRUE;
    }
    if (D_8011C230 > 0 && !gShadowBuildOverflow) {
#else
    if (D_8011C230 > 0) {
#endif
        if ((obj->shading != NULL) && isWater == FALSE) {
            obj->shading->unk0 = func_8002FA64();
        }
        func_8002F2AC();
        func_8002F440();
    }
#ifdef NATIVE_PORT
    /*
     * Geometry for one object is a transaction. If any descriptor/triangle/
     * vertex reservation failed, abandon every count from that object. Bytes
     * already written remain inside their allocation and are unreachable.
     */
    if (gShadowBuildOverflow) {
        gShadowTail = buildDataStart;
        gNewShadowTriCount = buildTriStart;
        gNewShadowVtxCount = buildVtxStart;
        gShadowOverflowDrops++;
        if (isWater == FALSE) {
            obj->shadow->meshStart = -1;
            obj->shadow->meshEnd = -1;
        } else {
            obj->waterEffect->meshStart = -1;
            obj->waterEffect->meshEnd = -1;
        }
        shadow_note_high_water();
        return;
    }
    /*
     * The legacy code left meshStart==meshEnd for an object with no receiver
     * polygons. shadow_render still dereferenced the terminal descriptor's first
     * vertex before its empty loop, which is uninitialised (or one-past-end at a
     * full heap). Represent an empty mesh explicitly instead.
     */
    if (gShadowTail == buildDataStart) {
        gShadowEmptyMeshes++;
        if (isWater == FALSE) {
            obj->shadow->meshStart = -1;
            obj->shadow->meshEnd = -1;
        } else {
            obj->waterEffect->meshStart = -1;
            obj->waterEffect->meshEnd = -1;
        }
        return;
    }
#endif
    if (isWater == FALSE) {
        obj->shadow->meshEnd = gShadowTail;
    } else {
        obj->waterEffect->meshEnd = gShadowTail;
    }
#ifdef NATIVE_PORT
    shadow_note_high_water();
#endif
}

void func_8002E904(LevelModelSegment *arg0, s32 arg1, s32 arg2) {
    unk8011C8B8 sp100[8];
    Vec2f spD0[4];
    s32 spAC;
    Triangle *triangles;
    s32 nextFacesOffset;
    Vertex *vertices;
    s32 yPos;
    s32 minY;
    s32 foundIndex;
    s32 maxY;
    s32 temp_t6;
    s32 sp88;
    s32 someCount;
    s32 i2;
    s32 i;
    s32 curFacesOffset;

    spD0[0].x = gNewShadowObj->trans.x_position + gNewShadowWidth;
    spD0[0].y = gNewShadowObj->trans.z_position + gNewShadowLength;
    spD0[1].x = gNewShadowObj->trans.x_position - gNewShadowWidth;
    spD0[1].y = gNewShadowObj->trans.z_position + gNewShadowLength;
    spD0[2].x = gNewShadowObj->trans.x_position - gNewShadowWidth;
    spD0[2].y = gNewShadowObj->trans.z_position - gNewShadowLength;
    spD0[3].x = gNewShadowObj->trans.x_position + gNewShadowWidth;
    spD0[3].y = gNewShadowObj->trans.z_position - gNewShadowLength;

    for (spAC = 0; spAC < arg0->numberOfBatches; spAC++) {
        if ((arg2 && (DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC].flags & RENDER_WATER)) ||
            (!arg2 && !(DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC].flags & FLAGS_8002E904))) {
            curFacesOffset = DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC].facesOffset;
            nextFacesOffset = DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC + 1].facesOffset;
            vertices = &DKR_PTR(Vertex, arg0->vertices)[DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC].verticesOffset];
            sp88 = (DKR_PTR(TriangleBatchInfo, arg0->batches)[spAC].flags >> 0x13) & 7;
            for (; curFacesOffset < nextFacesOffset; curFacesOffset++) {
                if (((DKR_PTR(s16, arg0->unk10)[curFacesOffset] & arg1) & 0xFF) && ((DKR_PTR(s16, arg0->unk10)[curFacesOffset] & arg1) & 0xFF00)) {
                    triangles = &DKR_PTR(Triangle, arg0->triangles)[curFacesOffset];
                    maxY = minY = vertices[triangles->verticesArray[1]].y;
                    for (i = 1; i < 3; i++) {
                        yPos = vertices[triangles->verticesArray[i + 1]].y;
                        if (yPos < minY) {
                            minY = yPos;
                        } else if (maxY < yPos) {
                            maxY = yPos;
                        }
                    }
                    if (gNewShadowY2 >= minY) {
                        if (maxY >= gNewShadowY1) {
                            for (i = 0; i < 3; i++) {
                                sp100[i].x = vertices[triangles->verticesArray[i + 1]].x;
                                sp100[i].z = vertices[triangles->verticesArray[i + 1]].z;
                                sp100[i].unkC_union.s.unkE = -1;
                            }
                            // @note while the cast to Vec4f is incorrect, func_8002FD74 x uses unk0 and z which
                            // are both floats so this is fine as the size is the same
                            if (func_8002FD74(spD0[2].x, spD0[2].y, spD0[0].x, spD0[0].y, 3, (Vec4f *) sp100) != 0) {
                                temp_t6 = DKR_PTR(CollisionFacetPlanes, arg0->collisionFacets)[curFacesOffset].basePlaneIndex * 4;
                                D_8011D0BC = (unk8011C8B8 *) &DKR_PTR(f32, (arg0->collisionPlanes))[temp_t6];
                                if (DKR_PTR(f32, arg0->collisionPlanes)[temp_t6 + 1] != 0) {
                                    if (D_8011D0F0 > 0.0f) {
                                        func_800304C8(sp100);
                                    }
                                    someCount = func_8002FF6C(3, sp100, 4, spD0);
                                    if (someCount >= 3) {
#ifdef NATIVE_PORT
                                        if (someCount > ARRAY_COUNT(D_8011C238[0].unk2) ||
                                            D_8011C230 < 0 ||
                                            D_8011C230 >= ARRAY_COUNT(D_8011C238)) {
                                            gShadowBuildOverflow = TRUE;
                                            return;
                                        }
#endif
                                        D_8011C238[D_8011C230].unk1 = 0;
                                        for (i2 = 0; i2 < someCount; i2++) {
                                            if (sp100[i2].unkC_union.s.unkE < 0) {
                                                foundIndex = -1;
                                                i = 0;
                                                while ((i < D_8011B118) && (foundIndex == -1)) {
                                                    if ((D_8011B120[i].x == sp100[i2].x) &&
                                                        (D_8011B120[i].z == sp100[i2].z)) {
                                                        foundIndex = i;
                                                    }
                                                    i++;
                                                }
                                                if (foundIndex == -1) {
#ifdef NATIVE_PORT
                                                    if (D_8011B118 < 0 ||
                                                        D_8011B118 >= ARRAY_COUNT(D_8011B120)) {
                                                        gShadowBuildOverflow = TRUE;
                                                        return;
                                                    }
#endif
                                                    D_8011B120[D_8011B118].x = sp100[i2].x;
                                                    D_8011B120[D_8011B118].unkC = D_8011D0BC;
                                                    D_8011B120[D_8011B118].z = sp100[i2].z;
                                                    D_8011C238[D_8011C230].unk2[i2] = D_8011B118++;
                                                } else {
                                                    D_8011C238[D_8011C230].unk2[i2] = foundIndex;
                                                }
                                            } else {
#ifdef NATIVE_PORT
                                                if (sp100[i2].unkC_union.s.unkE >=
                                                    ARRAY_COUNT(D_8011B330)) {
                                                    gShadowBuildOverflow = TRUE;
                                                    return;
                                                }
#endif
                                                D_8011C238[D_8011C230].unk2[i2] = sp100[i2].unkC_union.s.unkE;
                                                D_8011C238[D_8011C230].unk1 |= 1 << i2;
                                            }
                                        }
                                        D_8011C238[D_8011C230].unk0 = someCount;
                                        D_8011C238[D_8011C230].unkA = sp88;
                                        D_8011C230 += 1;
                                        if ((D_8011D0E8 >= 0) && (sp88 != D_8011D0E8)) {
                                            D_8011D0EC = 0;
                                        }
                                        D_8011D0E8 = sp88;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Handles water shadow generation?
void func_8002EEEC(s32 arg0) {
    unk8011C8B8 spA8[8];
    Vec2f sp88[4];
    s32 var_v0;
    s32 var_a0;
    s32 var_a1;
    s32 temp_v0;
    s32 temp_v0_3;
    s32 var_s4;
    s32 var_t1;
    s32 var_v1;
    s32 tempIdx;

    sp88[0].x = gNewShadowObj->trans.x_position + gNewShadowWidth;
    sp88[0].y = gNewShadowObj->trans.z_position + gNewShadowLength;
    sp88[1].x = gNewShadowObj->trans.x_position - gNewShadowWidth;
    sp88[1].y = gNewShadowObj->trans.z_position + gNewShadowLength;
    sp88[2].x = gNewShadowObj->trans.x_position - gNewShadowWidth;
    sp88[2].y = gNewShadowObj->trans.z_position - gNewShadowLength;
    sp88[3].x = gNewShadowObj->trans.x_position + gNewShadowWidth;
    sp88[3].y = gNewShadowObj->trans.z_position - gNewShadowLength;
    // clang-format off
#ifdef NATIVE_PORT
    /* NATIVE_PORT: the bound func_800BDC80() was missing. Two output arrays, so
     * the smaller remaining capacity governs: D_8011C3B8 is written from index 0,
     * D_8011C8B8 from the RUNNING offset D_8011D0B8, which is why the second term
     * is a subtraction and not just ARRAY_COUNT. */
    {
        s32 room8B8 = (s32) ARRAY_COUNT(D_8011C8B8) - (s32) D_8011D0B8;
        s32 room3B8 = (s32) ARRAY_COUNT(D_8011C3B8);
        s32 outputCapacity;

        if (D_8011D0B8 < 0 || room8B8 <= 0) {
            gShadowBuildOverflow = TRUE;
            return;
        }
        outputCapacity = room8B8 < room3B8 ? room8B8 : room3B8;
        temp_v0 = func_800BDC80(
            arg0, D_8011C3B8, &D_8011C8B8[D_8011D0B8],
            sp88[2].x, sp88[2].y,
            sp88[0].x, sp88[0].y,
            outputCapacity
        );
        if (temp_v0 < 0 || temp_v0 > outputCapacity) {
            gShadowBuildOverflow = TRUE;
            return;
        }
    }
#else
    temp_v0 = func_800BDC80(
        arg0, D_8011C3B8, &D_8011C8B8[D_8011D0B8],
        sp88[2].x, sp88[2].y,
        sp88[0].x, sp88[0].y
    );
#endif
    // clang-format on

    for (var_s4 = 0; var_s4 < temp_v0; var_s4++) {
        var_a0 = D_8011C3B8[var_s4].y1;
        var_a1 = D_8011C3B8[var_s4].y1;
        if (D_8011C3B8[var_s4].y2 < var_a0) {
            var_a0 = D_8011C3B8[var_s4].y2;
        } else if (var_a1 < D_8011C3B8[var_s4].y2) {
            var_a1 = D_8011C3B8[var_s4].y2;
        }
        if (D_8011C3B8[var_s4].y3 < var_a0) {
            var_a0 = D_8011C3B8[var_s4].y3;
        } else if (var_a1 < D_8011C3B8[var_s4].y3) {
            var_a1 = D_8011C3B8[var_s4].y3;
        }
        if (gNewShadowY2 >= var_a0) {
            if (var_a1 >= gNewShadowY1) {
                spA8[0].x = D_8011C3B8[var_s4].x1;
                spA8[0].z = D_8011C3B8[var_s4].z1;
                spA8[1].x = D_8011C3B8[var_s4].x2;
                spA8[1].z = D_8011C3B8[var_s4].z2;
                spA8[2].x = D_8011C3B8[var_s4].x3;
                spA8[2].z = D_8011C3B8[var_s4].z3;

                for (var_v0 = 0; var_v0 != 3; var_v0++) {
                    spA8[var_v0].unkC_union.s.unkE = -1;
                }

                D_8011D0BC = &D_8011C8B8[D_8011D0B8 + var_s4];
                temp_v0_3 = func_8002FF6C(3, spA8, 4, sp88);
                if (temp_v0_3 >= 3) {
#ifdef NATIVE_PORT
                    if (temp_v0_3 > ARRAY_COUNT(D_8011C238[0].unk2) ||
                        D_8011C230 < 0 ||
                        D_8011C230 >= ARRAY_COUNT(D_8011C238)) {
                        gShadowBuildOverflow = TRUE;
                        return;
                    }
#endif
                    tempIdx = D_8011C230;
                    D_8011C238[tempIdx].unk1 = 0;
                    for (var_t1 = 0; var_t1 < temp_v0_3; var_t1++) {
                        if (spA8[var_t1].unkC_union.s.unkE < 0) {
                            var_a1 = -1;
                            var_v1 = 0;
                            while (var_v1 < D_8011B118 && var_a1 == -1) {
                                if ((D_8011B120[var_v1].x == spA8[var_t1].x) &&
                                    (D_8011B120[var_v1].z == spA8[var_t1].z)) {
                                    var_a1 = var_v1;
                                }
                                var_v1++;
                            }
                            if (var_a1 == -1) {
#ifdef NATIVE_PORT
                                if (D_8011B118 < 0 ||
                                    D_8011B118 >= ARRAY_COUNT(D_8011B120)) {
                                    gShadowBuildOverflow = TRUE;
                                    return;
                                }
#endif
                                D_8011B120[D_8011B118].x = spA8[var_t1].x;
                                D_8011B120[D_8011B118].unkC = D_8011D0BC;
                                D_8011B120[D_8011B118].z = spA8[var_t1].z;
                                D_8011C238[tempIdx].unk2[var_t1] = D_8011B118++;
                            } else {
                                D_8011C238[tempIdx].unk2[var_t1] = var_a1;
                            }
                        } else {
#ifdef NATIVE_PORT
                            if (spA8[var_t1].unkC_union.s.unkE >=
                                ARRAY_COUNT(D_8011B330)) {
                                gShadowBuildOverflow = TRUE;
                                return;
                            }
#endif
                            D_8011C238[tempIdx].unk2[var_t1] = spA8[var_t1].unkC_union.s.unkE;
                            D_8011C238[tempIdx].unk1 |= 1 << var_t1;
                        }
                    }
                    D_8011C230 = tempIdx + 1;
                    D_8011C238[tempIdx].unk0 = temp_v0_3;
                }
            }
        }
    }

    D_8011D0B8 += temp_v0;
}

void func_8002F2AC(void) {
    f32 temp_f12;
    f32 temp_f16;
    unk8011C8B8 *var_v0;
    s32 i, j, k;
    unk8011B330 *curr;

    for (i = 0; i < D_8011B118; i++) {
        var_v0 = D_8011B120[i].unkC;
        temp_f16 = D_8011B120[i].x * var_v0->x;
        temp_f12 = D_8011B120[i].z * var_v0->z;
        D_8011B120[i].y = (f32) (-(temp_f16 + temp_f12 + var_v0->unkC_union.w) / var_v0->y);
    }

    for (i = 0; i < ARRAY_COUNT(D_8011B320); i++) {
        /* Row stride is 32 ELEMENTS (matches the population site `var_v0 << 5`).
         * The decomp writes `i * sizeof(unk8011B330)` because sizeof==0x20 on N64;
         * on LP64 the struct's pointer field (unkC) inflates it, so use the literal
         * element stride to keep the index correct. */
        for (j = 0, k = i << 5; j < D_8011B320[i]; j++, k++) {
            var_v0 = D_8011B330[k].unkC;
            temp_f16 = D_8011B330[k].x * var_v0->x;
            temp_f12 = D_8011B330[k].z * var_v0->z;
            D_8011B330[k].y = (f32) (-(temp_f16 + temp_f12 + var_v0->unkC_union.w) / var_v0->y);
        }
    }
}

void func_8002F440(void) {
    s32 spAC;
    s32 var_t0;
    Triangle *tri;
    Vertex *vert;
    s32 alpha;
#ifdef NATIVE_PORT
    /* N64 declares sp90[6]/sp80[6] but func_8002FF6C clips a 3-vertex triangle
     * against a 4-plane frustum, yielding up to 7 output verts (unk0/unk2[8]).
     * Writing sp90[6]/sp80[6] is a benign stack overrun on N64 (no canary) but
     * trips -fstack-protector here, so size them to the real max. */
    s16 sp90[8];
    s32 var_s2;
    s16 sp80[8];
#else
    s16 sp90[6];
    s32 var_s2;
    s16 sp80[6];
#endif
    f32 temp_f18;
    f32 yRotCos;
    f32 yRotSin;
    f32 zDiff;
    f32 someY;
    f32 scale;
    f32 xDiff;
    s32 i;
    UNUSED s32 pad;

    alpha = 255;
    yRotSin = sins_f(gNewShadowObj->trans.rotation.y_rotation);
    yRotCos = coss_f(gNewShadowObj->trans.rotation.y_rotation);
    temp_f18 = gNewShadowTexture->width * 16;
    scale = ((gNewShadowTexture->width * 16) / gNewShadowWidth);
    scale *= 1.4142f;
    if (D_8011D0F0 > 0.0f) {
        someY = gNewShadowObj->trans.y_position - D_8011D0D0;
        if (D_8011D0F0 < someY) {
            alpha = 255;
            alpha -= (s32) ((255 * (someY - D_8011D0F0)) / D_8011D0F4);
            if (alpha < 0) {
                alpha = 0;
            }
        }
        if (someY > 0.0f) {
            scale *= 1.0f + (0.005f * someY);
        }
    }
    alpha *= gShadowOpacity;
    var_s2 = 25;
#ifdef NATIVE_PORT
    if (D_8011C230 < 0 || D_8011C230 > ARRAY_COUNT(D_8011C238) ||
        D_8011B118 < 0 || D_8011B118 > ARRAY_COUNT(D_8011B120)) {
        gShadowBuildOverflow = TRUE;
        return;
    }
#endif
    for (spAC = 0; spAC < D_8011C230; spAC++) {
#ifdef NATIVE_PORT
        s32 polygonVertexCount = D_8011C238[spAC].unk0;
        s32 polygonTriangleCount = polygonVertexCount > 2 ? polygonVertexCount - 2 : 0;
        s32 needsDescriptor = (polygonVertexCount + var_s2) >= 24;
        s32 sourceFlags = D_8011C238[spAC].unk1;

        if (polygonVertexCount > ARRAY_COUNT(D_8011C238[spAC].unk2) ||
            gShadowTail < 0 ||
            (needsDescriptor && gShadowTail + 1 >= gShadowDataCap) ||
            gNewShadowVtxCount < 0 ||
            polygonVertexCount > gShadowVtxCap - gNewShadowVtxCount ||
            gNewShadowTriCount < 0 ||
            polygonTriangleCount > gShadowTriCap - gNewShadowTriCount) {
            gShadowBuildOverflow = TRUE;
            break;
        }
        for (i = 0; i < polygonVertexCount; i++) {
            s32 sourceIndex = D_8011C238[spAC].unk2[i];
            s32 sourceCapacity =
                (sourceFlags & 1) ? ARRAY_COUNT(D_8011B330) : D_8011B118;
            if (sourceIndex < 0 || sourceIndex >= sourceCapacity) {
                gShadowBuildOverflow = TRUE;
                break;
            }
            sourceFlags >>= 1;
        }
        if (gShadowBuildOverflow) {
            break;
        }
#endif
        if ((D_8011C238[spAC].unk0 + var_s2) >= 24) {
            gCurrShadowHeapData[gShadowTail].texture = gNewShadowTexture;
            gCurrShadowHeapData[gShadowTail].triCount = gNewShadowTriCount;
            gCurrShadowHeapData[gShadowTail].vtxCount = gNewShadowVtxCount;
            gShadowTail++;
            var_s2 = 0;
        }
        var_t0 = D_8011C238[spAC].unk1;

        for (i = 0; i < D_8011C238[spAC].unk0; i++) {
            vert = &gCurrShadowVerts[gNewShadowVtxCount];
            gNewShadowVtxCount++;
            if (var_t0 & 1) {
                vert->x = D_8011B330[D_8011C238[spAC].unk2[i]].x;
                xDiff = D_8011B330[D_8011C238[spAC].unk2[i]].x - gNewShadowObj->trans.x_position;
                vert->y = (D_8011B330[D_8011C238[spAC].unk2[i]].y + D_8011D0C8);
                vert->z = D_8011B330[D_8011C238[spAC].unk2[i]].z;
                zDiff = D_8011B330[D_8011C238[spAC].unk2[i]].z - gNewShadowObj->trans.z_position;
            } else {
                vert->x = D_8011B120[D_8011C238[spAC].unk2[i]].x;
                xDiff = D_8011B120[D_8011C238[spAC].unk2[i]].x - gNewShadowObj->trans.x_position;
                vert->y = (D_8011B120[D_8011C238[spAC].unk2[i]].y + D_8011D0C8);
                vert->z = D_8011B120[D_8011C238[spAC].unk2[i]].z;
                zDiff = D_8011B120[D_8011C238[spAC].unk2[i]].z - gNewShadowObj->trans.z_position;
            }
            var_t0 >>= 1;
            vert->r = 255;
            vert->g = 255;
            vert->b = 255;
            vert->a = alpha;
            sp90[i] = dkr_f32_to_s16_wrap(
                (((xDiff * yRotCos) - (zDiff * yRotSin)) * scale) + temp_f18);
            sp80[i] = dkr_f32_to_s16_wrap(
                (((zDiff * yRotCos) + (xDiff * yRotSin)) * scale) + temp_f18);
        }

        for (i = 1; i < (D_8011C238[spAC].unk0 - 1); i++) {
            tri = &gCurrShadowTris[gNewShadowTriCount];
            gNewShadowTriCount++;
            tri->flags = 0x40;
            tri->vi0 = var_s2 + i;
            tri->vi1 = var_s2 + i + 1;
            tri->vi2 = var_s2;
            tri->uv0.u = sp90[i];
            tri->uv0.v = sp80[i];
            tri->uv1.u = sp90[i + 1];
            tri->uv1.v = sp80[i + 1];
            tri->uv2.u = sp90[0];
            tri->uv2.v = sp80[0];
        }
        var_s2 += D_8011C238[spAC].unk0;
    }
}

// Transition points between different lighting levels, used by certain objects
f32 func_8002FA64(void) {
    f32 var_f2;
    f32 x0, z0, x1, z1, x2, z2;
    s32 temp_t5;
    s32 var_s2;
    s32 i;

    var_f2 = 0.0f;
    if (D_8011C230 > 0) {
        if (D_8011D0EC != 0) {
            if (D_8011D0E8 > 0) {
                var_f2 = D_800DC884[D_8011D0E8] * D_8011D0E4;
            }
        } else {
            for (i = 0; i < D_8011C230; i++) {
                if (D_8011C238[i].unkA > 0) {
                    temp_t5 = D_8011C238[i].unk1;
                    if (temp_t5 & 1) {
                        x0 = D_8011B330[D_8011C238[i].unk2[0]].x;
                        z0 = D_8011B330[D_8011C238[i].unk2[0]].z;
                    } else {
                        x0 = D_8011B120[D_8011C238[i].unk2[0]].x;
                        z0 = D_8011B120[D_8011C238[i].unk2[0]].z;
                    }
                    temp_t5 >>= 1;
                    if (temp_t5 & 1) {
                        x1 = D_8011B330[D_8011C238[i].unk2[1]].x;
                        z1 = D_8011B330[D_8011C238[i].unk2[1]].z;
                    } else {
                        x1 = D_8011B120[D_8011C238[i].unk2[1]].x;
                        z1 = D_8011B120[D_8011C238[i].unk2[1]].z;
                    }
                    temp_t5 >>= 1;
                    for (var_s2 = 2; var_s2 < D_8011C238[i].unk0; var_s2++) {
                        if (temp_t5 & 1) {
                            x2 = D_8011B330[D_8011C238[i].unk2[var_s2]].x;
                            z2 = D_8011B330[D_8011C238[i].unk2[var_s2]].z;
                        } else {
                            x2 = D_8011B120[D_8011C238[i].unk2[var_s2]].x;
                            z2 = D_8011B120[D_8011C238[i].unk2[var_s2]].z;
                        }
                        temp_t5 >>= 1;
                        var_f2 += area_triangle_2d(x0, z0, x1, z1, x2, z2) * D_800DC884[D_8011C238[i].unkA];
                        x1 = x2;
                        z1 = z2;
                    }
                }
            }
        }

        if (D_8011D0E4 < var_f2) {
            var_f2 = D_8011D0E4 * 0.99f;
        }
    }
    return (D_8011D0E4 - var_f2) / D_8011D0E4;
}

s32 func_8002FD74(f32 x0, f32 z0, f32 x1, f32 x2, s32 count, Vec4f *arg5) {
    if (count > 0) {
        f32 minX = arg5[0].x;
        f32 maxX = arg5[0].x;
        f32 minZ = arg5[0].z;
        f32 maxZ = arg5[0].z;
        s32 i;

        for (i = 1; i < count; i++) {
            if (arg5[i].x < minX) {
                minX = arg5[i].x;
            } else if (maxX < arg5[i].x) {
                maxX = arg5[i].x;
            }
            if (arg5[i].z < minZ) {
                minZ = arg5[i].z;
            } else if (maxZ < arg5[i].z) {
                maxZ = arg5[i].z;
            }
        }

        if ((x0 <= maxX) && (z0 <= maxZ) && (minX <= x1) && (minZ <= x2)) {
            return -1;
        }
    }

    return 0;
}

// arg0 is always 3
// arg1 always has size 8 (that's why spE0 is also of size 8)
// arg2 is always 4
// arg3 always has size 4
s32 func_8002FF6C(s32 arg0, unk8011C8B8 *arg1, s32 arg2, Vec2f *arg3) {
    unk8011C8B8 spE0[8];
    f32 temp_f12;
    f32 temp_f14;
    f32 temp_f16;
    f32 temp_f22;
    f32 temp_f24;
    f32 var_f2;
    s32 var_a1;
    s32 var_v1_3;
    s32 var_a0;
    s32 var_t2;
    UNUSED s32 var_t5;
    s32 var_v0;
    s32 var_v1;
    s32 var_t1;
    s32 var_s2;
    unk8011C8B8 *var_s0;
    unk8011C8B8 *var_s3;
    unk8011C8B8 *swap; // swap var for var_s0 and var_s3

    var_s3 = arg1;
    var_s0 = spE0;
    var_s2 = arg0;
    var_v0 = 0;

    while (var_v0 < arg2 && var_s2 >= 3) {
        var_v1 = var_v0 + 1;
        if (var_v1 >= arg2) {
            var_v1 = 0;
        }

        temp_f12 = arg3[var_v1].y - arg3[var_v0].y;
        temp_f14 = -(arg3[var_v1].x - arg3[var_v0].x);
        if (arg3[var_v0].x < arg3[var_v1].x) {
            var_f2 = (temp_f12 * arg3[var_v0].x) + (arg3[var_v0].y * temp_f14);
            var_f2 = -var_f2;
        } else {
            var_f2 = (temp_f12 * arg3[var_v1].x) + (arg3[var_v1].y * temp_f14);
            var_f2 = -var_f2;
        }

        for (var_t1 = 0, var_t2 = 0; var_t1 < var_s2; var_t1++) {
            var_v1 = var_t1 + 1;
            if (var_v1 >= var_s2) {
                var_v1 = 0;
            }
            temp_f16 = (temp_f12 * var_s3[var_t1].x) + (var_s3[var_t1].z * temp_f14) + var_f2;
            temp_f22 = (temp_f12 * var_s3[var_v1].x) + (var_s3[var_v1].z * temp_f14) + var_f2;
            if ((temp_f16 >= 0.0f && temp_f22 < 0.0f) || (temp_f16 < 0.0f && temp_f22 >= 0.0f)) {
                var_a0 = D_8011B320[var_v0];
                var_a1 = -1;
                var_v1_3 = var_v0 << 5;
                while (var_a0 > 0 && var_a1 < 0) {
                    if ((D_8011B330[var_v1_3].unk10 == var_s3[var_t1].x) &&
                        (D_8011B330[var_v1_3].unk14 == var_s3[var_t1].z) &&
                        (D_8011B330[var_v1_3].unk18 == var_s3[var_v1].x) &&
                        (D_8011B330[var_v1_3].unk1C == var_s3[var_v1].z)) {
                        var_a1 = var_v1_3;
                    } else if ((D_8011B330[var_v1_3].unk10 == var_s3[var_v1].x) &&
                               (D_8011B330[var_v1_3].unk14 == var_s3[var_v1].z) &&
                               (D_8011B330[var_v1_3].unk18 == var_s3[var_t1].x) &&
                               (D_8011B330[var_v1_3].unk1C == var_s3[var_t1].z)) {
                        var_a1 = var_v1_3;
                    }
                    var_a0 -= 1;
                    var_v1_3++;
                }
                if (var_a1 >= 0) {
                    var_s0[var_t2].unkC_union.s.unkE = var_a1;
                    var_s0[var_t2].x = D_8011B330[var_a1].x;
                    var_s0[var_t2].z = D_8011B330[var_a1].z;
                    var_t2++;
                } else {
                    temp_f24 = temp_f16 / (temp_f16 - temp_f22);
                    var_s0[var_t2].x = var_s3[var_t1].x + ((var_s3[var_v1].x - var_s3[var_t1].x) * temp_f24);
                    var_s0[var_t2].z = var_s3[var_t1].z + ((var_s3[var_v1].z - var_s3[var_t1].z) * temp_f24);
                    if (D_8011B320[var_v0] > 0x1F) {
                        D_8011B320[var_v0] = 0x1F;
                    }
                    var_v1_3 = D_8011B320[var_v0] + (var_v0 << 5);
                    D_8011B330[var_v1_3].unk10 = var_s3[var_t1].x;
                    D_8011B330[var_v1_3].unk14 = var_s3[var_t1].z;
                    D_8011B330[var_v1_3].unk18 = var_s3[var_v1].x;
                    D_8011B330[var_v1_3].unk1C = var_s3[var_v1].z;
                    D_8011B330[var_v1_3].x = var_s0[var_t2].x;
                    D_8011B330[var_v1_3].z = var_s0[var_t2].z;
                    D_8011B330[var_v1_3].unkC = D_8011D0BC;
                    D_8011B320[var_v0]++;
                    var_s0[var_t2].unkC_union.s.unkE = var_v1_3;
                    var_t2++;
                }
            }
            if (temp_f22 <= 0.0f) {
                var_s0[var_t2].unkC_union.s.unkE = var_s3[var_v1].unkC_union.s.unkE;
                var_s0[var_t2].x = var_s3[var_v1].x;
                var_s0[var_t2].z = var_s3[var_v1].z;
                var_t2++;
            }
        }
        var_s2 = var_t2;
        var_v0++;

        swap = var_s3;
        var_s3 = var_s0;
        var_s0 = swap;
    }

    if (var_s2 >= 3) {
        if (var_s3 != arg1) {
            for (var_t1 = 0; var_t1 < var_s2; var_t1++) {
                arg1[var_t1].x = var_s3[var_t1].x;
                arg1[var_t1].z = var_s3[var_t1].z;
                arg1[var_t1].unkC_union.s.unkE = var_s3[var_t1].unkC_union.s.unkE;
            }
        }
    } else {
        var_s2 = 0;
    }
    return var_s2;
}

void func_800304C8(unk8011C8B8 arg0[3]) {
    f32 diff;
    f32 shadowX;
    f32 shadowZ;
    f32 var_f6;
    f32 var_f8;
    f32 var_f10;
    s32 found1;
    s32 found2;
    s32 found3;

    found1 = FALSE;
    found2 = FALSE;
    found3 = FALSE;

    shadowX = gNewShadowObj->trans.x_position;
    shadowZ = gNewShadowObj->trans.z_position;

    diff = (shadowX - arg0[0].x) * (arg0[1].z - arg0[0].z) - (arg0[1].x - arg0[0].x) * (shadowZ - arg0[0].z);
    if (diff >= 0.0f) {
        found1 = TRUE;
    }

    diff = (shadowX - arg0[1].x) * (arg0[2].z - arg0[1].z) - (arg0[2].x - arg0[1].x) * (shadowZ - arg0[1].z);
    if (diff >= 0.0f) {
        found2 = TRUE;
    }

    if (found1 != found2) {
        return;
    }

    diff = (shadowX - arg0[2].x) * (arg0[0].z - arg0[2].z) - (arg0[0].x - arg0[2].x) * (shadowZ - arg0[2].z);
    if (diff >= 0.0f) {
        found3 = TRUE;
    }
    if (found2 != found3) {
        return;
    }

    diff = -(D_8011D0BC->x * shadowX + D_8011D0BC->z * shadowZ + D_8011D0BC->unkC_union.w) / D_8011D0BC->y;
    if (D_8011D0D0 < diff) {
        D_8011D0D0 = diff;
    }
}

/**
 * Instantly update current fog properties.
 * Official Name: trackSetFog
 */
void set_fog(s32 fogIdx, s16 near, s16 far, u8 red, u8 green, u8 blue) {
    s32 tempNear;
    FogData *fogData;

    fogData = &gFogData[fogIdx];

    if (far < near) {
        tempNear = near;
        near = far;
        far = tempNear;
    }

    if (far > 1023) {
        far = 1023;
    }
    if (near >= far - 5) {
        near = far - 5;
    }

    fogData->addFog.near = 0;
    fogData->addFog.far = 0;
    fogData->addFog.r = 0;
    fogData->addFog.g = 0;
    fogData->addFog.b = 0;
    fogData->fog.r = red << 16;
    fogData->fog.g = green << 16;
    fogData->fog.b = blue << 16;
    fogData->fog.near = near << 16;
    fogData->fog.far = far << 16;
    fogData->intendedFog.r = red;
    fogData->intendedFog.g = green;
    fogData->intendedFog.b = blue;
    fogData->intendedFog.near = near;
    fogData->intendedFog.far = far;
    fogData->switchTimer = 0;
    fogData->fogChanger = NULL;
}

#ifdef NATIVE_PORT
/**
 * The colour-only half of set_fog().
 *
 * rain_fog() recomputes the storm fog from gLightningFrequency EVERY frame and
 * pushed it through set_fog(), whose last two lines are `switchTimer = 0` and
 * `fogChanger = NULL`. Those two are set_fog's "this is a fresh, authoritative
 * fog state" half, and rain re-asserting them per frame had two side effects
 * that outlive the frame:
 *
 *   - `fogChanger` is obj_loop_fogchanger's don't-retrigger LATCH (it skips a
 *     changer when `obj == gFogData[i].fogChanger`). NULLing it re-arms every
 *     fog-changer object on the track, once per frame, for as long as a racer
 *     is inside its radius -- so the changer recomputed its addFog ramp from
 *     scratch every frame instead of running once;
 *   - `switchTimer` is the remaining duration of a fade started by
 *     slowly_change_fog() (object_functions.c's Taj and TT cutscenes both fade
 *     the fog out and back), so a rain level cancelled those fades outright.
 *
 * This variant writes only what rain actually decides -- the current and
 * intended colour and range -- and leaves the fade state machine alone. On a
 * non-rain level rain_fog() does nothing at all, so nothing here is reachable
 * and the behaviour is bit-identical.
 */
void set_fog_colour(s32 fogIdx, s16 near, s16 far, u8 red, u8 green, u8 blue) {
    s32 tempNear;
    FogData *fogData;

    fogData = &gFogData[fogIdx];

    if (far < near) {
        tempNear = near;
        near = far;
        far = tempNear;
    }

    if (far > 1023) {
        far = 1023;
    }
    if (near >= far - 5) {
        near = far - 5;
    }

    fogData->addFog.near = 0;
    fogData->addFog.far = 0;
    fogData->addFog.r = 0;
    fogData->addFog.g = 0;
    fogData->addFog.b = 0;
    fogData->fog.r = red << 16;
    fogData->fog.g = green << 16;
    fogData->fog.b = blue << 16;
    fogData->fog.near = near << 16;
    fogData->fog.far = far << 16;
    fogData->intendedFog.r = red;
    fogData->intendedFog.g = green;
    fogData->intendedFog.b = blue;
    fogData->intendedFog.near = near;
    fogData->intendedFog.far = far;
}
#endif

/**
 * Writes the current fog settings to the arguments.
 * Pre-shifts the data, so the raw values are correct.
 * Official Name: trackGetFog
 */
void get_fog_settings(s32 playerID, s16 *near, s16 *far, u8 *r, u8 *g, u8 *b) {
    *near = gFogData[playerID].fog.near >> 16;
    *far = gFogData[playerID].fog.far >> 16;
    *r = gFogData[playerID].fog.r >> 16;
    *g = gFogData[playerID].fog.g >> 16;
    *b = gFogData[playerID].fog.b >> 16;
}

/**
 * Sets the fog of the player ID to the default values.
 * Current fog attributes are rightshifted 16 bytes.
 * Official Name: trackSetFogOff
 */
void reset_fog(s32 playerID) {
    gFogData[playerID].addFog.near = 0;
    gFogData[playerID].addFog.far = 0;
    gFogData[playerID].addFog.r = 0;
    gFogData[playerID].addFog.g = 0;
    gFogData[playerID].addFog.b = 0;
    gFogData[playerID].fog.near = 1018 << 16;
    gFogData[playerID].fog.far = 1023 << 16;
    gFogData[playerID].intendedFog.r = gFogData[playerID].fog.r >> 16;
    gFogData[playerID].intendedFog.g = gFogData[playerID].fog.g >> 16;
    gFogData[playerID].intendedFog.b = gFogData[playerID].fog.b >> 16;
    gFogData[playerID].intendedFog.near = 1018;
    gFogData[playerID].intendedFog.far = 1023;
    gFogData[playerID].switchTimer = 0;
    gFogData[playerID].fogChanger = NULL;
}

/**
 * If the fog override timer is active, apply that and slowly degrade.
 * Otherwise, set the current fog to the intended fog settings.
 */
void update_fog(s32 viewportCount, s32 updateRate) {
    s32 i;
    for (i = 0; i < viewportCount; i++) {
        if (gFogData[i].switchTimer > 0) {
            if (updateRate < gFogData[i].switchTimer) {
                gFogData[i].fog.r += gFogData[i].addFog.r * updateRate;
                gFogData[i].fog.g += gFogData[i].addFog.g * updateRate;
                gFogData[i].fog.b += gFogData[i].addFog.b * updateRate;
                gFogData[i].fog.near += gFogData[i].addFog.near * updateRate;
                gFogData[i].fog.far += gFogData[i].addFog.far * updateRate;
                gFogData[i].switchTimer -= updateRate;
            } else {
                gFogData[i].fog.r = gFogData[i].intendedFog.r << 16;
                gFogData[i].fog.g = gFogData[i].intendedFog.g << 16;
                gFogData[i].fog.b = gFogData[i].intendedFog.b << 16;
                gFogData[i].fog.near = gFogData[i].intendedFog.near << 16;
                gFogData[i].fog.far = gFogData[i].intendedFog.far << 16;
                gFogData[i].switchTimer = 0;
            }
        }
    }
}

#ifdef NATIVE_PORT
/**
 * The presentation accumulators, hoisted out of render_scene.
 *
 * None of these four has a gameplay reader, but every one of them integrates
 * with the update rate, so under the eventual
 * high-rate presentation loop they would run N times per authoritative tick and
 * animate N times too fast, and they are already wrong for 2-4 players, where
 * everything under render_scene runs once per VIEWPORT. Moving the advance out
 * of the draw is what makes "one simulation tick, many renders" mean the same
 * thing for them as for the rest of the scene.
 *
 * In render_scene's order these ran as: colour cycles, pulsating lights, ...,
 * skydome scroll, ..., particle texture scroll. That order is preserved here.
 * Collapsing the particle scroll up next to the other three is order-neutral:
 * the four touch disjoint state (level-header colour blobs, the pulsating-light
 * struct, the skydome header fields, and the particle UV arrays) and read
 * nothing from each other.
 *
 * The pause gate is render_scene's own `is_game_paused() ? 0 : updateRate`,
 * applied by the caller.
 *
 * KNOWN, DELIBERATELY NOT FIXED HERE: update_colour_cycle and
 * update_pulsating_light_data both mutate shared gAssetsMiscSection ROM blobs in
 * place, reset only at level load. Relocating them to per-level copies is a
 * separate change; this function only moves WHEN they advance.
 */
void scene_presentation_tick(s32 updateRate) {
    s32 i;

    for (i = 0; i < 7; i++) {
        if ((s32) gCurrentLevelHeader2->unk74[i] != -1) {
            update_colour_cycle(DKR_PTR(LevelHeader_70, gCurrentLevelHeader2->unk74[i]), updateRate);
        }
    }
    if ((s32) gCurrentLevelHeader2->pulseLightData != -1) {
        update_pulsating_light_data(DKR_PTR(PulsatingLightData, gCurrentLevelHeader2->pulseLightData), updateRate);
    }
    if (gCurrentLevelHeader2->skyDome == -1) {
        TextureHeader *skyTex = DKR_PTR(TextureHeader, gCurrentLevelHeader2->unkA4);
        i = (skyTex->width << 9) - 1;
        gCurrentLevelHeader2->unkA8 =
            (gCurrentLevelHeader2->unkA8 + (gCurrentLevelHeader2->unkA2 * updateRate)) & i;
        i = (skyTex->height << 9) - 1;
        gCurrentLevelHeader2->unkAA =
            (gCurrentLevelHeader2->unkAA + (gCurrentLevelHeader2->unkA3 * updateRate)) & i;
        /* The fixed tick owns the one skydome-scroll roll migrated from the
         * native render. The separately rendered skydome object remains outside
         * ordinary-object authored scope and therefore uses presentation RNG. */
        tex_animate_texture_cadence_compat(skyTex, &gTrackTexAnimFlags, &gTrackTexAnimOffset, updateRate);
    }
    scroll_particle_textures(updateRate);
}
#endif

#ifdef NATIVE_PORT
/**
 * weather_tick under render_scene's own gate.
 *
 * render_scene calls weather_update() inside the viewport loop, guarded by
 * `gCurrentLevelHeader2->weatherEnable > 0 && numViewports < 2` -- so it runs at
 * most once per frame, in single player only. The tick reproduces that gate
 * exactly; the pause gate (render_scene's tempUpdateRate) is applied by the
 * caller in thread3_main.c.
 */
void scene_weather_tick(s32 updateRate) {
    if (gCurrentLevelHeader2 != NULL && gCurrentLevelHeader2->weatherEnable > 0 &&
        scene_visibility_viewport_count() < 2) {
        weather_tick(updateRate);
    }
}
#endif

#ifdef NATIVE_PORT
/**
 * The fog integrator, hoisted out of render.
 *
 * gFogData is NOT presentation-only. update_fog() integrates
 * gFogData[].fog / .switchTimer with the update rate, and the result is read
 * back by the simulation: obj_loop_fogchanger() latches on it, and
 * get_fog_settings() is called from object_functions.c (the TT and Taj cutscene
 * objects save the current fog before fading it and restore it afterwards).
 * Running it inside render_scene meant the fog fade advanced once per RENDER,
 * so under high-rate presentation it would fade N times too fast, and the
 * saved/restored values a cutscene object reads depend on how many times the
 * scene was drawn.
 *
 * rain_fog() moves with it, and in the same order, so the pair keeps the exact
 * within-frame sequence render_scene used (rain_fog then update_fog). The
 * viewport count is render's own `cam_set_layout(gScenePlayerViewports)`, via
 * the idempotent helper the sort/visibility ticks already use.
 */
void fog_tick(s32 updateRate) {
    /*
     * Resolve the layout BEFORE rain_fog, not as update_fog's argument.
     * rain_fog() reads cam_get_viewport_layout() itself (weather.c: it only
     * fires on VIEWPORT_LAYOUT_1_PLAYER), and cam_set_layout -- which is what
     * scene_visibility_viewport_count() is -- is the call that publishes it.
     * Evaluated as update_fog's argument it ran AFTER rain_fog, so rain_fog was
     * reading whatever layout the previous frame left behind. It happens to be
     * fresh today because hud_tick and obj_sort_tick's basis both resolve the
     * layout earlier in the same tick, but that is an accident of ordering and
     * exactly the kind of latent staleness this lane exists to remove.
     */
    s32 numViewports = scene_visibility_viewport_count();
    rain_fog();
    update_fog(numViewports, updateRate);
}
#endif

/**
 * Sets the fog settings for the active viewport based on the parameters of the environment data.
 */
void apply_fog(s32 playerID) {
    gDPSetFogColor(gTrackDL++, gFogData[playerID].fog.r >> 0x10, gFogData[playerID].fog.g >> 0x10,
                   gFogData[playerID].fog.b >> 0x10, 0xFF);
    gSPFogPosition(gTrackDL++, (gFogData[playerID].fog.near >> 0x10), (gFogData[playerID].fog.far >> 0x10));
}

/**
 * Sets the active viewport's fog target when passed through.
 * Used in courses to make less, or more dense.
 * @bug: Timer doesn't account for PAL, meaning fog will scroll 20% slower on PAL systems.
 * Official Name: trackChangeFog
 */
void obj_loop_fogchanger(Object *obj) {
    s32 nearTemp;
    s32 fogNear;
    s32 views;
    s32 playerIndex;
    s32 index;
    UNUSED s32 pad;
    s32 fogFar;
    s32 i;
    s32 fogR;
    s32 fogG;
    s32 fogB;
    f32 x;
    f32 z;
    s32 switchTimer;
    LevelObjectEntry_FogChanger *fogChanger;
    Object **racers;
    Object_Racer *racer;
    UNUSED s32 pad2;
    FogData *fog;
    Camera *camera;

    racers = NULL;
    fogChanger = (LevelObjectEntry_FogChanger *) obj->level_entry;
    camera = NULL;

    if (check_if_showing_cutscene_camera()) {
        camera = cam_get_cameras();
        views = cam_get_viewport_layout() + 1;
    } else {
        racers = get_racer_objects(&views);
    }

    for (i = 0; i < views; i++) {
        index = PLAYER_COMPUTER;
        if (racers != NULL) {
            racer = racers[i]->racer;
            playerIndex = racer->playerIndex;
            if (playerIndex >= PLAYER_ONE && playerIndex <= PLAYER_FOUR && obj != gFogData[playerIndex].fogChanger) {
                index = playerIndex;
                x = racers[i]->trans.x_position;
                z = racers[i]->trans.z_position;
            }
        } else if (i <= PLAYER_FOUR && obj != gFogData[i].fogChanger) {
            index = i;
            x = camera[i].trans.x_position;
            z = camera[i].trans.z_position;
        }
        if (index != PLAYER_COMPUTER) {
            x -= obj->trans.x_position;
            z -= obj->trans.z_position;
            if (1) {} // Fakematch
            if ((x * x) + (z * z) < obj->properties.distance.radius) {
                fogNear = fogChanger->near;
                fogFar = fogChanger->far;
                fogR = fogChanger->r;
                fogG = fogChanger->g;
                fogB = fogChanger->b;
                switchTimer = fogChanger->switchTimer;
                // Swap near and far if they're the wrong way around.
                if (fogFar < fogNear) {
                    nearTemp = fogNear;
                    fogNear = fogFar;
                    fogFar = nearTemp;
                }
                if (fogFar > 1023) {
                    fogFar = 1023;
                }
                if (fogNear >= fogFar - 5) {
                    fogNear = fogFar - 5;
                }
                fog = &gFogData[index];
                fog->intendedFog.r = fogR;
                fog->intendedFog.g = fogG;
                fog->intendedFog.b = fogB;
                fog->intendedFog.near = fogNear;
                fog->intendedFog.far = fogFar;
                fog->addFog.r = ((fogR << 16) - fog->fog.r) / switchTimer;
                fog->addFog.g = ((fogG << 16) - fog->fog.g) / switchTimer;
                fog->addFog.b = ((fogB << 16) - fog->fog.b) / switchTimer;
                fog->addFog.near = ((fogNear << 16) - fog->fog.near) / switchTimer;
                fog->addFog.far = ((fogFar << 16) - fog->fog.far) / switchTimer;
                fog->switchTimer = switchTimer;
                fog->fogChanger = obj;
            }
        }
    }
}

/**
 * Set the fog properties from the current values to the target, over a time specified by switchTimer.
 * @bug: Timer doesn't account for PAL, meaning fog will scroll 20% slower on PAL systems.
 * Official Name: trackFadeFog
 */
void slowly_change_fog(s32 fogIdx, s32 red, s32 green, s32 blue, s32 near, s32 far, s32 switchTimer) {
    s32 temp;
    FogData *fogData;

    fogData = &gFogData[fogIdx];

    if (far < near) {
        temp = near;
        near = far;
        far = temp;
    }

    if (far > 1023) {
        far = 1023;
    }
    if (near >= far - 5) {
        near = far - 5;
    }

    fogData->intendedFog.r = red;
    fogData->intendedFog.g = green;
    fogData->intendedFog.b = blue;
    fogData->intendedFog.near = near;
    fogData->intendedFog.far = far;
    fogData->addFog.r = ((red << 16) - fogData->fog.r) / switchTimer;
    fogData->addFog.g = ((green << 16) - fogData->fog.g) / switchTimer;
    fogData->addFog.b = ((blue << 16) - fogData->fog.b) / switchTimer;
    fogData->addFog.near = ((near << 16) - fogData->fog.near) / switchTimer;
    fogData->addFog.far = ((far << 16) - fogData->fog.far) / switchTimer;
    fogData->switchTimer = switchTimer;
    fogData->fogChanger = NULL;
}

/**
 * Updates the stored perspective of the camera, as well as the envmap values derived from it.
 */
UNUSED void update_perspective_and_envmap(void) {
    gSceneActiveCamera = cam_get_active_camera();
    compute_scene_camera_transform_matrix();
    update_envmap_position((f32) gScenePerspectivePos.x / 65536.0f, (f32) gScenePerspectivePos.y / 65536.0f,
                           (f32) gScenePerspectivePos.z / 65536.0f);
}

/**
 * Take the current camera position and calculate the perspective position, for envmapping.
 */
void compute_scene_camera_transform_matrix(void) {
    MtxF mtx;
    ObjectTransform trans;

    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = -65536.0f;

    trans.rotation.z_rotation = gSceneActiveCamera->trans.rotation.z_rotation;
    trans.rotation.x_rotation = gSceneActiveCamera->trans.rotation.x_rotation;
    trans.rotation.y_rotation = gSceneActiveCamera->trans.rotation.y_rotation;
    trans.x_position = 0.0f;
    trans.y_position = 0.0f;
    trans.z_position = 0.0f;
    trans.scale = 1.0f;

    mtxf_from_transform(&mtx, &trans);
    mtxf_transform_point(mtx, x, y, z, &x, &y, &z);

    // Store x/y/z as integers
    gScenePerspectivePos.x = x;
    gScenePerspectivePos.y = y;
    gScenePerspectivePos.z = z;
}
