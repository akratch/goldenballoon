#ifndef _TRACKS_H_
#define _TRACKS_H_

#include "structs.h"
#include "f3ddkr.h"
#ifdef NATIVE_PORT
#include "camera_obstruction.h"
#endif

#ifdef NATIVE_PORT
#define LOCAL_OFFSET_TO_RAM_ADDRESS(type, ptr) \
    (ptr) = DKR_TOK((u8 *)(mdl) + (u32)(ptr))
#else
#define LOCAL_OFFSET_TO_RAM_ADDRESS(type, ptr) \
    (ptr) = (type)((s32)((u8 *)(ptr)) + (s32)((u8 *)(mdl)))
#endif

enum ShadowUpdate { SHADOW_NONE, SHADOW_SCENERY, SHADOW_ACTORS };

/* Size: 0x10 bytes */
typedef struct unk8011C8B8 {
    /* 0x00 */ f32 x;
    /* 0x04 */ f32 y;
    /* 0x08 */ f32 z;
    union {
        /* 0x0C */ f32 w;
        struct {
            /* 0x0C */ s16 unkC;
            /* 0x0E */ s16 unkE;
        } s;
    } unkC_union;
} unk8011C8B8;

/* Size: 0x10 bytes */
typedef struct unk8011B120 {
    /* 0x00 */ f32 x;
    /* 0x04 */ f32 y;
    /* 0x08 */ f32 z;
    /* 0x0C */ unk8011C8B8 *unkC;
} unk8011B120;

/* Size: 0x20 bytes */
typedef struct unk8011B330 {
    /* 0x00 */ f32 x;
    /* 0x04 */ f32 y;
    /* 0x08 */ f32 z;
    /* 0x0C */ unk8011C8B8 *unkC;
    /* 0x10 */ f32 unk10;
    /* 0x14 */ f32 unk14;
    /* 0x18 */ f32 unk18;
    /* 0x1C */ f32 unk1C;
} unk8011B330;

/* Size: 0x10 bytes */
typedef struct VoidMesh {
    Triangle *tris[2];
    Vertex *verts[2];
} VoidMesh;

typedef struct unk8011C238 {
    u8 unk0;
    u8 unk1;
    u8 unk2[8];
    s16 unkA;
} unk8011C238;

/* Size: 0x8 bytes */
typedef struct unk8011D478 {
    s16 unk0;
    s16 unk2;
    s16 unk4;
    s8 unk6;
    s8 unk7;
} unk8011D478;

/* Size: 0x14 bytes */
typedef struct unk8011C3B8 {
    /* 0x00 */ s16 x1;
    /* 0x02 */ s16 y1;
    /* 0x04 */ s16 z1;
    /* 0x06 */ s16 x2;
    /* 0x08 */ s16 y2;
    /* 0x0A */ s16 z2;
    /* 0x0C */ s16 x3;
    /* 0x0E */ s16 y3;
    /* 0x10 */ s16 z3;
    /* 0x12 */ s16 unk12;
} unk8011C3B8;

// In this struct, data is rightshifted 16 bytes, so make the smooth transition more precise.
typedef struct EnvironmentFog {
    s32 r;
    s32 g;
    s32 b;
    s32 near;
    s32 far;
} EnvironmentFog;

typedef struct EnvironmentFogCompact {
    u8 r;
    u8 g;
    u8 b;
    s16 near;
    s16 far;
} EnvironmentFogCompact;

/* Size: 0x38 bytes */
typedef struct FogData {
    EnvironmentFog fog;    // Current fog properties. What you'll actually see ingame.
    EnvironmentFog addFog; // Fog override. This will apply over the current fog to give a smooth transition effect.
    EnvironmentFogCompact intendedFog; // Fog properties the game will try to be if the switch timer is 0.
    s32 switchTimer;
    Object *fogChanger;
} FogData;

/* Size: 0x14 bytes */
typedef struct WaterProperties {
    /* 0x00 */ f32 waveHeight;
    /* 0x04 */ Vec3f rot;
    /* 0x10 */ s8 type;
} WaterProperties;

s32 set_scene_viewport_num(s32 numPorts);
void void_free(void);
void skydome_spawn(s32 objectID);
void set_skydome_visbility(s32 renderSky);
void skydome_render(void);
void set_anti_aliasing(s32 setting);
void add_segment_to_order(s32 segmentIndex, s32 *segmentsOrderIndex, u8 *segmentsOrder);
#ifdef NATIVE_PORT
/* NATIVE_PORT: all three of these take an added `maxOut` -- the capacity of the
 * caller's array. Each writes an a-priori unknown number of elements through a
 * bare pointer that the ROM's signature leaves unbounded; see the block comments
 * at the definitions in tracks.c, docs/OPEN_ITEMS.md, and
 * tools/sweep_bug_shapes.py, which enumerates the whole class. Passing
 * ARRAY_COUNT() at every call site is what makes the bound audit-able. */
s32 get_inside_segment_count_xz(s32 x, s32 z, s32 *arg2, s32 maxOut);
s32 get_inside_segment_count_xyz(s32 *arg0, s16 xPos1, s16 yPos1, s16 zPos1, s16 xPos2, s16 yPos2, s16 zPos2,
                                 s32 maxOut);
