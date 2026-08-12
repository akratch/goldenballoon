/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "audio_spatial.h"
#include "fade_transition.h"
#include "joypad.h"
#include "memory.h"
#include "racer.h"
#include "rollback_authority_view.h"
#include "structs.h"
#include "thread3_main.h"
#include "waves.h"
#include "platform/rollback/rollback_game_authority.h"

static uint8_t sPoolDescriptor[16];
static uint8_t sPoolBacking[128];
static uint8_t sModelPoolDescriptor[16];
static uint8_t sModelPoolBacking[128];
static uint8_t sAllocations[28][32];
static uint8_t sTransitionWorkspace[256];
static uint64_t
    sTransitionState[MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT];
static uint64_t sInputState[MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT];
static uint64_t
    sAudioSpatialState[MDKR_AUDIO_SPATIAL_ROLLBACK_STATE_SPAN_COUNT];
static Settings sSettings;
static Object sObjects[3];
static Object *sObjectPointers[3] = {&sObjects[0], &sObjects[1], &sObjects[2]};
static int sViewOk = 1;
static int sFailAllocation = -1;
static int sPresentationResets;
static int sCameraResets;

s32 gCurrentRNGSeed = 7;
s32 gPrevRNGSeed = 6;
s32 gObjectCount = 3;
s32 gObjectListStart = 1;
s32 gParticleCount = 2;
s32 gCollisionObjectCount = 4;
s32 gFreeListCount = 5;
s32 gCameraObjCount = 6;
s32 gNumRacers = 8;
s32 gEventCountdown = 80;
s32 gEventStartTimer = 100;
s32 gRaceFinishTriggered = 0;
s16 gRaceEndTimer = 300;
s8 gRaceEndStage = 0;
s32 gNumFinishedRacers = 0;
s8 gFirstTimeFinish = 1;
u32 gBalloonCutsceneTimer = 9;
s8 gNumRacersSaved = 8;
s32 gPathUpdateOff = 0;
s32 gNumberOfMainCheckpoints = 20;
s32 gNumberOfTotalCheckpoints = 24;
s32 gAINodeTail[2] = {1, 2};
s32 gInitAINodes = 1;
u8 gTimeTrialEnabled = 0;
u8 gIsTimeTrial = 0;
s8 gIsTajChallenge = 0;
s8 gTajRaceInit = 0;
s8 gChallengePrevMusic = 3;
u8 *gObjectMapSpawnList[2];
s32 gObjectMapSize[2] = {10, 11};
s32 gObjectMapID[2] = {12, 13};
s32 *gObjectMap[2];
s32 gObjectMapIndex = 0;
s32 gGameMode = 1;
s32 gPlayableMapId = 2;
s32 gSaveDataFlags = 0;
s16 gLevelLoadTimer = 0;
s8 gIsPaused = 0;
s8 gPostRaceViewPort = 0;
s32 D_801234F8 = 0;
s32 D_801234FC = 0;
s8 gLevelSettings[16];
s32 gRaceStartTimer = 100;
Camera gCameras[8];
s32 gTTCamPlayerID = 0;
s32 gTTCamID = 0;
s32 gTTCamSmoothTimer = 0;
s32 gTTCamSpectateIndex[10];
s32 gNumTriangleParticles = 1;
s32 gTriangleParticleBufferFull = 0;
s32 gNumRectangleParticles = 2;
s32 gRectangleParticleBufferFull = 0;
s32 gNumSpriteParticles = 3;
s32 gSpriteParticleBufferFull = 0;
s32 gNumLineParticles = 4;
s32 gLineParticleBufferFull = 0;
s32 gNumPointParticles = 5;
s32 gPointParticleBufferFull = 0;
Object **gObjPtrList = sObjectPointers;
Vec2s *gWaveHeightIndices = (Vec2s *)sAllocations[21];
LevelModel_Alternate *gWaveModel =
    (LevelModel_Alternate *)sAllocations[22];
WaveControl gWaveController = {.magnitude = 1.0f};
s32 gWaveGenCount = 2;
f32 gWavePowerBase = 1.0f;
f32 gWaveMagnitude = 0.25f;
s32 gWavePowerDivisor = 10;
u32 gWaveBlockCount = 1u;

Settings *get_settings(void) {
    return &sSettings;
}

