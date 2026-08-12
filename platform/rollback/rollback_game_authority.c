#include "rollback_game_authority.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "camera_obstruction_runtime.h"
#include "audio_spatial.h"
#include "fade_transition.h"
#include "joypad.h"
#include "memory.h"
#include "objects.h"
#include "presentation_snapshot.h"
#include "racer.h"
#include "rollback_authority_view.h"
#include "rollback_engine_registry.h"
#include "thread3_main.h"
#include "textures_sprites.h"
#include "waves.h"

extern s32 gCurrentRNGSeed;
extern s32 gPrevRNGSeed;
extern s32 gObjectCount;
extern s32 gObjectListStart;
extern s32 gParticleCount;
extern s32 gCollisionObjectCount;
extern s32 gFreeListCount;
extern s32 gCameraObjCount;
extern s32 gNumRacers;
extern s32 gEventCountdown;
extern s32 gEventStartTimer;
extern s32 gRaceFinishTriggered;
extern s16 gRaceEndTimer;
extern s8 gRaceEndStage;
extern s32 gNumFinishedRacers;
extern s8 gFirstTimeFinish;
extern u32 gBalloonCutsceneTimer;
extern s8 gNumRacersSaved;
extern s32 gPathUpdateOff;
extern s32 gNumberOfMainCheckpoints;
extern s32 gNumberOfTotalCheckpoints;
extern s32 gAINodeTail[2];
extern s32 gInitAINodes;
extern u8 gTimeTrialEnabled;
extern u8 gIsTimeTrial;
extern s8 gIsTajChallenge;
extern s8 gTajRaceInit;
extern s8 gChallengePrevMusic;
extern u8 *gObjectMapSpawnList[2];
extern s32 gObjectMapSize[2];
extern s32 gObjectMapID[2];
extern s32 *gObjectMap[2];
extern s32 gObjectMapIndex;
extern s32 gGameMode;
extern s32 gPlayableMapId;
extern s32 gSaveDataFlags;
extern s16 gLevelLoadTimer;
extern s8 gIsPaused;
extern s8 gPostRaceViewPort;
extern s32 D_801234F8;
extern s32 D_801234FC;
extern s8 gLevelSettings[16];
extern s32 gRaceStartTimer;
extern Camera gCameras[8];
extern s32 gTTCamPlayerID;
extern s32 gTTCamID;
extern s32 gTTCamSmoothTimer;
extern s32 gTTCamSpectateIndex[10];
extern s32 gNumTriangleParticles;
extern s32 gTriangleParticleBufferFull;
extern s32 gNumRectangleParticles;
extern s32 gRectangleParticleBufferFull;
extern s32 gNumSpriteParticles;
extern s32 gSpriteParticleBufferFull;
extern s32 gNumLineParticles;
extern s32 gLineParticleBufferFull;
extern s32 gNumPointParticles;
extern s32 gPointParticleBufferFull;
extern Object **gObjPtrList;
extern Vec2s *gWaveHeightIndices;
extern LevelModel_Alternate *gWaveModel;
extern WaveControl gWaveController;
extern s32 gWaveGenCount;
extern f32 gWavePowerBase;
extern f32 gWaveMagnitude;
extern s32 gWavePowerDivisor;
extern u32 gWaveBlockCount;