#else
s32 get_inside_segment_count_xz(s32 x, s32 z, s32 *arg2);
s32 get_inside_segment_count_xyz(s32 *arg0, s16 xPos1, s16 yPos1, s16 zPos1, s16 xPos2, s16 yPos2, s16 zPos2);
#endif
LevelModelSegment *block_get(s32 segmentID);
LevelModelSegmentBoundingBox *block_boundbox(s32 segmentID);
void set_collision_mode(s32 mode);
s32 get_collision_normal(f32 *outX, f32 *outY, f32 *outZ);
s32 func_8002B9BC(Object *obj, f32 *arg1, Vec3f *arg2, s32 arg3);
void func_8002C71C(LevelModelSegment *segment);
LevelModel *get_current_level_model(void);
void get_fog_settings(s32 playerID, s16 *near, s16 *far, u8 *r, u8 *g, u8 *b);
void reset_fog(s32 playerID);
void update_fog(s32 viewportCount, s32 updateRate);
void apply_fog(s32 playerID);
void compute_scene_camera_transform_matrix(void);
void track_tex_anim(s32 updateRate);
s32 check_if_inside_segment(Object *obj, s32 segmentIndex);
s32 get_level_segment_index_from_position(f32 xPos, f32 yPos, f32 zPos);
void traverse_segments_bsp_tree(s32 nodeIndex, s32 segmentIndex, s32 segmentIndex2, u8 *segmentsOrder,
                                s32 *segmentsOrderIndex);
void render_level_geometry_and_objects(void);
#ifdef NATIVE_PORT
/* Static level geometry only. Dynamic object meshes join this query in CAM-05.
 * The cache owns copied, immutable data and never aliases gameplay collision
 * candidates or the level-model heap. */
#define MDKR_CAMERA_TRACK_OCCLUSION_HARD_MASK 0x00000001u

typedef struct MdkrTrackOcclusionTelemetry {
    size_t segment_count;
    size_t broadphase_chunk_count;
    size_t vertex_count;
    size_t hard_triangle_count;
    size_t visual_no_collision_hard_triangle_count;
    size_t nonblocking_triangle_count;
    size_t rejected_triangle_count;
    size_t unknown_policy_triangle_count;
    size_t malformed_batch_count;
    size_t bytes;
    uint64_t build_ns;
    uint64_t exact_sweep_count;
    uint64_t exact_segment_candidate_count;
    uint64_t exact_triangle_candidate_count;
    uint64_t exact_analytic_sat_count;
    uint64_t exact_analytic_revalidation_miss_count;
    uint64_t exact_bounded_interval_test_count;
    uint64_t exact_bounded_interval_exhaustion_count;
    uint64_t exact_stationary_test_count;
    uint64_t exact_advance_iteration_count;
    uint64_t exact_refinement_test_count;
    uint64_t exact_interval_fallback_count;
    uint64_t exact_interval_sample_count;
    uint64_t exact_ambiguous_interval_count;
    uint64_t exact_publication_revalidation_count;
    uint64_t exact_invalid_sweep_count;
    uint64_t exact_max_triangle_candidates_per_sweep;
    uint64_t exact_max_stationary_tests_per_sweep;
} MdkrTrackOcclusionTelemetry;

/* Read-only no-truncation query over the loaded track's hard visual shell.
 * Callers must treat INVALID as fail-closed and retain/recover a safe pose. */
MdkrCameraSweepStatus mdkr_track_occlusion_sweep(
    const MdkrCameraSweepInput *input,
    MdkrCameraSweepHit *out_hit);
/* Exact fixed-basis rounded-lens counterpart. It is intentionally separate
 * from the sphere source until runtime policy explicitly opts into CAM-02. */
MdkrCameraSweepStatus mdkr_track_occlusion_rounded_lens_sweep(
    const MdkrCameraRoundedLensSweepInput *input,
    MdkrCameraSweepHit *out_hit);
void mdkr_track_occlusion_get_telemetry(MdkrTrackOcclusionTelemetry *out);

/* Build the view basis of the LAST viewport of the frame (the one whose sort
 * survived into the next tick) without drawing, and render's own private
 * per-viewport draw order. See tracks.c. */
void scene_build_last_viewport_basis(void);
/* Advance the three-player spectator camera once per fixed tick. Rendering
 * consumes camera 3 but never advances it. */