s32 mdkr_mempool_pool_state_spans(
    MemoryPools poolIndex,
    MdkrMemorySpan spans[MDKR_MEMPOOL_STATE_SPAN_COUNT]) {
    if (spans == NULL) return FALSE;
    if (poolIndex == POOL_OBJECT) {
        spans[0] = (MdkrMemorySpan){sPoolDescriptor, sizeof(sPoolDescriptor)};
        spans[1] = (MdkrMemorySpan){sPoolBacking, sizeof(sPoolBacking)};
    } else if (poolIndex == POOL_UNUSED_2) {
        spans[0] = (MdkrMemorySpan){sModelPoolDescriptor,
                                    sizeof(sModelPoolDescriptor)};
        spans[1] = (MdkrMemorySpan){sModelPoolBacking,
                                    sizeof(sModelPoolBacking)};
    } else {
        return FALSE;
    }
    return TRUE;
}

s32 mdkr_mempool_allocation_span(
    const void *address, void **allocationBase, size_t *allocationSize) {
    int index;
    if (allocationBase == NULL || allocationSize == NULL) return FALSE;
    *allocationBase = NULL;
    *allocationSize = 0u;
    if (address == &sSettings) {
        *allocationBase = &sSettings;
        *allocationSize = sizeof(sSettings);
        return TRUE;
    }
    if (address == sTransitionWorkspace) {
        *allocationBase = sTransitionWorkspace;
        *allocationSize = sizeof(sTransitionWorkspace);
        return TRUE;
    }
    for (index = 0; index < 28; index++) {
        if (address == sAllocations[index]) {
            if (index == sFailAllocation) return FALSE;
            *allocationBase = sAllocations[index];
            *allocationSize = sizeof(sAllocations[index]);
            return TRUE;
        }
    }
    return FALSE;
}

s32 audspat_rollback_view(MdkrAudioSpatialRollbackView *view) {
    unsigned index;
    if (view == NULL) return FALSE;
    memset(view, 0, sizeof(*view));
    view->allocations[0] = sAllocations[25];
    view->allocations[1] = sAllocations[26];
    view->allocations[2] = sAllocations[27];
    for (index = 0u;
         index < MDKR_AUDIO_SPATIAL_ROLLBACK_STATE_SPAN_COUNT; index++) {
        view->state[index] = (MdkrAudioSpatialRollbackSpan){
            &sAudioSpatialState[index], sizeof(sAudioSpatialState[index])};
    }
    return TRUE;
}

s32 mdkr_mempool_allocation_span_in_pool(
    MemoryPools poolIndex, const void *address, void **allocationBase,
    size_t *allocationSize) {
    if (poolIndex != POOL_MAIN) return FALSE;
    return mdkr_mempool_allocation_span(
        address, allocationBase, allocationSize);
}

void *transition_workspace_address(void) {
    return sTransitionWorkspace;
}

s32 transition_rollback_state_spans(
    MdkrTransitionRollbackSpan
        spans[MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT]) {
    unsigned index;
    if (spans == NULL) return FALSE;
    for (index = 0u;
         index < MDKR_TRANSITION_ROLLBACK_STATE_SPAN_COUNT; index++) {
        spans[index] = (MdkrTransitionRollbackSpan){
            &sTransitionState[index], sizeof(sTransitionState[index])};
    }
    return TRUE;
}

s32 input_rollback_state_spans(
    MdkrInputRollbackSpan spans[MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT]) {
    unsigned index;
    if (spans == NULL) return FALSE;
    for (index = 0u; index < MDKR_INPUT_ROLLBACK_STATE_SPAN_COUNT; index++) {
        spans[index] = (MdkrInputRollbackSpan){
            &sInputState[index], sizeof(sInputState[index])};
    }
    return TRUE;
}

s32 mdkr_object_rollback_view(MdkrObjectRollbackView *view) {
    const void **fields;
    int index;
    if (!sViewOk || view == NULL) return FALSE;
    memset(view, 0, sizeof(*view));
    fields = (const void **)view;
    for (index = 0; index < 21; index++) {
        fields[index] = sAllocations[index];
    }
    return TRUE;
}

s32 mdkr_object_assets_rollback_references(
    s16 **references, s32 capacity) {
    (void)references;
    (void)capacity;
    return 0;
}

void presentation_snapshot_stage_reset(void) {
    sPresentationResets++;
}

void camera_obstruction_runtime_reset(void) {
    sCameraResets++;
}