enum MdkrGameAuthorityTag {
    TAG_ALLOC_OBJECT_LIST = MDKR_ROLLBACK_TAG_OBJECT_LIST,
    TAG_ALLOC_PARTICLE_LIST,
    TAG_ALLOC_COLLISION_OBJECTS,
    TAG_ALLOC_ANIMATION_OBJECTS,
    TAG_ALLOC_TRACK_CHECKPOINTS,
    TAG_ALLOC_CAMERA_OBJECTS,
    TAG_ALLOC_RACERS,
    TAG_ALLOC_RACERS_BY_POSITION,
    TAG_ALLOC_RACERS_BY_PORT,
    TAG_ALLOC_AI_NODES,
    TAG_ALLOC_DRAWBRIDGE_TIMERS,
    TAG_ALLOC_BEHAVIOR_SCRATCH,
    TAG_ALLOC_LOADED_HEADERS,
    TAG_ALLOC_HEADER_REFERENCES,
    TAG_ALLOC_OBJECT_MAP_0,
    TAG_ALLOC_OBJECT_MAP_1,
    TAG_ALLOC_PARTICLE_TRIANGLE,
    TAG_ALLOC_PARTICLE_RECTANGLE,
    TAG_ALLOC_PARTICLE_SPRITE,
    TAG_ALLOC_PARTICLE_LINE,
    TAG_ALLOC_PARTICLE_POINT,
    TAG_ALLOC_WAVE_HEIGHT_INDICES,
    TAG_ALLOC_WAVE_MODEL,
    TAG_ALLOC_SETTINGS,
    TAG_ALLOC_AUDIO_POINT_POOL,
    TAG_ALLOC_AUDIO_FREE_POINTS,
    TAG_ALLOC_AUDIO_ACTIVE_POINTS,
    TAG_RNG_CURRENT,
    TAG_RNG_PREVIOUS,
    TAG_OBJECT_COUNT,
    TAG_OBJECT_LIST_START,
    TAG_PARTICLE_COUNT,
    TAG_COLLISION_COUNT,
    TAG_FREE_LIST_COUNT,
    TAG_CAMERA_OBJECT_COUNT,
    TAG_RACER_COUNT,
    TAG_EVENT_COUNTDOWN,
    TAG_EVENT_START_TIMER,
    TAG_RACE_FINISH_TRIGGERED,
    TAG_RACE_END_TIMER,
    TAG_RACE_END_STAGE,
    TAG_FINISHED_RACER_COUNT,
    TAG_FIRST_TIME_FINISH,
    TAG_BALLOON_CUTSCENE_TIMER,
    TAG_SAVED_RACER_COUNT,
    TAG_PATH_UPDATE_OFF,
    TAG_MAIN_CHECKPOINT_COUNT,
    TAG_TOTAL_CHECKPOINT_COUNT,
    TAG_AI_NODE_TAIL,
    TAG_AI_NODE_INIT,
    TAG_TIME_TRIAL_ENABLED,
    TAG_IS_TIME_TRIAL,
    TAG_IS_TAJ_CHALLENGE,
    TAG_TAJ_RACE_INIT,
    TAG_CHALLENGE_PREV_MUSIC,
    TAG_OBJECT_MAP_SPAWN_LISTS,
    TAG_OBJECT_MAP_SIZES,
    TAG_OBJECT_MAP_IDS,
    TAG_OBJECT_MAP_POINTERS,
    TAG_OBJECT_MAP_INDEX,
    TAG_GAME_MODE,
    TAG_PLAYABLE_MAP,
    TAG_SAVE_FLAGS,
    TAG_LEVEL_LOAD_TIMER,
    TAG_PAUSED,
    TAG_POST_RACE_VIEWPORT,
    TAG_SCENE_LOAD_PENDING,
    TAG_SCENE_LOAD_MODE,
    TAG_LEVEL_SETTINGS,
    TAG_RACE_START_TIMER,
    TAG_CAMERAS,
    TAG_TT_CAMERA_PLAYER,
    TAG_TT_CAMERA_ID,
    TAG_TT_CAMERA_SMOOTH_TIMER,
    TAG_TT_CAMERA_SPECTATE,
    TAG_PARTICLE_TRIANGLE_COUNT,
    TAG_PARTICLE_TRIANGLE_FULL,
    TAG_PARTICLE_RECTANGLE_COUNT,
    TAG_PARTICLE_RECTANGLE_FULL,
    TAG_PARTICLE_SPRITE_COUNT,
    TAG_PARTICLE_SPRITE_FULL,
    TAG_PARTICLE_LINE_COUNT,
    TAG_PARTICLE_LINE_FULL,
    TAG_PARTICLE_POINT_COUNT,
    TAG_PARTICLE_POINT_FULL,
    TAG_WAVE_CONTROLLER,
    TAG_WAVE_GENERATOR_COUNT,
    TAG_WAVE_POWER_BASE,
    TAG_WAVE_MAGNITUDE,
    TAG_WAVE_POWER_DIVISOR,
    TAG_WAVE_BLOCK_COUNT,
    TAG_ALLOC_TRANSITION_WORKSPACE,
    TAG_AUDIO_SPATIAL_STATE_BASE,
    TAG_TRANSITION_STATE_BASE = TAG_AUDIO_SPATIAL_STATE_BASE +
                                MDKR_AUDIO_SPATIAL_ROLLBACK_STATE_SPAN_COUNT,
    TAG_INPUT_STATE_BASE = TAG_TRANSITION_STATE_BASE +
                           MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT,
};