void scene_tt_camera_tick(s32 updateRate);
s32 scene_build_private_draw_order(s32 startIndex, s32 lastIndex);
f32 scene_object_view_distance(Object *obj);
s32 scene_visibility_viewport_count(void);
void scene_visibility_prepare_viewport(s32 viewportIndex, s32 numViewports, s32 ttCam);
s32 scene_object_admitted(Object *obj);
void scene_authoritative_render_tick(s32 updateRate);
s32 scene_object_render_opacity(const Object *obj);
extern f32 gSceneDrawDistance;
extern s32 gSceneDrawDistanceValid;
#endif
void watereffect_render(Object *obj, WaterEffect *effect);
void shadow_render(Object *obj, ShadowData *shadow);
s32 block_visible(LevelModelSegmentBoundingBox *bb);
s32 check_if_in_draw_range(Object *obj);
void func_8002C954(LevelModelSegment *segment, LevelModelSegmentBoundingBox *bbox, s32 arg2);
void trackbg_render_gradient(void);
void shadow_update(s32 group, s32 waterGroup, s32 updateRate);
void obj_loop_fogchanger(Object *obj);
void initialise_player_viewport_vars(s32 updateRate);
s32 get_wave_properties(f32 yPos, f32 *waterHeight, Vec3f *rotation);
void render_level_segment(s32 segmentId, s32 nonOpaque);
void render_scene(Gfx **dList, Mtx **mtx, Vertex **vtx, Triangle **tris, s32 updateRate);
void set_fog(s32 fogIdx, s16 near, s16 far, u8 red, u8 green, u8 blue);
#ifdef NATIVE_PORT
void set_fog_colour(s32 fogIdx, s16 near, s16 far, u8 red, u8 green, u8 blue);
void fog_tick(s32 updateRate);
void scene_weather_tick(s32 updateRate);
void scene_presentation_tick(s32 updateRate);
#endif
void slowly_change_fog(s32 fogIdx, s32 red, s32 green, s32 blue, s32 near, s32 far, s32 switchTimer);
s32 func_8002FD74(f32 x0, f32 z0, f32 x1, f32 x2, s32 count, Vec4f *arg5);
void func_80026C14(s16 arg0, s16 arg1, s32 arg2);
void func_80026E54(s16 arg0, s8 *arg1, f32 arg2, f32 arg3);
void func_80026070(LevelModelSegmentBoundingBox *arg0, f32 arg1, f32 arg2, f32 arg3);
void func_80026430(LevelModelSegment *segment, f32 arg1, f32 arg2, f32 arg3);

void free_track(void);
void void_check(u8 *segmentIds, s32 numberOfSegments, s32 viewportIndex);
s32 func_80027568(void);
s32 track_init_collision(LevelModelSegment *);
s32 get_level_segment_waves(s32, f32 xIn, f32 zIn,
                  WaterProperties ***); // Definitely not triple pointer, but easiest way to fix warns.
#ifdef NATIVE_PORT
/* get_level_segment_waves() has no private output --
 * it publishes into the single global cache (D_8011D128 storage, gTrackWaves
 * ordering, D_8011D308 count) that get_wave_properties() reads WITHOUT
 * refreshing. Any caller that queries a position other than the one the
 * gameplay consumers care about must bracket its query with these. */
typedef struct WaveQueryCache {
    WaterProperties props[20];
    WaterProperties *order[20];
    s32 count;
} WaveQueryCache;
void wave_query_cache_save(WaveQueryCache *cache);
void wave_query_cache_restore(const WaveQueryCache *cache);
#endif
void ttcam_update(s32);
void trackbg_render_flashy(void);
void initialise_player_viewport_vars(s32);
void func_8002A31C(void);
void update_colour_cycle(LevelHeader_70 *arg0, s32 updateRate);
void waves_update(s32);
void waves_render(Gfx **, Mtx **, s32);
void func_8002DE30(Object *);
void shadow_generate(Object *, s32);
void func_8002E904(LevelModelSegment *, s32, s32 arg2);
void func_8002EEEC(s32 arg0);
void func_8002F2AC(void);
void func_8002F440(void);
#ifdef NATIVE_PORT
void mdkr_shadow_stats(s32 *dataPeak, s32 *triPeak, s32 *vtxPeak,
                       s32 *overflowDrops, s32 *emptyMeshes,
                       s32 *drawGroups, s32 *nonDecalDrawGroups,
                       s32 *dataCap, s32 *triCap, s32 *vtxCap);
#endif
f32 func_8002FA64(void);
#ifdef NATIVE_PORT
#define COLLISION_Y_QUERY_CAPACITY 16
s32 collision_get_y(s32 levelSegmentIndex, f32 xIn, f32 zIn, f32 *yOut, s32 maxOut);
#else
s32 collision_get_y(s32 levelSegmentIndex, f32 xIn, f32 zIn, f32 *yOut);
#endif
void init_track(u32 geometry, u32 skybox, s32 numberOfPlayers, Vehicle vehicle, u32 entranceId, u32 collectables,
                u32 arg6);
void waves_init(LevelModel *, LevelHeader *, s32);
void void_init(s32);
void generate_track(s32 modelId);
void func_800304C8(unk8011C8B8 arg0[3]);
s32 void_generate_primitive(f32 *arg0, f32 *arg1, f32 arg2, f32 arg3);
s32 func_8002FF6C(s32, unk8011C8B8 *, s32, Vec2f *);
#ifdef NATIVE_PORT
s32 func_800BDC80(s32, unk8011C3B8 *, unk8011C8B8 *, f32, f32, f32, f32, s32 maxOut);
#else
s32 func_800BDC80(s32, unk8011C3B8 *, unk8011C8B8 *, f32, f32, f32, f32);
#endif

#endif