int main(void) {
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackSnapshotRegistry failed;
    size_t bytes;
    uint8_t *blob;
    unsigned index;

    for (index = 0u; index < 28u; index++) {
        memset(sAllocations[index], (int)(index + 1u),
               sizeof(sAllocations[index]));
    }
    gObjectMap[0] = (s32 *)sAllocations[14];
    gObjectMap[1] = (s32 *)sAllocations[15];
    gObjectMapSpawnList[0] = sAllocations[14] + 4;
    gObjectMapSpawnList[1] = sAllocations[15] + 4;
    sObjects[1].anyBehaviorData = sAllocations[23];
    gCameras[PLAYER_ONE].zoom = 55u;
    gCameras[PLAYER_FOUR].zoom = 40u;
    sSettings.courseId = 5;
    sTransitionWorkspace[0] = 0xA5u;
    sModelPoolDescriptor[0] = 0x3Cu;
    sModelPoolBacking[0] = 0x5Au;
    sTransitionState[0] = UINT64_C(0x1122334455667788);
    sInputState[0] = UINT64_C(0x8877665544332211);

    mdkr_rollback_snapshot_registry_init(&registry, UINT64_C(0x1234));
    assert(mdkr_rollback_game_authority_register(&registry));
    assert(registry.range_count == 145u);
    assert(registry.rebuild_count == 1u);
    assert(!mdkr_rollback_game_authority_register(&registry));
    assert(registry.range_count == 145u);
    assert(mdkr_rollback_game_authority_validate_dynamic_coverage(&registry));
    sObjects[1].anyBehaviorData = sAllocations[24];
    assert(!mdkr_rollback_game_authority_validate_dynamic_coverage(&registry));
    sObjects[1].anyBehaviorData = sAllocations[23];
    assert(mdkr_rollback_snapshot_freeze(&registry, UINT64_C(0x5678)));
    bytes = mdkr_rollback_snapshot_bytes(&registry);
    blob = (uint8_t *)malloc(bytes);
    assert(blob != NULL);
    assert(mdkr_rollback_snapshot_capture(&registry, 10u, blob, bytes));

    gCurrentRNGSeed = 99;
    gObjectCount = 77;
    gPostRaceViewPort = 1;
    D_801234F8 = 1;
    D_801234FC = 2;
    gNumSpriteParticles = 99;
    gWaveController.magnitude = 9.0f;
    gWavePowerDivisor = 0;
    gCameras[PLAYER_ONE].zoom = 99u;
    sAllocations[0][0] = 0u;
    sAllocations[23][0] = 0u;
    sSettings.courseId = 44;
    sTransitionWorkspace[0] = 0u;
    sModelPoolDescriptor[0] = 0u;
    sModelPoolBacking[0] = 0u;
    sTransitionState[0] = 0u;
    sInputState[0] = 0u;
    assert(mdkr_rollback_snapshot_restore(
        &registry, 10u, blob, bytes, true));
    assert(gCurrentRNGSeed == 7 && gObjectCount == 3 &&
           gNumSpriteParticles == 3);
    assert(gPostRaceViewPort == 0 && D_801234F8 == 0 && D_801234FC == 0);
    assert(gWaveController.magnitude == 1.0f && gWavePowerDivisor == 10);
    assert(gCameras[PLAYER_ONE].zoom == 55u);
    assert(sAllocations[0][0] == 1u && sSettings.courseId == 5);
    assert(sAllocations[23][0] == 24u);
    assert(sTransitionWorkspace[0] == 0xA5u);
    assert(sModelPoolDescriptor[0] == 0x3Cu &&
           sModelPoolBacking[0] == 0x5Au);
    assert(sTransitionState[0] == UINT64_C(0x1122334455667788));
    assert(sInputState[0] == UINT64_C(0x8877665544332211));
    assert(sPresentationResets == 1 && sCameraResets == 1);
    free(blob);

    mdkr_rollback_snapshot_registry_init(&failed, 1u);
    sFailAllocation = 7;
    assert(!mdkr_rollback_game_authority_register(&failed));
    assert(failed.range_count == 0u && failed.rebuild_count == 0u &&
           failed.total_bytes == 0u);
    sFailAllocation = -1;
    sViewOk = 0;
    assert(!mdkr_rollback_game_authority_register(&failed));
    assert(failed.range_count == 0u && failed.rebuild_count == 0u);
    puts("test_rollback_game_authority: PASS");
    return 0;
}