#define TAG_ALLOC_OBJECT_AUX_BASE UINT32_C(0x4F410000)
#define TAG_ITEM_MODEL_REFERENCE_BASE UINT32_C(0x4D520000)
#define MDKR_ITEM_MODEL_REFERENCE_MAX 32

bool mdkr_rollback_game_authority_is_input_tag(uint32_t tag) {
    return tag >= TAG_INPUT_STATE_BASE &&
           tag < TAG_INPUT_STATE_BASE + MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT;
}

static bool add_range_flags(
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES], size_t *count,
    void *address, size_t size, uint32_t tag, uint32_t flags) {
    if (*count >= MDKR_ROLLBACK_MAX_RANGES) {
        return false;
    }
    ranges[*count] = (MdkrRollbackRangeSpec){address, size, tag, flags};
    (*count)++;
    return true;
}

static bool add_range(
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES], size_t *count,
    void *address, size_t size, uint32_t tag) {
    return add_range_flags(ranges, count, address, size, tag, 0u);
}

static bool add_allocation(
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES], size_t *count,
    const void *interior, uint32_t tag, bool required) {
    void *base = NULL;
    size_t size = 0u;
    if (interior == NULL) {
        return !required;
    }
    if (!mdkr_mempool_allocation_span_in_pool(
            POOL_MAIN, interior, &base, &size)) {
        return false;
    }
    return add_range_flags(
        ranges, count, base, size, tag,
        MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION);
}

static bool add_optional_main_allocation(
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES], size_t *count,
    const void *interior, uint32_t tag) {
    void *base = NULL;
    size_t size = 0u;
    size_t index;
    if (interior == NULL ||
        !mdkr_mempool_allocation_span_in_pool(
            POOL_MAIN, interior, &base, &size)) {
        return true;
    }
    for (index = 0u; index < *count; index++) {
        const uintptr_t begin = (uintptr_t)ranges[index].address;
        const uintptr_t end = begin + ranges[index].size;
        const uintptr_t candidate_begin = (uintptr_t)base;
        const uintptr_t candidate_end = candidate_begin + size;
        if (begin == candidate_begin && end == candidate_end) {
            return true;
        }
        if (begin < candidate_end && candidate_begin < end) {
            return false;
        }
    }
    return add_range_flags(
        ranges, count, base, size, tag,
        MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION);
}

bool mdkr_rollback_game_authority_validate_dynamic_coverage(
    const MdkrRollbackSnapshotRegistry *registry) {
    size_t object_index;
    if (registry == NULL || gObjPtrList == NULL || gObjectCount < 0) {
        return false;
    }
    for (object_index = 0u; object_index < (size_t)gObjectCount;
         object_index++) {
        const Object *object = gObjPtrList[object_index];
        void *base = NULL;
        size_t size = 0u;
        unsigned range;
        bool covered = false;
        /* Particles share gObjPtrList but not Object's union layout. Their five
         * fixed mutable buffers are registered explicitly above; reading the
         * overlapping anyBehaviorData field would mistake sprite/model state
         * for a separately owned behaviour allocation. */
        if (object == NULL ||
            (object->trans.flags & OBJ_FLAGS_PARTICLE) != 0 ||
            object->anyBehaviorData == NULL ||
            !mdkr_mempool_allocation_span_in_pool(
                POOL_MAIN, object->anyBehaviorData, &base, &size)) {
            continue;
        }
        for (range = 0u; range < registry->range_count; range++) {
            if (registry->ranges[range].address == base &&
                registry->ranges[range].size == size &&
                (registry->ranges[range].flags &
                 MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION) != 0u) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            fprintf(stderr,
                    "[ROLLBACK] uncovered behaviour allocation object=%zu "
                    "behavior=%d base=%p size=%zu\n",
                    object_index, object->behaviorId, base, size);
            return false;
        }
    }
    return true;
}

static void rebuild_presentation_after_restore(void *context) {
    (void)context;
    presentation_snapshot_stage_reset();
    camera_obstruction_runtime_reset();
}

#define ADD_SCALAR(ranges, count, value, tag) \
    add_range((ranges), (count), &(value), sizeof(value), (tag))

bool mdkr_rollback_game_authority_register(
    MdkrRollbackSnapshotRegistry *registry) {
    MdkrRollbackRangeSpec ranges[MDKR_ROLLBACK_MAX_RANGES];
    MdkrMemorySpan pool_spans[MDKR_MEMPOOL_STATE_SPAN_COUNT];
    MdkrMemorySpan model_pool_spans[MDKR_MEMPOOL_STATE_SPAN_COUNT];
    MdkrObjectRollbackView objects;
    MdkrTransitionRollbackSpan
        transition_spans[MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT];
    MdkrInputRollbackSpan
        input_spans[MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT];
    MdkrAudioSpatialRollbackView audio_spatial;
    Settings *settings;
    const void *allocations[21];
    s16 *model_references[MDKR_ITEM_MODEL_REFERENCE_MAX];
    s32 model_reference_count;
    size_t count = 0u;
    size_t index;
    void *transition_workspace;
    const uint8_t original_range_count = registry != NULL ? registry->range_count : 0u;
    const size_t original_total = registry != NULL ? registry->total_bytes : 0u;

    if (registry == NULL || registry->frozen ||
        registry->rebuild_count >= MDKR_ROLLBACK_MAX_REBUILD_HOOKS ||
        !mdkr_mempool_pool_state_spans(POOL_OBJECT, pool_spans) ||
        !mdkr_mempool_pool_state_spans(POOL_UNUSED_2, model_pool_spans) ||
        !mdkr_object_rollback_view(&objects) ||
        !audspat_rollback_view(&audio_spatial)) {
        return false;
    }
    model_reference_count = mdkr_object_assets_rollback_references(
        model_references, MDKR_ITEM_MODEL_REFERENCE_MAX);
    if (model_reference_count < 0) return false;
    settings = get_settings();
    transition_workspace = transition_workspace_address();
    if (settings == NULL ||
        transition_workspace == NULL ||
        !transition_rollback_state_spans(transition_spans) ||
        !input_rollback_state_spans(input_spans) ||
        !add_range(ranges, &count, pool_spans[0].base, pool_spans[0].size,
                   MDKR_ROLLBACK_TAG_OBJECT_POOL_DESCRIPTOR) ||
        !add_range_flags(
            ranges, &count, pool_spans[1].base, pool_spans[1].size,
            MDKR_ROLLBACK_TAG_OBJECT_POOL_BACKING,
            MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION) ||
        !add_range(ranges, &count, model_pool_spans[0].base,
                   model_pool_spans[0].size,
                   MDKR_ROLLBACK_TAG_MODEL_POOL_DESCRIPTOR) ||
        !add_range_flags(
            ranges, &count, model_pool_spans[1].base,
            model_pool_spans[1].size,
            MDKR_ROLLBACK_TAG_MODEL_POOL_BACKING,
            MDKR_ROLLBACK_RANGE_LIVE_ALLOCATION)) {
        return false;
    }

    allocations[0] = objects.object_list;
    allocations[1] = objects.particle_list;
    allocations[2] = objects.collision_objects;
    allocations[3] = objects.animation_objects;
    allocations[4] = objects.track_checkpoints;
    allocations[5] = objects.camera_objects;
    allocations[6] = objects.racers;
    allocations[7] = objects.racers_by_position;
    allocations[8] = objects.racers_by_port;
    allocations[9] = objects.ai_nodes;
    allocations[10] = objects.drawbridge_timers;
    allocations[11] = objects.behavior_scratch;
    allocations[12] = objects.loaded_object_headers;
    allocations[13] = objects.object_header_references;
    allocations[14] = objects.object_maps[0];
    allocations[15] = objects.object_maps[1];
    allocations[16] = objects.particle_buffers[0];
    allocations[17] = objects.particle_buffers[1];
    allocations[18] = objects.particle_buffers[2];
    allocations[19] = objects.particle_buffers[3];
    allocations[20] = objects.particle_buffers[4];
    for (index = 0u; index < 21u; index++) {
        const bool required = index < 14u || index >= 16u;
        if (!add_allocation(ranges, &count, allocations[index],
                            TAG_ALLOC_OBJECT_LIST + (uint32_t)index,
                            required)) {
            return false;
        }
    }
    if (!add_allocation(
            ranges, &count, gWaveHeightIndices,
            TAG_ALLOC_WAVE_HEIGHT_INDICES, false) ||
        !add_allocation(
            ranges, &count, gWaveModel, TAG_ALLOC_WAVE_MODEL, false) ||
        !add_allocation(
            ranges, &count, settings, TAG_ALLOC_SETTINGS, true) ||
        !add_allocation(
            ranges, &count, transition_workspace,
            TAG_ALLOC_TRANSITION_WORKSPACE, true) ||
        !add_allocation(
            ranges, &count, audio_spatial.allocations[0],
            TAG_ALLOC_AUDIO_POINT_POOL, true) ||
        !add_allocation(
            ranges, &count, audio_spatial.allocations[1],
            TAG_ALLOC_AUDIO_FREE_POINTS, true) ||
        !add_allocation(
            ranges, &count, audio_spatial.allocations[2],
            TAG_ALLOC_AUDIO_ACTIVE_POINTS, true)) {
        return false;
    }

    /* Some behaviours own mutable auxiliary state in POOL_MAIN rather than
     * inside the object-pool allocation. A notable example is Object_Log,
     * whose phase drives buoy/log height. Register each unique behaviour
     * allocation in stable object-list order; embedded/static pointers are
     * intentionally ignored because their owning storage is handled elsewhere. */
    for (index = 0u; index < (size_t)gObjectCount; index++) {
        if (gObjPtrList[index] != NULL &&
            (gObjPtrList[index]->trans.flags & OBJ_FLAGS_PARTICLE) == 0 &&
            !add_optional_main_allocation(
                ranges, &count, gObjPtrList[index]->anyBehaviorData,
                TAG_ALLOC_OBJECT_AUX_BASE + (uint32_t)index)) {
            return false;
        }
    }
    /* Dynamic item instances increment shared model reference counts. Restore
     * those tiny lifetime fields with the object pool so discarded replays do
     * not accumulate invisible references and leak level assets. */
    for (index = 0u; index < (size_t)model_reference_count; index++) {
        if (!add_range(
                ranges, &count, model_references[index],
                sizeof(*model_references[index]),
                TAG_ITEM_MODEL_REFERENCE_BASE + (uint32_t)index)) {
            return false;
        }
    }

    if (!ADD_SCALAR(ranges, &count, gCurrentRNGSeed, TAG_RNG_CURRENT) ||
        !ADD_SCALAR(ranges, &count, gPrevRNGSeed, TAG_RNG_PREVIOUS) ||
        !ADD_SCALAR(ranges, &count, gObjectCount, TAG_OBJECT_COUNT) ||
        !ADD_SCALAR(ranges, &count, gObjectListStart, TAG_OBJECT_LIST_START) ||
        !ADD_SCALAR(ranges, &count, gParticleCount, TAG_PARTICLE_COUNT) ||
        !ADD_SCALAR(ranges, &count, gCollisionObjectCount, TAG_COLLISION_COUNT) ||
        !ADD_SCALAR(ranges, &count, gFreeListCount, TAG_FREE_LIST_COUNT) ||
        !ADD_SCALAR(ranges, &count, gCameraObjCount, TAG_CAMERA_OBJECT_COUNT) ||
        !ADD_SCALAR(ranges, &count, gNumRacers, TAG_RACER_COUNT) ||
        !ADD_SCALAR(ranges, &count, gEventCountdown, TAG_EVENT_COUNTDOWN) ||
        !ADD_SCALAR(ranges, &count, gEventStartTimer, TAG_EVENT_START_TIMER) ||
        !ADD_SCALAR(ranges, &count, gRaceFinishTriggered, TAG_RACE_FINISH_TRIGGERED) ||
        !ADD_SCALAR(ranges, &count, gRaceEndTimer, TAG_RACE_END_TIMER) ||
        !ADD_SCALAR(ranges, &count, gRaceEndStage, TAG_RACE_END_STAGE) ||
        !ADD_SCALAR(ranges, &count, gNumFinishedRacers, TAG_FINISHED_RACER_COUNT) ||
        !ADD_SCALAR(ranges, &count, gFirstTimeFinish, TAG_FIRST_TIME_FINISH) ||
        !ADD_SCALAR(ranges, &count, gBalloonCutsceneTimer, TAG_BALLOON_CUTSCENE_TIMER) ||
        !ADD_SCALAR(ranges, &count, gNumRacersSaved, TAG_SAVED_RACER_COUNT) ||
        !ADD_SCALAR(ranges, &count, gPathUpdateOff, TAG_PATH_UPDATE_OFF) ||
        !ADD_SCALAR(ranges, &count, gNumberOfMainCheckpoints, TAG_MAIN_CHECKPOINT_COUNT) ||
        !ADD_SCALAR(ranges, &count, gNumberOfTotalCheckpoints, TAG_TOTAL_CHECKPOINT_COUNT) ||
        !ADD_SCALAR(ranges, &count, gAINodeTail, TAG_AI_NODE_TAIL) ||
        !ADD_SCALAR(ranges, &count, gInitAINodes, TAG_AI_NODE_INIT) ||
        !ADD_SCALAR(ranges, &count, gTimeTrialEnabled, TAG_TIME_TRIAL_ENABLED) ||
        !ADD_SCALAR(ranges, &count, gIsTimeTrial, TAG_IS_TIME_TRIAL) ||
        !ADD_SCALAR(ranges, &count, gIsTajChallenge, TAG_IS_TAJ_CHALLENGE) ||
        !ADD_SCALAR(ranges, &count, gTajRaceInit, TAG_TAJ_RACE_INIT) ||
        !ADD_SCALAR(ranges, &count, gChallengePrevMusic, TAG_CHALLENGE_PREV_MUSIC) ||
        !ADD_SCALAR(ranges, &count, gObjectMapSpawnList, TAG_OBJECT_MAP_SPAWN_LISTS) ||
        !ADD_SCALAR(ranges, &count, gObjectMapSize, TAG_OBJECT_MAP_SIZES) ||
        !ADD_SCALAR(ranges, &count, gObjectMapID, TAG_OBJECT_MAP_IDS) ||
        !ADD_SCALAR(ranges, &count, gObjectMap, TAG_OBJECT_MAP_POINTERS) ||
        !ADD_SCALAR(ranges, &count, gObjectMapIndex, TAG_OBJECT_MAP_INDEX) ||
        !ADD_SCALAR(ranges, &count, gGameMode, TAG_GAME_MODE) ||
        !ADD_SCALAR(ranges, &count, gPlayableMapId, TAG_PLAYABLE_MAP) ||
        !ADD_SCALAR(ranges, &count, gSaveDataFlags, TAG_SAVE_FLAGS) ||
        !ADD_SCALAR(ranges, &count, gLevelLoadTimer, TAG_LEVEL_LOAD_TIMER) ||
        !ADD_SCALAR(ranges, &count, gIsPaused, TAG_PAUSED) ||
        !ADD_SCALAR(ranges, &count, gPostRaceViewPort, TAG_POST_RACE_VIEWPORT) ||
        !ADD_SCALAR(ranges, &count, D_801234F8, TAG_SCENE_LOAD_PENDING) ||
        !ADD_SCALAR(ranges, &count, D_801234FC, TAG_SCENE_LOAD_MODE) ||
        !ADD_SCALAR(ranges, &count, gLevelSettings, TAG_LEVEL_SETTINGS) ||
        !ADD_SCALAR(ranges, &count, gRaceStartTimer, TAG_RACE_START_TIMER) ||
        !ADD_SCALAR(ranges, &count, gCameras, TAG_CAMERAS) ||
        !ADD_SCALAR(ranges, &count, gTTCamPlayerID, TAG_TT_CAMERA_PLAYER) ||
        !ADD_SCALAR(ranges, &count, gTTCamID, TAG_TT_CAMERA_ID) ||
        !ADD_SCALAR(ranges, &count, gTTCamSmoothTimer, TAG_TT_CAMERA_SMOOTH_TIMER) ||
        !ADD_SCALAR(ranges, &count, gTTCamSpectateIndex, TAG_TT_CAMERA_SPECTATE) ||
        !ADD_SCALAR(ranges, &count, gNumTriangleParticles, TAG_PARTICLE_TRIANGLE_COUNT) ||
        !ADD_SCALAR(ranges, &count, gTriangleParticleBufferFull, TAG_PARTICLE_TRIANGLE_FULL) ||
        !ADD_SCALAR(ranges, &count, gNumRectangleParticles, TAG_PARTICLE_RECTANGLE_COUNT) ||
        !ADD_SCALAR(ranges, &count, gRectangleParticleBufferFull, TAG_PARTICLE_RECTANGLE_FULL) ||
        !ADD_SCALAR(ranges, &count, gNumSpriteParticles, TAG_PARTICLE_SPRITE_COUNT) ||
        !ADD_SCALAR(ranges, &count, gSpriteParticleBufferFull, TAG_PARTICLE_SPRITE_FULL) ||
        !ADD_SCALAR(ranges, &count, gNumLineParticles, TAG_PARTICLE_LINE_COUNT) ||
        !ADD_SCALAR(ranges, &count, gLineParticleBufferFull, TAG_PARTICLE_LINE_FULL) ||
        !ADD_SCALAR(ranges, &count, gNumPointParticles, TAG_PARTICLE_POINT_COUNT) ||
        !ADD_SCALAR(ranges, &count, gPointParticleBufferFull, TAG_PARTICLE_POINT_FULL) ||
        !ADD_SCALAR(ranges, &count, gWaveController, TAG_WAVE_CONTROLLER) ||
        !ADD_SCALAR(ranges, &count, gWaveGenCount, TAG_WAVE_GENERATOR_COUNT) ||
        !ADD_SCALAR(ranges, &count, gWavePowerBase, TAG_WAVE_POWER_BASE) ||
        !ADD_SCALAR(ranges, &count, gWaveMagnitude, TAG_WAVE_MAGNITUDE) ||
        !ADD_SCALAR(ranges, &count, gWavePowerDivisor, TAG_WAVE_POWER_DIVISOR) ||
        !ADD_SCALAR(ranges, &count, gWaveBlockCount, TAG_WAVE_BLOCK_COUNT)) {
        return false;
    }
    for (index = 0u;
         index < MDKR_AUDIO_SPATIAL_ROLLBACK_STATE_SPAN_COUNT; index++) {
        if (!add_range(
                ranges, &count, audio_spatial.state[index].address,
                audio_spatial.state[index].size,
                TAG_AUDIO_SPATIAL_STATE_BASE + (uint32_t)index)) {
            return false;
        }
    }
    for (index = 0u;
         index < MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT; index++) {
        if (!add_range(
                ranges, &count, transition_spans[index].address,
                transition_spans[index].size,
                TAG_TRANSITION_STATE_BASE + (uint32_t)index)) {
            return false;
        }
    }
    for (index = 0u; index < MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT;
         index++) {
        if (!add_range(
                ranges, &count, input_spans[index].address,
                input_spans[index].size,
                TAG_INPUT_STATE_BASE + (uint32_t)index)) {
            return false;
        }
    }

    if (!mdkr_rollback_snapshot_register_batch(registry, ranges, count)) {
        return false;
    }
    if (!mdkr_rollback_snapshot_register_rebuild(
            registry, rebuild_presentation_after_restore, NULL)) {
        memset(&registry->ranges[original_range_count], 0,
               (registry->range_count - original_range_count) *
                   sizeof(registry->ranges[0]));
        registry->range_count = original_range_count;
        registry->total_bytes = original_total;
        return false;
    }
    return true;
}

#undef ADD_SCALAR
