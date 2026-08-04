#include "objects.h"
#include "memory.h"

#include "asset_enums.h"
#include "asset_loading.h"
#ifdef NATIVE_PORT
#include "mdkr_adventure.h"
#include "asset_swap.h"
#include "mdkr_challenge.h"
#include "mdkr_taj.h"
#include "taj_mod.h"
#include "mdkr_trace.h"
#include "taj_visual.h"
#include "presentation_snapshot.h"
#include "gameplay_event_trace.h"
#include "fast3d/gfx_level_lighting.h"
#endif
/* The level-object-map header is 16 bytes; gObjectMap[] is s32*, so the entries
 * begin 4 s32-elements in. The original code wrote this as sizeof(uintptr_t),
 * which is 4 on the N64 but 8 on LP64 — pinned to 4 to keep the 16-byte skip. */
#define OBJ_MAP_HEADER_S32S 4
#include "audio_spatial.h"
#include "audio_vehicle.h"
#include "audiosfx.h"
#include "camera.h"
#include "fade_transition.h"
#include "game.h"
#include "game_text.h"
#include "game_ui.h"
#include "gzip.h"
#include "joypad.h"
#include "level_object_entries.h"
#include "lights.h"
#include "macros.h"
#include "math_util.h"
#include "menu.h"
#include "object_functions.h"
#include "object_layout.h"
#include "object_models.h"
#include "particles.h"
#include "PR/os_cont.h"
#include "PR/os_convert.h"
#include "PR/os_system.h"
#include "PR/rcp.h"
#include "PRinternal/piint.h"
#include "PRinternal/viint.h"
#include "printf.h"
#include "racer.h"
#ifdef NATIVE_PORT
#include "taj_physics.h"
#endif
#include "runtime_contracts.h"
#include "save_data.h"
#include "structs.h"
#include "textures_sprites.h"
#include "thread0_epc.h"
#include "thread3_main.h"
#include "tracks.h"
#include "types.h"
#include "vehicle_misc.h"
#include "video.h"
#include "waves.h"
#include "weather.h"

#ifdef NATIVE_PORT
#include <limits.h> /* INT_MAX */
#include <stdio.h>  /* fprintf — loud corruption asserts below */
#include <stdlib.h> /* abort */
#include <string.h> /* strchr — native trace/test-hook parsing */

void mdkr_objcoll_hit(void);   /* platform/hasm_stubs_temp.c — [OBJCOLL] telemetry */
int  mdkr_objcoll_legacy(void); /*   ""      — MDKR_OBJCOLL=legacy A/B arm */

static void mdkr_taj_trace_phase(
    const char *phase, s32 vehicle, const s8 *thresholds,
    s32 reason, s32 menu, s32 tick);

extern Object *(*gRacers)[10];
extern s32 gNumRacers;

/*
 * End-of-present oracle state for every racer. Unlike the historical [PACE]
 * probe in racer.c, this is sampled after the complete game update, so its
 * position, progress and clock all describe the same instant as the retail-ROM
 * RDRAM sample taken by tools/prepare_ares_oracle.sh.
 */
void mdkr_oracle_trace_racers(s32 frame) {
    static s32 sEnabled = -1;
    extern s32 gCurrentRNGSeed;
    extern s32 sLogicUpdateRate;
    extern u8 gVideoDeltaTime;
    s32 i;

    if (sEnabled < 0) {
        const char *value = getenv("MDKR_ORACLE_STATE");
        sEnabled = value != NULL && value[0] != '\0' && atoi(value) != 0;
    }
    if (!sEnabled || gRacers == NULL || gNumRacers <= 0) {
        return;
    }
    /*
     * The live array has ten entries. Normal game setup already constrains
     * gNumRacers, but an observability-only hook must not turn a corrupted
     * count into a new out-of-bounds read before the real fault is reported.
     */
    for (i = 0; i < gNumRacers && i < 10; i++) {
        Object *obj = (*gRacers)[i];
        Object_Racer *racer;
        u32 clock = 0;
        s32 lapIndex;
        s32 lapCount;

        if (obj == NULL || obj->racer == NULL) {
            continue;
        }
        racer = obj->racer;
        lapCount = racer->countLap < 0
            ? 0
            : (racer->countLap < 5 ? racer->countLap + 1 : 5);
        for (lapIndex = 0; lapIndex < lapCount; lapIndex++) {
            clock += racer->lap_times[lapIndex];
        }
        mdkr_trace(
            "[ORACLE] frame=%d map=%d slot=%d "
            "x=%.9g y=%.9g z=%.9g xv=%.9g yv=%.9g zv=%.9g "
            "fvel=%.9g vel=%.9g cp=%d next=%d lap=%d countlap=%d "
            "fin=%d fpos=%d ridx=%d pidx=%d vehicle=%d grounded=%d "
            "clock=%u start=%d delta=%u rate=%d rng=%u",
            frame, get_ingame_map_id(), i,
            obj->trans.x_position, obj->trans.y_position,
            obj->trans.z_position, obj->x_velocity, obj->y_velocity,
            obj->z_velocity, racer->forwardVel, racer->velocity,
            racer->courseCheckpoint, racer->nextCheckpoint, racer->lap,
            racer->countLap, racer->raceFinished, racer->finishPosition,
            racer->racerIndex, racer->playerIndex, racer->vehicleID,
            racer->groundedWheels, clock, get_race_start_timer(),
            gVideoDeltaTime, sLogicUpdateRate, (u32) gCurrentRNGSeed);
    }
}

/* CollisionFacetPlanes is 4 × u16. func_80017A18 indexes an array of it that
 * object_models.c allocates as `count * sizeof(CollisionFacetPlanes)`, so the
 * two only agree while this stays 8 bytes. Lock it (CONTRIBUTING.md rule 3). */
DKR_ASSERT_SIZE(CollisionFacetPlanes, 8);

enum MdkrRemasterLightClass {
    MDKR_REMASTER_LIGHT_NONE = 0,
    MDKR_REMASTER_LIGHT_RACER = 1,
    MDKR_REMASTER_LIGHT_CHARACTER = 2,
};

/*
 * Renderer classification is deliberately semantic and narrow. Terrain,
 * billboards, pickups, particles and generic scenery never inherit this state;
 * only racers and the engine's character-animation behaviours opt in.
 */
static enum MdkrRemasterLightClass
mdkr_remaster_light_class(const Object *obj) {
    if (obj == NULL || obj->header == NULL) {
        return MDKR_REMASTER_LIGHT_NONE;
    }
    switch (obj->behaviorId) {
        case BHV_RACER:
            return MDKR_REMASTER_LIGHT_RACER;
        case BHV_DINO_WHALE:
        case BHV_STOPWATCH_MAN:
        case BHV_CAR_ANIMATION:
        case BHV_CHARACTER_SELECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_PARK_WARDEN:
        case BHV_PARK_WARDEN_2:
        case BHV_ANIMATED_OBJECT_2:
        case BHV_ANIMATED_OBJECT_3:
        case BHV_ANIMATED_OBJECT_4:
            return MDKR_REMASTER_LIGHT_CHARACTER;
        default:
            return MDKR_REMASTER_LIGHT_NONE;
    }
}

static u32 mdkr_remaster_light_target(const Object *obj) {
    const GfxLevelLightingRig *rig = gfx_level_lighting_current();
    enum MdkrRemasterLightClass light_class =
        mdkr_remaster_light_class(obj);
    ObjectTransform inverse;
    MtxF inverse_matrix;
    Vec3f world_direction;
    Vec3f local_direction;
    u32 packed;

    if (light_class == MDKR_REMASTER_LIGHT_NONE ||
        rig == NULL || !rig->valid) {
        return 0;
    }

    /*
     * Smooth normals are stored in model space. Carry the sun in that same
     * space so object rotation—and the queued display-list delay—cannot make a
     * world-space uniform disagree with the normal stream.
     */
    memset(&inverse, 0, sizeof(inverse));
    inverse.rotation.s[0] = -obj->trans.rotation.s[0];
    inverse.rotation.s[1] = -obj->trans.rotation.s[1];
    inverse.rotation.s[2] = -obj->trans.rotation.s[2];
    inverse.scale = 1.0f;
    mtxf_from_inverse_transform(&inverse_matrix, &inverse);
    world_direction.x = rig->direction_world[0];
    world_direction.y = rig->direction_world[1];
    world_direction.z = rig->direction_world[2];
    mtxf_transform_dir(&inverse_matrix, &world_direction,
                       &local_direction);
    packed = gfx_level_lighting_pack_direction(local_direction.f);
    if (packed == 0) {
        return 0;
    }
    return packed | ((u32)light_class << 30);
}

/*
 * ObjectModel normals are a compact stream, not necessarily one entry per
 * model vertex: BATCH_VTX_COL batches consume them only for environment maps.
 * Reproduce the engine's obj_shade_fast cursor walk so each display-list
 * gSPVertex receives exactly the normal subspan for that batch.
 */
static Vec3s *mdkr_object_batch_normals(ObjectModel *model,
                                       s32 batch_index) {
    TriangleBatchInfo *batches;
    Vec3s *normals;
    s32 normal_offset = 0;

    if (model == NULL || model->normals == 0 ||
        batch_index < 0 || batch_index >= model->numberOfBatches) {
        return NULL;
    }
    batches = DKR_PTR(TriangleBatchInfo, model->batches);
    normals = DKR_PTR(Vec3s, model->normals);
    if (batches == NULL || normals == NULL) {
        return NULL;
    }
    for (s32 i = 0; i <= batch_index; i++) {
        s32 count =
            batches[i + 1].verticesOffset -
            batches[i].verticesOffset;
        bool consumes =
            batches[i].miscData != BATCH_VTX_COL ||
            (batches[i].flags & RENDER_ENVMAP) != 0;
        if (i == batch_index) {
            return consumes ? normals + normal_offset : NULL;
        }
        if (consumes && count > 0) {
            normal_offset += count;
        }
    }
    return NULL;
}
#endif

#define OBJECT_MAP_SIZE 0x3000
#define MAX_CHECKPOINTS 60
#ifdef NATIVE_PORT
/* LP64: 0x15800 is the N64 byte budget for the object sub-pool. Every allocation
 * in it (Object + its variable-size trailing payload: modelInstances[], shading,
 * shadow, interaction, collision, attach points, emitters, lightData[], plus the
 * ObjectHeaders themselves) is built out of REAL host pointers, which are 8 bytes
 * here and 4 on N64 — so the same scene needs roughly twice the bytes. Scale the
 * budget by the host pointer size; on N64 sizeof(void *) == 4 and this is exactly
 * the original 0x15800, and 0x2b000 here. Measured peaks against that budget: a
 * driven Time-Trial race 0x12fe0 (77792); the attract demo through its first
 * track 0x14b50 (84752); and the demo's second track, which is what exposed this,
 * demanded at least 0x158d0 (88272 = 86240 already used + a 2032-byte request
 * that failed) — i.e. more than the whole old 0x15800 budget. Slot count is
 * unchanged and was never the limit (peak 296-297 of 512). See docs/OPEN_ITEMS.md. */
#define OBJECT_POOL_SIZE (0x15800 * (sizeof(void *) / 4))
#else
#define OBJECT_POOL_SIZE 0x15800
#endif
#define OBJECT_BLUEPRINT_SIZE 0x800
#define OBJECT_SLOT_COUNT 512
#define OBJECT_COLLISION_COUNT 20
#define AINODE_COUNT 128
#define CAMCONTROL_COUNT 20
#define BOOST_VERT_COUNT 9
#define BOOST_TRI_COUNT 8

#ifndef _ALIGN16
#define _ALIGN16(a) (((u32) (a) & ~0xF) + 0x10)
#endif

#define SET_SHIFT_AND_MASK(varShift, varMask, x) \
    varShift = x;                                \
    varMask = 0xFFFF >> x;

/************ .data ************/

FadeTransition gTajChallengeTransition = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 30, 15);
FadeTransition gBalloonCutsceneTransition = FADE_TRANSITION(FADE_CIRCLE, FADE_FLAG_NONE, FADE_COLOR_BLACK, 30, 15);

s32 D_800DC700 = 0;
s32 D_800DC704 = 0; // Currently unknown, might be a different type.
s16 D_800DC708 = 0;
s32 D_800DC70C = 0; // Currently unknown, might be a different type.
s16 D_800DC710 = 1;
s32 D_800DC714 = 0; // Currently unknown, might be a different type.
Object *gGhostObjStaff = NULL;
s8 gRollingDemo = FALSE;
s32 gObjectTexAnim = FALSE;
s16 gTimeTrialTime = 10800;
s16 gTimeTrialVehicle = -1;
s16 gTimeTrialCharacter = 0;
u8 gHasGhostToSave = FALSE;
u8 gTimeTrialStaffGhost = FALSE;
u8 gBeatStaffGhost = FALSE;
s8 gLeadPlayerIndex = 0;
s8 gTwoActivePlayersInAdventure = FALSE; // Has basically the same purpose as gTwoPlayerAdvRace
s8 gSwapLeadPlayer = FALSE;
s8 gIsP2LeadPlayer = FALSE;
Vertex *gBoostVerts[2] = { 0, 0 };
Triangle *gBoostTris[2] = { 0, 0 };
Object *gShieldEffectObject = NULL;
s32 gBoostObjOverrideID = 9;
Object *gMagnetEffectObject = NULL;

f32 D_800DC768[16] = { 0.0f, 1.0f,  0.70711f,  0.70711f,  1.0f,  0.0f, 0.70711f,  -0.70711f,
                       0.0f, -1.0f, -0.70711f, -0.70711f, -1.0f, 0.0f, -0.70711f, 0.70711f };

u16 gRacerObjectTable[] = {
    // Car
    ASSET_OBJECT_ID_KREMCAR,
    ASSET_OBJECT_ID_BADGERCAR,
    ASSET_OBJECT_ID_TORTCAR,
    ASSET_OBJECT_ID_CONKACAR,
    ASSET_OBJECT_ID_TIGERCAR,
    ASSET_OBJECT_ID_BANJOCAR,
    ASSET_OBJECT_ID_CHICKENCAR,
    ASSET_OBJECT_ID_MOUSECAR,
    ASSET_OBJECT_ID_SWCAR,
    ASSET_OBJECT_ID_DIDDYCAR,
    // Hover
    ASSET_OBJECT_ID_KREMLINHOVER,
    ASSET_OBJECT_ID_BADGERHOVER,
    ASSET_OBJECT_ID_TORTHOVER,
    ASSET_OBJECT_ID_CONKAHOVER,
    ASSET_OBJECT_ID_TIGERHOVER,
    ASSET_OBJECT_ID_BANJOHOVER,
    ASSET_OBJECT_ID_CHICKENHOVER,
    ASSET_OBJECT_ID_MOUSEHOVER,
    ASSET_OBJECT_ID_TICKTOCKHOVER,
    ASSET_OBJECT_ID_DIDDYHOVER,
    // Plane
    ASSET_OBJECT_ID_KREMPLANE,
    ASSET_OBJECT_ID_BADGERPLANE,
    ASSET_OBJECT_ID_TORTPLANE,
    ASSET_OBJECT_ID_CONKA,
    ASSET_OBJECT_ID_TIGPLANE,
    ASSET_OBJECT_ID_BANJOPLANE,
    ASSET_OBJECT_ID_CHICKENPLANE,
    ASSET_OBJECT_ID_MOUSEPLANE,
    ASSET_OBJECT_ID_TICKTOCKPLANE,
    ASSET_OBJECT_ID_DIDDYPLANE,
    // Car
    ASSET_OBJECT_ID_KREMCAR,
    ASSET_OBJECT_ID_BADGERCAR,
    ASSET_OBJECT_ID_TORTCAR,
    ASSET_OBJECT_ID_CONKACAR,
    ASSET_OBJECT_ID_TIGERCAR,
    ASSET_OBJECT_ID_BANJOCAR,
    ASSET_OBJECT_ID_CHICKENCAR,
    ASSET_OBJECT_ID_MOUSECAR,
    ASSET_OBJECT_ID_SWCAR,
    ASSET_OBJECT_ID_DIDDYCAR,
    // Car again?
    ASSET_OBJECT_ID_KREMCAR,
    ASSET_OBJECT_ID_BADGERCAR,
    ASSET_OBJECT_ID_TORTCAR,
    ASSET_OBJECT_ID_CONKACAR,
    ASSET_OBJECT_ID_TIGERCAR,
    ASSET_OBJECT_ID_BANJOCAR,
    ASSET_OBJECT_ID_CHICKENCAR,
    ASSET_OBJECT_ID_MOUSECAR,
    ASSET_OBJECT_ID_SWCAR,
    ASSET_OBJECT_ID_DIDDYCAR,
    // Special
    ASSET_OBJECT_ID_TRICKYTOPS,
    ASSET_OBJECT_ID_WALRUS,
    ASSET_OBJECT_ID_DRAGONBOSS,
    ASSET_OBJECT_ID_TERRYBOSS,
    ASSET_OBJECT_ID_SNOWBALLBOSS,
    ASSET_OBJECT_ID_FLYINGCARPET,
    ASSET_OBJECT_ID_OCTOPUS,
    ASSET_OBJECT_ID_WIZPIG,
    ASSET_OBJECT_ID_WIZPIGROCKET,
};

// A table of which vehicles to use for boss races.
// https://www.youtube.com/watch?v=WQJAtns_rMk
BossRaceVehicles gBossVehicles[] = {
    { VEHICLE_CAR, VEHICLE_TRICKY },         // Tricky 1
    { VEHICLE_HOVERCRAFT, VEHICLE_BLUEY },   // Bluey 1
    { VEHICLE_PLANE, VEHICLE_SMOKEY },       // Smokey 1
    { VEHICLE_CAR, VEHICLE_TRICKY },         // Tricky 2
    { VEHICLE_HOVERCRAFT, VEHICLE_BLUEY },   // Bluey 2
    { VEHICLE_HOVERCRAFT, VEHICLE_BUBBLER }, // Bubbler 1
    { VEHICLE_HOVERCRAFT, VEHICLE_BUBBLER }, // Bubbler 2
    { VEHICLE_PLANE, VEHICLE_SMOKEY },       // Smokey 2
    { VEHICLE_CAR, VEHICLE_WIZPIG },         // Wizpig 1
    { VEHICLE_PLANE, VEHICLE_ROCKET },       // Wizpig 2
};

s8 D_800DC834[10] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

s8 D_800DC840[8] = { 9, 1, 2, 3, 4, 5, 7, 0 };

s8 gNoBoundsCheck = FALSE;
u32 gMagnetColours[3] = {
    COLOUR_RGBA32(255, 64, 16, 0), // Level 1
    COLOUR_RGBA32(16, 64, 255, 0), // Level 2
    COLOUR_RGBA32(16, 255, 64, 0), // Level 3
};
FadeTransition gRaceEndFade = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_BLACK, 40, FADE_STAY);
FadeTransition gRaceEndTransition = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 40, 0);

/*******************************/

/************ .rodata ************/

UNUSED const char sObjectOutofMemString[] = "Objects out of ram(1) !!\n";
UNUSED const char sDoorNumberErrorString[] = "Door numbering error %d!!\n";
UNUSED const char sObjectScopeErrorString[] = "objGetScope: Unknown scope for object %d\n";

/*********************************/

/************ .bss ************/

s16 D_8011AC20[128];
s8 D_8011AD20;
s8 D_8011AD21;
s8 D_8011AD22[2];
s8 D_8011AD24[2];
s8 D_8011AD26;
f32 gObjectModelScaleY;
s32 D_8011AD2C;
f32 gCurrentLightIntensity;
Object *gGhostObjPlayer;
s32 gTimeTrialContPak; // gTimeTrialContPak is ultimately set by func_80074B34, and is almost definitely SIDeviceStatus
s8 D_8011AD3C;
s8 D_8011AD3D;
s8 D_8011AD3E;
Object *gTransformObject; // Set but never read.
s8 gTransformTimer;
s8 gOverworldVehicle;
s16 gTransformPosX;
s16 gTransformPosY;
s16 gTransformPosZ;
s16 gTransformAngleY;
s16 gRaceEndTimer;
s8 gRaceEndStage;
s8 gNumRacersSaved;
UNUSED s8 unused_D_8011AD52;
s8 D_8011AD53;
s32 D_8011AD54;
s32 *gSpawnObjectHeap;
s32 D_8011AD5C;
s32 D_8011AD60;
s32 *gAssetsObjectHeadersTable;
s32 gAssetsObjectHeadersTableLength;
s32 *gAssetsMiscSection;
s32 *gAssetsMiscTable;
s32 D_8011AD74;
Gfx *D_8011AD78[10];
s32 gAssetsMiscTableLength;
s16 D_8011ADA4;
f32 gObjectUpdateRateF;
s32 gPathUpdateOff;
s32 gEventCountdown;
s32 gRaceFinishTriggered;
s32 gEventStartTimer;
s32 D_8011ADBC;
s32 gNumFinishedRacers;
s8 gFirstTimeFinish;
s8 D_8011ADC5;
u32 gBalloonCutsceneTimer;
s8 (*gDrawbridgeTimers)[8];
f32 gObjectOffsetY;
#ifdef NATIVE_PORT
static f32 gObjectSavedTumbleY; /* exact pre-tumble bits; see do/undo pair */
static Object *gObjectSavedTumbleFor; /* pairing guard, see gObjectSavedBobFor */
static f32 gObjectSavedBobX, gObjectSavedBobY, gObjectSavedBobZ; /* carBob pair */
static Object *gObjectSavedBobFor; /* pairing guard for the exact-bits restore.
    Saved bits are valid only for the object that saved them, so the restore is
    conditional on the pointer matching. The save and restore guards are today
    identical and unnested
    (`!(flags & OBJ_FLAGS_PARTICLE) && header->behaviorId == BHV_RACER` on both
    sides, set_temp_model_transforms/unset_temp_model_transforms), so no
    save-less restore path exists and the guard never fires; it stands so that a
    future nested or re-entrant draw cannot write one racer's position onto
    another. With no saved bits the original arithmetic subtract runs, which is
    untouched ROM behaviour. The tumble pair below carries the same guard over a
    wider bracket: obj_tex_animate, mtx_cam_push and (particles.c) the whole
    per-racer emitter loop. */
static Object *gObjectRenderModelFor;
static s32 gObjectRenderModelIndex;
static s32 gObjectRenderRacerTexOffset;
static Vertex *gObjectSavedCurVertData;
static Object *gObjectSavedCurVertFor;
static s32 object_render_model_index(const Object *obj);
static s32 racer_model_index_for_view(Object *obj, Object_Racer *racer,
                                      f32 viewDistance, f32 *outScale);
static s32 obj_door_batch_texture_offset(const ObjectModel *model,
                                         const Object *obj,
                                         s32 batchIndex, s32 *outOffset,
                                         s32 *outDigitPlace);
#endif
s8 D_8011ADD4;
s8 gOverrideDoors;
Object *D_8011ADD8[10]; // Array of OverRidePos objects
s8 D_8011AE00;          // Number of OverRidePos objects in D_8011ADD8
s8 D_8011AE01;          // A boolean? I've seen it either as 0 or 1
s8 gIsNonCarRacers;
s8 gIsSilverCoinRace;
Object *D_8011AE08[16];
ObjectHeader *(*gLoadedObjectHeaders)[ASSET_OBJECTS_COUNT];
u8 (*gObjectHeaderReferences)[ASSET_OBJECTS_COUNT];
TextureHeader *D_8011AE50;
TextureHeader *D_8011AE54;
Object **gObjPtrList; // Not sure about the number of elements
s32 gObjectCount;
s32 gObjectListStart;
s32 gParticleCount;
Object *gObjectMemoryPool;
Object **gCollisionObjects;
s32 gCollisionObjectCount;
Object **D_8011AE74; // Pointer to an array of Animation objects
s16 D_8011AE78;      // Number of Animation objects in D_8011AE74
s16 gCutsceneID;
s16 gFirstActiveObjectId;
s8 D_8011AE7E;
s16 gTTGhostTimeToBeat;
s16 gPrevTimeTrialVehicle; // Current Vehicle being used in track?
s16 gMapDefaultVehicle;    // Vehicle enum
s32 D_8011AE88;
Gfx *gObjectCurrDisplayList;
Mtx *gObjectCurrMatrix;
Vertex *gObjectCurrVertexList;
u8 *gObjectMapSpawnList[2];
s32 gObjectMapSize[2];
s32 gObjectMapID[2];
s32 *gObjectMap[2];
s16 *gAssetsLvlObjTranslationTable;
s32 gAssetsLvlObjTranslationTableLength;
#ifdef NATIVE_PORT
/* Full allocated entry count, including zero-filled/reserved object IDs. The
 * historical "...Length" above is the last nonzero ID and is not a safe array
 * bound. */
size_t gAssetsLvlObjTranslationTableCapacity;
#endif
s32 gObjectMapIndex;
Object **gParticlePtrList;
s32 gFreeListCount;
CheckpointNode *gTrackCheckpoints; // Array of structs, unknown number of members
s32 gNumberOfMainCheckpoints;
s32 gNumberOfTotalCheckpoints;
s16 gTajChallengeType;
Object *(*gCameraObjList)[CAMCONTROL_COUNT]; // Camera objects with a maximum of 20
s32 gCameraObjCount;                         // The number of camera objects in the above list
Object *(*gRacers)[10];                      // Official Name: playerlist
// Similar to gRacers, but sorts the pointer by the players' current position in the race.
Object **gRacersByPosition;
// Similar to gRacers, but sorts the pointer by controller ports 1-4, then CPUs.
Object **gRacersByPort;
s32 gNumRacers;
u8 gTimeTrialEnabled;
u8 gIsTimeTrial;
s8 gIsTajChallenge;
s8 gTajRaceInit;
s8 gChallengePrevMusic;
s32 gChallengePrevInstruments;
s8 D_8011AF00;
Object *(*gAINodes)[AINODE_COUNT];
s32 gAINodeTail[2];
s32 gInitAINodes;
s32 D_8011AF14;
f32 gElevationHeights[5];
s32 D_8011AF2C;
#ifdef NATIVE_PORT
/*
 * The matching global layout splits one 0x30-byte ShadeProperties object
 * across gWorldShading plus the adjacent D_8011AF34/D_8011AF38 symbols.
 * Reaching across named C objects is undefined, and LP64 makes the first
 * "pointer" eight bytes wide anyway. Give the native port real typed storage;
 * retain the historical split only where exact N64 layout is required.
 */
static ShadeProperties gWorldShading;
#else
ShadeProperties *gWorldShading; // Effectively unused.
#endif
s32 D_8011AF34;
s32 D_8011AF38[10];
Object_MidiFade *D_8011AF60;
TexCoords D_8011AF68[32];
Vec3s gEnvmapPos[2];
unk800179D0 *D_8011AFF4;
s32 gBoostVertCount;
s32 gNumOfBoostVerts;
s32 gBoostTriCount;
s32 gNumOfBoostTris;
s32 gBoostVertFlip; // indexes gBoostVerts and gBoostTris
u8 gShieldSineTime[16];
Object *gBoostEffectObjects[NUMBER_OF_CHARACTERS];
u8 D_8011B048[16]; // Vehicle IDs for the boost objects, used to determine which racer is using which boost object.
u8 D_8011B058[16];
u8 D_8011B068[16];
RacerFXData gRacerFXData[NUMBER_OF_CHARACTERS];

extern s16 gGhostMapID;

/******************************/

/**
 * Spawns control objects for racer boost visuals, as well as shield and magnet visuals.
 * Boost geometry is made in real time, and allocated here.
 * This function is called on every level load, but only racers use the stuff here.
 */
void racerfx_alloc(s32 numberOfVertices, s32 numberOfTriangles) {
    Object_Boost *boostObj;
    LevelObjectEntry_Boost2 objEntry;
    s32 i;

    gBoostTris[0] = (Triangle *) mempool_alloc_safe(
        ((numberOfTriangles * sizeof(Triangle)) + (numberOfVertices * sizeof(Vertex))) * 2, COLOUR_TAG_BLUE);
    /* LP64: (u32) truncates the arena base returned by mempool_alloc_safe, so the
     * derived boost triangle/vertex buffers would be wild pointers written to on
     * every race that has racers. Carry the full pointer through uintptr_t. */
    gBoostTris[1] = (Triangle *) ((uintptr_t) gBoostTris[0] + numberOfTriangles * sizeof(Triangle));
    gBoostVerts[0] = (Vertex *) ((uintptr_t) gBoostTris[1] + numberOfTriangles * sizeof(Triangle));
    gBoostVerts[1] = (Vertex *) ((uintptr_t) gBoostVerts[0] + numberOfVertices * sizeof(Vertex));
    gBoostVertCount = numberOfVertices;
    gNumOfBoostVerts = 0;
    gBoostTriCount = numberOfTriangles;
    gNumOfBoostTris = 0;
    gBoostVertFlip = 0;
    boostObj = GET_BOOST_TABLE();
    // Makes 10 boost objects, but only 8 racers can actually exist at once.
    for (i = 0; i < NUMBER_OF_CHARACTERS; i++) {
        objEntry.common.objectID = ASSET_OBJECT_ID_BOOST;
        objEntry.common.size = sizeof(LevelObjectEntry_Boost2);
        objEntry.common.x = 0;
        objEntry.common.y = 0;
        objEntry.common.z = 0;
        objEntry.racerIndex = i;
        gBoostEffectObjects[i] = spawn_object(&objEntry.common, OBJECT_SPAWN_UNK01);
        if (gBoostEffectObjects[i] != NULL) {
            gBoostEffectObjects[i]->properties.common.unk0 = 0;
            gBoostEffectObjects[i]->properties.common.unk4 = 0;
            boostObj[i].unk70 = 0;
            boostObj[i].unk74 = 0.0f;
            boostObj[i].sprite = tex_load_sprite(boostObj[i].spriteId, 0);
            boostObj[i].tex = load_texture(boostObj[i].textureId);
            boostObj[i].unk72 = rand_range(0, 255);
            boostObj[i].unk73 = 0;
            // This is for shields, not boosts.
            gShieldSineTime[i] = rand_range(0, 255);
        }
        D_8011B068[i] = TRUE;
    }
    gBoostObjOverrideID = 9;
    objEntry.common.objectID = ASSET_OBJECT_ID_SHIELD;
    objEntry.common.size = sizeof(LevelObjectEntry_Boost2);
    objEntry.common.x = 0;
    objEntry.common.y = 0;
    objEntry.common.z = 0;
    gShieldEffectObject = spawn_object(&objEntry.common, OBJECT_SPAWN_NONE);
    for (i = 0; i < NUMBER_OF_CHARACTERS; i++) {
        gRacerFXData[i].unk0 = 0;
        gRacerFXData[i].unk1 = rand_range(0, 255);
        gRacerFXData[i].unk2 = rand_range(0, 255);
        gRacerFXData[i].unk3 = 0;
    }
    objEntry.common.objectID = ASSET_OBJECT_ID_AINODE;
    objEntry.common.size = sizeof(LevelObjectEntry_Boost2) + 0x80; // Not sure where this 0x80 comes from.
    objEntry.common.x = 0;
    objEntry.common.y = 0;
    objEntry.common.z = 0;
    gMagnetEffectObject = spawn_object(&objEntry.common, OBJECT_SPAWN_NONE);
}

/**
 * Attempts to free the boost, shield and magnet objects and assets.
 */
void racerfx_free(void) {
    Sprite *sprite;
    TextureHeader *texture;
    Object_Boost *objBoost;
    u32 i;

    if (gBoostTris[0]) {
        mempool_free(gBoostTris[0]);
        gBoostTris[0] = NULL;
        gBoostTris[1] = NULL;
        gBoostVerts[0] = NULL;
        gBoostVerts[1] = NULL;
    }
    objBoost = GET_BOOST_TABLE();
    for (i = 0; i < NUMBER_OF_CHARACTERS; i++) {
        sprite = objBoost[i].sprite;
        if (sprite != NULL) {
            sprite_free(sprite);
            objBoost[i].sprite = NULL;
        }
        texture = objBoost[i].tex;
        if (texture != NULL) {
            tex_free(texture);
            objBoost[i].tex = NULL;
        }
    }
    if (gShieldEffectObject != NULL) {
        free_object(gShieldEffectObject);
    }
    gShieldEffectObject = NULL;

    if (gMagnetEffectObject != NULL) {
        free_object(gMagnetEffectObject);
    }
    gMagnetEffectObject = NULL;
    gParticlePtrList_flush();
}

void func_8000B38C(Vertex *vertices, Triangle *triangles, ObjectTransform *trans, f32 arg3, f32 arg4, s16 arg5,
                   TextureHeader *tex) {
    s32 sp80[8];
    s32 i;
    s32 height, width;
    s16 *v;
    Vec3f sp64;
    s32 *tri;
    f32 *ptr;
    s32 temp;

    v = (s16 *) vertices;

    sp64.z = -arg4;
    vec3f_rotate_py(&trans->rotation, &sp64);

    // A rather strange way to fill structures

    *v++ = sp64.f[0] + trans->x_position;
    *v++ = sp64.f[1] + trans->y_position;
    *v++ = sp64.f[2] + trans->z_position;
    *v++ = -1;
    *v++ = -1;

    ptr = D_800DC768;
    for (i = 0; i < 8; i++) {
        sp64.x = *ptr++ * arg3;
        sp64.y = *ptr++ * arg3;
        sp64.z = 0.0f;

        vec3f_rotate(&trans->rotation, &sp64);

        *v++ = sp64.f[0] + trans->x_position;
        *v++ = sp64.f[1] + trans->y_position;
        *v++ = sp64.f[2] + trans->z_position;
        *v++ = -1;
        *v++ = -1;
    }

    width = (tex->width - 1) << 4;
    height = (tex->height - 1) << 4;

    for (i = 0; i < 8; i++) {
        sp80[i] = width + ((sins_s16(arg5) * width) >> 16);
        sp80[i] |= ((height << 16) + height * coss_s16(arg5)) & 0xFFFF0000;
        arg5 += 0x2000;
    }

    tri = (s32 *) triangles;
    for (i = 0; i < 8; i++) {
        *tri++ = DKR_TRIANGLE(BACKFACE_CULL, 0, i + 1, ((i + 1) & 7) + 1);
        *tri++ = ((width & 0xFFFF) << 16) | (height & 0xFFFF);
        *tri++ = sp80[i];
        *tri++ = sp80[(i + 1) & 7];
    }
}

void func_8000B750(Object *racerObj, s32 racerIndex, s32 vehicleIDPrev, s32 boostType, s32 arg4) {
    Vec3f sp74;
    f32 temp_f0;
    f32 var_f2;
    Object_Boost *boostAsset;
    ObjectTransform objTrans;
    Object_Boost *objBoostRacer;
    Object_Boost *objBoostType;
    Object_Boost_Inner *boostData;

    if (racerIndex == -1) {
        racerIndex = gBoostObjOverrideID;
        gBoostObjOverrideID--;
    }
    if (gBoostObjOverrideID < 0) {
        gBoostObjOverrideID = 0;
    }
    if (racerIndex >= 0 && racerIndex < NUMBER_OF_CHARACTERS) {
        boostAsset = GET_BOOST_TABLE();
        objBoostType = &boostAsset[boostType];
        objBoostRacer = &boostAsset[racerIndex];
        if (gBoostEffectObjects[racerIndex] != NULL) {
            switch (vehicleIDPrev) {
                default:
                    boostData = NULL;
                    break;
                case VEHICLE_CAR:
                    boostData = &objBoostRacer->carBoostData;
                    break;
                case VEHICLE_HOVERCRAFT:
                    boostData = &objBoostRacer->hovercraftBoostData;
                    break;
                case VEHICLE_PLANE:
                    boostData = &objBoostRacer->flyingBoostData;
                    break;
                case VEHICLE_ROCKET:
                    boostData = &objBoostRacer->flyingBoostData;
                    break;
            }
            if (boostData != NULL) {
                D_8011B048[racerIndex] = vehicleIDPrev;
                D_8011B058[racerIndex] = boostType;
                if (objBoostRacer->unk70 == 2) {
                    temp_f0 = coss_f(objBoostRacer->unk72 << 12);
                    var_f2 = (boostData->unk14 + (temp_f0 * boostData->unk18)) * objBoostRacer->unk74;
                    temp_f0 = (boostData->unk1C + (temp_f0 * boostData->unk20)) * objBoostRacer->unk74;
                    if ((boostType & 3) == BOOST_MEDIUM) {
                        var_f2 *= 1.09f;
                        temp_f0 *= 1.09f;
                    }
                    if ((boostType & 3) >= BOOST_LARGE) {
                        var_f2 *= 1.18f;
                        temp_f0 *= 1.18f;
                    }
                    objTrans.x_position = boostData->position.x;
                    objTrans.y_position = boostData->position.y;
                    objTrans.z_position = boostData->position.z;
                    objTrans.scale = 1.0f;
                    objTrans.rotation.y_rotation = -0x8000;
                    objTrans.rotation.x_rotation = 0;
                    objTrans.rotation.z_rotation = 0;
                    func_8000B38C(&gBoostVerts[gBoostVertFlip][gNumOfBoostVerts],
                                  &gBoostTris[gBoostVertFlip][gNumOfBoostTris], &objTrans, var_f2, temp_f0,
                                  objBoostRacer->unk72 << 12, objBoostType->tex);
                    gBoostEffectObjects[racerIndex]->properties.boost.indexes =
                        ((u32) racerIndex << 28) | ((u32) gNumOfBoostVerts << 14) |
                        (u32) gNumOfBoostTris;
                    gNumOfBoostVerts += BOOST_VERT_COUNT;
                    gNumOfBoostTris += BOOST_TRI_COUNT;
                }
                gBoostEffectObjects[racerIndex]->properties.boost.obj = racerObj;
                gBoostEffectObjects[racerIndex]->trans.x_position = 0.0f;
                gBoostEffectObjects[racerIndex]->trans.y_position = 0.0f;
                gBoostEffectObjects[racerIndex]->trans.z_position = 0.0f;
                sp74.x = boostData->position.x;
                sp74.y = boostData->position.y;
                sp74.z = boostData->position.z;
                vec3f_rotate(&racerObj->trans.rotation, &sp74);
                ignore_bounds_check();
                move_object(gBoostEffectObjects[racerIndex], racerObj->trans.x_position + sp74.f[0],
                            racerObj->trans.y_position + sp74.f[1], racerObj->trans.z_position + sp74.f[2]);
            }
            if (arg4 != FALSE) {
                D_8011B068[racerIndex] = FALSE;
            }
        }
    }
}

#ifdef NATIVE_PORT
/*
 * Headless verification hook for the boost/exhaust graphics (see
 * tests/README.md). MDKR_FORCE_BOOST=<frame>[:<len>] hands every racer the exact
 * state a SURFACE_ZIP_PAD gives it (racer.c: boostTimer = normalise_time(45),
 * boostType = BOOST_LARGE) on every frame in [frame, frame+len), so a dumped
 * frame is guaranteed to contain a rendered boost. Default len is 90 frames.
 *
 * This exists because no deterministic input fixture reliably reaches a zip pad
 * (the port's driving physics currently strands the racer before the first one),
 * and the attract demo only ever boosts racers that the demo camera is not
 * following, so their boost objects are correctly culled before the draw.
 * Zero cost when the variable is unset.
 */
void dkr_force_boost_hook(Object_Racer *racer) {
    extern int g_frameCounter;
    static s32 sStart = -2;
    static s32 sLen = 90;

    if (sStart == -2) {
        const char *e = getenv("MDKR_FORCE_BOOST");
        sStart = -1;
        if (e != NULL && e[0] != '\0') {
            const char *colon = strchr(e, ':');
            sStart = atoi(e);
            if (colon != NULL && colon[1] != '\0') {
                sLen = atoi(colon + 1);
            }
        }
    }
    if (sStart < 0 || g_frameCounter < sStart || g_frameCounter >= sStart + sLen) {
        return;
    }
    racer->boostTimer = normalise_time(45);
    racer->boostType = BOOST_LARGE;
}

/* Deterministic verification content for the shield/magnet shear path. Both
 * pixel-comparison arms receive this same authored-tick state; only the replay
 * interpolation test seam differs between them. */
static void dkr_force_shield_hook(Object_Racer *racer) {
    extern int g_frameCounter;
    static s32 sStart = -2;
    static s32 sLen = 90;

    if (sStart == -2) {
        const char *value = getenv("MDKR_FORCE_SHIELD");
        sStart = -1;
        if (value != NULL && value[0] != '\0') {
            const char *colon = strchr(value, ':');
            sStart = atoi(value);
            if (colon != NULL && colon[1] != '\0') {
                sLen = atoi(colon + 1);
            }
        }
    }
    if (racer == NULL || sStart < 0 || g_frameCounter < sStart ||
        g_frameCounter >= sStart + sLen) {
        return;
    }
    racer->shieldTimer = normalise_time(90);
    racer->shieldType = SHIELD_LEVEL3;
}

/*
 * ---------------------------------------------------------------------------
 * G1: zip-pad boost magnitude — instrument and positive control.
 * ---------------------------------------------------------------------------
 *
 * WHY THIS EXISTS RATHER THAN A PAD-CROSSING FIXTURE. The register's 44.9
 * units/frame measurement came from a zip pad that `nav_to_time_trial_race.txt`
 * + `MDKR_AUTOPILOT` happened to drive over. It does not any more: the AI line
 * moved when the wave "closedloop" ROM-fidelity corrections landed, and the same
 * route now peaks at 14.3 (tests/check_race_drive.py's own reported figure).
 * That is exactly the fixture class tests/README.md warns about — the *line* is
 * chaotic with respect to any simulation change, so no committed route can be
 * relied on to keep crossing a particular pad.
 *
 * So the magnitude is measured by arming the boost directly, in the state a pad
 * arms it in, and letting the authored physics run. Everything downstream of the
 * two assignments below — `racer.c`'s `traction = 2.0f` throttle override, the
 * `boostTimer -= updateRate` decay and every velocity/drag term — is untouched
 * decomp code (verified statement-for-statement against the recorded decomp
 * baseline), so what this measures IS the shipping boost, only with a
 * deterministic trigger instead of a chaotic one.
 *
 *   MDKR_ZIPPAD_BOOST=<frame>[:<ticks>]
 *       On the first update at or after <frame>, arm the HUMAN racer exactly as
 *       `racer.c:5727` arms it for `SURFACE_ZIP_PAD` on a car:
 *       `boostTimer = normalise_time(ticks)`, `boostType = BOOST_LARGE`.
 *       <ticks> defaults to 45, the authored constant. Armed once per run.
 *       Passing any other <ticks> is the perturbed-constant BROKEN DIRECTION
 *       arm: it must move the speed trace out of the baseline envelope.
 *
 *   MDKR_BOOST_TRACE=1
 *       Emit a greppable per-update [BOOST] row for the human racer: boost
 *       state, velocity and world position, enough to reconstruct the per-frame
 *       speed trace and to see a *real* pad arming a boost on any route.
 *
 * Unlike MDKR_FORCE_BOOST above (which re-arms every racer on every frame of a
 * window, for the benefit of the boost *renderer*), this arms the one racer
 * once, so the boost decays on its own schedule and the magnitude that comes out
 * is the authored one. Both are zero cost when unset.
 */
void mdkr_zippad_boost_hook(Object *obj, Object_Racer *racer) {
    extern int g_frameCounter;
    static s32 sFrame = -2;
    static s32 sTicks = 45;
    static s32 sArmed = FALSE;

    if (sFrame == -2) {
        const char *e = getenv("MDKR_ZIPPAD_BOOST");
        sFrame = -1;
        if (e != NULL && e[0] != '\0') {
            const char *colon = strchr(e, ':');
            sFrame = atoi(e);
            if (colon != NULL && colon[1] != '\0') {
                sTicks = atoi(colon + 1);
            }
        }
    }
    if (sFrame < 0 || sArmed || obj == NULL || racer == NULL) {
        return;
    }
    /* Player one only. Arming the CPU field too would make the measurement a
     * function of the AI's traffic rather than of the boost. */
    if (racer->playerIndex == PLAYER_COMPUTER || racer->racerIndex != 0) {
        return;
    }
    if (g_frameCounter < sFrame) {
        return;
    }
    sArmed = TRUE;
    racer->boostTimer = normalise_time(sTicks);
    racer->boostType = BOOST_LARGE;
    mdkr_trace("[BOOSTARM] frame=%d ticks=%d timer=%d", g_frameCounter, sTicks,
               racer->boostTimer);
}

void mdkr_boost_trace(Object *obj, Object_Racer *racer) {
    extern int g_frameCounter;
    static s32 sEnabled = -1;

    if (sEnabled < 0) {
        const char *e = getenv("MDKR_BOOST_TRACE");
        sEnabled = e != NULL && e[0] != '\0' && atoi(e) != 0;
    }
    if (!sEnabled || obj == NULL || racer == NULL) {
        return;
    }
    if (racer->playerIndex == PLAYER_COMPUTER || racer->racerIndex != 0) {
        return;
    }
    mdkr_trace("[BOOST] frame=%d timer=%d type=%d vel=%.9g x=%.9g y=%.9g z=%.9g "
               "surf=%d grounded=%d start=%d",
               g_frameCounter, racer->boostTimer, racer->boostType,
               racer->velocity, obj->trans.x_position, obj->trans.y_position,
               obj->trans.z_position, racer->wheel_surfaces[0],
               racer->groundedWheels, get_race_start_timer());
}
#endif

/**
 * Updates the racer FX object states.
 * This includes the shield wobble timer, and the texture frames for the magnet and shield.
 */
void racerfx_update(s32 updateRate) {
    s32 i;
    Object_Boost *boostObj;
    s32 temp;
    Object_Boost *asset20;
    f32 updateRateF;
    Object_Racer *racer;

    gBoostVertFlip = 1 - gBoostVertFlip;
    gNumOfBoostVerts = 0;
    gNumOfBoostTris = 0;
    asset20 = GET_BOOST_TABLE();
    gBoostObjOverrideID = 9;
    for (i = 0; i < NUMBER_OF_CHARACTERS; i++) {
        if (D_8011B068[i] && gBoostEffectObjects[i] != NULL) {
            gBoostEffectObjects[i]->properties.common.unk0 = 0;
        }
        D_8011B068[i] = TRUE;
    }
    for (i = 0; i < gNumRacers; i++) {
        updateRateF = (f32) updateRate;
        if (osTvType == OS_TV_TYPE_PAL) {
            updateRateF *= 1.2f;
        }
        racer = (*gRacers)[i]->racer;
        boostObj = &asset20[racer->racerIndex];
#ifdef NATIVE_PORT
        dkr_force_boost_hook(racer);
        dkr_force_shield_hook(racer);
        mdkr_zippad_boost_hook((*gRacers)[i], racer);
        mdkr_boost_trace((*gRacers)[i], racer);
#endif
        if (racer->shieldTimer != 0) {
            gShieldSineTime[racer->racerIndex] += updateRate;
        }
        boostObj->unk72 += updateRate;
        if (racer->boostTimer != 0) {
            boostObj->unk73 = 20;
            if (boostObj->unk70 == 0) {
                boostObj->unk74 += updateRateF * 0.25f;
                updateRateF = 0.0f;
                if (boostObj->unk74 > 2.4f) {
                    boostObj->unk74 = 4.8f - boostObj->unk74;
                    boostObj->unk70 = 1;
                }
            }
            if (boostObj->unk70 == 1) {
                boostObj->unk74 -= updateRateF * 0.25f;
                updateRateF = 0.0f;
                if (boostObj->unk74 < 1.0f) {
                    boostObj->unk70 = 2;
                    boostObj->unk74 = 1.0f - boostObj->unk74;
                }
            }
            if (boostObj->unk70 == 2) {
                if (boostObj->unk74 < 1.0f) {
                    boostObj->unk74 += updateRateF * 0.125f;
                    if (boostObj->unk74 > 1.0f) {
                        boostObj->unk74 = 1.0f;
                    }
                }
            }
        } else {
            if (boostObj->unk73 > 0) {
                boostObj->unk73 -= updateRate;
            } else {
                if (boostObj->unk70 == 2) {
                    boostObj->unk74 -= updateRateF * 0.05f;
                    updateRateF = 0.0f;
                    if (boostObj->unk74 < 0.0f) {
                        boostObj->unk70 = 0;
                        boostObj->unk74 += 1.0f;
                    }
                }
                if (boostObj->unk70 < 2) {
                    boostObj->unk74 -= (updateRateF * 0.1f);
                    if (boostObj->unk74 < 0.0f) {
                        boostObj->unk74 = 0.0f;
                    }
                    boostObj->unk70 = 0;
                }
            }
        }
        if ((boostObj->unk70 > 0) || (boostObj->unk74 > 0.0f)) {
            func_8000B750((*gRacers)[i], racer->racerIndex, racer->vehicleIDPrev, racer->boostType, 0);
        }
        temp = racer->racerIndex;
        gRacerFXData[temp].unk1 += updateRate;
        gRacerFXData[temp].unk2 += updateRate;
        if (racer->magnetTimer != 0) {
            if (gRacerFXData[temp].unk3 + (updateRate << 2) < 32) {
                gRacerFXData[temp].unk3 += (updateRate << 2);
            } else {
                gRacerFXData[temp].unk3 = 32;
            }
        } else {
            if (gRacerFXData[temp].unk3 - updateRate > 0) {
                gRacerFXData[temp].unk3 -= updateRate;
            } else {
                gRacerFXData[temp].unk3 = 0;
            }
        }
    }
    if (gMagnetEffectObject != NULL) {
        obj_tex_animate(gMagnetEffectObject, updateRate);
    }
}

/**
 * Returns the boost object with the given ID.
 * Returns a specific ID if the arg passed is BOOST_DEFAULT.
 */
Object *racerfx_get_boost(s32 boostID) {
    if (boostID == BOOST_DEFAULT) {
        boostID = gBoostObjOverrideID;
    }
    if (boostID < 0 || boostID >= NUMBER_OF_CHARACTERS) {
        return NULL;
    }
    return gBoostEffectObjects[boostID];
}

/**
 * Allocate memory for objects and object related systems.
 * This includes the objects themselves, particles, and all of the pointer lists for tracking objects.
 */
void allocate_object_pools(void) {
    s32 i;
#ifdef NATIVE_PORT
    s32 translationTableBytes;
#endif

    set_world_shading(0.67f, 0.33f, 0, -0x2000, 0);
    gObjectMemoryPool = (Object *) mempool_new_sub(OBJECT_POOL_SIZE, OBJECT_SLOT_COUNT);
    gParticlePtrList = mempool_alloc_safe(sizeof(uintptr_t) * 200, COLOUR_TAG_BLUE);
    gCollisionObjects = mempool_alloc_safe(sizeof(uintptr_t) * OBJECT_COLLISION_COUNT, COLOUR_TAG_BLUE);
    D_8011AE74 = mempool_alloc_safe(sizeof(uintptr_t) * 128, COLOUR_TAG_BLUE);
    gTrackCheckpoints = mempool_alloc_safe(sizeof(CheckpointNode) * MAX_CHECKPOINTS, COLOUR_TAG_BLUE);
    gCameraObjList = mempool_alloc_safe(sizeof(uintptr_t *) * CAMCONTROL_COUNT, COLOUR_TAG_BLUE);
    gRacers = mempool_alloc_safe(sizeof(uintptr_t) * 10, COLOUR_TAG_BLUE);
    gRacersByPort = mempool_alloc_safe(sizeof(uintptr_t) * 10, COLOUR_TAG_BLUE);
    gRacersByPosition = mempool_alloc_safe(sizeof(uintptr_t) * 10, COLOUR_TAG_BLUE);
    gAINodes = mempool_alloc_safe(sizeof(uintptr_t) * AINODE_COUNT, COLOUR_TAG_BLUE);
    gDrawbridgeTimers = mempool_alloc_safe(8, COLOUR_TAG_BLUE);
    D_8011AFF4 = mempool_alloc_safe(sizeof(unk800179D0) * 16, COLOUR_TAG_BLUE);
    gAssetsLvlObjTranslationTable = (s16 *) asset_table_load(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE);
#ifdef NATIVE_PORT
    translationTableBytes =
        asset_table_size(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE);
    if (translationTableBytes <= 0 ||
        translationTableBytes % (s32) sizeof(*gAssetsLvlObjTranslationTable) != 0) {
        fprintf(stderr,
                "[FATAL] level-object translation table has invalid byte size %d\n",
                translationTableBytes);
        abort();
    }
    gAssetsLvlObjTranslationTableCapacity =
        (size_t) translationTableBytes /
        sizeof(*gAssetsLvlObjTranslationTable);
    if (gAssetsLvlObjTranslationTable == NULL ||
        gAssetsLvlObjTranslationTableCapacity == 0) {
        fprintf(stderr,
                "[FATAL] level-object translation table is empty or failed to load\n");
        abort();
    }
#endif
#ifdef NATIVE_PORT
    gAssetsLvlObjTranslationTableLength =
        (s32) gAssetsLvlObjTranslationTableCapacity - 1;
#else
    gAssetsLvlObjTranslationTableLength =
        (asset_table_size(ASSET_LEVEL_OBJECT_TRANSLATION_TABLE) >> 1) - 1;
#endif
    while (gAssetsLvlObjTranslationTableLength > 0 &&
           gAssetsLvlObjTranslationTable[gAssetsLvlObjTranslationTableLength] == 0) {
        gAssetsLvlObjTranslationTableLength--;
    }
    gSpawnObjectHeap = mempool_alloc_safe(OBJECT_BLUEPRINT_SIZE, COLOUR_TAG_BLUE);
    gAssetsObjectHeadersTable = (s32 *) asset_table_load(ASSET_OBJECT_HEADERS_TABLE);
    gAssetsObjectHeadersTableLength = 0;
    while (-1 != gAssetsObjectHeadersTable[gAssetsObjectHeadersTableLength]) {
        gAssetsObjectHeadersTableLength++;
    }
    gAssetsObjectHeadersTableLength--;
    /* LP64: gLoadedObjectHeaders is an array of real `ObjectHeader *`, which is 4
     * bytes on N64 but 8 on a 64-bit host. The original hardcodes 4, so every
     * header index >= tableLength/2 (>= 152 of 304 on US v80) stored its pointer
     * PAST the end of this allocation — straight over gObjectHeaderReferences,
     * which mempool places immediately after it, and then over the allocations
     * after that. A splattered refcount makes load_object_header() take its
     * "already loaded" branch and hand back the never-written gLoadedObjectHeaders
     * slot (NULL / stale), which is the "freshly-loaded header reads back all
     * zeros" crash in spawn_object(). sizeof(ObjectHeader *) is 4 on N64, so this
     * expression is unchanged for the N64 build. */
    gLoadedObjectHeaders =
        mempool_alloc_safe(gAssetsObjectHeadersTableLength * (s32) sizeof(ObjectHeader *), COLOUR_TAG_WHITE);
    gObjectHeaderReferences = mempool_alloc_safe(gAssetsObjectHeadersTableLength, COLOUR_TAG_WHITE);

    for (i = 0; i < gAssetsObjectHeadersTableLength; i++) {
        (*gObjectHeaderReferences)[i] = 0;
        /* mempool does not zero; a stale slot must never be reachable even if a
         * refcount is ever wrong again (see try_free_object_header). */
        (*gLoadedObjectHeaders)[i] = NULL;
    }

    gAssetsMiscSection = (s32 *) asset_table_load(ASSET_MISC);
    gAssetsMiscTable = (s32 *) asset_table_load(ASSET_MISC_TABLE);
    gAssetsMiscTableLength = 0;
    while (-1 != gAssetsMiscTable[gAssetsMiscTableLength]) {
        gAssetsMiscTableLength++;
    }

#ifdef NATIVE_PORT
    /* Normalize the big-endian ASSET_MISC sub-assets exactly once, here, while the
     * section pointer is fresh. See dkr_misc_normalize_tables(). */
    dkr_misc_normalize_tables();
#endif
    decrypt_magic_codes(
        &gAssetsMiscSection[gAssetsMiscTable[ASSET_MISC_MAGIC_CODES]],
        (gAssetsMiscTable[ASSET_MISC_TITLE_SCREEN_DEMO_IDS] - gAssetsMiscTable[ASSET_MISC_MAGIC_CODES]) *
#ifdef NATIVE_PORT
            /* LP64: MISC-table entries are s32 word offsets, so the decrypted
             * byte length is (wordCount * 4). The original uses sizeof(s32 *),
             * which is 4 on N64 but 8 on a 64-bit host — doubling the length
             * and overrunning gAssetsMiscSection into the adjacent
             * gAssetsMiscTable allocation. Use sizeof(s32) (== 4 everywhere). */
            sizeof(s32));
#else
            sizeof(s32 *));
#endif
#ifdef NATIVE_PORT
    /* The cheat table's u16 index block is big-endian and ASSET_MISC is punted
     * at load, so menu.c reads gNumberOfCheats byte-reversed (us.v80: 29 ->
     * 7424) and every string offset likewise (176 -> 45056), indexing far past
     * a 1520-byte blob. It must be swapped HERE, after decryption: the count
     * that bounds the index block is only meaningful post-decrypt. The cipher
     * transposes bit pairs across each four-byte group, so byte-swapping before
     * decrypting is NOT equivalent and corrupts the plaintext. Decrypt first,
     * then swap only the plaintext index block; the ASCII strings stay bytes. */
    if (!asset_swap_misc_magic_codes(
            &gAssetsMiscSection[gAssetsMiscTable[ASSET_MISC_MAGIC_CODES]],
            (u32) get_misc_asset_size(ASSET_MISC_MAGIC_CODES))) {
        fprintf(stderr, "[FATAL] decrypted magic-code table failed structural validation\n");
        abort();
    }
#endif
    gObjPtrList = mempool_alloc_safe(sizeof(uintptr_t) * OBJECT_SLOT_COUNT, COLOUR_TAG_BLUE);
    gFirstTimeFinish = 0;
    gTimeTrialEnabled = 0;
    gIsTimeTrial = FALSE;
    gObjectUpdateRateF = 2.0f;
    clear_object_pointers();
}

// Decrypts cheats
void decrypt_magic_codes(s32 *data, s32 length) {
    s32 i;
    s32 j;
    u8 *ptr = (u8 *) data;
    u8 temp[4];

    for (i = 0; i < (length >> 2); i++) {
        // Swap bits according to the following pattern:
        // AABBCCDD EEFFGGHH IIJJKKLL MMNNOOPP -> AAEEIIMM BBFFJJNN CCGGKKOO DDHHLLPP
        // clang-format off
        temp[0] = ((ptr[0] & 0xC0)     ) |
                  ((ptr[1] & 0xC0) >> 2) |
                  ((ptr[2] & 0xC0) >> 4) |
                  ((ptr[3] & 0xC0) >> 6);
        temp[1] = ((ptr[0] & 0x30) << 2) |
                  ((ptr[1] & 0x30)     ) |
                  ((ptr[2] & 0x30) >> 2) |
                  ((ptr[3] & 0x30) >> 4);
        temp[2] = ((ptr[0] & 0x0C) << 4) |
                  ((ptr[1] & 0x0C) << 2) |
                  ((ptr[2] & 0x0C)     ) |
                  ((ptr[3] & 0x0C) >> 2);
        temp[3] = ((ptr[0] & 0x03) << 6) |
                  ((ptr[1] & 0x03) << 4) |
                  ((ptr[2] & 0x03) << 2) |
                  ((ptr[3] & 0x03)     );
        // clang-format on

        // Swap the odd and even bits
        for (j = 0; j < 4; j++) {
            *ptr++ = (((temp[j] & 0xAA) >> 1) | ((temp[j] & 0x55) << 1));
        }
    }
}

/**
 * Set all object counters and headers to zero, effectively telling the game there are no objects currently in the
 * scene.
 */
void clear_object_pointers(void) {
    s32 i;

    D_8011AD26 = TRUE;
    D_8011AD5C = 0;
    D_8011AD60 = 0;
    gFreeListCount = 0;
    gCollisionObjectCount = 0;
    gNumberOfMainCheckpoints = 0;
    gNumberOfTotalCheckpoints = 0;
    gNumRacers = 0;
    D_8011AE78 = 0;
    D_8011AD21 = 0;
    D_8011AD22[0] = 0;
    D_8011AD22[1] = 0;

    for (i = 0; i < AINODE_COUNT; i++) {
        (*gAINodes)[i] = NULL;
    }
    for (i = 0; i < 8; i++) {
        (*gDrawbridgeTimers)[i] = 0;
    }
    for (i = 0; i < 16; i++) {
        D_8011AFF4[i].unk0 = 0;
    }

    gAINodeTail[0] = 0xFF;
    gAINodeTail[1] = 0xFF;
    gObjectCount = 0;
    gObjectListStart = 0;
    gParticleCount = 0;
    D_8011AE88 = 0;
    D_8011ADD4 = 0;
    gCutsceneID = 0;
    D_8011AE7E = TRUE;
    gFirstActiveObjectId = 0;
    gTransformTimer = 0;
    gIsTajChallenge = FALSE;
    gTajRaceInit = 0;
    D_8011AF60 = NULL;
    D_8011AE00 = 0;
    D_8011AE01 = TRUE;
    D_8011AD53 = 0;
    gOverrideDoors = FALSE;
}

/**
 * Clear all objects from memory. Also clear rumble.
 * Swap lead player in adventure two if the other player finished ahead of the lead player.
 */
void free_all_objects(void) {
    s32 i, len;
#ifdef NATIVE_PORT
    /* Queue native Taj companions before this direct destruction walk. */
    taj_visual_reset();
#endif
    timetrial_free_staff_ghost();
    gIsP2LeadPlayer = FALSE;
    if (gRollingDemo) {
        rumble_init(TRUE);
    }
    gRollingDemo = FALSE;
    if (gSwapLeadPlayer && is_in_two_player_adventure()) {
        gSwapLeadPlayer = FALSE;
        swap_lead_player();
    }
    gParticlePtrList_flush();
    len = gObjectCount;
    for (i = 0; i < len; i++) {
        obj_destroy(gObjPtrList[i], 1);
    }
    gFreeListCount = 0;
    gObjectCount = 0;
    gObjectListStart = 0;
    clear_object_pointers();
    mempool_free((void *) gObjectMap[0]);
    mempool_free((void *) gObjectMap[1]);
}

/**
 * Set the object's header.
 * Search if the intended header is already loaded and use that.
 * Otherwise, allocate space and load it into ROM and set it to that.
 */
ObjectHeader *load_object_header(s32 index) {
    s32 assetOffset;
    s32 size;
    ObjectHeader *address;

    if ((*gObjectHeaderReferences)[index] != 0) {
#ifdef NATIVE_PORT
        /* Invariant: refcount != 0 implies a live header pointer. If this ever
         * trips, some writer has splattered the header table / refcount array
         * (that is exactly what the LP64 sizing bug in allocate_object_pools used
         * to do) — fail loudly rather than hand the caller a NULL/stale header
         * that only crashes later, deep inside spawn_object(). */
        if ((*gLoadedObjectHeaders)[index] == NULL) {
            fprintf(stderr,
                    "[FATAL] load_object_header(%d): refcount=%d but the header table slot is NULL - "
                    "the object-header table or refcount array has been corrupted.\n",
                    index, (*gObjectHeaderReferences)[index]);
            abort();
        }
#endif
        (*gObjectHeaderReferences)[index]++;
        return (*gLoadedObjectHeaders)[index];
    }
    assetOffset = gAssetsObjectHeadersTable[index];
    size = gAssetsObjectHeadersTable[index + 1] - assetOffset;
    address = mempool_alloc_pool((MemoryPoolSlot *) gObjectMemoryPool, size);
    if (address != NULL) {
        asset_load(ASSET_OBJECTS, (uintptr_t)address, assetOffset, size);
#ifdef NATIVE_PORT
        /* ObjectHeader is loaded big-endian; normalize before the offsets below
         * are read and patched into arena pointer tokens. */
        asset_swap_normalize(ASSET_OBJECTS, address, (u32) size);
#endif
        address->unk24 = DKR_TOK((u8 *) address + (u32) address->unk24);
        address->objectParticles =
            DKR_TOK((u8 *) address + (u32) address->objectParticles);
        address->vehiclePartIds =
            DKR_TOK((u8 *) address + (u32) address->vehiclePartIds);
        address->vehiclePartIndices =
            DKR_TOK((u8 *) address + (u32) address->vehiclePartIndices);
        address->modelIds = DKR_TOK((u8 *) address + (u32) address->modelIds);
        (*gLoadedObjectHeaders)[index] = address;
        (*gObjectHeaderReferences)[index] = 1;
    } else {
        return NULL;
    }
    return address;
}

/**
 * Remove this object from the loaded header's references.
 * If the reference number is zero, free the header.
 */
void try_free_object_header(s32 index) {
    if ((*gObjectHeaderReferences)[index] != 0) {
        (*gObjectHeaderReferences)[index]--;
        if ((*gObjectHeaderReferences)[index] == 0) {
            mempool_free((void *) (*gLoadedObjectHeaders)[index]);
#ifdef NATIVE_PORT
            /* The original leaves the freed pointer in the table. The refcount
             * alone gates the "already loaded" branch of load_object_header(), so
             * on N64 that is benign — but it means a single bad refcount byte
             * turns into a use-after-free instead of a clean reload. Null the slot
             * so the table can never hand out freed memory. */
            (*gLoadedObjectHeaders)[index] = NULL;
#endif
        }
    }
}

/**
 * Converts the passed value into an accurate countdown value based on the systems region.
 * Since PAL runs at 50Hz, it therefore will reduce the timer to 5/6 to match, keeping
 * it consistent with non PAL timers, running 60Hz.
 * Official Name: objTvTimes
 */
s32 normalise_time(s32 timer) {
    if (osTvType != OS_TV_TYPE_PAL || timer < 0) {
        return timer;
    } else {
        return (timer * 5) / 6;
    }
}

#ifdef NATIVE_PORT
/*
 * The object-map asset is big-endian on disk. The generic asset swap
 * (platform/asset_swap.c swap_level_object_map) only byteswaps the common
 * per-entry x/y/z at +0x02/+0x04/+0x06 — the type-specific body params (>= +0x08)
 * are polymorphic per object behaviour and were punted there. They are swapped
 * here instead, where each entry's behaviour is resolvable exactly the way
 * spawn_object() resolves it (translation table -> object header). Every field is
 * only swapped when it fits inside the entry's actual stride, so short entries are
 * never over-read. Field offsets/widths come from level_object_entries.h and the
 * behaviour->struct mapping is the obj_init dispatch (objects.c ~L10390).
 */
static size_t mdkr_level_object_min_size(s32 behaviorId) {
#define MDKR_ENTRY_SIZE(type) return sizeof(type)
    switch (behaviorId) {
        case BHV_RACER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Racer);
        case BHV_SCENERY:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Scenery);
        case BHV_FISH:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Fish);
        case BHV_ANIMATOR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Animator);
        case BHV_SMOKE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Smoke);
        case BHV_UNK_19:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Unknown25);
        case BHV_BOMB_EXPLOSION:
            MDKR_ENTRY_SIZE(LevelObjectEntry_BombExplosion);
        case BHV_EXIT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Exit);
        case BHV_AUDIO:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Audio);
        case BHV_AUDIO_LINE:
        case BHV_AUDIO_LINE_2:
            MDKR_ENTRY_SIZE(LevelObjectEntry_AudioLine);
        case BHV_AUDIO_REVERB:
            MDKR_ENTRY_SIZE(LevelObjectEntry_AudioReverb);
        case BHV_CAMERA_CONTROL:
            MDKR_ENTRY_SIZE(LevelObjectEntry_CameraControl);
        case BHV_SETUP_POINT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_SetupPoint);
        case BHV_DINO_WHALE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Dino_Whale);
        case BHV_CHECKPOINT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Checkpoint);
        case BHV_MODECHANGE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_ModeChange);
        case BHV_BONUS:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Bonus);
        case BHV_DOOR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Door);
        case BHV_TT_DOOR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_TTDoor);
        case BHV_FOG_CHANGER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_FogChanger);
        case BHV_AINODE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_AiNode);
        case BHV_WEAPON_BALLOON:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WeaponBalloon);
        case BHV_BALLOON_POP:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WBalloonPop);
        case BHV_WEAPON:
        case BHV_WEAPON_2:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Weapon);
        case BHV_SKY_CONTROL:
            MDKR_ENTRY_SIZE(LevelObjectEntry_SkyControl);
        case BHV_TORCH_MIST:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Torch_Mist);
        case BHV_TEXTURE_SCROLL:
            MDKR_ENTRY_SIZE(LevelObjectEntry_TexScroll);
        case BHV_STOPWATCH_MAN:
            MDKR_ENTRY_SIZE(LevelObjectEntry_StopWatchMan);
        case BHV_BANANA:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Banana);
        case BHV_LIGHT_RGBA:
            MDKR_ENTRY_SIZE(LevelObjectEntry_RgbaLight);
        case BHV_BUOY_PIRATE_SHIP:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Buoy_PirateShip);
        case BHV_LOG:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Log);
        case BHV_WEATHER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Weather);
        case BHV_BRIDGE_WHALE_RAMP:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Bridge_WhaleRamp);
        case BHV_RAMP_SWITCH:
            MDKR_ENTRY_SIZE(LevelObjectEntry_RampSwitch);
        case BHV_SEA_MONSTER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_SeaMonster);
        case BHV_LENS_FLARE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_LensFlare);
        case BHV_LENS_FLARE_SWITCH:
            MDKR_ENTRY_SIZE(LevelObjectEntry_LensFlareSwitch);
        case BHV_COLLECT_EGG:
            MDKR_ENTRY_SIZE(LevelObjectEntry_CollectEgg);
        case BHV_EGG_CREATOR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_EggCreator);
        case BHV_CHARACTER_FLAG:
            MDKR_ENTRY_SIZE(LevelObjectEntry_CharacterFlag);
        case BHV_ANIMATION:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Animation);
        case BHV_INFO_POINT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_InfoPoint);
        case BHV_TRIGGER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Trigger);
        case BHV_ZIPPER_WATER:
        case BHV_ZIPPER_AIR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_AirZippers_WaterZippers);
        case BHV_TIMETRIAL_GHOST:
            MDKR_ENTRY_SIZE(LevelObjectEntry_TimeTrial_Ghost);
        case BHV_WAVE_GENERATOR:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WaveGenerator);
        case BHV_BUTTERFLY:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Butterfly);
        case BHV_PARK_WARDEN:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Parkwarden);
        case BHV_WORLD_KEY:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WorldKey);
        case BHV_BANANA_SPAWNER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_BananaCreator);
        case BHV_TREASURE_SUCKER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_TreasureSucker);
        case BHV_LAVA_SPURT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_LavaSpurt);
        case BHV_POS_ARROW:
            MDKR_ENTRY_SIZE(LevelObjectEntry_PosArrow);
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_3:
            MDKR_ENTRY_SIZE(LevelObjectEntry_HitTester);
        case BHV_HIT_TESTER_2:
        case BHV_DYNAMIC_LIGHT_OBJECT_2:
        case BHV_HIT_TESTER_4:
            MDKR_ENTRY_SIZE(LevelObjectEntry_DynamicLightingObject);
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_3:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Unknown96);
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_4:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Snowball);
        case BHV_MIDI_FADE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_MidiFade);
        case BHV_MIDI_FADE_POINT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_MidiFadePoint);
        case BHV_MIDI_CHANNEL_SET:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Midichset);
        case BHV_EFFECT_BOX:
            MDKR_ENTRY_SIZE(LevelObjectEntry_EffectBox);
        case BHV_TROPHY_CABINET:
            MDKR_ENTRY_SIZE(LevelObjectEntry_TrophyCab);
        case BHV_BUBBLER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Bubbler);
        case BHV_FLY_COIN:
            MDKR_ENTRY_SIZE(LevelObjectEntry_FlyCoin);
        case BHV_GOLDEN_BALLOON:
            MDKR_ENTRY_SIZE(LevelObjectEntry_GoldenBalloon);
        case BHV_LASER_BOLT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Laserbolt);
        case BHV_LASER_GUN:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Lasergun);
        case BHV_ZIPPER_GROUND:
            MDKR_ENTRY_SIZE(LevelObjectEntry_GroundZipper);
        case BHV_OVERRIDE_POS:
            MDKR_ENTRY_SIZE(LevelObjectEntry_OverridePos);
        case BHV_WIZPIG_SHIP:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WizpigShip);
        case BHV_BOOST:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Boost);
        case BHV_SILVER_COIN:
            MDKR_ENTRY_SIZE(LevelObjectEntry_SilverCoin);
        case BHV_WARDEN_SMOKE:
            MDKR_ENTRY_SIZE(LevelObjectEntry_WardenSmoke);
        case BHV_UNK_5E:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Unknown94);
        case BHV_TELEPORT:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Teleport);
        case BHV_ROCKET_SIGNPOST:
        case BHV_ROCKET_SIGNPOST_2:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Lighthouse_RocketSignpost);
        case BHV_RANGE_TRIGGER:
            MDKR_ENTRY_SIZE(LevelObjectEntry_RangeTrigger);
        case BHV_FIREBALL_OCTOWEAPON:
        case BHV_FIREBALL_OCTOWEAPON_2:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Fireball_Octoweapon);
        case BHV_FROG:
            MDKR_ENTRY_SIZE(LevelObjectEntry_Frog);
        case BHV_SILVER_COIN_2:
            MDKR_ENTRY_SIZE(LevelObjectEntry_SilverCoinAdv2);
        case BHV_LEVEL_NAME:
            MDKR_ENTRY_SIZE(LevelObjectEntry_LevelName);
        default:
            MDKR_ENTRY_SIZE(LevelObjectEntryCommon);
    }
#undef MDKR_ENTRY_SIZE
}

/*
 * MDKR_OBJMAP_PROBE=1 -- one row per swapped body field, naming the value the
 * game would have consumed (raw, big-endian-as-read) and the value it consumes
 * now. This is the seam that produced the evidence for every case in the table
 * below: a field whose `raw` is a five-digit number and whose `swapped` is a
 * small plausible radius/count is a field this table was previously getting
 * wrong. Off by default; one cached getenv when off.
 */
static s32 mdkr_objmap_probe_enabled(void) {
    static s32 enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("MDKR_OBJMAP_PROBE");
        enabled = (v != NULL && v[0] == '1') ? 1 : 0;
    }
    return enabled;
}

/* Swap one 16-bit body field in place. `behaviorId` is carried only so the probe
 * above can name the row; it has no effect on the bytes. The `off + 2 <= stride`
 * guard is load-bearing: entries are variable-stride, so a field declared in the
 * struct may simply not be present in this level's record, and swapping past the
 * stride would corrupt the *next* entry. Never widen it. */
static void mdkr_objmap_swap_u16(s32 behaviorId, u8 *e, s32 off, s32 stride) {
    if (off + 2 <= stride) {
        u8 t = e[off];
        u16 before;
        u16 after;
        /* `before` is what the field decoded to with this case absent -- i.e.
         * exactly what every build before this table entry consumed. Read it
         * the way the game reads it: a host-order u16 load off the record. */
        memcpy(&before, e + off, sizeof(before));
        e[off] = e[off + 1];
        e[off + 1] = t;
        memcpy(&after, e + off, sizeof(after));
        if (mdkr_objmap_probe_enabled()) {
            fprintf(stderr, "[OBJSWAP] bhv=%d off=0x%02X stride=%d before=%u after=%u\n",
                    behaviorId, off, stride, (u32) before, (u32) after);
        }
    }
}

static void mdkr_objmap_swap_entry_body(s32 behaviorId, u8 *e, s32 stride) {
    switch (behaviorId) {
        case BHV_RACER: /* s16 angleZ, angleX, angleY, playerIndex */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            break;
        case BHV_AUDIO: /* u16 soundId, range */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            break;
        case BHV_AUDIO_LINE:
        case BHV_AUDIO_LINE_2: /* u16 soundID @0x0A, unkE @0x0E */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            break;
        case BHV_FOG_CHANGER: /* s16 near, far, switchTimer */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x10, stride);
            break;
        case BHV_TEXTURE_SCROLL: /* s16 textureIndex */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;
        case BHV_LIGHT_RGBA: /* s16 radius + unk10..unk1A */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x10, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x12, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x14, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x16, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x18, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x1A, stride);
            break;
        case BHV_WEATHER: /* s16 radius, unkA, unkC, unkE, unk12 */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x12, stride);
            break;
        case BHV_LENS_FLARE: /* s16 angleX, angleY */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            break;
        case BHV_LENS_FLARE_SWITCH: /* s16 radius */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;
        case BHV_CHARACTER_FLAG: /* s16 angleZ, radius, angleY, playerIndex */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            break;
        case BHV_ANIMATION: /* s16 objectIdToSpawn @0x0C, animationStartDelay @0x0E, pauseFrameCount @0x24 */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x24, stride);
            break;
        case BHV_WAVE_GENERATOR: /* u16 waveSize @0x0A, unkC @0x0C, unkE @0x0E */
            /* Missing from this table since bring-up: all three u16s were
             * decoded byte-swapped. Measured on Boulder Canyon (level 19):
             * waveSize 30977 for 377, unkC 10243 (amplitude 640 world units)
             * for 808 (50.5), unkE 26882 for a 617-unit wavelength. Level 34's
             * two generators corroborate: unkC decoded as 32769 (amplitude
             * 2048) and 1 (amplitude 0.06) where the authored values are 384
             * (24.0) and 256 (16.0). unk8/unk9/unk10/unk11 are u8 and were
             * never wrong.
             *
             * NOTE, measured 2026-07-30, do not repeat the earlier claim that
             * this is what made Boulder Canyon's water "gigantic": it is not,
             * because today the swell never reaches the surface at all.
             * wavegen_add's [WAVEGEN-ADD] row reports `reg=0 count=0` on both
             * level 19 and level 34 -- wavegen_register finds no free slot
             * because func_800BBF78 derives gWaveGenObjs with pointer
             * arithmetic on `WaveGen *` (`gWaveGenList + sizeof(WaveGen *) * 8`,
             * i.e. 64 whole WaveGen structs, measured 0x1000 bytes) and lands
             * on memory that later wave-block setup overwrites, so all 32 slots
             * read back non-NULL. gWaveGenCount therefore stays 0, waves_get_y
             * returns early, and the water is flat whatever these fields say.
             * A/B against the parent binary on level 19 is byte-identical in
             * both the frame dump and the [SIMHASH] stream, exactly as that
             * predicts. This case is still correct and still required -- the
             * decode was wrong -- but its visible payoff is gated behind that
             * separate, pre-existing registration defect, which is deliberately
             * NOT fixed here: turning the swell system on for the first time is
             * its own change and needs its own evidence. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0E, stride);
            break;
        case BHV_WAVE_POWER: /* u16 radius @0x08, power @0x0A, divisor @0x0C */
            /* Same omission, same class: obj_loop_wavepower divides power by
             * 256 into the global wave magnitude and ramps over divisor ticks,
             * so a swapped power would arm the ramp with a nonsense target.
             * Unmeasured in practice: a 64-level probe sweep found no level
             * that spawns BHV_WAVE_POWER, so this case has no observed
             * instance and no A/B evidence. It is here because the decode is
             * wrong if one ever does spawn, not because it fixes something
             * seen. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0C, stride);
            break;

        /* ---- class sweep: the remaining behaviours whose bodies carry
         * multi-byte fields that game code actually reads. Each was omitted for
         * the same reason the wave pair was, and each was consuming a
         * byte-swapped value on every level that spawns it. Enumerated by
         * walking every LevelObjectEntry_* struct in level_object_entries.h for
         * a u16/s16/u32/s32/f32 at offset >= 0x08, mapping it to its behaviour
         * through run_object_init_func's dispatch, and grepping the reader. */

        case BHV_FISH: /* u16 unk8 @0x08 -- orbit radius (Object_Fish.unk114, f32) */
            /* obj_init_fish: fish->unk114 = fishEntry->unk8, then the swim
             * position is sins_f(phase) * unk114 -- a swapped radius flings the
             * fish a whole map away from the pond it was authored in. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;
        case BHV_BUTTERFLY: /* u16 unk8 @0x08 -- home/aggro radius */
            /* obj_loop_butterfly passes unk8 straight to obj_dist_racer() as the
             * search radius and uses (unk8 * 2) as the leash distance. Swapped,
             * the leash is effectively the whole level: butterflies chase racers
             * across the map and never return to their authored perch. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;
        case BHV_BUBBLER: /* u16 particleDensity @0x0A */
            /* obj_init_bubbler stores it into ObjPropertyBubbler.unk0, which
             * obj_loop_bubbler compares against rand_range(0, 1024) each tick.
             *
             * This one looked wrong at first and is not. Recorded in full so
             * nobody re-opens it. Exactly one BHV_BUBBLER record exists in the
             * whole game (level 9) and its two bytes are 04 01, so the declared
             * big-endian u16 is 1025; decoded the old way it was 260. Every
             * other field in this sweep gets *more* plausible when swapped, but
             * rand_range(0, 1024) is inclusive (span = max - min + 1), so its
             * ceiling is 1024 and 1025 makes the comparison always true --
             * which reads like it defeats the "variable density" roll the
             * decomp comment describes.
             * Settled against the upstream decomp (Diddy-Kong-Racing
             * 38d7f9ba, include/level_object_entries.h): particleDensity is
             * u16 at 0x0A there too, and obj_init_bubbler/obj_loop_bubbler are
             * character-for-character with the vendored copy. That decomp is a reading
             * of the N64 binary, where this map is consumed big-endian with no
             * swap at all -- so on real hardware these bytes are 0x0401 = 1025
             * and the emitter genuinely does run every tick. 1025 is an
             * authored "always on", not a decode error; the comment describes
             * the mechanism, not this object's authored value. The swap
             * reproduces console behaviour exactly.
             * Measured consequence: [SIMHASH] on level 9 diverges from the
             * parent binary from tick 3, because a permanently-enabled emitter
             * draws from the RNG every tick. That divergence is the port
             * becoming console-correct. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            break;
        case BHV_RANGE_TRIGGER: /* u16 radius @0x08, particleFlags @0x0A */
            /* obj_loop_rangetrigger: radius is the obj_dist_racer() range that
             * gates the emitter, and particleFlags is assigned wholesale to
             * obj->particleEmittersEnabled -- so a swapped entry both mistimes
             * the trigger and enables the wrong emitter bits. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            break;
        case BHV_FROG: /* s16 homeRadius @0x08 */
            /* obj_init_frog: frog->homeRadius, squared into homeRadiusSquare.
             * Swapped, the square overflows the authored leash and the frog
             * wanders without bound (and drumstick's frog with it). */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;
        case BHV_MIDI_FADE_POINT: /* u16 unk8 @0x08 (inner), unkA @0x0A (outer) */
            /* obj_init_midifadepoint uses both as distance thresholds for the
             * music crossfade AND divides each by the model's vertex radius to
             * derive obj->trans.scale. Swapped, the fade region is nonsense and
             * the object's scale with it. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            mdkr_objmap_swap_u16(behaviorId, e, 0x0A, stride);
            break;
        case BHV_MIDI_CHANNEL_SET: /* u16 unk8 @0x08 -- channel mask */
            /* obj_init_midichset: Object_MidiChannelSet.unk0, itself a u16. */
            mdkr_objmap_swap_u16(behaviorId, e, 0x08, stride);
            break;

        /* Deliberately NOT swapped, recorded so the next sweep does not have to
         * re-derive it:
         *   LevelObjectEntry_Hud (s32 offsetY @0x08) -- never read from the
         *     object map; game_ui.c's hud_element_render builds one on the
         *     stack in native byte order.
         *   LevelObjectEntry8000E2B4 (s16 @0x08..0x0E) -- same, built on the
         *     stack by transform_player_vehicle().
         *   LevelObjectEntry_Unk8000CC7C (s16 @0x08..0x0E) -- no reader
         *     anywhere in the tree; a decomp placeholder.
         *   LevelObjectEntry_CharacterSelect (s16 unk24 @0x24) -- no reader;
         *     BHV_CHARACTER_SELECT is not even in mdkr_level_object_min_size,
         *     so it resolves to the 8-byte common view and has no body. */
        default:
            break;
    }
}

/* Resolve a header type -> behaviourId. An object header's behaviour is a static
 * property of the asset, so the result is cached for the whole program lifetime:
 * load_object_header() gzip-decompresses on a cache miss, so resolving per map
 * entry (with many repeats, every level load) is prohibitively slow. With the
 * cache each unique header type is decompressed at most once ever, and since the
 * spawn loop loads the same headers anyway this adds ~no cost. -3 = unknown. */
#define MDKR_OBJ_BHV_CACHE_N 1024
static s16 mdkr_obj_behavior_of(s16 hdrType) {
    static s16 cache[MDKR_OBJ_BHV_CACHE_N];
    static s32 inited = 0;
    ObjectHeader *hdr;
    s16 bhv;
    if (!inited) {
        s32 i;
        for (i = 0; i < MDKR_OBJ_BHV_CACHE_N; i++) {
            cache[i] = -3;
        }
        inited = 1;
    }
    if (hdrType < 0 || hdrType >= MDKR_OBJ_BHV_CACHE_N) {
        return -1;
    }
    if (cache[hdrType] != -3) {
        return cache[hdrType];
    }
    hdr = load_object_header(hdrType);
    if (hdr == NULL) {
        cache[hdrType] = -1;
        return -1;
    }
    bhv = hdr->behaviorId;
    try_free_object_header(hdrType); /* keep the refcount balanced (spawn loop reloads). */
    cache[hdrType] = bhv;
    return bhv;
}

/*
 * Walk the freshly inflated, common-field-normalized object map and byteswap
 * each behaviour-specific body before the spawn loop reads it. This is also
 * the representation boundary: no typed LevelObjectEntry view is formed until
 * every embedded stride, object id, translated header, and behaviour minimum
 * has been checked against the actual remaining byte count.
 */
static s32 mdkr_objmap_swap_bodies(u8 *entries, s32 totalSize) {
    size_t walked = 0;

    if (entries == NULL || totalSize < 0) {
        fprintf(stderr,
                "[FATAL] level-object map has invalid buffer/size (%p, %d)\n",
                (void *) entries, totalSize);
        return FALSE;
    }

    while (walked < (size_t) totalSize) {
        MdkrLevelObjectRecordView view;
        MdkrLevelObjectRecordError error;
        s16 hdrType;
        s32 behaviorId;
        size_t minimumSize;

        error = mdkr_level_object_record_parse(
            entries + walked,
            (size_t) totalSize - walked,
            gAssetsLvlObjTranslationTableCapacity,
            &view);
        if (error != MDKR_LEVEL_OBJECT_RECORD_OK) {
            fprintf(stderr,
                    "[FATAL] invalid level-object record at map byte %zu/%d: %s\n",
                    walked, totalSize,
                    mdkr_level_object_record_error_string(error));
            return FALSE;
        }

        hdrType = gAssetsLvlObjTranslationTable[view.object_id];
        if (hdrType < 0 || hdrType >= gAssetsObjectHeadersTableLength) {
            fprintf(stderr,
                    "[FATAL] level-object id %u at map byte %zu resolves to "
                    "invalid header %d (count=%d)\n",
                    view.object_id, walked, hdrType,
                    gAssetsObjectHeadersTableLength);
            return FALSE;
        }

        behaviorId = mdkr_obj_behavior_of(hdrType);
        if (behaviorId < 0) {
            fprintf(stderr,
                    "[FATAL] level-object id %u at map byte %zu could not "
                    "resolve header %d\n",
                    view.object_id, walked, hdrType);
            return FALSE;
        }

        minimumSize = mdkr_level_object_min_size(behaviorId);
        if (view.size < minimumSize) {
            fprintf(stderr,
                    "[FATAL] level-object id %u behavior %d at map byte %zu "
                    "has %zu bytes; initializer requires at least %zu\n",
                    view.object_id, behaviorId, walked, view.size,
                    minimumSize);
            return FALSE;
        }

        mdkr_objmap_swap_entry_body(
            behaviorId, entries + walked, (s32) view.size);
        walked += view.size;
    }

    return walked == (size_t) totalSize;
}
#endif

/**
 * Load the object map into RAM, then start spawning objects into the world.
 * Also decides whether this race type should be for silver coins or not.
 */
void track_spawn_objects(s32 mapID, s32 index) {
    s32 assetSize;
    Settings *settings;
    s32 i;
    s32 *mem;
    s32 var_s0;
    u32 assetOffset;
    u32 *objMapTable;
    UNUSED s32 pad;
    u8 *compressedAsset;
    s32 temp_t3;
#ifdef NATIVE_PORT
    s32 decompressedSize;
    size_t inflatedSize;
#endif

    settings = get_settings();
    assetOffset = settings->bosses | 0x820; // 0x820 = Wizpig 2 and some unknown 0x800 boss bit
    gIsSilverCoinRace = ((settings->courseFlagsPtr[settings->courseId] & RACE_CLEARED_SILVER_COINS) == FALSE) &&
                        (((1 << settings->worldId) & assetOffset) != 0);

    if ((settings->courseFlagsPtr[settings->courseId] & RACE_CLEARED) == FALSE) {
        gIsSilverCoinRace = FALSE;
    }
    if (is_in_tracks_mode()) {
        gIsSilverCoinRace = FALSE;
    }
    if (level_type()) {
        gIsSilverCoinRace = FALSE;
    }

    D_8011AD3E = 0;
    mem = mempool_alloc_safe(OBJECT_MAP_SIZE, COLOUR_TAG_BLUE);
    gObjectMap[index] = mem;
    gObjectMapSpawnList[index] = (u8 *) (gObjectMap[index] + OBJ_MAP_HEADER_S32S);
    gObjectMapSize[index] = 0;
    gObjectMapID[index] = mapID;
    objMapTable = (u32 *) asset_table_load(ASSET_LEVEL_OBJECT_MAPS_TABLE);
    for (i = 0; objMapTable[i] != 0xFFFFFFFF; i++) {}
    i--;
    if (mapID >= i) {
        mapID = 0;
    }
    assetOffset = objMapTable[mapID];
    assetSize = objMapTable[mapID + 1] - assetOffset;

    if (assetSize != 0) {
        compressedAsset = (u8 *) mem;
#ifdef NATIVE_PORT
        decompressedSize =
            gzip_size_uncompressed(ASSET_LEVEL_OBJECT_MAPS, assetOffset);
        if (decompressedSize < (s32) (OBJ_MAP_HEADER_S32S * sizeof(s32)) ||
            decompressedSize > OBJECT_MAP_SIZE - 0x20 ||
            assetSize < 0 ||
            assetSize > decompressedSize + 0x20) {
            fprintf(stderr,
                    "[FATAL] object map %d has invalid compressed/inflated "
                    "sizes (%d -> %d; buffer=%d)\n",
                    mapID, assetSize, decompressedSize, OBJECT_MAP_SIZE);
            abort();
        }
        compressedAsset =
            compressedAsset + decompressedSize - assetSize + 0x20;
#else
        compressedAsset =
            ((compressedAsset + gzip_size_uncompressed(ASSET_LEVEL_OBJECT_MAPS, assetOffset)) - (0, assetSize)) + 0x20;
#endif
        asset_load(ASSET_LEVEL_OBJECT_MAPS, (uintptr_t)compressedAsset, assetOffset, assetSize);
        gzip_inflate(compressedAsset, (u8 *) mem);
#ifdef NATIVE_PORT
        if (gzip_inflate_output < (u8 *) mem ||
            gzip_inflate_output > (u8 *) mem + decompressedSize) {
            fprintf(stderr,
                    "[FATAL] object map %d inflater ended outside its %d-byte "
                    "destination\n",
                    mapID, decompressedSize);
            abort();
        }
        inflatedSize = (size_t) (gzip_inflate_output - (u8 *) mem);
        if (inflatedSize < OBJ_MAP_HEADER_S32S * sizeof(s32)) {
            fprintf(stderr,
                    "[FATAL] object map %d inflated to only %zu bytes\n",
                    mapID, inflatedSize);
            abort();
        }
        /* Decompressed object map is still big-endian; normalize the header
         * fileSize + per-entry x/y/z before the spawn loop reads them. */
        asset_swap_normalize(
            ASSET_LEVEL_OBJECT_MAPS, mem, (u32) inflatedSize);
#endif
        mempool_free(objMapTable);
        gObjectMapSpawnList[index] = (u8 *) (gObjectMap[index] + OBJ_MAP_HEADER_S32S);
        gObjectMapSize[index] = *mem;
        gObjectMapIndex = index;
#ifdef NATIVE_PORT
        if (gObjectMapSize[index] < 0 ||
            (size_t) gObjectMapSize[index] >
                inflatedSize - OBJ_MAP_HEADER_S32S * sizeof(s32)) {
            fprintf(stderr,
                    "[FATAL] object map %d declares %d entry bytes but only "
                    "%zu were inflated after its header\n",
                    mapID, gObjectMapSize[index],
                    inflatedSize - OBJ_MAP_HEADER_S32S * sizeof(s32));
            abort();
        }
        if (!mdkr_objmap_swap_bodies(
                (u8 *) (gObjectMap[index] + OBJ_MAP_HEADER_S32S),
                gObjectMapSize[index])) {
            abort();
        }
#endif
        for (var_s0 = 0; var_s0 < gObjectMapSize[index]; var_s0 += temp_t3) {
#ifdef NATIVE_PORT
            /* The complete stream was validated above; capture the validated
             * stride before spawn callbacks can mutate unrelated map state. */
            temp_t3 = gObjectMapSpawnList[index][1] & 0x3F;
#endif
            spawn_object((LevelObjectEntryCommon *) gObjectMapSpawnList[index], OBJECT_SPAWN_UNK01);
#ifdef NATIVE_PORT
            gObjectMapSpawnList[index] =
                &gObjectMapSpawnList[index][temp_t3];
#else
            gObjectMapSpawnList[index] = &gObjectMapSpawnList[index][temp_t3 = gObjectMapSpawnList[index][1] & 0x3F];
#endif
        }
        gObjectMapSpawnList[index] = (u8 *) (gObjectMap[index] + OBJ_MAP_HEADER_S32S);
        gCollisionObjectCount = 0;
        gNumFinishedRacers = 1;
        if (gPathUpdateOff == FALSE) {
            gParticlePtrList_flush();
            checkpoint_update_all();
            spectate_update();
            func_8001E93C();
        }
        gPathUpdateOff = TRUE;
    }
}

// Reset all values of D_8011AE08 to NULL
void func_8000CBC0(void) {
    s32 i; // Required to be one line to match
    // clang-format off
    for (i = 0; i < ARRAY_COUNT(D_8011AE08); i++) { D_8011AE08[i] = NULL; }
    // clang-format on
}

// Set the object value for the given index if it's not already set
void func_8000CBF0(Object *obj, s32 index) {
    if (D_8011AE08[index] == NULL) {
        D_8011AE08[index] = obj;
    } else {
        if (D_8011AE08[index]) {}
    }
}

// Set the next available value in D_8011AE08, and return it's index value. -1 if it's not set.
s32 func_8000CC20(Object *obj) {
    s32 i;
    s32 NextFreeIndex;

    NextFreeIndex = -1;
    for (i = 0; i < ARRAY_COUNT(D_8011AE08); i++) {
        if (D_8011AE08[i] == NULL) {
            NextFreeIndex = i;
            i = ARRAY_COUNT(D_8011AE08); // Why not just break?
        }
    }
    if (NextFreeIndex != -1) {
        D_8011AE08[NextFreeIndex] = obj;
    }
    return NextFreeIndex;
}

/**
 * Takes the level header and decides which race type to activate.
 * Sets up the racer spawning. Initialising vehicle types, racer count, then
 * spawning them into the world in their assigned order.
 */
void track_setup_racers(Vehicle vehicle, u32 entranceID, s32 playerCount) {
    LevelObjectEntry_Racer *racerEntry;
    Object *curObj;
    s32 numPlayers;
    Object *racerObj;
    LevelObjectEntry *objEntry;
    enum GameMode gameMode;
    Object_Racer *curRacer;
    s32 cutsceneID;
    s32 spawnObjFlags;
    s8 *miscAsset16;
    s8 sp127;
    u8 spawnedObjCount;
    s16 objectID;
    s8 racerActive[8];
    s8 racerIDs[8];
    s32 spawnX[8];
    s32 spawnY[8];
    s32 spawnZ[8];
    s32 spawnAngle[8];
    Settings *settings;
    s32 j;
    s32 var_s4;
    s32 i;
    s32 k;
    u8 raceType;
    LevelHeader *levelHeader;
    Camera *cutsceneCameraSegment;
#ifdef NATIVE_PORT
    Vehicle requestedVehicle = vehicle;
#endif

#ifdef NATIVE_PORT
    /* Racer slots and their backing addresses are reused between levels. */
    taj_physics_reset();
    taj_mod_begin_racer_bindings();
#endif
    D_8011AD20 = FALSE;
    gEventCountdown = 0;
    gFirstTimeFinish = FALSE;
    gNumRacers = 0;
    D_8011AF00 = 0;
    set_taj_status(TAJ_WANDER);
    levelHeader = level_header();
    raceType = levelHeader->race_type;
    if (raceType == RACETYPE_CUTSCENE_1 || raceType == RACETYPE_CUTSCENE_2) {
        return;
    }
    if (raceType == RACETYPE_BOSS || raceType & RACETYPE_CHALLENGE) {
        gIsTimeTrial = FALSE;
        gTimeTrialEnabled = FALSE;
    }
    cutsceneID = -1;
    if (is_time_trial_enabled() && raceType == RACETYPE_DEFAULT) {
        cutsceneCameraSegment = cam_get_cameras();
        cutsceneID = cutsceneCameraSegment->zoom;
        cutsceneCameraSegment->zoom = 1;
    }
    gameMode = get_game_mode();
    settings = get_settings();
    miscAsset16 = (s8 *) get_misc_asset(ASSET_MISC_3);
    gPrevTimeTrialVehicle = D_8011ADC5;
    if (!(settings->courseFlagsPtr[settings->courseId] &
          RACE_VISITED)) { // Check if the player has not visited the course yet.
        settings->courseFlagsPtr[settings->courseId] |= RACE_VISITED;
        D_8011AF00 = 2;
    }
    if (raceType != RACETYPE_DEFAULT) {
        D_8011AF00 = 2;
    }
    for (j = 0; j < ARRAY_COUNT(spawnZ); j++) {
        spawnX[j] = 0;
        spawnY[j] = 0;
        spawnZ[j] = 0;
#ifdef NATIVE_PORT
        /*
         * spawnAngle[] is the fourth member of this quartet and the only one the
         * original loop leaves uninitialised, while both readers below --
         * `racerEntry->angleY = spawnAngle[i]` and
         * `racerObj->trans.rotation.y_rotation = spawnAngle[i]` -- read it
         * unconditionally for every racer index. The writer only fires for a
         * BHV_SETUP_POINT that matches BOTH this entranceID and this racerIndex,
         * so a level whose object map has no such setup point hands the racer a
         * heading read straight off the host stack.
         *
         * On the N64 that stack slot held whatever the previous call left there,
         * which was the SAME thing on every boot, so the bug was invisible: the
         * console is deterministic even when it is wrong. On a hosted process it
         * is not -- the slot moves with ASLR, the environment block and the
         * dynamic loader -- so the racer's heading differed run to run and the
         * authoritative [SIMHASH] stream diverged from ITSELF. Measured on
         * levels 41 and 54 (Smokey 1/2), which have no matching setup point:
         * three runs gave 0x038cf920 / 0x03083916 / 0x02e83920 into an s16 field,
         * against x = y = z = 0 from the zeroed siblings -- a racer parked at the
         * world origin facing a different way each process.
         *
         * Zeroing it here is the minimal repair and matches its three siblings
         * exactly: where a setup point exists the value is overwritten and
         * nothing changes, and where none exists the racer now faces a defined
         * direction instead of an undefined one. The original build is untouched.
         */
        spawnAngle[j] = 0;
#endif
    }
    for (j = 0; j < gObjectCount; j++) {
        racerObj = gObjPtrList[j];
        if (!(racerObj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            if (racerObj->behaviorId == BHV_SETUP_POINT) {
                if (entranceID == racerObj->properties.setupPoint.entranceID) {
                    if (racerObj->properties.setupPoint.racerIndex < 8) {
                        spawnX[racerObj->properties.setupPoint.racerIndex] = racerObj->trans.x_position;
                        spawnY[racerObj->properties.setupPoint.racerIndex] = racerObj->trans.y_position;
                        spawnZ[racerObj->properties.setupPoint.racerIndex] = racerObj->trans.z_position;
                        spawnAngle[racerObj->properties.setupPoint.racerIndex] = racerObj->trans.rotation.y_rotation;
                    }

                    objEntry = racerObj->level_entry;
                    if (objEntry->setupPoint.vehicle != -1) {
                        vehicle = objEntry->setupPoint.vehicle;
                    }
                }
            }
        }
    }
    D_8011ADC5 = vehicle; // UB if all setup points don't have an assigned vehicle ID.
    gPrevTimeTrialVehicle = D_8011ADC5;
    numPlayers = playerCount + 1;
    gNumRacers = 8;
    gTwoActivePlayersInAdventure = FALSE;
    if (race_is_adventure_2P()) {
        numPlayers = 2;
        gTwoActivePlayersInAdventure = TRUE;
        set_scene_viewport_num(VIEWPORT_LAYOUT_2_PLAYERS);
    }
    if (raceType == RACETYPE_HUBWORLD) {
        gTimeTrialEnabled = 0;
    }

    gIsTimeTrial = gTimeTrialEnabled;
    if (gIsTimeTrial) {
        raceType = RACETYPE_HUBWORLD; // ???
    }
    if (raceType == RACETYPE_HUBWORLD || numPlayers >= 3) {
        gNumRacers = numPlayers;
        if (level_properties_get() == 0 && D_800DC708 != 0) {
            spawnAngle[0] += D_800DC708;
            D_800DC708 = 0;
        }
    } else if (numPlayers == 2) {
        gNumRacers = get_multiplayer_racer_count();
    }

    if (raceType & RACETYPE_CHALLENGE) {
        gNumRacers = 4;
    }
    D_8011AD3C = 0;
    if (raceType == RACETYPE_BOSS) {
        gNumRacers = 2;
        numPlayers = 1;
        D_8011AD3C = 1;
    }
    gRollingDemo = FALSE;
    if (gameMode == GAMEMODE_MENU && raceType == RACETYPE_DEFAULT) {
        gNumRacers = 6;
        gRollingDemo = TRUE;
        D_8011AD3C = 2;
    }
#ifdef NATIVE_PORT
    if (mdkr_trace_enabled() && gRollingDemo) {
        mdkr_trace("demo_vehicle: level=%d requested=%d path=%d racer=%d",
                   (int) settings->courseId, (int) requestedVehicle,
                   (int) D_8011ADC5, (int) levelHeader->vehicle);
    }
#endif

    /* The whole array, not just [0, gNumRacers): the reads below are indexed by
     * settings->racers[i].starting_position, and GCC (-O3, mingw-w64) cannot
     * prove gNumRacers covers them — it reports racerActive as possibly
     * uninitialized. Zeroing all 8 slots is a superset of the original loop
     * (FALSE == 0) and removes the indeterminate-read UB outright. */
    for (i = 0; i < ARRAY_COUNT(racerActive); i++) {
        racerActive[i] = FALSE;
    }

    spawnedObjCount = 0;
    // Mark active for human players.
    for (i = 0; i < numPlayers; i++) {
        if (settings->racers[i].starting_position < gNumRacers) {
            if (racerActive[settings->racers[i].starting_position] == FALSE) {
                racerActive[settings->racers[i].starting_position] = TRUE;
                continue;
            }
        }
        racerIDs[spawnedObjCount++] = i;
    }

    // Mark active for computer players.
    for (i = numPlayers; i < gNumRacers; i++) {
        if (settings->racers[i].starting_position < gNumRacers) {
            if (racerActive[settings->racers[i].starting_position] == FALSE) {
                racerActive[settings->racers[i].starting_position] = TRUE;
                continue;
            }
        }
        racerIDs[spawnedObjCount++] = i;
    }

    // Assign spawn locations for active racers.
    for (i = 0; i < spawnedObjCount; i++) {
        for (j = 0; j < gNumRacers; j++) {
            if (racerActive[j] == FALSE) {
                racerActive[j] = TRUE;
                settings->racers[racerIDs[i]].starting_position = j;
                j = gNumRacers; // Break
            }
        }
    }
    racerEntry = mempool_alloc_safe(sizeof(LevelObjectEntry_Racer), COLOUR_TAG_YELLOW);
    racerEntry->angleY = 0;
    racerEntry->angleX = 0;
    racerEntry->angleZ = 0;
    if (levelHeader->vehicle == VEHICLE_CAR) {
        gIsNonCarRacers = FALSE;
    } else {
        gIsNonCarRacers = TRUE;
    }
    D_8011AD24[1] = levelHeader->bossRaceID;
    sp127 = -1;
    for (i = 0; i < gNumRacers; i++) {
        if (raceType != RACETYPE_HUBWORLD && !(raceType & RACETYPE_CHALLENGE) && D_8011AD3C == 0) {
            j = 0;
            var_s4 = j;
            do {
                if (i == settings->racers[j].starting_position) {
                    var_s4 = j;
                    j = gNumRacers;
                }
                j++;
            } while (j < gNumRacers);
        } else {
            var_s4 = i;
        }
        racerEntry->playerIndex = var_s4 < numPlayers ? var_s4 : 4;
        if (raceType != RACETYPE_HUBWORLD || racerEntry->playerIndex != 4) {
            if (D_8011AD3C == 1) {
                if (i == 0) {
                    vehicle = gBossVehicles[D_8011AD24[1]].playerVehicle;
                } else {
                    vehicle = gBossVehicles[D_8011AD24[1]].bossVehicle;
                }
            } else if (D_8011AD3C == 2) {
                vehicle = levelHeader->vehicle;
            } else {
                if (racerEntry->playerIndex == 4 || race_is_adventure_2P()) {
                    vehicle = get_player_selected_vehicle(PLAYER_ONE);
                } else if (numPlayers >= 2) {
                    vehicle = get_player_selected_vehicle(racerEntry->playerIndex);
                }
            }

            // Are these assignments correct? Seems weird.
            if (D_8011AD3C == 2) {
                objectID = gRacerObjectTable[D_800DC840[i] + (vehicle * NUM_CHARACTERS)];
            } else if (vehicle < 5) {
                objectID = gRacerObjectTable[(settings->racers[var_s4].character) + (vehicle * NUM_CHARACTERS)];
            } else {
                objectID = gRacerObjectTable[vehicle + 45];
            }

            racerEntry->common.objectID = objectID;
            racerEntry->common.size = ((objectID & 0x100) >> 1) | 0x10;
            racerEntry->common.x = spawnX[i];
            racerEntry->common.y = spawnY[i];
            racerEntry->common.z = spawnZ[i];
            racerEntry->angleY = spawnAngle[i];
            if (racerEntry->playerIndex == 4) {
                model_anim_offset(1);
            }

            spawnObjFlags = OBJECT_BEHAVIOUR_SHADED;
            if (racerEntry->playerIndex == 4) {
                spawnObjFlags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_WATER_EFFECT;
                // @fake but required for the | OBJECT_BEHAVIOUR_ANIMATION below to work
                if (1) {
                    spawnObjFlags |= 0;
                }
                if (numPlayers >= 2) {
                    spawnObjFlags |= OBJECT_BEHAVIOUR_ANIMATION;
                }
            }
            if (racerEntry->playerIndex != 4) {
                if (numPlayers == 1) {
                    spawnObjFlags |= OBJECT_BEHAVIOUR_INTERACTIVE;
                }
            }
            if (vehicle >= VEHICLE_BOSSES) {
                spawnObjFlags = OBJECT_BEHAVIOUR_SHADED;
                model_anim_offset(0);
            }
            racerObj = spawn_object((LevelObjectEntryCommon *) racerEntry, spawnObjFlags);
#ifdef NATIVE_PORT
            /* The original assumes a racer can always be spawned and dereferences
             * this immediately. The only way it returns NULL is an exhausted
             * object sub-pool or a corrupt header — both are bugs in this port,
             * and both used to surface here as a bare SIGSEGV with no explanation
             * (see OBJECT_POOL_SIZE above). Say what actually happened; memory.c
             * has already printed the pool's occupancy. */
            if (racerObj == NULL) {
                fprintf(stderr, "[FATAL] track_setup_racers: spawn_object failed for racer %d (objectID %d).\n", i,
                        (int) objectID);
                abort();
            }
#endif
            racerObj->trans.rotation.y_rotation = spawnAngle[i];
            (*gRacers)[i] = racerObj;
            gRacersByPosition[i] = racerObj;
            gRacersByPort[var_s4] = racerObj;
            curRacer = racerObj->racer;
            racerObj->level_entry = NULL;
            curRacer->vehicleID = vehicle;
            curRacer->vehicleIDPrev = vehicle;
            if (sp127 != -1 && sp127 != (s32) vehicle) {
                D_8011AD20 = TRUE;
            }
            sp127 = vehicle;
            if (curRacer->vehicleID == VEHICLE_PLANE || curRacer->vehicleID == VEHICLE_SMOKEY ||
                curRacer->vehicleID == VEHICLE_PTERODACTYL) {
                gIsNonCarRacers = TRUE;
            }
            curRacer->unk1CB = vehicle;
            if (curRacer->unk1CB < VEHICLE_CAR || curRacer->unk1CB > VEHICLE_PLANE) {
                curRacer->unk1CB = 0;
            }
            curRacer->racerIndex = var_s4;
            curRacer->characterId = settings->racers[var_s4].character;
            if (D_8011AD3C == 2) {
                curRacer->characterId = D_800DC840[i];
            } else {
                curRacer->characterId = settings->racers[var_s4].character;
            }
            if (raceType == RACETYPE_CHALLENGE_BATTLE) {
                curRacer->bananas = 8;
            } else {
                curRacer->bananas = 0;
            }
            if (get_filtered_cheats() & CHEAT_START_WITH_10_BANANAS && !(raceType & RACETYPE_CHALLENGE)) {
                if (curRacer->playerIndex != PLAYER_COMPUTER) {
                    curRacer->bananas = 10;
                }
            }
            if ((gameMode != GAMEMODE_MENU || D_8011AD3C == 2) && vehicle < VEHICLE_BOSSES) {
                curRacer->vehicleSound = racer_sound_init(curRacer->characterId, curRacer->vehicleID);
            } else {
                curRacer->vehicleSound = NULL;
            }

            racerObj->interactObj->pushForce = miscAsset16[curRacer->characterId] + 1;
            switch (curRacer->vehicleID) {
                case VEHICLE_TRICKY:
                case VEHICLE_BLUEY:
                case VEHICLE_SMOKEY:
                case VEHICLE_PTERODACTYL:
                case VEHICLE_SNOWBALL:
                case VEHICLE_BUBBLER:
                case VEHICLE_WIZPIG:
                case VEHICLE_ROCKET:
                    racer_special_init(racerObj, curRacer);
                    break;
                default:
                    break;
            }
        }
    }
    var_s4 = 0;
    if (raceType != RACETYPE_BOSS) {
        D_8011AD3C = 0;
    }
    if (D_8011AD3C != 0) {
        D_8011AD20 = FALSE;
    }
    // Remove unwanted objects depending on race type.
    if (get_game_mode() == GAMEMODE_INGAME) {
        for (j = 0; j < gObjectCount; j++) {
            racerObj = gObjPtrList[j];
            i = racerObj->header->flags;
            if (i & OBJECT_HEADER_NO_TIME_TRIAL && gIsTimeTrial) {
                free_object(racerObj);
            } else if (i & OBJECT_HEADER_NO_MULTIPLATER && numPlayers >= 2) {
                free_object(racerObj);
            }
        }
    }
    gGhostObjStaff = NULL;
    timetrial_free_staff_ghost();
    gTimeTrialContPak = -1;
    if (gIsTimeTrial && numPlayers == 1) {
        timetrial_reset_player_ghost();
        gTimeTrialContPak = timetrial_init_player_ghost(PLAYER_ONE);
        gHasGhostToSave = 0;
        if (gTimeTrialVehicle >= VEHICLE_BOSSES) {
            gTimeTrialVehicle = VEHICLE_CAR;
        }
        // Spawn player ghost.
        if (timetrial_valid_player_ghost()) {
            objectID = gRacerObjectTable[gTimeTrialCharacter + (gTimeTrialVehicle * NUM_CHARACTERS)];
            racerEntry->common.size = ((objectID & 0x100) >> 1) | 0x10;
            racerEntry->common.objectID = objectID;
            racerEntry->common.x = spawnX[0];
            racerEntry->common.y = spawnY[0];
            racerEntry->common.z = spawnZ[0];
            racerEntry->angleY = spawnAngle[0];
            curObj = spawn_object((LevelObjectEntryCommon *) racerEntry, 1);
            curObj->level_entry = NULL;
            curObj->behaviorId = BHV_TIMETRIAL_GHOST;
            curObj->shadow->scale = 0.01f;
            curObj->interactObj->flags = INTERACT_FLAGS_NONE;
            gGhostObjPlayer = curObj;
            curRacer = gGhostObjPlayer->racer;
            curRacer->transparency = 96;
        }
        // Spawn staff ghost
        if (timetrial_init_staff_ghost(level_id())) {
            objectID = gRacerObjectTable[(gMapDefaultVehicle * NUM_CHARACTERS) + 8];

            racerEntry->common.size = ((objectID & 0x100) >> 1) | 0x10;
            racerEntry->common.objectID = objectID;
            racerEntry->common.x = spawnX[0];
            racerEntry->common.y = spawnY[0];
            racerEntry->common.z = spawnZ[0];
            racerEntry->angleY = spawnAngle[0];
            curObj = spawn_object((LevelObjectEntryCommon *) racerEntry, 1);
            curObj->level_entry = NULL;
            curObj->behaviorId = BHV_TIMETRIAL_GHOST;
            curObj->shadow->scale = 0.01f;
            curObj->interactObj->flags = INTERACT_FLAGS_NONE;
            gGhostObjStaff = curObj;
            curRacer = gGhostObjStaff->racer;
            curRacer->transparency = 96;
        }
    }

    for (j = 0, gEventCountdown = 100; j < gNumRacers; j++) {
        racerObj = (*gRacers)[j];
        curRacer = racerObj->racer;
        for (k = 0; k < 10; k++) {
            update_player_racer(racerObj, LOGIC_30FPS); // Settle racers.
        }
        if (curRacer->playerIndex == PLAYER_COMPUTER) {
            var_s4++;
            var_s4 &= 1;
            for (k = 0; k < racerObj->header->numberOfModelIds; k++) {
                if (racerObj->modelInstances[k] != NULL) {
                    if (racerObj->modelInstances[k]->animUpdateTimer != 0) {
                        racerObj->modelInstances[k]->animUpdateTimer = (var_s4 * 2);
                    }
                }
            }
        } else {
            // curRacer is a human racer.
            for (k = 0; k < racerObj->header->numberOfModelIds; k++) {
                if (racerObj->modelInstances[k] != NULL) {
                    if (racerObj->modelInstances[k]->animUpdateTimer != 0) {
                        racerObj->modelInstances[k]->animUpdateTimer = 0;
                    }
                }
            }
        }
        // Apply size cheats to racers.
        if (get_filtered_cheats() & CHEAT_BIG_CHARACTERS) {
            racerObj->trans.scale *= 1.4f;
        }
        if (get_filtered_cheats() & CHEAT_SMALL_CHARACTERS) {
            racerObj->trans.scale *= 0.714f;
        }
        curRacer->stretch_height_cap = 1.0f;
        curRacer->stretch_height = 1.0f;
    }
    if (raceType == RACETYPE_DEFAULT || raceType & RACETYPE_CHALLENGE || gIsTimeTrial || D_8011AD3C != 0) {
        gEventCountdown = 80;
    } else {
        gEventCountdown = 0;
    }
    if (raceType == RACETYPE_DEFAULT && (playerCount + 1) == 1 && is_in_adventure_two() == FALSE) {
        if (!race_is_adventure_2P()) {
            for (j = 0; j < 3; j++) {
                racerEntry->common.objectID = ASSET_OBJECT_ID_POSARROW;
                racerEntry->common.size = sizeof(LevelObjectEntryCommon);
                racerEntry->common.x = 0;
                racerEntry->common.y = 0;
                racerEntry->common.z = 0;
                curObj = spawn_object((LevelObjectEntryCommon *) racerEntry, OBJECT_SPAWN_UNK01);
                curObj->properties.common.unk0 = j;
                curObj->level_entry = NULL;
            }
        }
    }
    gRaceEndTimer = 0;
    gRaceFinishTriggered = FALSE;
    set_next_taj_challenge_menu(0);
    if (settings->worldId == WORLD_CENTRAL_AREA) {
        if (is_in_tracks_mode() == FALSE) {
            s32 tajFlagsBefore;
            s32 requestedVehicle;
            Object *requestedRacer;

            miscAsset16 = (s8 *) get_misc_asset(ASSET_MISC_16);
            j = 0;
            tajFlagsBefore = settings->tajFlags;
            mdkr_taj_trace_phase(
                "load", -1, miscAsset16, -1, 0, 0);

            // settings->balloonsPtr[0] is the total balloon count.
            if (!(settings->tajFlags & TAJ_FLAGS_CAR_CHAL_UNLOCKED) && (settings->balloonsPtr[0] >= miscAsset16[0])) {
                j = 1;
            } else if (!(settings->tajFlags & TAJ_FLAGS_HOVER_CHAL_UNLOCKED) &&
                       (settings->balloonsPtr[0] >= miscAsset16[1])) {
                j = 2;
            } else if (!(settings->tajFlags & TAJ_FLAGS_PLANE_CHAL_UNLOCKED) &&
                       (settings->balloonsPtr[0] >= miscAsset16[2])) {
                j = 3;
            }

            // Taj will teleport straight to the player to initiate a challenge.
            if (j) {
                set_taj_voice_line(SOUND_VOICE_TAJ_CHALLENGE_RACE);
                /* DKR_SHL32 (NATIVE_PORT): j is 1..3 here, so the ROM's `1 << (j + 31)`
                 * means TAJ_FLAGS_CAR_CHAL_UNLOCKED << (j - 1) via MIPS `sllv` masking.
                 * As a plain C shift the count is >= 32 -- UB -- and the if/else-if
                 * chain above pins j to exactly {1,2,3}, which is all clang needs.
                 *
                 * Measured: at -O2 this whole statement is DELETED, on arm64 and
                 * wasm32 alike -- the emitted block calls set_taj_voice_line(),
                 * set_taj_status(), set_next_taj_challenge_menu() and
                 * safe_mark_write_save_file() and never touches tajFlags at all. So in
                 * any optimized build the OFFERED half of tajFlags is never set, while
                 * the BEATEN half (mode_end_taj_race) is written normally, and the save
                 * is flushed in that state, so an already-completed genie test is
                 * offered again on every entry to Timber's Island. */
                settings->tajFlags |= DKR_SHL32(1, j + 31);
                set_taj_status(TAJ_TELEPORT);
                set_next_taj_challenge_menu(j);
                safe_mark_write_save_file(get_save_file_index());
                mdkr_taj_trace_phase(
                    "offer", j - 1, miscAsset16, -1, j, 0);
            } else {
                /*
                 * Already-completed replay fixture. First-time challenges take
                 * the untouched offer/dialogue/transform path above. This seam
                 * is accepted only when both the offered and completed bits
                 * already exist and the requested vehicle is the live vehicle.
                 */
                requestedVehicle = mdkr_taj_requested_vehicle();
                requestedRacer = get_racer_object(PLAYER_ONE);
                if (requestedVehicle >= VEHICLE_CAR &&
                    requestedVehicle <= VEHICLE_PLANE &&
                    (tajFlagsBefore &
                     (TAJ_FLAGS_CAR_CHAL_UNLOCKED << requestedVehicle)) &&
                    (tajFlagsBefore &
                     (TAJ_FLAGS_CAR_CHAL_COMPLETED << requestedVehicle)) &&
                    requestedRacer != NULL &&
                    requestedRacer->racer != NULL &&
                    requestedRacer->racer->vehicleID == requestedVehicle) {
                    init_racer_for_challenge(requestedVehicle);
                }
            }
        }
    }
    D_8011AD24[0] = TRUE;
    if (cutsceneID >= 0) {
        cutsceneCameraSegment->zoom = cutsceneID;
    }
    // Menu demos will skip straight to the action.
    if (racetype_demo()) {
        rumble_init(FALSE);
        gEventCountdown = 0;
        level_music_start(1.0f);
    }
    //!@bug: Free timer is already 0 when loading levels.
    mempool_free_timer(0);
    mempool_free(racerEntry);
    mempool_free_timer(2);
}

/**
 * Return an error status for the controller pak.
 * Categorises multiple different controller pak messages into one for fewer cases.
 */
s32 get_contpak_error(void) {
    // gTimeTrialContPak is likely an SIDeviceStatus value, but not 100% sure yet.
    switch (gTimeTrialContPak) {
        case CONTROLLER_PAK_NOT_FOUND:
            return CONTPAK_ERROR_MISSING;
        case CONTROLLER_PAK_RUMBLE_PAK_FOUND:
            return CONTPAK_ERROR_NONE;
        case CONTROLLER_PAK_INCONSISTENT:
        case CONTROLLER_PAK_WITH_BAD_ID:
            return CONTPAK_ERROR_DAMAGED;
        case CONTROLLER_PAK_FULL:
        case CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS:
            return CONTPAK_ERROR_FULL;
        case CONTROLLER_PAK_GOOD:
        case CONTROLLER_PAK_CHANGED:
        case CONTROLLER_PAK_SWITCH_TO_RUMBLE:
            return timetrial_ghost_full();
        default:
            return CONTPAK_ERROR_NONE;
    }
}

void instShowBearBar(void) {
    D_800DC708 = 0x8000;
}

s8 func_8000E138(void) {
    return D_8011AD20;
}

/**
 * Returns true if currently in a rolling demo.
 */
s8 racetype_demo(void) {
    return gRollingDemo;
}

/**
 * Returns true if we're in a race started by P2 in adventure mode.
 */
s8 is_race_started_by_player_two(void) {
    if (gTwoActivePlayersInAdventure) {
        return gLeadPlayerIndex;
    } else {
        return FALSE;
    }
}

/**
 * Returns true if P2 is the lead player in adventure mode.
 */
s8 is_player_two_in_control(void) {
    return gIsP2LeadPlayer;
}

/**
 * Swaps the lead player index during 2P adventure mode.
 */
void toggle_lead_player_index(void) {
    gLeadPlayerIndex = 1 - gLeadPlayerIndex;
    gTwoActivePlayersInAdventure = FALSE;
}

/**
 * Resets the lead player index.
 * Official name: fontUseFont (why?)
 */
void reset_lead_player_index(void) {
    gLeadPlayerIndex = 0;
    gTwoActivePlayersInAdventure = FALSE;
}

/**
 * Return true if there exist players piloting planes or hovercraft.
 * Used to determine whether to use certain zippers.
 */
s8 find_non_car_racers(void) {
    return gIsNonCarRacers;
}

/**
 * Return true if the silver coin race mode is active.
 * Used to determine whether to spawn silver coins.
 */
s8 check_if_silver_coin_race(void) {
    return gIsSilverCoinRace;
}

/**
 * Store some things about the racer object then remove it.
 */
void despawn_player_racer(Object *obj, s32 vehicleID) {
#ifdef NATIVE_PORT
    taj_physics_reset();
#endif
    gTransformObject = obj;
    gTransformTimer = 4;
    gOverworldVehicle = vehicleID;
    gTransformPosX = obj->trans.x_position;
    gTransformPosY = obj->trans.y_position;
    gTransformPosZ = obj->trans.z_position;
    gTransformAngleY = obj->trans.rotation.y_rotation;
    free_object(obj);
    gNumRacers = 0;
}

/**
 * Spawn a new racer object and set the initial position and rotation to what was set
 * before the old one was freed.
 */
void transform_player_vehicle(void) {
    Object *player;
    LevelObjectEntry8000E2B4 spawnObj;
    Settings *settings;
    Object_Racer *racer;
    s16 objectID;

    if (gTransformTimer == 0) {
        return;
    }
    gTransformTimer--;
    if (gTransformTimer) {
        return;
    }
    settings = get_settings();
    spawnObj.unkE = 0;
    spawnObj.common.size = 16;
    if (gOverworldVehicle < VEHICLE_BOSSES) {
        objectID =
            ((s16 *) gRacerObjectTable)[settings->racers[PLAYER_ONE].character + gOverworldVehicle * NUM_CHARACTERS];
    } else {
        objectID = gRacerObjectTable[gOverworldVehicle + 45];
    }
    set_level_default_vehicle(gOverworldVehicle);
    spawnObj.common.size |= (objectID & 0x100) >> 1;
    spawnObj.unkA = 0;
    spawnObj.unk8 = 0;
    spawnObj.common.objectID = objectID;
    spawnObj.common.x = gTransformPosX;
    spawnObj.common.y = gTransformPosY;
    spawnObj.common.z = gTransformPosZ;
    spawnObj.unkC = gTransformAngleY;
    set_taj_status(TAJ_DIALOGUE);
    player = spawn_object(&spawnObj.common, OBJECT_SPAWN_NO_LODS | OBJECT_SPAWN_UNK01);
    gNumRacers = 1;
    (*gRacers)[PLAYER_ONE] = player;
    gRacersByPort[PLAYER_ONE] = player;
    *gRacersByPosition = player;
    racer = player->racer;
    racer->vehicleID = gOverworldVehicle;
    racer->vehicleIDPrev = gOverworldVehicle;
    racer->racerIndex = 0;
    racer->characterId = settings->racers[PLAYER_ONE].character;
    racer->playerIndex = 0;
    racer->vehicleSound = 0;
    if (get_filtered_cheats() & CHEAT_BIG_CHARACTERS) {
        player->trans.scale *= 1.4f;
    }
    if (get_filtered_cheats() & CHEAT_SMALL_CHARACTERS) {
        player->trans.scale *= 0.714f;
    }
    player->level_entry = NULL;
    player->trans.rotation.y_rotation = gTransformAngleY;
    player->trans.y_position = gTransformPosY;
}

/**
 * Enables or Disables time trial mode.
 */
void set_time_trial_enabled(s32 status) {
    gTimeTrialEnabled = status;
}

/**
 * Returns the value in gTimeTrialEnabled.
 */
u8 is_time_trial_enabled(void) {
    return gTimeTrialEnabled;
}

/**
 * Returns true if the player is currently performaing a time trial.
 */
u8 is_in_time_trial(void) {
    return gIsTimeTrial;
}

UNUSED void func_8000E4E8(s32 index) {
    s32 *temp_v0;
    s32 i;
    u8 *temp_a1;

    temp_v0 = gObjectMap[index];
    temp_v0[0] = gObjectMapSize[index];
    temp_v0[3] = 0;
    temp_v0[2] = 0;
    temp_v0[1] = 0;
    temp_a1 = &gObjectMapSpawnList[index][gObjectMapSize[index]];

    // The backslash here is needed to match. And no, a for loop doesn't match.
    // clang-format off
    i = 0; \
    while (i < 16) {
        temp_a1[i] = 0;
        i++;
    }
    // clang-format on
}

UNUSED s32 func_8000E558(Object *arg0) {
#ifdef NATIVE_PORT
    uintptr_t entryAddress;
    uintptr_t mapStart;
    size_t mapSpan;
#else
    s32 temp_v0;
    s32 new_var, new_var2;
#endif
    if (arg0 == NULL || arg0->level_entry == NULL) {
        return TRUE;
    }
#ifdef NATIVE_PORT
    entryAddress = (uintptr_t) arg0->level_entry;
    mapStart = (uintptr_t) gObjectMapSpawnList[0];
    mapSpan = gObjectMapSpawnList[0] != NULL && gObjectMapSize[0] > 0
                  ? (size_t) gObjectMapSize[0]
                  : 0;
    if (entryAddress >= mapStart && entryAddress - mapStart < mapSpan) {
        return FALSE;
    }
    mapStart = (uintptr_t) gObjectMapSpawnList[1];
    mapSpan = gObjectMapSpawnList[1] != NULL && gObjectMapSize[1] > 0
                  ? (size_t) gObjectMapSize[1]
                  : 0;
    if (entryAddress >= mapStart && entryAddress - mapStart < mapSpan) {
        return TRUE;
    }
#else
    temp_v0 = (s32) arg0->level_entry;
    new_var2 = (s32) gObjectMapSpawnList[0];
    if ((temp_v0 >= new_var2) && (((gObjectMapSize[0] * 8) + new_var2) >= temp_v0)) {
        return FALSE;
    }
    new_var = (s32) gObjectMapSpawnList[1];
    // Why even bother with this check?
    if (temp_v0 >= new_var && temp_v0 <= ((gObjectMapSize[1] * 8) + new_var)) {
        return TRUE;
    }
#endif
    return TRUE;
}

void func_8000E5EC(LevelObjectEntryCommon *arg0) {
#ifdef NATIVE_PORT
    uintptr_t mapStart[2];
    uintptr_t mapEnd[2];
    uintptr_t entryAddress;
#else
    u8 *src;
    u8 *dst;
    s32 end;
    s32 sp30[2];
#endif
    s32 size;
    s32 i;
    s32 pad;
    s32 sp1C;

#ifdef NATIVE_PORT
    if (arg0 == NULL || gObjectMapSpawnList[0] == NULL ||
        gObjectMapSpawnList[1] == NULL || gObjectMapSize[0] < 0 ||
        gObjectMapSize[1] < 0) {
        return;
    }
#endif
    size = arg0->size & 0x3F;

#ifdef NATIVE_PORT
    if (size <= 0) {
        return;
    }
    mapStart[0] = (uintptr_t) gObjectMapSpawnList[0];
    mapStart[1] = (uintptr_t) gObjectMapSpawnList[1];
    if ((size_t) gObjectMapSize[0] > UINTPTR_MAX - mapStart[0] ||
        (size_t) gObjectMapSize[1] > UINTPTR_MAX - mapStart[1]) {
        return;
    }
    mapEnd[0] = mapStart[0] + (size_t) gObjectMapSize[0];
    mapEnd[1] = mapStart[1] + (size_t) gObjectMapSize[1];
    entryAddress = (uintptr_t) arg0;
    if (entryAddress >= mapStart[0] && entryAddress < mapEnd[0]) {
        sp1C = 0;
    } else if (entryAddress >= mapStart[1] && entryAddress < mapEnd[1]) {
        sp1C = 1;
    } else {
        return;
    }
    if ((size_t) size > mapEnd[sp1C] - entryAddress) {
        return;
    }
    memmove(arg0, (u8 *) arg0 + size,
            (size_t) (mapEnd[sp1C] - entryAddress - (size_t) size));
#else
    sp30[0] = (s32) gObjectMapSpawnList[0] + gObjectMapSize[0];
    sp30[1] = (s32) gObjectMapSpawnList[1] + gObjectMapSize[1];

    if ((s32) arg0 >= (s32) gObjectMapSpawnList[0] && (s32) arg0 < sp30[0]) {
        sp1C = 0;
    } else if ((s32) arg0 >= (s32) gObjectMapSpawnList[1] && (s32) arg0 < sp30[1]) {
        sp1C = 1;
    }
#ifdef AVOID_UB
    else {
        sp1C = 0;
    }
#endif

    dst = (u8 *) arg0;
    src = (u8 *) ((s32) arg0 + size);
    end = sp30[sp1C];
    if ((u32) src < (u32) end) {
        do {
            *dst++ = *src++;
        } while ((u32) src != (u32) end);
    }
#endif
    gObjectMapSize[sp1C] -= size;

    for (i = 0; i < gObjectCount; i++) {
        Object *obj = gObjPtrList[i];
        if (obj != NULL && obj->level_entry != NULL) {
#ifdef NATIVE_PORT
            uintptr_t objectEntryAddress = (uintptr_t) obj->level_entry;
            if (entryAddress < objectEntryAddress &&
                objectEntryAddress < mapEnd[sp1C]) {
                obj->level_entry =
                    (LevelObjectEntry *) ((u8 *) obj->level_entry - size);
            } else if (entryAddress == objectEntryAddress) {
                obj->level_entry = NULL;
            }
#else
            if ((s32) arg0 < (s32) obj->level_entry && (s32) obj->level_entry < end) {
                obj->level_entry = (LevelObjectEntry *) ((s32) obj->level_entry - size);
            } else if ((s32) arg0 == (s32) obj->level_entry) {
                obj->level_entry = NULL;
            }
#endif
        }
    }
}

void func_8000E79C(u8 *arg0, u8 *arg1) {
    s32 arg0Value;
    s32 arg0Value2;
    s32 arg1Value;
    u8 *var_a3;
#ifdef NATIVE_PORT
    uintptr_t entry;
    uintptr_t mapStart;
    uintptr_t mapEnd;
    s32 sizeDelta;
    size_t trailingBytes;
#else
    u8 *var_t0;
    u8 *temp_t2;
    u8 *var_a2;
    s32 j;
    s32 k;
#endif
    s32 i;

#ifdef NATIVE_PORT
    if (arg0 == NULL || arg1 == NULL) {
        return;
    }
#endif

    arg0Value = arg0[1] & 0x3F;
    arg0Value2 = arg0Value;
    arg1Value = arg1[1] & 0x3F;
    i = gObjectMapIndex;
    var_a3 = (u8 *) gObjectMap[i] + gObjectMapSize[i];
    var_a3 += 16;
#ifdef NATIVE_PORT
    entry = (uintptr_t) arg0;
    mapStart = (uintptr_t) gObjectMap[i] + 16u;
    mapEnd = (uintptr_t) var_a3;
    sizeDelta = arg1Value - arg0Value2;

    if (entry < mapStart || entry > mapEnd ||
        (size_t) arg0Value2 > mapEnd - entry ||
        (sizeDelta > 0 &&
         (size_t) gObjectMapSize[i] + (size_t) sizeDelta >
             OBJECT_MAP_SIZE - 16u)) {
        stubbed_printf("OBJMAP Error: Invalid entry replacement!!\n");
        return;
    }
    trailingBytes = (size_t) (mapEnd - entry) - (size_t) arg0Value2;
    memmove(arg0 + arg1Value, arg0 + arg0Value2, trailingBytes);
    memcpy(arg0, arg1, (size_t) arg1Value);
#else
    if (arg1Value < arg0Value2) {
        var_a2 = arg0 + arg1Value;
        var_t0 = arg0 + arg0Value2;
        k = (u32) var_a3;
        while (((u32) var_t0) < (u32) k) {
            *var_a2 = *var_t0;
            var_a2++;
            var_t0++;
        }
    } else if (arg0Value2 < arg1Value) {
        var_a2 = var_a3 + arg1Value;
        var_a2 -= arg0Value2;
        var_t0 = var_a3;
        k = (u32) (arg0 + arg1Value);
        while ((u32) k < ((u32) var_a2)) {
            var_a2--;
            var_t0--;
            *var_a2 = *var_t0;
        }
    }

    j = 0;
    do {
        arg0[j] = arg1[j];
        j++;
    } while (j < arg1Value);
#endif

    gObjectMapSize[i] += arg1Value - arg0Value;
}

UNUSED u8 *func_8000E898(u8 *arg0, s32 arg1) {
    s32 temp_t6;
    s32 i;
    u8 *temp_v1;
    u8 *new_var;
    u8 *new_var2;

    temp_t6 = arg0[1] & 0x3F;
    new_var = arg0;
    new_var = &gObjectMapSpawnList[arg1][gObjectMapSize[arg1]];
    new_var2 = arg0;
    temp_v1 = new_var;
    gObjectMapSize[arg1] += temp_t6;
    for (i = 0; i < temp_t6; i++) {
        temp_v1[i] = new_var2[i];
    }
    return temp_v1;
}

/**
 * Returns the object at the current offset by ID.
 * Official name: objGetObject
 */
Object *get_object(s32 index) {
    if (index < 0 || index >= gObjectCount) {
        stubbed_printf("ObjList (Part) Overflow %d!!!\n");
        return 0;
    }
    return gObjPtrList[index];
}

/**
 * Return the standard object list index and how many objects are in that list.
 */
Object **objGetObjList(s32 *arg0, s32 *cnt) {
    *arg0 = gObjectListStart;
    *cnt = gObjectCount;
    return gObjPtrList;
}

/**
 * Return the number of objects currently existing.
 */
UNUSED s32 obj_count(void) {
    return gObjectCount;
}

/**
 * Return the number of particles currently existing.
 */
UNUSED s32 particle_count(void) {
    return gParticleCount;
}

void add_particle_to_entity_list(Object *obj) {
    obj->trans.flags |= OBJ_FLAGS_PARTICLE;
    func_800245B4(obj->headerType | (OBJ_FLAGS_PARTICLE | OBJ_FLAGS_INVISIBLE));
    gObjPtrList[gObjectCount++] = obj;
    if (1) {} // Fakematch
    gParticleCount++;
#ifdef NATIVE_PORT
    /* The second spawn site: particles come from particle_allocate(), not
     * spawn_object(), but they enter the SAME gObjPtrList the snapshot walks
     * and they render, so they need identities too. Their pool churns far
     * faster than the object pool, which makes the generation matter more
     * here, not less. */
    presentation_snapshot_note_spawn(obj);
#endif
}

#ifdef NATIVE_PORT
static void *mdkr_spawn_layout_require(
    MdkrObjectLayout *layout,
    size_t count,
    size_t elementSize,
    size_t alignment,
    const char *field,
    s32 objectId) {
    void *result = mdkr_object_layout_append_array(
        layout, count, elementSize, alignment);

    if (result == NULL) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): object blueprint could not place "
                "%s (%zu x %zu, align %zu): %s\n",
                objectId, field, count, elementSize, alignment,
                mdkr_object_layout_error_string(
                    mdkr_object_layout_error(layout)));
        abort();
    }
    return result;
}

static void mdkr_spawn_require_alignment(
    const void *pointer,
    size_t alignment,
    const char *field,
    s32 objectId) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): internal %s alignment %zu is "
                "not a power of two\n",
                objectId, field, alignment);
        abort();
    }
    if (pointer != NULL &&
        ((uintptr_t) pointer & (alignment - 1)) != 0) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): rebased %s pointer %p is not "
                "%zu-byte aligned\n",
                objectId, field, pointer, alignment);
        abort();
    }
}
#endif

// Official Name: ObjSetupObject
Object *spawn_object(LevelObjectEntryCommon *entry, s32 spawnFlags) {
    s32 objType;
    Settings *settings;
    s32 i;
    s32 unused2;
    s32 unused;
    s32 behaviourFlags;
    s16 headerType;
    u8 *address;
    s32 sizeOfobj;
    Object *curObj;
    Object *prevObj;
    s32 assetCount;
    s32 modelSlotCount;
    s8 failed;
#ifdef NATIVE_PORT
    MdkrObjectLayout objectLayout;
    size_t entrySize;
    size_t finalObjectSize = 0;
    size_t propertySize;
#endif

    settings = get_settings();
#ifdef NATIVE_PORT
    if (entry == NULL) {
        fprintf(stderr, "[FATAL] spawn_object: NULL level-object entry\n");
        abort();
    }
    entrySize = entry->size & 0x3F;
    if (entrySize < sizeof(LevelObjectEntryCommon) ||
        (entrySize & 1u) != 0) {
        fprintf(stderr,
                "[FATAL] spawn_object: invalid level-entry stride %zu "
                "(minimum %zu, must be even)\n",
                entrySize, sizeof(LevelObjectEntryCommon));
        abort();
    }
#endif
    objType = entry->objectID | ((entry->size & 0x80) << 1);
    update_object_stack_trace(OBJECT_SPAWN, objType);
    if (spawnFlags & OBJECT_SPAWN_UNK02) {
        headerType = objType;
    } else {
#ifdef NATIVE_PORT
        if ((size_t) objType >=
            gAssetsLvlObjTranslationTableCapacity) {
            fprintf(stderr,
                    "[FATAL] spawn_object: object id %d exceeds the "
                    "%zu-entry translation table\n",
                    objType, gAssetsLvlObjTranslationTableCapacity);
            abort();
        }
#endif
        headerType = gAssetsLvlObjTranslationTable[objType];
    }
#ifdef NATIVE_PORT
    if (headerType < 0 ||
        headerType >= gAssetsObjectHeadersTableLength) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): invalid object header %d "
                "(count=%d)\n",
                objType, headerType, gAssetsObjectHeadersTableLength);
        abort();
    }
#else
    if (headerType >= gAssetsObjectHeadersTableLength) {
        headerType = 0;
    }
#endif

    for (i = 0; i < OBJECT_BLUEPRINT_SIZE / 4; i++) {
        gSpawnObjectHeap[i] = 0;
    }

#ifdef NATIVE_PORT
    mdkr_object_layout_init(
        &objectLayout, gSpawnObjectHeap, OBJECT_BLUEPRINT_SIZE);
    curObj = mdkr_spawn_layout_require(
        &objectLayout, 1, sizeof(Object), _Alignof(Object),
        "Object", objType);
#else
    curObj = (Object *) gSpawnObjectHeap;
#endif
    prevObj = curObj;
    curObj->trans.flags = OBJ_FLAGS_UNK_0002;
    curObj->header = load_object_header(headerType);
    if (curObj->header == NULL) {
        return NULL;
    }
    if (curObj->header->flags & HEADER_FLAGS_UNK_0080) {
        curObj->trans.flags |= OBJ_FLAGS_UNK_0080;
    }
    if (curObj->header->behaviorId == BHV_ROCKET_SIGNPOST && (settings->cutsceneFlags & CUTSCENE_LIGHTHOUSE_ROCKET)) {
        update_object_stack_trace(OBJECT_SPAWN, -1);
#ifdef NATIVE_PORT
        try_free_object_header(headerType);
#endif
        stubbed_printf("ObjSetupObject(1) Memory fail!!\n");
        return NULL;
    }
#ifdef NATIVE_PORT
    if (entrySize <
        mdkr_level_object_min_size(curObj->header->behaviorId)) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): behavior %d received %zu entry "
                "bytes; initializer requires at least %zu\n",
                objType, curObj->header->behaviorId, entrySize,
                mdkr_level_object_min_size(
                    curObj->header->behaviorId));
        abort();
    }
#endif

    curObj->trans.x_position = entry->x;
    curObj->trans.y_position = entry->y;
    curObj->trans.z_position = entry->z;
    curObj->segmentID = get_level_segment_index_from_position(curObj->trans.x_position, curObj->trans.y_position,
                                                              curObj->trans.z_position);

    curObj->headerType = headerType;
    curObj->level_entry = (LevelObjectEntry *) entry;
    curObj->objectID = objType;
    func_800245B4(objType);
    curObj->trans.scale = curObj->header->scale;
    curObj->unk34 = curObj->header->unk50 * curObj->trans.scale;
    curObj->opacity = 255;
    behaviourFlags = obj_init_property_flags(curObj->header->behaviorId);
    curObj->header->unk52++;

    assetCount = curObj->header->numberOfModelIds;
    modelSlotCount = curObj->header->numberOfModelIds;

    objType = curObj->header->modelType;
#ifdef NATIVE_PORT
    if (modelSlotCount < 0) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): header %d has negative model "
                "slot count %d\n",
                curObj->objectID, headerType, modelSlotCount);
        abort();
    }
    curObj->modelInstances = mdkr_spawn_layout_require(
        &objectLayout, (size_t) modelSlotCount,
        sizeof(*curObj->modelInstances), _Alignof(ModelInstance *),
        "model/sprite pointer array", curObj->objectID);
#else
    curObj->modelInstances = (ModelInstance **) &curObj[1];
#endif
    if (spawnFlags & OBJECT_SPAWN_NO_LODS) {
        assetCount = 1;
    }
    i = 0; // a2
    switch (curObj->header->behaviorId) {
        case BHV_PARK_WARDEN:
            model_anim_offset(7);
            break;
        case BHV_ANIMATED_OBJECT_4:
            if (!mdkr_model_load_selection(
                    get_character_id_from_slot(PLAYER_ONE), modelSlotCount,
                    &i, &assetCount)) {
                i = 0;
                assetCount = 0;
            }
            curObj->modelIndex = i;
            break;
        case BHV_UNK_5B: {
            u32 trophyState;
            if (mdkr_trophy_state(settings->trophies, settings->worldId,
                                  &trophyState) &&
                trophyState != 0) {
                if (!mdkr_model_load_selection(
                        (s32) trophyState - 1, modelSlotCount, &i,
                        &assetCount)) {
                    i = 0;
                    assetCount = 0;
                }
                curObj->modelIndex = i;
            }
            break;
        }
        case BHV_DYNAMIC_LIGHT_OBJECT_2:
            if (!mdkr_model_load_selection(
                    settings->wizpigAmulet, modelSlotCount, &i,
                    &assetCount)) {
                i = 0;
                assetCount = 0;
            }
            curObj->modelIndex = i;
            break;
        case BHV_ROCKET_SIGNPOST_2:
            objType = settings->trophies;
            for (assetCount = 0; assetCount < 4; assetCount++) {
                if ((objType & 3) == 3) {
                    i++;
                }
                objType >>= 2;
            }
            if (!mdkr_model_load_selection(i, modelSlotCount, &i,
                                           &assetCount)) {
                i = 0;
                assetCount = 0;
            }
            curObj->modelIndex = i;
            break;
        case BHV_GOLDEN_BALLOON:
            assetCount = 1;
            if (is_in_adventure_two()) {
                DKR_PTR(s32, curObj->header->modelIds)[0] = DKR_PTR(s32, curObj->header->modelIds)[1];
            }
            curObj->header->numberOfModelIds = 1;
            break;
    }
    if (!(spawnFlags & OBJECT_SPAWN_UNK02)) {
        switch (curObj->objectID) {
            case ASSET_OBJECT_ID_POLYGOLDBALOON:
                assetCount = 1;
                if (is_in_adventure_two()) {
                    DKR_PTR(s32, curObj->header->modelIds)[0] = DKR_PTR(s32, curObj->header->modelIds)[1];
                }
                curObj->header->numberOfModelIds = 1;
                break;
            case ASSET_OBJECT_ID_LEVELDOOR:
                if (is_in_adventure_two()) {
                    for (i = 0; i < 5; i++) {
                        DKR_PTR(s32, curObj->header->modelIds)[i] = DKR_PTR(s32, curObj->header->modelIds)[i + 5];
                    }
                }
                assetCount = 5;
                curObj->header->numberOfModelIds = 5;
                i = 0;
                break;
        }
    }

    failed = FALSE;
#ifdef NATIVE_PORT
    if (assetCount < 0 || assetCount > modelSlotCount ||
        i < 0 || i > assetCount) {
        fprintf(stderr,
                "[FATAL] spawn_object: header %d (objectID %d, behavior %d) "
                "would write asset range [%d,%d) into %d pointer slots\n",
                headerType, curObj->objectID, curObj->header->behaviorId,
                i, assetCount, modelSlotCount);
        abort();
    }
    /* A header that claims models/sprites/textures must have a resolvable
     * modelIds array. This used to be a silent "failed spawn" guard papering over
     * the object-header table overrun in allocate_object_pools(); that is fixed at
     * root, so any hit here is a NEW corruption and must be loud, not hidden. */
    if (assetCount > 0 && DKR_PTR(s32, curObj->header->modelIds) == NULL) {
        fprintf(stderr,
                "[FATAL] spawn_object: header %d (objectID %d, behaviour %d) claims %d assets but modelIds "
                "resolves NULL - corrupted object header.\n",
                headerType, curObj->objectID, curObj->header->behaviorId, assetCount);
        abort();
    }
#endif
    if (objType == OBJECT_MODEL_TYPE_3D_MODEL) {
        while (i < assetCount) {
            if (i == 0 && (spawnFlags & OBJECT_SPAWN_UNK04)) {
                curObj->modelInstances[i] = NULL;
            } else if (i == 1 && (spawnFlags & OBJECT_SPAWN_UNK08)) {
                curObj->modelInstances[i] = NULL;
            } else {
                curObj->modelInstances[i] = object_model_init(DKR_PTR(s32, curObj->header->modelIds)[i], behaviourFlags);
                if (curObj->modelInstances[i] == NULL) {
                    failed = TRUE;
                }
            }
            i++;
        }
    } else if (objType == OBJECT_MODEL_TYPE_MISC) {
        while (i < assetCount) {
            curObj->textures[i] = load_texture(DKR_PTR(s32, curObj->header->modelIds)[i]);
            if (curObj->textures[i] == NULL) {
                failed = TRUE;
            }
            i++;
        }
    } else {
        while (i < assetCount) {
            curObj->sprites[i] = tex_load_sprite(DKR_PTR(s32, curObj->header->modelIds)[i], 10);
            if (curObj->sprites[i] == NULL) {
                failed = TRUE;
            }
            i++;
        }
    }
    if (failed) {
        objFreeAssets(curObj, assetCount, objType);
        try_free_object_header(headerType);
        stubbed_printf("ObjSetupObject(2) Memory fail!!\n");
        return NULL;
    }

    D_8011AE50 = NULL;
    D_8011AE54 = NULL;

#ifdef NATIVE_PORT
    /*
     * Every typed tail field is placed by the same checked cursor. Behaviour
     * storage is 16-byte aligned because three behaviours historically require
     * it and several native payloads contain pointers; the remaining fields use
     * their exact host alignments.
     */
    propertySize = (size_t) get_object_property_size(curObj, NULL);
    if (propertySize > 0) {
        curObj->anyBehaviorData = mdkr_spawn_layout_require(
            &objectLayout, 1, propertySize, 16,
            "behavior storage", curObj->objectID);
    }

    if (behaviourFlags & OBJECT_BEHAVIOUR_SHADED) {
        init_object_shading(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, 1, sizeof(ShadeProperties),
                _Alignof(ShadeProperties), "shading", curObj->objectID));
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_SHADOW) {
        sizeOfobj = init_object_shadow(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, 1, sizeof(ShadowData),
                _Alignof(ShadowData), "shadow", curObj->objectID));
        if (sizeOfobj == 0) {
            objFreeAssets(curObj, assetCount, objType);
            try_free_object_header(headerType);
            stubbed_printf("ObjSetupObject(5) Memory fail!!\n");
            return NULL;
        }
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_WATER_EFFECT) {
        sizeOfobj = init_object_water_effect(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, 1, sizeof(WaterEffect),
                _Alignof(WaterEffect), "water effect", curObj->objectID));
        if (sizeOfobj == 0) {
            if (D_8011AE50 != NULL) {
                tex_free((TextureHeader *) (uintptr_t) D_8011AE50);
            }
            objFreeAssets(curObj, assetCount, objType);
            try_free_object_header(headerType);
            stubbed_printf("ObjSetupObject(6) Memory fail!!\n");
            return NULL;
        }
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_INTERACTIVE) {
        init_object_interaction_data(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, 1, sizeof(ObjectInteraction),
                _Alignof(ObjectInteraction), "interaction", curObj->objectID));
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_COLLIDABLE) {
        obj_init_collision(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, 1, sizeof(ObjectCollision),
                _Alignof(ObjectCollision), "collision", curObj->objectID));
    }
    if (curObj->header->attachPointCount > 0 && curObj->header->attachPointCount < 10) {
        curObj->attachPoints = mdkr_spawn_layout_require(
            &objectLayout, 1, sizeof(AttachPoint),
            _Alignof(AttachPoint), "attach points", curObj->objectID);
    }
    if (curObj->header->particleCount > 0) {
        obj_init_emitter(
            curObj,
            mdkr_spawn_layout_require(
                &objectLayout, (size_t) curObj->header->particleCount,
                sizeof(ParticleEmitter), _Alignof(ParticleEmitter),
                "particle emitters", curObj->objectID));
    }
    if (curObj->header->numLightSources > 0) {
        curObj->lightData = mdkr_spawn_layout_require(
            &objectLayout, (size_t) curObj->header->numLightSources,
            sizeof(*curObj->lightData), _Alignof(ObjectLight *),
            "light pointer array", curObj->objectID);
    }

    if (!mdkr_object_layout_finish(
            &objectLayout, 16, &finalObjectSize) ||
        finalObjectSize > INT_MAX) {
        fprintf(stderr,
                "[FATAL] spawn_object(%d): invalid final blueprint size: %s "
                "(%zu bytes)\n",
                curObj->objectID,
                mdkr_object_layout_error_string(
                    mdkr_object_layout_error(&objectLayout)),
                finalObjectSize);
        abort();
    }
    sizeOfobj = (s32) finalObjectSize;
#else
    address = (u8 *) &curObj->modelInstances[curObj->header->numberOfModelIds];
    address += get_object_property_size(curObj, address);

    if (behaviourFlags & OBJECT_BEHAVIOUR_SHADED) {
        address += init_object_shading(curObj, (ShadeProperties *) address);
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_SHADOW) {
        sizeOfobj = init_object_shadow(curObj, (ShadowData *) address);
        address += sizeOfobj;
        if (sizeOfobj == 0) {
            objFreeAssets(curObj, assetCount, objType);
            try_free_object_header(headerType);
            stubbed_printf("ObjSetupObject(5) Memory fail!!\n");
            return NULL;
        }
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_WATER_EFFECT) {
        sizeOfobj = init_object_water_effect(curObj, (WaterEffect *) address);
        address += sizeOfobj;
        if (sizeOfobj == 0) {
            if (D_8011AE50 != NULL) {
                tex_free((TextureHeader *) (uintptr_t) D_8011AE50);
            }
            objFreeAssets(curObj, assetCount, objType);
            try_free_object_header(headerType);
            stubbed_printf("ObjSetupObject(6) Memory fail!!\n");
            return NULL;
        }
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_INTERACTIVE) {
        address += init_object_interaction_data(curObj, (ObjectInteraction *) address);
    }
    if (behaviourFlags & OBJECT_BEHAVIOUR_COLLIDABLE) {
        address += obj_init_collision(curObj, (ObjectCollision *) address);
    }
    if (curObj->header->attachPointCount > 0 && curObj->header->attachPointCount < 10) {
        curObj->attachPoints = (AttachPoint *) address;
        address += sizeof(AttachPoint);
    }
    if (curObj->header->particleCount > 0) {
        address += obj_init_emitter(curObj, (ParticleEmitter *) address);
    }
    if (curObj->header->numLightSources > 0) {
        curObj->lightData = (ObjectLight **) address;
        address += curObj->header->numLightSources * 4;
    }

    sizeOfobj = (s32) address - (s32) curObj;
#endif
    prevObj = curObj;
    curObj = mempool_alloc_pool((MemoryPoolSlot *) gObjectMemoryPool, sizeOfobj);
    if (curObj == NULL) {
        if (D_8011AE50 != NULL) {
            tex_free((TextureHeader *) (uintptr_t) D_8011AE50);
        }
        if (D_8011AE54 != NULL) {
            tex_free((TextureHeader *) (uintptr_t) D_8011AE54);
        }
        objFreeAssets(prevObj, assetCount, objType);
        try_free_object_header(headerType);
        stubbed_printf("ObjSetupObject(3) Memory fail!!\n");
        return NULL;
    }

#ifdef NATIVE_PORT
    memcpy(curObj, gSpawnObjectHeap, finalObjectSize);
#else
    i = 0;
    if (sizeOfobj & 0xF) {
        sizeOfobj = _ALIGN16(sizeOfobj);
    }

    sizeOfobj >>= 2;
    while (i < sizeOfobj) {
        ((u32 *) curObj)[i] = gSpawnObjectHeap[i];
        i++;
    }
#endif

    if (curObj->waterEffect != NULL) {
        curObj->waterEffect =
            (WaterEffect *) (((uintptr_t) curObj + (uintptr_t) curObj->waterEffect) - (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->shadow != NULL) {
        curObj->shadow =
            (ShadowData *) (((uintptr_t) curObj + (uintptr_t) curObj->shadow) - (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->shading != NULL) {
        curObj->shading =
            (ShadeProperties *) (((uintptr_t) curObj + (uintptr_t) curObj->shading) - (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->anyBehaviorData != NULL) {
        curObj->anyBehaviorData =
            (void *) (((uintptr_t) curObj + (uintptr_t) curObj->anyBehaviorData) - (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->interactObj != NULL) {
        curObj->interactObj = (ObjectInteraction *) (((uintptr_t) curObj + (uintptr_t) curObj->interactObj) -
                                                     (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->collisionData != NULL) {
        curObj->collisionData = (ObjectCollision *) (((uintptr_t) curObj + (uintptr_t) curObj->collisionData) -
                                                     (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->attachPoints != NULL) {
        curObj->attachPoints =
            (AttachPoint *) (((uintptr_t) curObj + (uintptr_t) curObj->attachPoints) - (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->header->particleCount > 0) {
        curObj->particleEmitter = (ParticleEmitter *) (((uintptr_t) curObj + (uintptr_t) curObj->particleEmitter) -
                                                       (uintptr_t) gSpawnObjectHeap);
    }
    if (curObj->header->numLightSources > 0) {
        curObj->lightData =
            (ObjectLight **) (((uintptr_t) curObj + (uintptr_t) curObj->lightData) - (uintptr_t) gSpawnObjectHeap);
    }
#ifdef NATIVE_PORT
    curObj->modelInstances =
        (ModelInstance **) (((uintptr_t) curObj +
                             (uintptr_t) curObj->modelInstances) -
                            (uintptr_t) gSpawnObjectHeap);

    mdkr_spawn_require_alignment(
        curObj->modelInstances, _Alignof(ModelInstance *),
        "model/sprite pointer array", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->anyBehaviorData, 16,
        "behavior storage", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->shading, _Alignof(ShadeProperties),
        "shading", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->shadow, _Alignof(ShadowData),
        "shadow", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->waterEffect, _Alignof(WaterEffect),
        "water effect", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->interactObj, _Alignof(ObjectInteraction),
        "interaction", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->collisionData, _Alignof(ObjectCollision),
        "collision", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->attachPoints, _Alignof(AttachPoint),
        "attach points", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->particleEmitter, _Alignof(ParticleEmitter),
        "particle emitters", curObj->objectID);
    mdkr_spawn_require_alignment(
        curObj->lightData, _Alignof(ObjectLight *),
        "light pointer array", curObj->objectID);
#else
    curObj->modelInstances = (ModelInstance **) &curObj[1];
#endif

    if (spawnFlags & OBJECT_SPAWN_UNK01) {
        if (curObj && curObj) {} // Fakematch
        gObjPtrList[gObjectCount++] = curObj;
        if (gObjectCount > OBJECT_SLOT_COUNT) {
            stubbed_printf("ObjList Overflow %d!!!\n", gObjectCount);
        }
    }
    run_object_init_func(curObj, entry, 0);
    if (curObj->interactObj != NULL) {
        curObj->interactObj->x_position = curObj->trans.x_position;
        curObj->interactObj->y_position = curObj->trans.y_position;
        curObj->interactObj->z_position = curObj->trans.z_position;
    }
    if (curObj->header->attachPointCount > 0 && curObj->header->attachPointCount < 10 && obj_init_attachpoint(curObj)) {
        if (D_8011AE50 != NULL) {
            tex_free((TextureHeader *) (uintptr_t) D_8011AE50);
        }
        if (D_8011AE54 != NULL) {
            tex_free((TextureHeader *) (uintptr_t) D_8011AE54);
        }
        objFreeAssets(curObj, assetCount, objType);
        try_free_object_header(headerType);
        mempool_free(curObj);
        if (spawnFlags & OBJECT_SPAWN_UNK01) {
            gObjectCount--;
        }
        stubbed_printf("ObjSetupObject(4) Memory fail!!\n");
        return NULL;
    }
    if (curObj->header->numLightSources > 0) {
        light_setup_light_sources(curObj);
    }
    model_anim_offset(0);
    update_object_stack_trace(OBJECT_SPAWN, -1);
#ifdef NATIVE_PORT
    /*
     * Presentation-snapshot identity. The object
     * pool recycles Object addresses constantly, so `Object *` alone is not
     * an identity: without a generation, a banana freed on tick N and a
     * weapon spawned into its address on tick N+1 would look like one object
     * that teleported, and the interpolator would draw the blend. Issue a
     * fresh generation here — the last statement of the successful path, so
     * every early-return failure above leaves the registry untouched.
     * No-op unless MDKR_PRESENT_SNAPSHOT is set.
     */
    presentation_snapshot_note_spawn(curObj);
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_SPAWN, curObj->objectID,
        curObj->header != NULL ? curObj->header->behaviorId : -1,
        spawnFlags, gObjectCount);
#endif
    return curObj;
}

/**
 * Run functions that will attempt to free the graphics data of the object
 * if there are no other references.
 */
void objFreeAssets(Object *obj, s32 count, s32 objType) {
    s32 i;
    if (objType == OBJECT_MODEL_TYPE_3D_MODEL) { // 3D model
        for (i = 0; i < count; i++) {
            if (obj->modelInstances[i] != NULL) {
                free_3d_model((ModelInstance *) (uintptr_t) obj->modelInstances[i]);
            }
        }
    } else if (objType == OBJECT_MODEL_TYPE_MISC) {
        for (i = 0; i < count; i++) {
            if (obj->textures[i] != NULL) {
                tex_free((TextureHeader *) (uintptr_t) obj->textures[i]);
            }
        }
    } else { // Sprite
        for (i = 0; i < count; i++) {
            if (obj->sprites[i] != NULL) {
                sprite_free(obj->sprites[i]);
            }
        }
    }
}
/**
 * Official Name: lightSetupLightSources
 */
void light_setup_light_sources(Object *obj) {
    s32 i;
    for (i = 0; i < obj->header->numLightSources; i++) {
        obj->lightData[i] = light_add_from_object_header(obj, &DKR_PTR(ObjectHeader24, obj->header->unk24)[i]);
    }
}

/**
 * Sets the shading properties of the object.
 */
s32 init_object_shading(Object *obj, ShadeProperties *shadeData) {
    s32 returnSize;
    s32 i;

    obj->shading = shadeData;
    returnSize = 0;
    if (obj->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
        for (i = 0; obj->modelInstances[i] == NULL; i++) {}
        if (obj->modelInstances[i] != NULL && obj->modelInstances[i]->objModel->normals != 0) {
            set_shading_properties(obj->shading, obj->header->shadeAmbient, obj->header->shadeDiffuse, 0,
                                   obj->header->shadeAngleY, obj->header->shadeAngleZ);
            if (obj->header->shadeIntensityy != 0) {
                obj->shading->lightR = obj->header->unk3A;
                obj->shading->lightG = obj->header->unk3B;
                obj->shading->lightB = obj->header->unk3C;
                obj->shading->lightIntensity = obj->header->shadeIntensityy;
                obj->shading->lightDirX = -(obj->shading->shadowDirX >> 1);
                obj->shading->lightDirY = -(obj->shading->shadowDirY >> 1);
                obj->shading->lightDirZ = -(obj->shading->shadowDirZ >> 1);
            }
            returnSize = sizeof(ShadeProperties);
        }
    } else if (obj->header->modelType == OBJECT_MODEL_TYPE_SPRITE_BILLBOARD) {
        obj->shading->unk0 = 1.0f;
        shadeData->lightR = 255;
        shadeData->lightG = 255;
        shadeData->lightB = 255;
        shadeData->lightIntensity = 0;
        returnSize = 8;
    }
    if (returnSize == 0) {
        obj->shading = NULL;
    }
    return (returnSize & ~3) + 4;
}

s32 obj_init_attachpoint(Object *obj) {
    Object *attachObj;
    AttachPoint *attachPoint;
    s32 i;
    s32 failed;

    attachPoint = obj->attachPoints;
    attachPoint->count = obj->header->attachPointCount;
    attachPoint->count = attachPoint->count; // Fakematch?
    failed = FALSE;
    for (i = 0; i < attachPoint->count; i++) {
        attachPoint->obj[i] = obj_spawn_attachment(DKR_PTR(s32, obj->header->vehiclePartIds)[i ^ 0]); // i ^ 0 fakematch
        if (attachPoint->obj[i] == NULL) {
            failed = TRUE;
        }
    }
    if (failed) {
        for (i = 0; i < attachPoint->count; i++) {
            attachObj = attachPoint->obj[i];
            if (attachObj != NULL) {
                objFreeAssets(attachObj, attachObj->header->numberOfModelIds, attachObj->header->modelType);
                try_free_object_header(attachObj->headerType);
                mempool_free(attachObj);
            }
        }
        return TRUE;
    }
    attachPoint->unk2C = DKR_PTR(s8, obj->header->vehiclePartIndices);
    return FALSE;
}

s32 obj_init_emitter(Object *obj, ParticleEmitter *emitter) {
    ObjHeaderParticleEntry *particleDataEntry;
    s32 i;

    obj->particleEmitter = emitter;
    particleDataEntry = DKR_PTR(ObjHeaderParticleEntry, obj->header->objectParticles);
    for (i = 0; i < obj->header->particleCount; i++) {
        if ((particleDataEntry[i].upper & 0xFFFF0000) == 0xFFFF0000) {
            emitter_init(&obj->particleEmitter[i], (particleDataEntry[i].upper >> 8) & 0xFF,
                         particleDataEntry[i].upper & 0xFF);
        } else {
            emitter_init_with_pos(&obj->particleEmitter[i], (particleDataEntry[i].upper >> 0x18) & 0xFF,
                                  (particleDataEntry[i].upper >> 0x10) & 0xFF, particleDataEntry[i].upper & 0xFFFF,
                                  (particleDataEntry[i].lower >> 0x10) & 0xFFFF, particleDataEntry[i].lower & 0xFFFF);
        }
    }
    return ((obj->header->particleCount * sizeof(ParticleEmitter)) + 3) & ~3;
}

/**
 * Assigns shadow data to an object. Loads and assigns the shadow texture, too.
 * Returns zero if the texture is missing.
 */
s32 init_object_shadow(Object *obj, ShadowData *shadow) {
    ObjectHeader *objHeader;

    obj->shadow = shadow;
    shadow->texture = NULL;
    objHeader = obj->header;
    if (objHeader->shadowGroup) {
        shadow->texture = load_texture(objHeader->unk34);
        objHeader = obj->header;
    }
    shadow->scale = objHeader->shadowScale;
    shadow->meshStart = -1;
    D_8011AE50 = shadow->texture;
    if (obj->header->shadowGroup && shadow->texture == NULL) {
        return 0;
    }
    return sizeof(ShadowData);
}

/**
 * Assigns water effect data to an object. Loads and assigns the effect texture, too.
 * Returns zero if the texture is missing.
 */
s32 init_object_water_effect(Object *obj, WaterEffect *waterEffect) {
    obj->waterEffect = waterEffect;
    waterEffect->scale = obj->header->unk8;
    waterEffect->textureFrame = 0;
    waterEffect->animationSpeed = obj->header->unk0 >> 8;
    waterEffect->texture = NULL;
    if (obj->header->waterEffectGroup) {
        waterEffect->texture = load_texture(obj->header->unk38);
    }
    waterEffect->meshStart = -1;
    D_8011AE54 = waterEffect->texture;
    if (obj->header->waterEffectGroup && waterEffect->texture == NULL) {
        return 0;
    }
    return sizeof(WaterEffect);
}

/**
 * Writes object interatction properties to the object.
 * Returns 40, to offset the pointer position used
 */
s32 init_object_interaction_data(Object *obj, ObjectInteraction *interactObj) {
    obj->interactObj = interactObj;
    interactObj->distance = 0xFF;
    return sizeof(ObjectInteraction);
}

/**
 * Sets up collision surface data for the object model.
 */
s32 obj_init_collision(Object *obj, ObjectCollision *colData) {
    obj->collisionData = colData;
    func_80016BC4(obj);
    return sizeof(ObjectCollision);
}

/**
 * Attempts to spawn an attachment object.
 * Similar to the regular object spawning function, but cut down considerably.
 */
Object *obj_spawn_attachment(s32 objID) {
    s32 modelType;
    Object *object;
    ObjectHeader *objHeader;
    s32 objSize;
    s32 i;
    s32 failedToLoadModel;
    s8 numModelIds;
    u8 *objectAsRawBytes;

    if (objID >= gAssetsObjectHeadersTableLength) {
        objID = 0;
    }
    objHeader = load_object_header(objID);
    if (objHeader == NULL) {
        return NULL;
    }
#ifdef NATIVE_PORT
    /* LP64: the allocation must hold the Object struct followed by the
     * modelInstances/sprites array laid out at &object[1] (line ~2510). The N64
     * formula hardcodes sizeof(Object)=0x80 and pointer=4; on a 64-bit host the
     * struct is larger and each array slot is 8 bytes, so the original size
     * under-allocates and the array writes spill into the next object (its 0x68
     * union then reads back as garbage -> render_3d_model faults). Size it from
     * the real struct + pointer widths. */
    objSize = (s32) sizeof(Object) + (objHeader->numberOfModelIds * (s32) sizeof(void *));
#else
    objSize = (objHeader->numberOfModelIds * 4) + 0x80;
#endif
    object = (Object *) mempool_alloc(objSize, COLOUR_TAG_BLUE);
    if (object == NULL) {
        try_free_object_header(objID);
        return NULL;
    }

    // clang-format off
    objectAsRawBytes = (u8 *) object;
    for (i = 0; i < objSize; i++) { objectAsRawBytes[i] = 0; } // Must be one line! (Why not use bzero?)
    // clang-format on

    object->trans.flags = OBJ_FLAGS_UNK_0002;
    object->header = objHeader;
    object->headerType = objID;
    object->objectID = objID;
    object->trans.scale = objHeader->scale;
    if (objHeader->flags & HEADER_FLAGS_UNK_0080) {
        object->trans.flags |= OBJ_FLAGS_UNK_0080;
    }
    numModelIds = object->header->numberOfModelIds;
    modelType = object->header->modelType;
    object->modelInstances = (ModelInstance **) &object[1];

    failedToLoadModel = FALSE;
    if (modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
        for (i = 0; i < numModelIds; i++) {
            object->modelInstances[i] = object_model_init(DKR_PTR(s32, object->header->modelIds)[i], 0);
            if (object->modelInstances[i] == NULL) {
                failedToLoadModel = TRUE;
            }
        }
    } else {
        for (i = 0; i < numModelIds; i++) {
            object->sprites[i] = tex_load_sprite(DKR_PTR(s32, object->header->modelIds)[i], 10);
            if (object->sprites[i] == NULL) {
                failedToLoadModel = TRUE;
            }
        }
    }
    if (failedToLoadModel) {
        objFreeAssets(object, numModelIds, modelType);
        try_free_object_header(objID);
        mempool_free(object);
        return NULL;
    }

    return object;
}

/**
 * Adds the object to the free list.
 * This object will be deallocated on the next update cycle.
 * Official Name: objFreeObject
 */
void free_object(Object *object) {
#ifdef NATIVE_PORT
    taj_visual_on_object_free(object);
    /* Particle allocation and recycling are presentation-owned and may change
     * when a scene draw is elided. Keep the gameplay-event stream restricted
     * to authoritative object lifecycle changes; snapshot identity still
     * tracks particles independently for interpolation safety. */
    if (!(object->trans.flags & OBJ_FLAGS_PARTICLE)) {
        GAMEPLAY_EVENT_TRACE(
            GAMEPLAY_EVENT_DESPAWN, object->objectID, object->headerType,
            FALSE, gFreeListCount);
    }
#endif
    func_800245B4(object->objectID | OBJ_FLAGS_PARTICLE);
    gParticlePtrList[gFreeListCount] = object;
    gFreeListCount++;
}

/**
 * Return the length of the object ID table.
 */
UNUSED s32 obj_table_ids(void) {
    return gAssetsLvlObjTranslationTableLength;
}

/**
 * Return true if the object ID is not higher than the header table length.
 */
UNUSED s32 obj_id_valid(s32 arg0) {
    return (gAssetsLvlObjTranslationTable[arg0] < gAssetsObjectHeadersTableLength);
}

/*
 * Clears all existing particles from the object list
 */
void gParticlePtrList_flush(void) {
    s32 j, i, search_indx;
    Object *searchObj;

    D_8011AE88 = 0;
    for (i = 0; i < gFreeListCount; i++) {
        search_indx = -1;
        searchObj = gParticlePtrList[i];

        for (j = 0; j < gObjectCount; j++) {
            if (searchObj == gObjPtrList[j]) {
                search_indx = j;
            }
        }

        // if object found
        if (search_indx != -1) {
            if (search_indx < gFirstActiveObjectId) {
                gFirstActiveObjectId--;
            }
            gObjectCount--;
            if (0) {} // Fakematch
            for (j = search_indx; j < gObjectCount; j++) {
                gObjPtrList[j] = gObjPtrList[j + 1];
            }
        }
        obj_destroy(searchObj, 0);
    }
    gFreeListCount = 0;
}

/**
 * Destroys an object and frees its resources.
 */
void obj_destroy(Object *obj, s32 arg1) {
    Object *tempObj;
    Object_Weapon *weapon;
    Object_Racer *racer;
    Object_AnimatedObject *snowball;
    Object_Weapon *fireball;
    Object_Log *log;
    Object_Butterfly *butterfly;
    SoundHandle soundMask;
    s32 numberOfModelIds;
    s32 i;
    s32 j;
    Object_AnimatedObject *obj64;
    ModelInstance *models;
    s32 modelType;

#ifdef NATIVE_PORT
    taj_visual_on_object_destroy(obj);
    /*
     * The retire half of the snapshot identity.
     * obj_destroy is the ONE place an Object's memory actually returns to
     * the pool: free_object() only queues onto gParticlePtrList (which,
     * despite the name, is the deferred FREE list), and
     * gParticlePtrList_flush() drains it through here. The level teardown
     * loop in objects.c also calls obj_destroy directly. Retiring the
     * identity here is therefore complete, and it is what makes a recycled
     * address get a fresh generation instead of inheriting a dead object's.
     * No-op unless MDKR_PRESENT_SNAPSHOT is set.
     */
    presentation_snapshot_note_free(obj);
#endif
    if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
        particle_deallocate((Particle *) obj);
        gParticleCount--;
        return;
    }

    if (obj->attachPoints != NULL) {
        for (i = 0; i < obj->attachPoints->count; i++) {
            tempObj = obj->attachPoints->obj[i];
            numberOfModelIds = tempObj->header->numberOfModelIds;
            modelType = tempObj->header->modelType;
            if (modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
                for (j = 0; j < numberOfModelIds; j++) {
                    free_3d_model((ModelInstance *) &tempObj->modelInstances[j]->objModel);
                }
            } else {
                for (j = 0; j < numberOfModelIds; j++) {
                    sprite_free(tempObj->sprites[j]);
                }
            }
            try_free_object_header(tempObj->headerType);
            mempool_free(tempObj);
        }
    }
    if (obj->lightData != NULL) {
        for (i = 0; i < obj->header->numLightSources; i++) {
            light_remove(obj->lightData[i]);
        }
    }
    switch (obj->behaviorId) {
        case BHV_RACER:
        case BHV_ANIMATED_OBJECT_3:
            for (i = 0; i < gObjectCount; i++) {
                tempObj = gObjPtrList[i];
                if (tempObj->behaviorId == BHV_BUTTERFLY) {
                    butterfly = tempObj->butterfly;
                    if (obj == butterfly->unk100) {
                        butterfly->unk100 = 0;
                        butterfly->unkFD = 1;
                    }
                }
            }

            i = BHV_RACER;
            break;
        case BHV_WEAPON:
        case BHV_WEAPON_2:
            weapon = obj->weapon;
            if (weapon->soundMask != NULL) {
                audspat_point_stop(weapon->soundMask);
                weapon->soundMask = NULL;
                if (obj->behaviorId == BHV_WEAPON_2) {
                    decrease_rocket_sound_timer();
                }
            }

            i = BHV_RACER;
            break;
        case BHV_FIREBALL_OCTOWEAPON_2:
            weapon = obj->weapon;
            if (weapon->soundMask != NULL) {
                audspat_point_stop(weapon->soundMask);
            }

            i = BHV_RACER;
            break;
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
            snowball = obj->animatedObject;
            if (snowball->soundMask != NULL) {
                audspat_point_stop((AudioPoint *) snowball->soundMask);
            }

            i = BHV_RACER;
            break;
        case BHV_WAVE_GENERATOR:
            wavegen_destroy(obj);

            i = BHV_RACER;
            break;
        case BHV_LIGHT_RGBA:
            light_remove(obj->light);

            i = BHV_RACER;
            break;
        case BHV_ANIMATION:
            if (obj->animTarget != NULL && arg1 == 0) {
                free_object(obj->animTarget);
            }

            i = BHV_RACER;
            break;
        case BHV_OVERRIDE_POS:
            for (j = 0; j < D_8011AE00 && obj != D_8011ADD8[j]; j++) {}
            if (j < D_8011AE00) {
                D_8011AE00--;
                for (; j < D_8011AE00; j++) {
                    D_8011ADD8[j] = D_8011ADD8[j + 1];
                }
            }

            i = BHV_RACER;
            break;
        case BHV_BUOY_PIRATE_SHIP:
        case BHV_LOG:
            log = obj->log;
            if (log != NULL) {
                mempool_free(log);
            }

            i = BHV_RACER;
            break;
        case BHV_LENS_FLARE:
            lensflare_remove(obj);

            i = BHV_RACER;
            break;
        case BHV_LENS_FLARE_SWITCH:
            lensflare_override_remove(obj);

            i = BHV_RACER;
            break;
        default:

            i = BHV_RACER;
            break;
    }
    switch (obj->behaviorId) {
        case BHV_DINO_WHALE:
        case BHV_ANIMATED_OBJECT:
        case BHV_CAMERA_ANIMATION:
        case BHV_CAR_ANIMATION:
        case BHV_CHARACTER_SELECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_PARK_WARDEN_2:
        case BHV_ANIMATED_OBJECT_2:
        case BHV_WIZPIG_SHIP:
        case BHV_ANIMATED_OBJECT_3:
        case BHV_ANIMATED_OBJECT_4:
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
        case BHV_HIT_TESTER_3:
        case BHV_HIT_TESTER_4:
        case BHV_DOOR_OPENER:
        case BHV_PIG_ROCKETEER:
        case BHV_WIZPIG_GHOSTS:
            obj64 = obj->animatedObject;
            soundMask = obj64->unk18;
            if (soundMask != NULL) {
                sndp_stop(soundMask);
            }
            break;
    }
    if (obj->behaviorId == i) {
        racer = obj->racer;
        if (racer->unk18 != NULL) {
            sndp_stop((SoundHandle) (uintptr_t) racer->unk18); // LP64: full ptr (sndp_stop derefs)
        }
        if (racer->unk10 != NULL) {
            sndp_stop((SoundHandle) (uintptr_t) racer->unk10); // LP64: full ptr (sndp_stop derefs)
        }
        if (racer->unk14 != NULL) {
            sndp_stop((SoundHandle) (uintptr_t) racer->unk14); // LP64: full ptr (sndp_stop derefs)
        }
        if (racer->unk1C != NULL) {
            sndp_stop((SoundHandle) (uintptr_t) racer->unk1C); // LP64: full ptr (sndp_stop derefs)
        }
        if (racer->unk20 != NULL) {
            sndp_stop((SoundHandle) (uintptr_t) racer->unk20); // LP64: full ptr (sndp_stop derefs)
        }
        if (racer->soundMask != NULL) {
            audspat_point_stop(racer->soundMask);
        }
        if (racer->shieldSoundMask != NULL) {
            audspat_point_stop(racer->shieldSoundMask);
        }
        if (racer->magnetSoundMask != NULL) {
            sndp_stop(racer->magnetSoundMask);
        }
        racer_sound_free(obj);
        for (j = 0; j < gObjectCount; j++) {
            if ((gObjPtrList[j]->trans.flags & OBJ_FLAGS_PARTICLE) &&
                (gObjPtrList[j]->level_entry == (LevelObjectEntry *) obj)) {
                gObjPtrList[j]->level_entry = NULL;
            }
            if (gObjPtrList[j]->behaviorId == BHV_WEAPON_2 || gObjPtrList[j]->behaviorId == BHV_FLY_COIN ||
                gObjPtrList[j]->behaviorId == BHV_WEAPON) {
                free_object(gObjPtrList[j]);
            }
        }
    }
    if (obj->shadow != NULL && obj->shadow->texture != NULL) {
        tex_free(obj->shadow->texture);
    }
    if (obj->waterEffect != NULL && obj->waterEffect->texture != NULL) {
        tex_free(obj->waterEffect->texture);
    }
    numberOfModelIds = obj->header->numberOfModelIds;
    modelType = obj->header->modelType;
    if (modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
        for (j = 0; j < numberOfModelIds; j++) {
            if (obj->modelInstances[j] != NULL) {
                models = obj->modelInstances[j];
                free_3d_model(models);
            }
        }
    } else if (modelType == OBJECT_MODEL_TYPE_MISC) {
        for (j = 0; j < numberOfModelIds; j++) {
            tex_free(obj->textures[j]);
        }
    } else {
        for (j = 0; j < numberOfModelIds; j++) {
            sprite_free(obj->sprites[j]);
        }
    }
    if (obj->header->particleCount > 0) {
        for (j = 0; j < obj->header->particleCount; j++) {
            emitter_cleanup(&obj->particleEmitter[j]);
        }
    }
    try_free_object_header(obj->headerType);
    mempool_free(obj);
}

/**
 * Updates all objects in the game.
 */
void obj_update(s32 updateRate) {
    s32 i;
    s32 j;
    Object_Racer *racer;
    ModelInstance *modInst;
    s32 sp54;
    Object *obj;

    func_800245B4(-1);
    gEventStartTimer = gEventCountdown;
    if (gEventCountdown > 0 && race_starting() != FALSE) {
        gEventCountdown -= updateRate;
        D_8011ADBC = 0;
    } else {
        D_8011ADBC += updateRate;
    }
    if (gEventCountdown <= 0) {
        gEventCountdown = 0;
    }
    D_8011AD3D = 0;
    D_8011AD21 = 1 - D_8011AD21;
    D_8011AD22[D_8011AD21] = 0;
    for (j = 0; j < gNumRacers; j++) {
        racer = (*gRacers)[j]->racer;
        racer->prev_x_position = (*gRacers)[j]->trans.x_position;
        racer->prev_y_position = (*gRacers)[j]->trans.y_position;
        racer->prev_z_position = (*gRacers)[j]->trans.z_position;
    }
    obj_tick_anims();
    process_object_interactions();
    func_8001E89C();
    // Update collidable objects first.
    for (i = 0; i < gCollisionObjectCount; i++) {
        run_object_loop_func(gCollisionObjects[i], updateRate);
    }
    func_8001E6EC(TRUE);
    for (i = 0; i < gCollisionObjectCount; i++) {
        obj_collision_transform(gCollisionObjects[i]);
    }
    // Update nonspecific objects
    j = gObjectCount;
    for (i = gObjectListStart; i < j; i++) {
        obj = gObjPtrList[i];
        if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            if ((obj->behaviorId != BHV_LIGHT_RGBA) && (obj->behaviorId != BHV_WEAPON) &&
                (obj->behaviorId != BHV_FOG_CHANGER)) {
                if (obj->interactObj != NULL) {
                    if (obj->interactObj->unk11 != 2) {
                        run_object_loop_func(obj, updateRate);
                    }
                } else {
                    run_object_loop_func(obj, updateRate);
                }
                if (obj->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
                    for (sp54 = 0; sp54 < obj->header->numberOfModelIds; sp54++) {
                        modInst = obj->modelInstances[sp54];
                        if (modInst != NULL) {
                            modInst->objModel->texOffsetUpdateRate = updateRate;
                        }
                    }
                    if (obj->header->unk72 != 0xFF) {
                        func_80014090(obj, updateRate);
                    }
                }
            }
        }
    }
    // Update racers
    for (i = 0; i < gNumRacers; i++) {
        update_player_racer((*gRacers)[i], updateRate);
    }
#ifdef NATIVE_PORT
    /* Physics has completed; companions now inherit the authoritative pose. */
    taj_visual_tick(updateRate);
#endif
    if (level_type() == RACETYPE_DEFAULT) {
        for (i = 0; i < gNumRacers; i++) {
            racer = gRacersByPosition[i]->racer;
            if (racer->playerIndex != -1) {
                increment_ai_behaviour_chances(gRacersByPosition[i], racer, updateRate);
                i = gNumRacers; // Why not just break?
            }
        }
    }
    racerfx_update(updateRate);
    for (i = gObjectListStart; i < j; i++) {
        obj = gObjPtrList[i];
        if ((!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && (obj->behaviorId == BHV_WEAPON)) ||
            (obj->behaviorId == BHV_FOG_CHANGER)) {
            run_object_loop_func(obj, updateRate);
        }
    }
    // Update particles
    if (gParticleCount > 0) {
        for (i = gObjectListStart; i < j; i++) {
            obj = gObjPtrList[i];
            if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                particle_update((Particle *) obj, updateRate);
            }
        }
    }

    // Update lights
    light_update_all(updateRate);
    if (light_count() > 0) {
        for (i = gObjectListStart; i < gObjectCount; i++) {
            obj = gObjPtrList[i];
            if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && (obj->shading != NULL)) {
                light_update_shading(obj);
            }
        }
    }
    func_8001E6EC(FALSE);
    if (gTajRaceInit) {
        mode_init_taj_race();
    }
    if (gPathUpdateOff == FALSE) {
        gParticlePtrList_flush();
        checkpoint_update_all();
        spectate_update();
        func_8001E93C();
    }
    if (gNumRacers != 0) {
        if (gRaceEndTimer == 0) {
            race_check_finish(updateRate);
        } else {
            race_transition_adventure(updateRate);
        }
    }
    audspat_update_all(gRacersByPort, gNumRacers, updateRate);
    gPathUpdateOff = TRUE;
    gObjectUpdateRateF = (f32) updateRate;
    D_8011AD24[0] = FALSE;
    D_8011AD53 = 0;
    transform_player_vehicle();
    dialogue_try_close();
    func_800179D0();

    // @fake
    do {
    } while (0);
    if (D_8011AF00 == 1) {
        if (gEventCountdown == 80 && gCutsceneID == CUTSCENE_NONE) {
            sp54 = 0;
            for (j = 0; j < MAXCONTROLLERS; j++) {
                sp54 |= input_pressed(j);
            }

            if (sp54 & A_BUTTON) {
                func_8001E45C(CUTSCENE_ID_UNK_64);
            } else if ((sp54 & B_BUTTON) && (get_trophy_race_world_id() == 0) && (is_in_tracks_mode() == 0)) {
                level_transition_begin(1); // FADE_BARNDOOR_HORIZONTAL?
            }
        }
    } else if (D_8011AF00 == 0) {
        D_8011AF00 = 1;
    }
}

/**
 * Handles texture animation for an object.
 * Applies texture offset based on the update rate.
 */
static void obj_tex_animate_model(ObjectModel *model, s32 updateRate, s32 authoredRng) {
    TriangleBatchInfo *batches;
    s32 offset;
    TextureHeader *tex;
    s16 textureIsAnimated;
    s32 batchNumber;
    batches = DKR_PTR(TriangleBatchInfo, model->batches);
    textureIsAnimated = model->hasAnimatedTexture;
    for (batchNumber = 0; textureIsAnimated > 0 && batchNumber < model->numberOfBatches; batchNumber++) {
        if (batches[batchNumber].flags & RENDER_TEX_ANIM) {
            if (batches[batchNumber].textureIndex != TEX_INDEX_NO_TEXTURE) {
                tex = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, model->textures)[batches[batchNumber].textureIndex].texture);
                offset = batches[batchNumber].texOffset;
                offset <<= 6;
#ifdef NATIVE_PORT
                if (authoredRng) {
                    tex_animate_texture_cadence_compat(
                        tex, &batches[batchNumber].flags, &offset,
                        updateRate);
                } else {
                    tex_animate_texture(tex, &batches[batchNumber].flags, &offset, updateRate);
                }
#else
                tex_animate_texture(tex, &batches[batchNumber].flags, &offset, updateRate);
#endif
                batches[batchNumber].texOffset = (offset >> 6) & 0xFF;
            }
        }
    }
}

void obj_tex_animate(Object *obj, s32 updateRate) {
    ModelInstance *modInst;

#ifdef NATIVE_PORT
    modInst = obj->modelInstances[object_render_model_index(obj)];
#else
    modInst = obj->modelInstances[obj->modelIndex];
#endif
    obj_tex_animate_model(modInst->objModel, updateRate, FALSE);
}

#ifdef NATIVE_PORT
/**
 * Commit the ordinary scene traversal's one texture-animation opportunity in
 * fixed-tick authority. The caller supplies this viewport's private distance
 * so racers select the same LOD model the draw would have selected. Render sees
 * texOffsetUpdateRate == 0 afterwards and cannot advance either RNG stream.
 *
 * Door numerals are deliberately absent. Multiple door objects share one
 * ObjectModel, but each object can require a different balloon count. Their
 * texture offsets therefore belong to display-list construction, where the
 * current Object is known, rather than to this shared-model tick.
 */
void obj_authoritative_texture_tick(Object *obj, s32 updateRate, f32 viewDistance) {
    ModelInstance *modInst;
    ObjectModel *model;
    s32 modelIndex = obj->modelIndex;

    if (obj == NULL || obj->header == NULL || obj->modelInstances == NULL ||
        (obj->trans.flags & (OBJ_FLAGS_PARTICLE | OBJ_FLAGS_INVISIBLE |
                             OBJ_FLAGS_SHADOW_ONLY)) ||
        obj->header->modelType != OBJECT_MODEL_TYPE_3D_MODEL) {
        return;
    }
    if (obj->header->behaviorId == BHV_RACER && obj->racer != NULL) {
        f32 unusedScale;
        modelIndex = racer_model_index_for_view(
            obj, obj->racer, viewDistance, &unusedScale);
    }
    modInst = obj->modelInstances[modelIndex];
    if (modInst == NULL || modInst->objModel == NULL) {
        return;
    }
    model = modInst->objModel;
    if (model->texOffsetUpdateRate && model->hasAnimatedTexture > 0) {
        obj_tex_animate_model(model, model->texOffsetUpdateRate, TRUE);
        model->texOffsetUpdateRate = 0;
    }
}
#endif

/**
 * Sets the texture offset on the door number based on the balloon requirement.
 */
static s32 obj_door_batch_texture_offset(const ObjectModel *model,
                                         const Object *obj,
                                         s32 batchIndex, s32 *outOffset,
                                         s32 *outDigitPlace) {
    const Object_Door *door;
    s32 current;
    s32 remaining;
    const TriangleBatchInfo *batch;
    const TextureInfo *textures;
    const TextureHeader *texture;

    if (model == NULL || obj == NULL || outOffset == NULL ||
        obj->behaviorId != BHV_DOOR || obj->door == NULL ||
        model->hasAnimatedTexture <= 0 || batchIndex < 0 ||
        batchIndex >= model->numberOfBatches || model->numberOfTextures <= 0) {
        return FALSE;
    }

    batch = DKR_PTR(const TriangleBatchInfo, model->batches);
    if (!(batch[batchIndex].flags & RENDER_TEX_ANIM) ||
        batch[batchIndex].textureIndex == TEX_INDEX_NO_TEXTURE ||
        batch[batchIndex].textureIndex >= model->numberOfTextures) {
        return FALSE;
    }
    textures = DKR_PTR(const TextureInfo, model->textures);
    texture = DKR_PTR(
        const TextureHeader,
        textures[batch[batchIndex].textureIndex].texture);
    if (texture == NULL) {
        return FALSE;
    }
    door = obj->door;
    remaining = door->balloonCount;
    current = ((remaining / 10) - 1) * 4;
    remaining = (remaining % 10) * 4;

    if (texture->numOfTextures > 0x900) {
        *outOffset = remaining;
        if (outDigitPlace != NULL) {
            *outDigitPlace = 1;
        }
        return TRUE;
    }
    if (current >= 0) {
        *outOffset = current;
        if (outDigitPlace != NULL) {
            *outDigitPlace = 10;
        }
        return TRUE;
    }
    return FALSE;
}

#ifndef NATIVE_PORT
/* The original renderer selected a door numeral immediately before emitting
 * that door's display list. Keep this mutating compatibility path out of the
 * native build: native ObjectModels are cached and shared, so per-object
 * material state must be resolved by render_mesh without writing the model. */
void obj_door_number(ObjectModel *model, Object *obj) {
    s32 i;
    s32 offset;
    TriangleBatchInfo *batch;

    if (model == NULL || model->hasAnimatedTexture <= 0) {
        return;
    }

    batch = DKR_PTR(TriangleBatchInfo, model->batches);
    i = 0;
    while (i < model->numberOfBatches) {
        if (obj_door_batch_texture_offset(model, obj, i, &offset, NULL)) {
            batch[i].texOffset = offset;
        }
        i++;
    }
}
#endif

/**
 * Do nothing. Unused.
 */
UNUSED void do_nothing_func_80011364(UNUSED s32 unused) {
}

/**
 * Return true if paths are intended to be updated.
 * The variable they use here is backwards in terms of use.
 * Yes means no, no means yes.
 */
UNUSED s32 path_update_check(void) {
    // Ever hear of return !gPathUpdateOff?
    if (gPathUpdateOff) {
        return FALSE;
    } else {
        return TRUE;
    }
}

/**
 * Signal to the game that checkpoints should be updated.
 */
void path_enable(void) {
    gPathUpdateOff = FALSE;
}

/**
 * Return the current race countdown timer.
 */
s32 get_race_countdown(void) {
    return gEventCountdown;
}

/**
 * Return the timer that the countdown is set to before the race starts.
 * There exists another variable in racer.c with exactly the same purpose.
 * This does not get used anywhere else.
 */
s32 get_race_start_timer(void) {
    return gEventStartTimer;
}

// Unused function, purpose currently unknown.
UNUSED s32 func_800113BC(void) {
    return D_8011ADBC;
}

/**
 * When the object reaches a certain anim frame, play a sound and shake the camera to emphasise the effect of their
 * movement.
 */
s32 play_footstep_sounds(Object *obj, s32 arg1, s32 frame, s32 oddSoundId, s32 evenSoundId) {
    s8 *asset;
    f32 shakeDist;
    f32 shakeMagnitude;
    s32 animFrame;
    s32 asset0;
    s8 nextAsset;
    s32 i;
    s32 ret;
    s32 soundId;

    ret = 0;
    if (arg1 < obj->header->unk5B) {
        // TODO: Figure this one out better. The index could be something like this:
        // obj->header->internalName[arg1 - 4]
        asset = (s8 *) get_misc_asset(*(&obj->header->unk5C + arg1));
        asset0 = asset[0];
        shakeDist = (asset[1] & 0xFF) * 8.0f;
        shakeMagnitude = asset[2];
        frame >>= 4;
        animFrame = obj->animFrame >> 4;
        for (i = 0; i < asset0; i++) {
            nextAsset = asset[i + 3];
            if ((animFrame >= nextAsset && frame < nextAsset) || (nextAsset >= animFrame && nextAsset < frame)) {
                set_camera_shake_by_distance(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                                             shakeDist, shakeMagnitude);
                if (i & 1) {
                    soundId = oddSoundId; // Always set to SOUND_STOMP2
                } else {
                    soundId = evenSoundId; // Always set to SOUND_STOMP3
                }
                audspat_play_sound_at_position(soundId, obj->trans.x_position, obj->trans.y_position,
                                               obj->trans.z_position, AUDIO_POINT_FLAG_ONE_TIME_TRIGGER, NULL);
                ret = i + 1;
                i = asset0; // Come on, just use break!
            }
        }
    }
    return ret;
}

/**
 * Make the next call of move_object never mark the object as out of bounds.
 * Official Name: objMoveXYZnocheck
 */
void ignore_bounds_check(void) {
    gNoBoundsCheck = TRUE;
}

/**
 * Sets the new position of the object using the differences given.
 * Compares against the outer edges of the level geometry to decide wether or not to apply a segment ID.
 * Official Name: objMoveXYZ
 */
s32 move_object(Object *obj, f32 xPos, f32 yPos, f32 zPos) {
    s32 segmentID;
    f32 x1, x2, y1, y2, z1, z2;
    f32 newXPos;
    f32 newYPos;
    f32 newZPos;
    LevelModel *levelModel;
    LevelModelSegmentBoundingBox *box;
    s32 outsideBBox;
    s32 outOfBounds;
    s32 intXPos, intYPos, intZPos;

    levelModel = get_current_level_model();
    newXPos = obj->trans.x_position + xPos;
    newYPos = obj->trans.y_position + yPos;
    newZPos = obj->trans.z_position + zPos;
    if (levelModel == NULL) {
        gNoBoundsCheck = FALSE;
        return FALSE;
    }
    outOfBounds = FALSE;
    x2 = (levelModel->upperXBounds + 1000.0);
    //!@bug should've compared against "obj->trans.x_position"
    if (newXPos > x2) {
        outOfBounds = TRUE;
    }
    x1 = (levelModel->lowerXBounds - 1000.0);
    if (obj->trans.x_position < x1) {
        outOfBounds = TRUE;
    }
    if (1) {}
    if (1) {}
    if (1) {} // Fakematch
    y2 = (levelModel->upperYBounds + 3000.0);
    if (obj->trans.y_position > y2) {
        outOfBounds = TRUE;
    }
    y1 = (levelModel->lowerYBounds - 500.00);
    if (obj->trans.y_position < y1) {
        outOfBounds = TRUE;
    }
    z2 = (levelModel->upperZBounds + 1000.0);
    if (obj->trans.z_position > z2) {
        outOfBounds = TRUE;
    }
    z1 = (levelModel->lowerZBounds - 1000.0);
    if (obj->trans.z_position < z1) {
        outOfBounds = TRUE;
    }
    if (gNoBoundsCheck) {
        outOfBounds = FALSE;
    }

    gNoBoundsCheck = FALSE;
    if (outOfBounds) {
        obj->segmentID = -1;
        return TRUE;
    }

    obj->trans.x_position = newXPos;
    obj->trans.y_position = newYPos;
    obj->trans.z_position = newZPos;
    box = block_boundbox(obj->segmentID);

    // For some reason the XYZ positions are converted into integers for the next section
    intXPos = newXPos, intYPos = newYPos, intZPos = newZPos;

    if (box == NULL) {
        obj->segmentID = get_level_segment_index_from_position(intXPos, intYPos, intZPos);
        return FALSE;
    } else {
        outsideBBox = FALSE;
        if (box->x2 < intXPos || intXPos < box->x1) {
            outsideBBox = TRUE;
        }
        if (box->y2 < intYPos || intYPos < box->y1) {
            outsideBBox = TRUE;
        }
        if (box->z2 < intZPos || intZPos < box->z1) {
            outsideBBox = TRUE;
        }
        if (outsideBBox) {
            segmentID = get_level_segment_index_from_position(intXPos, intYPos, intZPos);
            if (segmentID != -1) {
                obj->segmentID = segmentID;
            }
        }
    }
    return FALSE;
}

/**
 * Set up the basic model view matrix, load a texture, then render the mesh.
 * A much simpler, faster way to render an object model as opposed to render_3d_model
 */
void render_misc_model(Object *obj, Vertex *verts, u32 numVertices, Triangle *triangles, u32 numTriangles,
                       TextureHeader *tex, u32 flags, u32 texOffset, f32 scaleY) {
    s32 hasTexture = FALSE;
    mtx_cam_push(&gObjectCurrDisplayList, &gObjectCurrMatrix, &obj->trans, scaleY, 0.0f);
    gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
    gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
    if (tex != NULL) {
        hasTexture = TRUE;
    }
    material_set(&gObjectCurrDisplayList, tex, flags, texOffset);
    gSPVertexDKR(gObjectCurrDisplayList++, OS_K0_TO_PHYSICAL(verts), numVertices, 0);
    gSPPolygon(gObjectCurrDisplayList++, OS_K0_TO_PHYSICAL(triangles), numTriangles, hasTexture);
    mtx_pop(&gObjectCurrDisplayList);
}

/**
 * A few objects use unconventional means to render. They are handled here.
 */
void render_3d_misc(Object *obj) {
    f32 scaleY;
    Object_Fish *fish;
    Object_Butterfly *butterfly;
    CharacterFlagModel *characterFlagModel;

    switch (obj->behaviorId) {
        case BHV_CHARACTER_FLAG:
            if (obj->properties.characterFlag.characterID >= 0) {
                characterFlagModel = obj->characterFlagModel;
                render_misc_model(obj, characterFlagModel->vertices, 4, characterFlagModel->triangles, 2,
                                  characterFlagModel->texture,
                                  RENDER_ANTI_ALIASING | RENDER_Z_COMPARE | RENDER_FOG_ACTIVE, 0, 1.0f);
            }
            break;
        case BHV_BUTTERFLY:
            butterfly = obj->butterfly;
            render_misc_model(obj, &butterfly->vertices[butterfly->unkFC * 6], 6, butterfly->triangles, 8,
                              butterfly->texture, RENDER_Z_COMPARE | RENDER_FOG_ACTIVE, 0, 1.0f);
            break;
        case BHV_FISH:
            fish = obj->fish;
            scaleY = obj->level_entry->fish.unkD;
            scaleY *= 0.01f;
            render_misc_model(obj, &fish->vertices[fish->unkFC * 6], 6, fish->triangles, 8, fish->texture,
                              RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_CUTOUT, 0, scaleY);
            break;
        case BHV_BOOST:
            if (obj->properties.common.unk0 && (obj->boost->unk70 > 0 || obj->boost->unk74 > 0.0f)) {
                func_800135B8(obj);
            }
            break;
    }
}

/**
 * Render an object as a billboard.
 * A few tweaks are made depending on the behaviour ID of the object.
 * A few exceptions will not call to render a billboarded sprite.
 */
void render_3d_billboard(Object *obj) {
    s32 intensity;
    s32 flags;
    s32 alpha;
    s32 hasPrimCol;
    s32 hasEnvCol;
    ObjectTransformExt objTrans;
    Object *bubbleTrap;
    Sprite *sprite;

    intensity = 255;
    hasPrimCol = FALSE;
    hasEnvCol = FALSE;
    flags = obj->trans.flags | RENDER_Z_UPDATE | RENDER_FOG_ACTIVE;
    if (obj->shading != NULL) {
        hasPrimCol = TRUE;
        hasEnvCol = TRUE;
        intensity = obj->shading->unk0 * 255.0f;
    }

    alpha = scene_object_render_opacity(obj);
    if (obj->behaviorId == BHV_BOMB_EXPLOSION) {
        /* opacity is u8, so the original >255 arm was unreachable. Keep the
         * effective multiply, but make it a draw-local value so replaying or
         * skipping presentation cannot compound authoritative opacity. */
        alpha = (alpha * (obj->properties.bombExplosion.opacity & 0xFF)) >> 8;
    }
    if (alpha > 255) {
        alpha = 255;
    }

    // If the behavior is a wizpig ghost, then halve it's transparency.
    if (obj->behaviorId == BHV_WIZPIG_GHOSTS) {
        alpha >>= 1;
    }

    if (alpha < 255) {
        flags |= RENDER_SEMI_TRANSPARENT;
        hasPrimCol = TRUE;
    }
    if ((obj->behaviorId == 5) && (obj->trans.scale == 6.0f)) {
        gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, (intensity * 3) >> 2, intensity, intensity >> 1, alpha);
        hasPrimCol = TRUE;
    } else if (obj->behaviorId == BHV_WIZPIG_GHOSTS) { // If the behavior is a wizpig ghost
        gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 150, 230, 255, alpha);
        hasPrimCol = TRUE;
    } else if (hasPrimCol || alpha < 255) {
        gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, intensity, intensity, intensity, alpha);
    } else {
        gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
    }
    if (hasEnvCol) {
        gDPSetEnvColor(gObjectCurrDisplayList++, obj->shading->lightR, obj->shading->lightG, obj->shading->lightB,
                       obj->shading->lightIntensity);
    } else if (obj->behaviorId == BHV_LAVA_SPURT) {
        hasEnvCol = TRUE;
        gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 0, 255);
    } else {
        gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
    }
    sprite = obj->sprites[obj->modelIndex];
    bubbleTrap = NULL;
    if (obj->behaviorId == BHV_FIREBALL_OCTOWEAPON_2) {
        bubbleTrap = obj->properties.fireball.obj;
        if (obj->properties.fireball.timer > 0) {
            bubbleTrap = obj;
        }
    }

    // 5 = OilSlick, SmokeCloud, Bomb, BubbleWeapon
    if (bubbleTrap != NULL || !(obj->behaviorId != BHV_WEAPON || obj->weapon->weaponID != WEAPON_BUBBLE_TRAP)) {
        objTrans.trans.rotation.z_rotation = 0;
        objTrans.trans.rotation.x_rotation = 0;
        objTrans.trans.rotation.y_rotation = 0;
        objTrans.trans.scale = obj->trans.scale;
        objTrans.trans.x_position = 0.0f;
        objTrans.trans.z_position = 0.0f;
        objTrans.trans.y_position = 12.0f;
        objTrans.animFrame = obj->animFrame;
        objTrans.unk1A = 32;
        if (bubbleTrap == NULL) {
            bubbleTrap = obj->weapon->target;
            if (bubbleTrap == NULL) {
                bubbleTrap = obj;
            }
        }
#ifdef NATIVE_PORT
        render_bubble_trap_transform(
            &bubbleTrap->trans, sprite, &objTrans,
            RENDER_Z_COMPARE | RENDER_SEMI_TRANSPARENT | RENDER_Z_UPDATE);
#else
        render_bubble_trap(
            &bubbleTrap->trans, sprite, (Object *) &objTrans,
            RENDER_Z_COMPARE | RENDER_SEMI_TRANSPARENT | RENDER_Z_UPDATE);
#endif
    } else {
        render_sprite_billboard(&gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList, obj, sprite,
                                flags);
    }
    if (hasPrimCol) {
        gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
    }
    if (hasEnvCol) {
        gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
    }
}

#ifdef NATIVE_PORT
/**
 * Authoritative racer visibility, hoisted out of set_temp_model_transforms so
 * that render admission cannot drive AI RNG.
 *
 * set_temp_model_transforms only ran for objects the RENDER path admitted, and the
 * first thing it did for a racer was `objRacer->unk201 = 30` (:5227). That timer is
 * decremented in update_player_racer (racer.c:4306-4309) and at zero the AI STOPS
 * STEERING ENTIRELY (racer.c:9088 gates racer_AI_pathing_inputs), stops rolling
 * roll_percent_chance -> rand_range for its balloon upgrades (racer.c:382), and has
 * particleEmittersEnabled forced to none (racer.c:2247/3452). Whether a racer was
 * on screen -- and in how many viewports -- was therefore an input to how it drove.
 *
 * This evaluates render's own admission predicate for every viewport of the frame,
 * including the 3P TT-camera viewport, and OR-s the result: a racer
 * admitted by ANY viewport gets the timer. scene_visibility_prepare_viewport()
 * (tracks.c) rebuilds each viewport's camera basis, cull planes and visible-segment
 * set without drawing, and scene_object_admitted() is
 * render_level_geometry_and_objects' test verbatim.
 *
 * Exactness is measured, not argued: a throwaway probe recording every
 * (racer, viewport, frame) admission on both sides -- here and in
 * set_temp_model_transforms -- reported ZERO disagreements in either direction
 * over 6000 frames each of a 1P time trial (26441 agreeing admissions), a 3P
 * split (22014) and a 4P split (32932).
 *
 * Racer LOD and the brake/headlight presentation state are committed separately
 * by obj_lod_tick(). Keeping visibility, LOD and light cadence as distinct pure
 * operations made the multiplayer regression that originally exposed their
 * render ownership reproducible and independently testable.
 */
void obj_visibility_tick(void) {
    extern LevelModel *gCurrentLevelModel; /* tracks.c */
    s32 numViewports;
    s32 savedCameraID;
    s32 ttCam;
    s32 pass;
    s32 i;
    Object *obj;

    if (gCurrentLevelModel == NULL) {
        return;
    }
    savedCameraID = get_current_viewport();
    numViewports = scene_visibility_viewport_count();

    /* render_scene's viewport loop, then its 3P TT-camera fourth viewport. */
    ttCam = numViewports == 3 && level_type() != RACETYPE_CHALLENGE_EGGS &&
            level_type() != RACETYPE_CHALLENGE_BATTLE && level_type() != RACETYPE_CHALLENGE_BANANAS &&
            hud_setting() == 0;

    for (pass = 0; pass < numViewports + (ttCam ? 1 : 0); pass++) {
        scene_visibility_prepare_viewport(pass, numViewports, pass >= numViewports);
        for (i = gObjectListStart; i < gObjectCount; i++) {
            obj = gObjPtrList[i];
            if (obj == NULL || obj->header == NULL || obj->racer == NULL) {
                continue;
            }
            if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                continue;
            }
            /* set_temp_model_transforms keys on the HEADER behaviour, so time-trial
             * ghosts (obj->behaviorId == BHV_TIMETRIAL_GHOST) are included. */
            if (obj->header->behaviorId != BHV_RACER) {
                continue;
            }
            if (!scene_object_admitted(obj)) {
                continue;
            }
            obj->racer->unk201 = 30;
        }
    }

    set_active_camera(savedCameraID);
}

/**
 * Authoritative object animation, hoisted out of render_3d_model.
 *
 * The constraint it enforces:
 *
 *   - `obj->curVertData` was written ONLY on the render path
 *     (objects.c:4772/4785/4802, with obj_animate at :4774) and is READ by
 *     SIMULATION: sphere collision func_80016748 (:6382-6435, which writes racer
 *     velocity and fires rumble) and the Wizpig-ship attach logic
 *     (object_functions.c:1969-1972). An object that was never drawn therefore
 *     kept a NULL curVertData and was silently exempt from sphere collision.
 * This walks gObjPtrList once per authoritative tick, immediately before
 * render_scene, and does the animUpdateTimer-gated animate and the curVertData
 * assignment for EVERY 3D-model object, drawn or not.
 *
 * Faithfulness notes:
 *
 *   - The three curVertData assignments collapse to one. :4772 stores the
 *     pre-animate buffer, obj_animate flips modInst->animationTaskNum
 *     (hasm_native/obj_animate.c:272), and :4785/:4802 re-store the post-animate
 *     buffer; :4802 is unconditional, so the surviving value is always
 *     modInst->vertices[animationTaskNum] AFTER obj_animate. One store, same
 *     result.
 *   - Shading remains presentation work, but obj_animation_cadence_tick commits
 *     animUpdateTimer once in the fixed-step epilogue. A skipped or replayed
 *     presentation therefore cannot change animation speed.
 *   - scene_authoritative_render_tick commits ordinary-scene texture animation
 *     in the draw's viewport/pass order. Original cadence preserves authored
 *     RNG ownership; enhanced cadence uses the presentation stream.
 *
 * DIVERGENCE (intended -- it is the point of the migration):
 * objects that were never drawn now animate and gain a non-NULL curVertData, so
 * they become subject to sphere collision. Their animation cadence is now the
 * same fixed-tick cadence as drawn objects. Measured: this changes no racer
 * trajectory on any gate in the suite.
 *
 * NOT addressed here (recorded, separate concern): sphere collision takes its
 * model from modelInstances[0] (objects.c:6383) while curVertData tracks
 * obj->modelIndex, so the two disagree whenever a non-zero LOD is selected
 * (:5180). That mismatch predates this change and is untouched by it.
 */
void obj_animate_tick(void) {
    s32 i;
    Object *obj;
    ModelInstance *modInst;
    ObjectModel *objModel;

    for (i = gObjectListStart; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (obj == NULL || obj->header == NULL) {
            continue;
        }
        /* render_object_parts (:5227) sends particles to render_particle and only
         * OBJECT_MODEL_TYPE_3D_MODEL objects to render_3d_model. */
        if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
            continue;
        }
        if (obj->header->modelType != OBJECT_MODEL_TYPE_3D_MODEL || obj->modelInstances == NULL) {
            continue;
        }
        modInst = obj->modelInstances[obj->modelIndex];
        if (modInst == NULL || modInst->objModel == NULL) {
            continue;
        }
        objModel = modInst->objModel;

        if (modInst->animUpdateTimer <= 0 && modInst->modelType == MODELTYPE_ANIMATED) {
            obj_animate(obj);
        }
        obj->curVertData = modInst->vertices[modInst->animationTaskNum];
    }
    taj_visual_animation_witness_tick();
}
#endif

/**
 * Renders a 3D object, with support for vehicle part entities as part of the process.
 * Loads materials, and sets environment and/or primitive colours based on the material type.
 * Computes the view matrix for the model, and calls a function to draw meshes.
 * Loops through racers to find vehicle parts, which are wheels and propellers.
 */
void render_3d_model(Object *obj) {
    s32 i;
    s32 intensity;
    s32 opacity;
    s32 vertOffset;
    s32 attachPointCount;
    s32 hasOpacity;
    s32 hasLighting;
    s32 flags;
    s32 meshBatch;
    s32 cicFailed;
    f32 vtxX;
    f32 vtxY;
    f32 vtxZ;
    s8 index;
    s8 isCargo;
    Object *loopObj;
    ModelInstance *modInst;
    Object_Racer *racerObj;
    ObjectModel *objModel;
    Sprite *something;

#ifdef NATIVE_PORT
    modInst = obj->modelInstances[object_render_model_index(obj)];
#else
    modInst = obj->modelInstances[obj->modelIndex];
#endif
    if (modInst != NULL) {
        objModel = modInst->objModel;
#ifdef NATIVE_PORT
        /* obj_animate_tick() owns the authoritative curVertData pointer. A racer
         * may nevertheless draw a different per-viewport LOD, whose vertex counts
         * must match shading, mesh and attach-point work below. Temporarily select
         * that model's buffer and restore the exact authoritative pointer in
         * unset_temp_model_transforms(). This also covers the skydome, the one
         * measured 3D draw object outside gObjPtrList. */
        gObjectSavedCurVertData = obj->curVertData;
        gObjectSavedCurVertFor = obj;
        obj->curVertData = modInst->vertices[modInst->animationTaskNum];
#endif
        hasOpacity = FALSE;
        hasLighting = FALSE;
        intensity = 255;
        if (obj->shading != NULL) {
            intensity = (s32) (obj->shading->unk0 * 255.0f * gCurrentLightIntensity);
            hasOpacity = TRUE;
            hasLighting = TRUE;
        }
        if (obj->behaviorId == BHV_RACER) {
            racerObj = obj->racer;
            object_do_player_tumble(obj);
        } else {
            racerObj = NULL;
        }
        if (modInst->animUpdateTimer <= 0) {
#ifndef NATIVE_PORT
            /* NATIVE_PORT: moved to obj_animate_tick() -- curVertData is read by
             * SIMULATION (sphere collision, Wizpig attach), so the fixed step owns
             * it now. */
            obj->curVertData = modInst->vertices[modInst->animationTaskNum];
            if (modInst->modelType == MODELTYPE_ANIMATED) {
                obj_animate(obj);
            }
#endif
            if (modInst->modelType != MODELTYPE_BASIC && objModel->normals != 0) {
                flags = TRUE;
                if (racerObj != NULL && racerObj->vehicleID < VEHICLE_BOSSES &&
                    racerObj->playerIndex == PLAYER_COMPUTER) {
                    flags = FALSE;
                }
                if (cam_get_viewport_layout() != VIEWPORT_LAYOUT_1_PLAYER) {
                    flags = FALSE;
                }
#ifndef NATIVE_PORT
                obj->curVertData = modInst->vertices[modInst->animationTaskNum];
#endif
                if (obj->behaviorId == BHV_UNK_3F) { // 63 = stopwatchicon, stopwatchhand
                    obj_shade_fancy(objModel, obj, 0, gCurrentLightIntensity);
                } else if (flags) {
                    obj_shade_fancy(objModel, obj, -1, gCurrentLightIntensity);
                } else {
                    obj_shade_fast(objModel, obj, gCurrentLightIntensity);
                }
            }
            // Set the animation ticker for non player racers to 2, making them animate at half the framerate.
#ifndef NATIVE_PORT
            if ((racerObj != NULL) && (racerObj->playerIndex == PLAYER_COMPUTER) &&
                (racerObj->vehicleID < VEHICLE_BOSSES)) {
                modInst->animUpdateTimer = 2;
            } else {
                modInst->animUpdateTimer = 1;
            }
#endif
        }
#ifndef NATIVE_PORT
        obj->curVertData = modInst->vertices[modInst->animationTaskNum];
#endif
#ifndef NATIVE_PORT
        if (obj->behaviorId == BHV_DOOR) {
            obj_door_number(objModel, obj);
        }
#endif
        /* Draw-local animation uses the presentation stream; authored RNG is
         * never consumed by render traversal. */
        if (objModel->texOffsetUpdateRate && objModel->hasAnimatedTexture > 0) {
            obj_tex_animate(obj, objModel->texOffsetUpdateRate);
            modInst->objModel->texOffsetUpdateRate = 0;
        }
#ifdef NATIVE_PORT
        {
            u32 remaster_target =
                objModel->normals != 0
                    ? mdkr_remaster_light_target(obj)
                    : 0;
            gDkrSetRemasterTarget(
                gObjectCurrDisplayList++, remaster_target);
        }
#endif
        mtx_cam_push(&gObjectCurrDisplayList, &gObjectCurrMatrix, &obj->trans, gObjectModelScaleY, 0.0f);
        vertOffset = FALSE;
        if (racerObj != NULL) {
            object_undo_player_tumble(obj);
            if (obj->animationID == 0 || racerObj->vehicleID >= VEHICLE_BOSSES) {
                mtx_head_push(&gObjectCurrDisplayList, &gObjectCurrMatrix, modInst, racerObj->headAngle);
                vertOffset = TRUE;
            } else {
#ifndef NATIVE_PORT
                racerObj->headAngle = 0;
#endif
            }
        }
        opacity = scene_object_render_opacity(obj);
        if (opacity > 255) {
            opacity = 255;
        }
        // If the behavior is a water zipper, then halve it's transparency.
        if (obj->behaviorId == BHV_ZIPPER_WATER) {
            opacity >>= 1;
        }
        if (opacity < 255) {
            hasOpacity = TRUE;
        }
        if (hasLighting) {
            gDPSetEnvColor(gObjectCurrDisplayList++, obj->shading->lightR, obj->shading->lightG, obj->shading->lightB,
                           obj->shading->lightIntensity);
        } else {
            gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
        }
        if (obj->header->directionalPointLighting) {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, obj->shading->shadowR, obj->shading->shadowG,
                            obj->shading->shadowB, opacity);
            directional_lighting_on();
        } else if (hasOpacity) {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, intensity, intensity, intensity, opacity);
        } else {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
        }
        if (opacity < 255) {
            meshBatch = render_mesh(objModel, obj, 0, RENDER_SEMI_TRANSPARENT, vertOffset);
        } else {
            meshBatch = render_mesh(objModel, obj, 0, RENDER_NONE, vertOffset);
        }
        if (obj->header->directionalPointLighting) {
            if (hasOpacity) {
                gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, intensity, intensity, intensity, opacity);
            } else {
                gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
            }
            directional_lighting_off();
        }
        if (obj->attachPoints != NULL) {
            attachPointCount = obj->attachPoints->count;
            if (racerObj != NULL && racerObj->vehicleID == VEHICLE_FLYING_CAR) {
                attachPointCount = 0;
            }
            for (i = 0; i < attachPointCount; i++) {
                loopObj = obj->attachPoints->obj[i];
                if (!(loopObj->trans.flags & OBJ_FLAGS_INVISIBLE)) {
                    index = obj->attachPoints->unk2C[i];
                    if (index >= 0 && index < objModel->numberOfAttachPoints) {
                        something = loopObj->sprites[loopObj->modelIndex];
                        vtxX = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].x;
                        vtxY = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].y;
                        vtxZ = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].z;
                        loopObj->trans.x_position += vtxX;
                        loopObj->trans.y_position += vtxY;
                        loopObj->trans.z_position += vtxZ;
                        if (loopObj->header->modelType == OBJECT_MODEL_TYPE_SPRITE_BILLBOARD) {
                            flags = (RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_Z_UPDATE);
                        } else {
                            flags = (RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_Z_UPDATE | RENDER_ANTI_ALIASING);
                        }
                        if (opacity < 255) {
                            flags |= RENDER_SEMI_TRANSPARENT;
                        }
#ifdef ANTI_TAMPER
                        cicFailed = FALSE;
                        // Anti-Piracy check
                        if (osCicId != CIC_ID) {
                            cicFailed = TRUE;
                        }
                        if (!cicFailed) {
#else
                        if (1) {
#endif
                            // In this instance, cargo refers to the eggs in the fire mountain challenge.
                            isCargo = (loopObj->trans.flags & OBJ_FLAGS_UNK_0080 && attachPointCount == 3);
                            if (racerObj != NULL && racerObj->transparency < 255) {
                                isCargo = FALSE;
                            }
                            if (isCargo) {
                                func_80012C98(&gObjectCurrDisplayList);
                                gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
                                gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, intensity, intensity, intensity,
                                                opacity);
                            }
                            loopObj->properties.common.unk0 =
                                render_sprite_billboard(&gObjectCurrDisplayList, &gObjectCurrMatrix,
                                                        &gObjectCurrVertexList, loopObj, something, flags);
                            if (isCargo) {
                                gSPSelectMatrixDKR(gObjectCurrDisplayList++, G_MTX_DKR_INDEX_0);
                                func_80012CE8(&gObjectCurrDisplayList);
                            }
                        }
                        loopObj->trans.x_position -= vtxX;
                        loopObj->trans.y_position -= vtxY;
                        loopObj->trans.z_position -= vtxZ;
                    }
                }
            }
        }
        // This section draws the egg sprite being held by a racer.
        if (racerObj != NULL) {
            loopObj = racerObj->held_obj;
            if (loopObj != NULL) {
                index = obj->header->unk58;
                if (index >= 0 && index < objModel->numberOfAttachPoints) {
                    flags = (RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_Z_UPDATE);
                    something = loopObj->sprites[loopObj->modelIndex];
#ifndef NATIVE_PORT
                    /* NATIVE_PORT: the convergence lerp moved to
                     * racer_held_object_lerp(), called once per tick from
                     * update_player_racer. It ran once per DRAW here and was never
                     * restored, so the settle rate followed the viewport count and
                     * the frame rate, and a culled carrier froze its egg. */
                    vtxX = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].x;
                    vtxY = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].y;
                    vtxZ = obj->curVertData[DKR_PTR(s16, objModel->attachPoints)[index]].z;
                    loopObj->trans.x_position += (vtxX - loopObj->trans.x_position) * 0.25;
                    loopObj->trans.y_position += (vtxY - loopObj->trans.y_position) * 0.25;
                    loopObj->trans.z_position += (vtxZ - loopObj->trans.z_position) * 0.25;
#endif
                    if (loopObj->header->modelType == OBJECT_MODEL_TYPE_SPRITE_BILLBOARD) {
                        render_sprite_billboard(&gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList,
                                                loopObj, something, flags);
                    }
                }
            }
        }
        if (meshBatch != -1) {
            if (obj->header->directionalPointLighting) {
                gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, obj->shading->shadowR, obj->shading->shadowG,
                                obj->shading->shadowB, opacity);
                directional_lighting_on();
            }
            render_mesh(objModel, obj, meshBatch, RENDER_SEMI_TRANSPARENT, vertOffset);
            if (obj->header->directionalPointLighting) {
                directional_lighting_off();
            }
        }
        if (hasOpacity || obj->header->directionalPointLighting) {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
        }
        if (hasLighting) {
            gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
        }
        mtx_pop(&gObjectCurrDisplayList);
#ifdef NATIVE_PORT
        /* Never let a tagged character leak into the following object. */
        gDkrSetRemasterTarget(gObjectCurrDisplayList++, 0);
#endif
    }
}

void func_80012C30(void) {
    D_8011ADA4 = 0;
}

void func_80012C3C(Gfx **dList) {
    s32 i;
    for (i = 0; i < D_8011ADA4; i++) {
        gSPDisplayList((*dList)++, D_8011AD78[i]);
    }
}

void func_80012C98(Gfx **dList) {
    if (D_8011ADA4 < 9) {
        gSPNoOp((*dList)++); // Placeholder instruction?
        D_8011AD78[D_8011ADA4] = *dList;
    }
}

void func_80012CE8(Gfx **dList) {
    if (D_8011ADA4 < 9) {
        gSPEndDisplayList((*dList)++);
        gSPBranchList(D_8011AD78[D_8011ADA4] - 1, *dList);
        D_8011ADA4++;
    }
}

/**
 * Update the object stack trace, set the draw pointers, then begin rendering the object.
 * Official Name: objPrintObject
 */
void render_object(Gfx **dList, Mtx **mtx, Vertex **verts, Object *obj) {
    f32 scale;
#ifdef NATIVE_PORT
    if (taj_visual_suppress_donor_draw(obj)) {
        return;
    }
    if (taj_visual_select_sign_object(obj) &&
        taj_visual_select_sign_player(obj) < 0) {
        return;
    }
#endif
    if (obj->trans.flags & (OBJ_FLAGS_INVISIBLE | OBJ_FLAGS_SHADOW_ONLY)) {
        return;
    }
    update_object_stack_trace(OBJECT_DRAW, obj->objectID);
    gObjectCurrDisplayList = *dList;
    gObjectCurrMatrix = *mtx;
    gObjectCurrVertexList = *verts;
    scale = obj->trans.scale;
    render_object_parts(obj);
    obj->trans.scale = scale;
    *dList = gObjectCurrDisplayList;
    *mtx = gObjectCurrMatrix;
    *verts = gObjectCurrVertexList;
    update_object_stack_trace(OBJECT_DRAW, OBJECT_CLEAR);
}

/**
 * Official Name: objDoPlayerTumble
 */
void object_do_player_tumble(Object *this) {
    UNUSED s32 unused1;
    Object_Racer *sp_20;
    f32 tmp_f2;
    f32 offsetY;
    f32 tmp_f0;
    f32 temp;

    if (this->behaviorId == BHV_RACER) {

        sp_20 = this->racer;
        this->trans.rotation.y_rotation += sp_20->y_rotation_offset;
        this->trans.rotation.x_rotation += sp_20->x_rotation_offset;
        this->trans.rotation.z_rotation += sp_20->z_rotation_offset;
        offsetY = 0.0f;
        if (sp_20->vehicleIDPrev < VEHICLE_BOSSES) {

            offsetY = coss_f(sp_20->z_rotation_offset);
            tmp_f2 = offsetY;
            tmp_f0 = coss_f(sp_20->x_rotation_offset - sp_20->unk166) * tmp_f2;

            tmp_f0 = (tmp_f0 < 0.0f) ? 0.0f : tmp_f0 * tmp_f0;

            temp = (1.0f - tmp_f0) * 24.0f + sp_20->unkD0;
            if (0) {} // Fakematch
            offsetY = temp;
        }
#ifdef NATIVE_PORT
        /* Render purity: (y + offset) - offset is not bit-exact in floating
         * point — the historical add/subtract restore left racers 1 ULP off
         * on rendered ticks (caught by check_render_purity's skip-render
         * A/B: one object, one ULP, tick 3013). Save the exact pre-tumble
         * bits and restore them in undo. The single static mirrors
         * gObjectOffsetY's own single-global contract; the do/undo pairs are
         * verified non-nested.
         *
         * The pairing guard mirrors the carBob pair's. It is inert on every path
         * measured today -- the do/undo guards are identical -- but this bracket
         * is the wider of the two:
         * object_do_player_tumble/object_undo_player_tumble span obj_tex_animate
         * and mtx_cam_push in render_3d_model (:5002/:5097), the boost draw
         * (:5628/:5630), and in particles.c the entire per-racer emitter loop
         * (:855/:985). A nested pair over a DIFFERENT object would otherwise
         * write that object's saved y onto this one. */
        gObjectSavedTumbleY = this->trans.y_position;
        gObjectSavedTumbleFor = this;
#endif
        this->trans.y_position = this->trans.y_position + offsetY;
        gObjectOffsetY = offsetY;
    }
}

/**
 * Official Name: objUndoPlayerTumble
 */
void object_undo_player_tumble(Object *obj) {
    if (obj->behaviorId == BHV_RACER) {
        Object_Racer *racer = obj->racer;
        obj->trans.rotation.y_rotation -= racer->y_rotation_offset;
        obj->trans.rotation.x_rotation -= racer->x_rotation_offset;
        obj->trans.rotation.z_rotation -= racer->z_rotation_offset;
#ifdef NATIVE_PORT
        /* Exact bits only for the object that saved them; otherwise the
         * original arithmetic subtract, which is the untouched ROM path. */
        if (gObjectSavedTumbleFor == obj) {
            obj->trans.y_position = gObjectSavedTumbleY;
            gObjectSavedTumbleFor = NULL;
        } else {
            obj->trans.y_position -= gObjectOffsetY;
        }
#else
        obj->trans.y_position -= gObjectOffsetY;
#endif
    }
}

#ifdef NATIVE_PORT
static s32 object_render_model_index(const Object *obj) {
    if (gObjectRenderModelFor == obj) {
        return gObjectRenderModelIndex;
    }
    return obj->modelIndex;
}

/* Pure racer LOD selection. The caller supplies the viewport's private
 * distance and owns whether the result is committed to simulation (tick) or
 * retained as a draw-local override (render). */
static s32 racer_model_index_for_view(Object *obj, Object_Racer *racer,
                                      f32 distance, f32 *scaleMultiplier) {
    s32 assetIndex;
    s32 firstModel;
    s32 lastModel;
    s32 modelIndex;
    s32 scaledDistance;
    u8 *thresholds;

    *scaleMultiplier = 1.0f;
    if (racer->playerIndex != PLAYER_COMPUTER && racer->raceFinished) {
        modelIndex = 0;
    } else if (obj->behaviorId == BHV_TIMETRIAL_GHOST) {
        modelIndex = 1;
    } else {
        assetIndex = racer->vehicleID;
        if (assetIndex >= NUMBER_OF_PLAYER_VEHICLES) {
            assetIndex = 0;
        }
        thresholds = (u8 *)get_misc_asset(assetIndex + VEHICLE_BOSSES);
        thresholds += cam_get_viewport_layout() * 10;
        if (get_current_viewport() != racer->playerIndex) {
            thresholds += 5;
        }
        scaledDistance = (s32)distance >> 3;
        if (distance < 0.0f) {
            distance = 0.0f;
        } else if (distance > 3500.0f) {
            distance = 3500.0f;
        }
        *scaleMultiplier = (distance / 2700.0f) + 1.0f;
        scaledDistance *=
            ((f32 *)get_misc_asset(ASSET_MISC_4))[racer->characterId];
        if (scaledDistance < -50) {
            modelIndex = 5;
        } else {
            scaledDistance >>= 1;
            if (scaledDistance < 0) {
                scaledDistance = 0;
            }
            if (scaledDistance < thresholds[0]) {
                modelIndex = 0;
            } else if (scaledDistance < thresholds[1]) {
                modelIndex = 1;
            } else if (scaledDistance < thresholds[2]) {
                modelIndex = 2;
            } else if (scaledDistance < thresholds[3]) {
                modelIndex = 3;
            } else if (scaledDistance < thresholds[4]) {
                modelIndex = 4;
            } else {
                modelIndex = 5;
            }
        }
    }

    firstModel = 0;
    while (firstModel < obj->header->numberOfModelIds &&
           obj->modelInstances[firstModel] == NULL) {
        firstModel++;
    }
    lastModel = obj->header->numberOfModelIds - 1;
    while (lastModel >= 0 && obj->modelInstances[lastModel] == NULL) {
        lastModel--;
    }
    if (firstModel > lastModel) {
        return obj->modelIndex;
    }
    if (modelIndex < firstModel) {
        modelIndex = firstModel;
    }
    if (modelIndex > lastModel) {
        modelIndex = lastModel;
    }
    return modelIndex;
}

/* Advance the authored brake/headlight state once per fixed tick. Shading is
 * deliberately sampled here rather than by a draw, so skipped or additional
 * presentations cannot change the light phase. */
static void racer_light_tick(Object *obj, u8 *lightFlags) {
    if (obj->shading != NULL && obj->shading->unk0 < 0.6f) {
        *lightFlags |= RACER_LIGHT_NIGHT;
    } else {
        *lightFlags &= ~RACER_LIGHT_NIGHT;
    }
    if ((*lightFlags & RACER_LIGHT_TIMER) != 0) {
        if (*lightFlags & RACER_LIGHT_BRAKE) {
            *lightFlags =
                (*lightFlags & ~RACER_LIGHT_UNK10) | RACER_LIGHT_UNK20;
        } else if (*lightFlags & RACER_LIGHT_NIGHT) {
            *lightFlags =
                (*lightFlags & ~RACER_LIGHT_UNK20) | RACER_LIGHT_UNK10;
        }
    }
}

/* Pure texture-frame lookup for render_mesh. The shared model's
 * TriangleBatchInfo is never modified by presentation. */
static s32 racer_light_tex_offset(u8 lightFlags) {
    s32 offset = lightFlags & RACER_LIGHT_TIMER;

    if (offset != 0) {
        offset--;
        if (lightFlags & RACER_LIGHT_BRAKE) {
            offset += 1;
        } else if (lightFlags & RACER_LIGHT_NIGHT) {
            offset += 3;
        } else if (lightFlags & RACER_LIGHT_UNK20) {
            offset += 1;
        } else {
            offset += 3;
        }
    }
    return offset * 4;
}

/** Commit the final viewport's racer LOD and light phase once per fixed tick. */
void obj_lod_tick(void) {
    extern LevelModel *gCurrentLevelModel;
    s32 savedCamera;
    s32 i;
    f32 unusedScale;
    Object *obj;
    Object_Racer *racer;
    u8 lightFlags;

    if (gCurrentLevelModel == NULL) {
        return;
    }
    savedCamera = get_current_viewport();
    scene_build_last_viewport_basis();
    for (i = gObjectListStart; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (obj == NULL || (obj->trans.flags & OBJ_FLAGS_PARTICLE) ||
            obj->header == NULL || obj->header->behaviorId != BHV_RACER ||
            obj->racer == NULL || obj->modelInstances == NULL) {
            continue;
        }
        racer = obj->racer;
        obj->modelIndex = racer_model_index_for_view(
            obj, racer, obj->distanceToCamera, &unusedScale);
        lightFlags = racer->lightFlags;
        racer_light_tick(obj, &lightFlags);
        racer->lightFlags = lightFlags;
    }
    set_active_camera(savedCamera);
}
#endif

void set_temp_model_transforms(Object *obj) {
#ifdef NATIVE_PORT
    extern f32 gSceneDrawDistance; /* tracks.c: this viewport's private sort key */
    extern s32 gSceneDrawDistanceValid;
#endif
    s32 batchNum;
    ObjectModel *objModel;
    s32 var_v1;
    ModelInstance **modInstList;
    ModelInstance *modInst;
    u8 *bossAsset;
    f32 var_f0;
    u8 *var_a1;
    f32 ret2;
    UNUSED s32 pad;
    Object_Racer *objRacer;
    f32 ret1;
    s32 firstNonEmptyModelIndex;
    s32 modelIndex;
    s32 numberOfModels;
#ifdef NATIVE_PORT
    f32 lodScale;
#endif

    ret1 = 1.0f;
    ret2 = 1.0f;
    if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
        if (obj->header->behaviorId == BHV_RACER) {
            objRacer = obj->racer;
#ifndef NATIVE_PORT
            /* NATIVE_PORT: moved to obj_visibility_tick(). "This racer was drawn"
             * was a simulation input: unk201 gates the AI's steering and its RNG
             * (racer.c:9088 -> racer_AI_pathing_inputs, racer.c:382
             * roll_percent_chance -> rand_range) and particle emission
             * (racer.c:2247/3452). */
            objRacer->unk201 = 30;
#endif
            if (objRacer->unk206 > 0) {
                ret2 = 1.0f - (objRacer->unk206 * 0.05f);
                if (ret2 < 0.2f) {
                    ret2 = 0.2f;
                }
            }
#ifdef NATIVE_PORT
            var_f0 = gSceneDrawDistanceValid ? gSceneDrawDistance
                                             : obj->distanceToCamera;
            modelIndex = racer_model_index_for_view(
                obj, objRacer, var_f0, &lodScale);
            obj->trans.scale *= lodScale;
            gObjectRenderModelFor = obj;
            gObjectRenderModelIndex = modelIndex;
            gObjectRenderRacerTexOffset =
                racer_light_tex_offset(objRacer->lightFlags);
            modInst = obj->modelInstances[modelIndex];
            objModel = modInst->objModel;
#else
            if (objRacer->playerIndex != PLAYER_COMPUTER && objRacer->raceFinished) {
                modelIndex = 0;
                batchNum = 0;
            } else {
                if (obj->behaviorId == BHV_TIMETRIAL_GHOST) { // Ghost Object?
                    modelIndex = 1;
                    batchNum = 0;
                } else {
                    // Loads vehicles between VEHICLE_TRICKY and VEHICLE_SMOKEY. So all boss vehicles except wizpig.
                    batchNum = objRacer->vehicleID;
                    if (objRacer->vehicleID >= NUMBER_OF_PLAYER_VEHICLES) {
                        batchNum = 0;
                    }
                    bossAsset = (u8 *) get_misc_asset(batchNum + VEHICLE_BOSSES); // 40 bytes of data u8[8][5]?
                    batchNum = 0;
                    bossAsset += cam_get_viewport_layout() * 10;
                    if (get_current_viewport() != objRacer->playerIndex) {
                        bossAsset += 5;
                    }
                    var_f0 = obj->distanceToCamera;
                    var_v1 = (s32) var_f0 >> 3;
                    if (obj->distanceToCamera < 0.0f) {
                        var_f0 = 0.0f;
                    } else if (var_f0 > 3500.0f) {
                        var_f0 = 3500.0f;
                    }
                    var_f0 /= 2700.0f;
                    var_f0 += 1.0f;
                    obj->trans.scale *= var_f0;
                    var_v1 *= ((f32 *) get_misc_asset(ASSET_MISC_4))[objRacer->characterId];
                    // ASSET_MISC_4 is just 10 floats of 1.0f. One for each playable character.
                    if (var_v1 < -50) {
                        modelIndex = 5;
                    } else {
                        var_v1 >>= 1;
                        if (var_v1 < 0) {
                            var_v1 = 0;
                        }

                        if (var_v1 < bossAsset[0]) {
                            modelIndex = 0;
                        } else if (var_v1 < bossAsset[1]) {
                            modelIndex = 1;
                        } else if (var_v1 < bossAsset[2]) {
                            modelIndex = 2;
                        } else if (var_v1 < bossAsset[3]) {
                            modelIndex = 3;
                        } else if (var_v1 < bossAsset[4]) {
                            modelIndex = 4;
                        } else {
                            modelIndex = 5;
                        }
                    }
                }
            }

            firstNonEmptyModelIndex = 0;
            modInstList = &obj->modelInstances[firstNonEmptyModelIndex];

            while (*modInstList == NULL) {
                firstNonEmptyModelIndex++;
                modInstList++;
            }

            numberOfModels = obj->header->numberOfModelIds - 1;
            modInstList = &obj->modelInstances[numberOfModels];

            while (*modInstList == NULL) {
                numberOfModels--;
                modInstList--;
            }

            if (modelIndex < firstNonEmptyModelIndex) {
                modelIndex = firstNonEmptyModelIndex;
            }
            if (numberOfModels < modelIndex) {
                modelIndex = numberOfModels;
            }
            obj->modelIndex = modelIndex;
            if ((obj->shading != NULL) && (obj->shading->unk0 < 0.6f)) {
                objRacer->lightFlags |= RACER_LIGHT_NIGHT;
            } else {
                objRacer->lightFlags &= ~RACER_LIGHT_NIGHT;
            }
            modelIndex = objRacer->lightFlags & RACER_LIGHT_TIMER;
            modInst = obj->modelInstances[obj->modelIndex];
            objModel = modInst->objModel;
            if (modelIndex != 0) {
                modelIndex--;
                if (objRacer->lightFlags & RACER_LIGHT_BRAKE) {
                    modelIndex += 1;
                    objRacer->lightFlags = (objRacer->lightFlags & ~RACER_LIGHT_UNK10) | RACER_LIGHT_UNK20;
                } else if (objRacer->lightFlags & RACER_LIGHT_NIGHT) {
                    modelIndex += 3;
                    objRacer->lightFlags = (objRacer->lightFlags & ~RACER_LIGHT_UNK20) | RACER_LIGHT_UNK10;
                } else if (objRacer->lightFlags & RACER_LIGHT_UNK20) {
                    modelIndex += 1;
                } else {
                    modelIndex += 3;
                }
            }
            modelIndex *= 4;
            for (batchNum = 0; batchNum < objModel->numberOfBatches; batchNum++) {
                if ((DKR_PTR(TriangleBatchInfo, objModel->batches)[batchNum].flags & 0x810000) == RENDER_TEX_ANIM) {
                    DKR_PTR(TriangleBatchInfo, objModel->batches)[batchNum].texOffset = modelIndex;
                }
            }
#endif
#ifdef NATIVE_PORT
            /* Render purity: same 1-ULP add/subtract hazard as the tumble
             * pair (a += then -= round-trip is not bit-exact). Save the
             * exact bits; unset restores them. Caught by
             * check_render_purity's skip-render A/B. */
            gObjectSavedBobX = obj->trans.x_position;
            gObjectSavedBobY = obj->trans.y_position;
            gObjectSavedBobZ = obj->trans.z_position;
            gObjectSavedBobFor = obj;
#endif
            obj->trans.x_position += objRacer->carBobX;
            obj->trans.y_position += objRacer->carBobY;
            obj->trans.z_position += objRacer->carBobZ;
            ret1 = objRacer->stretch_height;
        } else if (obj->behaviorId == BHV_FROG) {
            ret1 = obj->frog->scaleY;
        }
    }
    gObjectModelScaleY = ret1;
    gCurrentLightIntensity = ret2;
}

/**
 * Determine which model type the object is using, then call the related function to render it.
 * Beforehand, call a function to apply a temporary transformation, mostly for racers.
 * Afterwards, undo that.
 */
void render_object_parts(Object *obj) {
    set_temp_model_transforms(obj);
    if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
        render_particle((Particle *) obj, &gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList,
                        PARTICLE_UNK_FLAG_8000);
    } else {
        if (obj->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
            render_3d_model(obj);
        } else if (obj->header->modelType == OBJECT_MODEL_TYPE_SPRITE_BILLBOARD) {
            render_3d_billboard(obj);
        } else if (obj->header->modelType == OBJECT_MODEL_TYPE_MISC) {
            render_3d_misc(obj);
        }
    }
    unset_temp_model_transforms(obj);
}

/**
 * After rendering, sets the object position back to normal.
 */
void unset_temp_model_transforms(Object *obj) {
#ifdef NATIVE_PORT
    if (gObjectSavedCurVertFor == obj) {
        obj->curVertData = gObjectSavedCurVertData;
        gObjectSavedCurVertFor = NULL;
    }
    if (gObjectRenderModelFor == obj) {
        gObjectRenderModelFor = NULL;
    }
#endif
    if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && obj->header->behaviorId == BHV_RACER) {
#ifdef NATIVE_PORT
        if (gObjectSavedBobFor == obj) {
            obj->trans.x_position = gObjectSavedBobX;
            obj->trans.y_position = gObjectSavedBobY;
            obj->trans.z_position = gObjectSavedBobZ;
            gObjectSavedBobFor = NULL;
        } else {
            obj->trans.x_position -= obj->racer->carBobX;
            obj->trans.y_position -= obj->racer->carBobY;
            obj->trans.z_position -= obj->racer->carBobZ;
        }
#else
        obj->trans.x_position -= obj->racer->carBobX;
        obj->trans.y_position -= obj->racer->carBobY;
        obj->trans.z_position -= obj->racer->carBobZ;
#endif
    }
}

// Renders the boost graphics.
void func_800135B8(Object *boostObj) {
    Vertex *vtx;
    Triangle *tri;
    ObjectTransform_800135B8 objTransform;
    Object_Boost_Inner *boostData;
    Object_Boost *boost;
    Object_Boost *asset;
    s32 hasTexture;
    s32 racerIndex;

    racerIndex = (boostObj->properties.boost.indexes >> 28) & 0xF;
    boost = boostObj->boost;
    switch (D_8011B048[racerIndex]) {
        case VEHICLE_CAR:
            boostData = &boost->carBoostData;
            break;
        case VEHICLE_HOVERCRAFT:
            boostData = &boost->hovercraftBoostData;
            break;
        default:
            boostData = &boost->flyingBoostData;
            break;
    }
    asset = GET_BOOST_TABLE();
    asset = &asset[D_8011B058[racerIndex]];
    object_do_player_tumble(boostObj->properties.boost.obj);
    mtx_cam_push(&gObjectCurrDisplayList, &gObjectCurrMatrix, &boostObj->properties.boost.obj->trans, 1.0f, 0.0f);
    object_undo_player_tumble(boostObj->properties.boost.obj);
    objTransform.trans.x_position = boostData->position.x;
    objTransform.trans.y_position = boostData->position.y;
    objTransform.trans.z_position = boostData->position.z;
    objTransform.trans.scale = boostData->unkC + (boostData->unk10 * coss_f(boost->unk72 << 12));
    if (boost->unk70 < 2) {
        objTransform.trans.scale *= boost->unk74;
    }
    if (D_8011B058[racerIndex] != 0) {
        objTransform.trans.scale *= 1.15f;
    }
    objTransform.trans.rotation.z_rotation = 0;
    objTransform.trans.rotation.x_rotation = 0;
    objTransform.trans.rotation.y_rotation = 0;
    objTransform.unk18 = 0;
    gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
    gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
#ifdef NATIVE_PORT
    render_sprite_billboard_transform(
        &gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList,
        &objTransform.trans, objTransform.unk18, asset->sprite,
        RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_Z_UPDATE);
#else
    render_sprite_billboard(
        &gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList,
        (Object *) &objTransform, asset->sprite,
        RENDER_Z_COMPARE | RENDER_FOG_ACTIVE | RENDER_Z_UPDATE);
#endif
    if (boost->unk70 == 2) {
        material_set(&gObjectCurrDisplayList, asset->tex,
                     (RENDER_Z_COMPARE | RENDER_SEMI_TRANSPARENT | RENDER_FOG_ACTIVE), 0);
        if (asset->tex != NULL) {
            hasTexture = TRUE;
        } else {
            hasTexture = FALSE;
        }

        vtx = &gBoostVerts[gBoostVertFlip][(boostObj->properties.boost.indexes >> 14) & 0x3FFF];
        tri = &gBoostTris[gBoostVertFlip][boostObj->properties.boost.indexes & 0x3FFF];
        gSPVertexDKR(gObjectCurrDisplayList++, OS_K0_TO_PHYSICAL(vtx), BOOST_VERT_COUNT, 0);
        gSPPolygon(gObjectCurrDisplayList++, OS_K0_TO_PHYSICAL(tri), BOOST_TRI_COUNT, hasTexture);
    }
    mtx_pop(&gObjectCurrDisplayList);
}

/**
 * Render the bubble trap weapon.
 */
#ifdef NATIVE_PORT
void render_bubble_trap_transform(const ObjectTransform *trans, Sprite *gfxData,
                                  ObjectTransformExt *obj, s32 flags) {
#else
void render_bubble_trap(ObjectTransform *trans, Sprite *gfxData, Object *obj, s32 flags) {
#endif
    f32 x;
    f32 y;
    f32 z;
    Camera *cameraSegment;
    f32 dist;

    vec3f_rotate(&trans->rotation, &obj->trans.position);
    obj->trans.x_position += trans->x_position;
    obj->trans.y_position += trans->y_position;
    obj->trans.z_position += trans->z_position;
    cameraSegment = cam_get_active_camera();
    x = cameraSegment->trans.x_position - obj->trans.x_position;
    y = cameraSegment->trans.y_position - obj->trans.y_position;
    z = cameraSegment->trans.z_position - obj->trans.z_position;
    dist = sqrtf((x * x) + (y * y) + (z * z));
    if (dist > 0.0) {
#ifdef NATIVE_PORT
        dist = obj->unk1A / dist;
#else
        dist = obj->numActiveEmitters / dist;
#endif
        x *= dist;
        y *= dist;
        z *= dist;
    }
    obj->trans.x_position += x;
    obj->trans.y_position += y;
    obj->trans.z_position += z;
#ifdef NATIVE_PORT
    render_sprite_billboard_transform(
        &gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList,
        &obj->trans, obj->animFrame, gfxData, flags);
#else
    render_sprite_billboard(&gObjectCurrDisplayList, &gObjectCurrMatrix, &gObjectCurrVertexList, obj, gfxData, flags);
#endif
}

/**
 * Get the racer object data, and fetch set visual shield properties based on that racer.
 * Afterwards, render the graphics with opacity scaling with the fadetimer.
 */
void render_racer_shield(Gfx **dList, Mtx **mtx, Vertex **vtxList, Object *obj) {
    Object *effectAnchor = obj;
    Object_Racer *racer;
    ModelInstance *modInst;
    ObjectModel *mdl;
    RacerShieldGfx *shield;
    s32 shieldType;
    s32 vehicleID;
    s32 racerIndex;
    f32 scale;
    f32 shear;

    racer = obj->racer;
    if (racer->shieldTimer > 0 && gShieldEffectObject != NULL) {
#ifdef NATIVE_PORT
        effectAnchor = taj_visual_effect_anchor(obj);
#endif
        gObjectCurrDisplayList = *dList;
        gObjectCurrMatrix = *mtx;
        gObjectCurrVertexList = *vtxList;
        racerIndex = racer->racerIndex;
        if (racerIndex > NUMBER_OF_CHARACTERS) {
            racerIndex = 0;
        }
        vehicleID = racer->vehicleID;
        if (vehicleID >= NUMBER_OF_PLAYER_VEHICLES) {
            vehicleID = VEHICLE_CAR;
        }
        shield = (RacerShieldGfx *) get_misc_asset(ASSET_MISC_SHIELD_DATA);
        vehicleID = (vehicleID * 10) + racerIndex;
        shield = shield + vehicleID;
        gShieldEffectObject->trans.x_position = shield->x_position;
        gShieldEffectObject->trans.y_position = shield->y_position;
        gShieldEffectObject->trans.z_position = shield->z_position;
        gShieldEffectObject->trans.y_position += shield->y_offset * sins_f(gShieldSineTime[racerIndex] * 0x200);
        shear = (coss_f(gShieldSineTime[racerIndex] * 0x400) * 0.05f) + 0.95f;
        gShieldEffectObject->trans.scale = shield->scale * shear;
        shear = shear * shield->turnSpeed;
        gShieldEffectObject->trans.rotation.y_rotation = gShieldSineTime[racerIndex] * 0x800;
        gShieldEffectObject->trans.rotation.x_rotation = 0x800;
        gShieldEffectObject->trans.rotation.z_rotation = 0;
        shieldType = racer->shieldType;
        if (shieldType != SHIELD_NONE) {
            shieldType--;
        }
        if (shieldType > SHIELD_LEVEL3 - 1) {
            shieldType = SHIELD_LEVEL3 - 1;
        }
        scale = ((f32) shieldType * 0.1) + 1.0f;
        gShieldEffectObject->trans.scale *= scale;
        shear *= scale;
        modInst = gShieldEffectObject->modelInstances[shieldType];
        mdl = modInst->objModel;
        gShieldEffectObject->curVertData = modInst->vertices[modInst->animationTaskNum];
        gDPSetEnvColor(gObjectCurrDisplayList++, 255, 255, 255, 0);
        if (racer->shieldTimer < 64) {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, racer->shieldTimer * 4);
        } else {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
        }
        mtx_shear_push(&gObjectCurrDisplayList, &gObjectCurrMatrix,
                       gShieldEffectObject, effectAnchor, shear);
        render_mesh(mdl, gShieldEffectObject, 0, RENDER_SEMI_TRANSPARENT, 0);
        gSPSelectMatrixDKR(gObjectCurrDisplayList++, G_MTX_DKR_INDEX_0);
        if (racer->shieldTimer < 64) {
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
        }
        *dList = gObjectCurrDisplayList;
        *mtx = gObjectCurrMatrix;
        *vtxList = gObjectCurrVertexList;
    }
}

/**
 * Get the racer object data, and fetch set visual magnet properties based on that racer.
 * Afterwards, render the graphics with opacity set by the properties.
 */
void render_racer_magnet(Gfx **dList, Mtx **mtx, Vertex **vtxList, Object *obj) {
    Object *effectAnchor = obj;
    Object_Racer *racer;
    ModelInstance *modInst;
    ObjectModel *mdl;
    f32 *magnet;
    s32 vehicleID;
    s32 racerIndex;
    s32 opacity;
    f32 shear;
    UNUSED s32 pad;

    racer = obj->racer;
    racerIndex = racer->racerIndex;
    if (gRacerFXData[racerIndex].unk3 != 0) {
        if (gMagnetEffectObject != NULL) {
#ifdef NATIVE_PORT
            effectAnchor = taj_visual_effect_anchor(obj);
#endif
            gObjectCurrDisplayList = *dList;
            gObjectCurrMatrix = *mtx;
            gObjectCurrVertexList = *vtxList;
            magnet = (f32 *) get_misc_asset(ASSET_MISC_MAGNET_DATA);
            vehicleID = racer->vehicleID;
            if (vehicleID < VEHICLE_CAR || vehicleID >= NUMBER_OF_PLAYER_VEHICLES) {
                vehicleID = VEHICLE_CAR;
            }
            magnet = &magnet[vehicleID * 5];
            racerIndex = racer->racerIndex;
            if (racerIndex > NUMBER_OF_CHARACTERS) {
                racerIndex = 0;
            }
            gMagnetEffectObject->trans.x_position = magnet[0];
            gMagnetEffectObject->trans.y_position = magnet[1];
            gMagnetEffectObject->trans.z_position = magnet[2];
            magnet += 3;
            shear = (coss_f((gRacerFXData[racerIndex].unk1 * 0x400)) * 0.02f) + 0.98f;
            gMagnetEffectObject->trans.scale = magnet[0] * shear;
            magnet += 1;
            shear = magnet[0] * shear;
            gMagnetEffectObject->trans.rotation.y_rotation = gRacerFXData[racerIndex].unk2 * 0x1000;
            gMagnetEffectObject->trans.rotation.x_rotation = 0;
            gMagnetEffectObject->trans.rotation.z_rotation = 0;
            modInst = gMagnetEffectObject->modelInstances[0];
            mdl = modInst->objModel;
            gMagnetEffectObject->curVertData = modInst->vertices[modInst->animationTaskNum];
            opacity = ((gRacerFXData[racerIndex].unk1 * 8) & 0x7F) + 0x80;
            gfx_init_basic_xlu(&gObjectCurrDisplayList, DRAW_BASIC_2CYCLE, COLOUR_RGBA32(255, 255, 255, opacity),
                               gMagnetColours[racer->magnetModelID]);
            mtx_shear_push(&gObjectCurrDisplayList, &gObjectCurrMatrix,
                           gMagnetEffectObject, effectAnchor, shear);
            gObjectTexAnim = TRUE;
            render_mesh(mdl, gMagnetEffectObject, 0, RENDER_SEMI_TRANSPARENT, 0);
            gObjectTexAnim = FALSE;
            gSPSelectMatrixDKR(gObjectCurrDisplayList++, G_MTX_DKR_INDEX_0);
            gDPSetPrimColor(gObjectCurrDisplayList++, 0, 0, 255, 255, 255, 255);
            rendermode_reset(&gObjectCurrDisplayList);
            *dList = gObjectCurrDisplayList;
            *mtx = gObjectCurrMatrix;
            *vtxList = gObjectCurrVertexList;
        }
    }
}

void func_80014090(Object *obj, s32 arg1) {
    ObjectHeader *objHeader;
    s16 width;
    s16 height;
    s32 i;
    s32 j;
    s32 k;
    s32 end;
    s16 objHeader72;
    s16 objHeader73;
    ObjectModel *objMdl;
    ModelInstance *modInst;
    TextureInfo *texInfo;
    Triangle *tri;
    s16 temp;
    s16 temp2;
    s16 newU1;
    s16 newU2;
    s16 newV1;
    s16 newV2;

    objHeader = obj->header;
    objHeader73 = objHeader->unk73;
    objHeader72 = objHeader->unk72;
    temp = (s16) (objHeader->unk74 * arg1);
    temp2 = (s16) (objHeader->unk75 * arg1);
    if ((objHeader73 == 0xFF) || (objHeader73 < objHeader->numberOfModelIds)) {
        if (objHeader73 == 0xFF) {
            end = objHeader->numberOfModelIds;
            objHeader73 = 0;
        } else {
            end = objHeader73 + 1;
        }
        for (i = objHeader73; i < end; i++) {
            modInst = obj->modelInstances[i];
            objMdl = modInst->objModel;
            if (objHeader72 < objMdl->numberOfTextures) {
                width = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, objMdl->textures)[objHeader72].texture)->width << 5;
                height = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, objMdl->textures)[objHeader72].texture)->height << 5;
                for (j = 0; j < objMdl->numberOfBatches; j++) {
                    if (objHeader72 == DKR_PTR(TriangleBatchInfo, objMdl->batches)[j].textureIndex) {
                        for (k = DKR_PTR(TriangleBatchInfo, objMdl->batches)[j].facesOffset; k < DKR_PTR(TriangleBatchInfo, objMdl->batches)[j + 1].facesOffset; k++) {
                            tri = &DKR_PTR(Triangle, objMdl->triangles)[k];
                            newU1 = (tri->uv1.u - tri->uv0.u);
                            newV1 = (tri->uv1.v - tri->uv0.v);
                            newU2 = (tri->uv2.u - tri->uv0.u);
                            newV2 = (tri->uv2.v - tri->uv0.v);
                            // s16 casts required
                            tri->uv0.u = (tri->uv0.u + temp) & (s16) (width - 1);
                            tri->uv0.v = (tri->uv0.v + temp2) & (s16) (height - 1);
                            tri->uv1.u = tri->uv0.u + newU1;
                            tri->uv1.v = tri->uv0.v + newV1;
                            tri->uv2.u = tri->uv0.u + newU2;
                            tri->uv2.v = tri->uv0.v + newV2;
                        }
                    }
                }
            }
        }
    }
}

/**
 * Loop through every object.
 * Check which ones have 3D models and count down the update timer.
 * The object will update its animation at 0.
 */
void obj_tick_anims(void) {
    s32 i = gObjectListStart;
    s32 j;
    Object *currObj;
    ModelInstance *modInst;

    for (; i < gObjectCount; i++) {
        currObj = gObjPtrList[i];
        if (!(currObj->trans.flags & OBJ_FLAGS_PARTICLE) && currObj->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL) {
            for (j = 0; j < currObj->header->numberOfModelIds; j++) {
                modInst = currObj->modelInstances[j];
                if (modInst != NULL && modInst->animUpdateTimer > 0) {
                    modInst->animUpdateTimer &= 3;
                    modInst->animUpdateTimer--;
                }
            }
        }
    }
}

#ifdef NATIVE_PORT
/**
 * Commit the animation cadence once per authoritative tick, after the draw has
 * had the opportunity to observe animUpdateTimer <= 0 for shading. The original
 * render path performed this write; keeping it in the fixed-step epilogue makes
 * skipped or additional presents unable to slow or accelerate model animation.
 */
void obj_animation_cadence_tick(void) {
    s32 i;
    Object *obj;
    Object_Racer *racer;
    ModelInstance *modInst;

    for (i = gObjectListStart; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (obj == NULL || (obj->trans.flags & OBJ_FLAGS_PARTICLE) ||
            obj->header == NULL ||
            obj->header->modelType != OBJECT_MODEL_TYPE_3D_MODEL ||
            obj->modelInstances == NULL || obj->modelIndex < 0 ||
            obj->modelIndex >= obj->header->numberOfModelIds) {
            continue;
        }
        modInst = obj->modelInstances[obj->modelIndex];
        if (modInst == NULL || modInst->animUpdateTimer > 0) {
            continue;
        }
        racer = obj->behaviorId == BHV_RACER ? obj->racer : NULL;
        if (racer != NULL && obj->animationID != 0 &&
            racer->vehicleID < VEHICLE_BOSSES) {
            racer->headAngle = 0;
        }
        if (racer != NULL && racer->playerIndex == PLAYER_COMPUTER &&
            racer->vehicleID < VEHICLE_BOSSES) {
            modInst->animUpdateTimer = 2;
        } else {
            modInst->animUpdateTimer = 1;
        }
    }
}
#endif

/**
 * Renders every triangle batch in an objects mesh.
 * If vertOffset is true, then draw in two passes, utilising the head matrix and vertex ID offset in the batch.
 */
s32 render_mesh(ObjectModel *objModel, Object *obj, s32 startIndex, s32 flags, s32 overrideVerts) {
    s32 i;
    s32 textureIndex;
    s32 triOffset;
    TextureHeader *texToSet;
    s32 endLoop;
    s32 numTris;
    s32 texEnabled;
    s32 texOffset;
    s32 numVertices;
    Vertex *vtx;
    s32 offsetStartVertex;
    s32 texToSetFlags;
    Triangle *tris;
    s32 vertOffset;
    Gfx *dList;
#ifdef NATIVE_PORT
    Vec3s *batchNormals;
    s32 doorTexOffset;
    s32 doorDigitPlace;
    static s32 sDoorTexTrace = -1;
#endif

    dList = gObjectCurrDisplayList;
    i = startIndex;
    endLoop = FALSE;
    while (i < objModel->numberOfBatches && !endLoop) {
#ifdef NATIVE_PORT
        /* Taj's select actor borrows only the authored numbered placard from
         * the Diddy select model. Character geometry never reaches the draw. */
        if (taj_visual_select_sign_object(obj) &&
            !taj_visual_select_sign_batch(obj, i)) {
            i++;
            continue;
        }
#endif
        if (!(DKR_PTR(TriangleBatchInfo, objModel->batches)[i].flags & RENDER_SEMI_TRANSPARENT) || flags & RENDER_SEMI_TRANSPARENT) {
            // Hidden/Invisible geometry
            textureIndex = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].flags & RENDER_HIDDEN;
            // Probably a fakematch to use textureIndex here, but it works.
            if (!textureIndex) {
                vertOffset = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].verticesOffset;
                triOffset = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].facesOffset;
                numVertices = DKR_PTR(TriangleBatchInfo, objModel->batches)[i + 1].verticesOffset - vertOffset;
                offsetStartVertex = (overrideVerts) ? DKR_PTR(TriangleBatchInfo, objModel->batches)[i].vertOverride : numVertices;
                numTris = DKR_PTR(TriangleBatchInfo, objModel->batches)[i + 1].facesOffset - triOffset;
                tris = &DKR_PTR(Triangle, objModel->triangles)[triOffset];
                vtx = &obj->curVertData[vertOffset];
                textureIndex = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].textureIndex;
#ifdef NATIVE_PORT
                if (taj_visual_select_sign_object(obj)) {
                    textureIndex = taj_visual_select_sign_player(obj);
                }
#endif
                // textureIndex of 0xFF is no texture
                if (textureIndex == 0xFF) {
                    texOffset = 0;
                    texToSet = NULL;
                    texEnabled = FALSE;
                } else {
#ifdef NATIVE_PORT
                    if (obj_door_batch_texture_offset(
                            objModel, obj, i, &doorTexOffset,
                            &doorDigitPlace)) {
                        texOffset = doorTexOffset << 14;
                        if (sDoorTexTrace < 0) {
                            const char *value = getenv("MDKR_DOOR_TEX_TRACE");
                            sDoorTexTrace = value != NULL && value[0] != '\0' &&
                                atoi(value) != 0;
                        }
                        if (sDoorTexTrace) {
                            extern s32 g_frameCounter;
                            mdkr_trace(
                                "[DOOR-TEX] frame=%d object=%d model=%p door=%d "
                                "balloons=%d batch=%d place=%d stored=%d applied=%d",
                                g_frameCounter, obj->objectID, (void *) objModel,
                                obj->door->doorID, obj->door->balloonCount, i,
                                doorDigitPlace,
                                DKR_PTR(TriangleBatchInfo, objModel->batches)[i].texOffset,
                                texOffset >> 14);
                        }
                    } else if (gObjectRenderModelFor == obj &&
                        (DKR_PTR(TriangleBatchInfo, objModel->batches)[i].flags &
                         0x810000) == RENDER_TEX_ANIM) {
                        texOffset = gObjectRenderRacerTexOffset << 14;
                    } else {
                        texOffset = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].texOffset << 14;
                    }
#else
                    texOffset = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].texOffset << 14;
#endif
                    texEnabled = TRUE;
                    texToSet = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, objModel->textures)[textureIndex].texture);
                }
                texToSetFlags = DKR_PTR(TriangleBatchInfo, objModel->batches)[i].flags | RENDER_FOG_ACTIVE;
                if (flags & RENDER_SEMI_TRANSPARENT &&
                    !(DKR_PTR(TriangleBatchInfo, objModel->batches)[i].flags & (flags & ~RENDER_SEMI_TRANSPARENT))) {
                    texToSetFlags |= RENDER_SEMI_TRANSPARENT;
                }
                if (gObjectTexAnim == FALSE) {
                    material_set(&dList, texToSet, texToSetFlags, texOffset);
                } else {
                    texToSet = set_animated_texture_header(texToSet, texOffset);
                    gDkrDmaDisplayList(gObjectCurrDisplayList++, OS_K0_TOKEN_TO_PHYSICAL(texToSet->cmd),
                                       texToSet->numberOfCommands);
                }
#ifdef NATIVE_PORT
                batchNormals = mdkr_object_batch_normals(objModel, i);
#endif
                if (offsetStartVertex == numVertices) {
#ifdef NATIVE_PORT
                    gDkrSetSmoothNormals(
                        dList++,
                        batchNormals != NULL
                            ? OS_K0_TO_PHYSICAL(batchNormals)
                            : 0);
#endif
                    gSPVertexDKR(dList++, OS_K0_TO_PHYSICAL(vtx), numVertices, 0);
                } else {
                    if (offsetStartVertex > 0) {
#ifdef NATIVE_PORT
                        gDkrSetSmoothNormals(
                            dList++,
                            batchNormals != NULL
                                ? OS_K0_TO_PHYSICAL(batchNormals)
                                : 0);
#endif
                        gSPVertexDKR(dList++, OS_K0_TO_PHYSICAL(vtx), offsetStartVertex, 0);
                        gSPSelectMatrixDKR(dList++, G_MTX_DKR_INDEX_2);
#ifdef NATIVE_PORT
                        gDkrSetSmoothNormals(
                            dList++,
                            batchNormals != NULL
                                ? OS_K0_TO_PHYSICAL(
                                      &batchNormals[offsetStartVertex])
                                : 0);
#endif
                        gSPVertexDKR(dList++, OS_K0_TO_PHYSICAL(&vtx[offsetStartVertex]),
                                     (numVertices - offsetStartVertex), 1);
                    } else {
                        gSPSelectMatrixDKR(dList++, G_MTX_DKR_INDEX_2);
#ifdef NATIVE_PORT
                        gDkrSetSmoothNormals(
                            dList++,
                            batchNormals != NULL
                                ? OS_K0_TO_PHYSICAL(batchNormals)
                                : 0);
#endif
                        gSPVertexDKR(dList++, OS_K0_TO_PHYSICAL(vtx), numVertices, 0);
                    }
                    gSPSelectMatrixDKR(dList++, G_MTX_DKR_INDEX_1);
                }
                gSPPolygon(dList++, OS_K0_TO_PHYSICAL(tris), numTris, texEnabled);
            }
            i++;
        } else {
            endLoop = TRUE;
        }
    }
    if (i >= objModel->numberOfBatches) {
        i = -1;
    }
    gObjectCurrDisplayList = dList;
    return i;
}

s32 get_first_active_object(s32 *retObjCount) {
    s32 i, j;
    s32 minIndex, maxIndex;
    s32 breakLoop;

    *retObjCount = gObjectCount;
    if (gFirstActiveObjectId != 0) {
        // Already sorted
        return gFirstActiveObjectId;
    }

    i = gObjectListStart;
    j = gObjectCount - 1;
    minIndex = i;
    maxIndex = j;

    while (i <= j) {
        breakLoop = 0;
        while (i <= maxIndex && breakLoop == 0) {
            if (!(gObjPtrList[i]->trans.flags & OBJ_FLAGS_PARTICLE)) {
                if (gObjPtrList[i]->header->flags & HEADER_FLAGS_UNK_0001) {
                    i++;
                } else {
                    // Break the loop if neither OBJ_FLAGS_PARTICLE nor bit 1 in header->flags is set
                    breakLoop = -1;
                }
            } else {
                i++;
            }
        }

        breakLoop = 0;
        while (j >= minIndex && breakLoop == 0) {
            if (gObjPtrList[j]->trans.flags & OBJ_FLAGS_PARTICLE) {
                // Break the loop if OBJ_FLAGS_PARTICLE is set
                breakLoop = -1;
            } else if (!(gObjPtrList[j]->header->flags & HEADER_FLAGS_UNK_0001)) {
                j--;
            } else {
                // Break the loop if bit 1 in header->flags is set
                breakLoop = -1;
            }
        }

        if (i < j) {
            // Swap active and inactive objects
            Object *tempObject = gObjPtrList[i];
            gObjPtrList[i] = gObjPtrList[j];
            gObjPtrList[j] = tempObject;
            i++;
            j--;
        }
    }

    gFirstActiveObjectId = i;
    return i;
}

UNUSED void func_800149C0(unk800149C0 *arg0, UNUSED s32 arg1, s32 arg2, s32 arg3, s32 *arg4, s32 *arg5, s32 arg6) {
    UNUSED s32 pad;
    s32 endVal;
    s32 startVal;
    f32 temp_f0;
    s32 i;

    temp_f0 = arg0->unk6;
    endVal = func_80014B50(arg2, arg3, temp_f0, arg0->unk4);
    startVal = func_80014B50(arg2, endVal - 1, temp_f0, arg0->unk4 + 8);

    for (i = startVal; i < endVal; i++) {
        gObjPtrList[i]->unk38 += arg6;
    }

    *arg4 = startVal;
    *arg5 = endVal - 1;
}

// Only used in the unused function func_800149C0
s32 func_80014B50(s32 arg0, s32 arg1, f32 arg2, u32 arg3) {
    Object *swapTemp;
    s32 var_a0;
    s32 var_a1;

    var_a0 = arg0;
    var_a1 = arg1;
    switch (arg3) {
        case 0:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.x_position - gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.x_position - gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
        case 1:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.y_position - gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.y_position - gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
        case 2:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.z_position - gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.z_position - gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
        case 8:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.x_position + gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.x_position + gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
        case 9:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.y_position + gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.y_position + gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
        case 10:
            while (arg1 >= arg0) {
                while ((var_a1 >= arg0) && ((gObjPtrList[arg0]->trans.z_position + gObjPtrList[arg0]->unk34) < arg2)) {
                    arg0++;
                }
                while ((arg1 >= var_a0) && (arg2 <= (gObjPtrList[arg1]->trans.z_position + gObjPtrList[arg1]->unk34))) {
                    arg1--;
                }
                if (arg0 < arg1) {
                    swapTemp = gObjPtrList[arg0];
                    gObjPtrList[arg0] = gObjPtrList[arg1];
                    gObjPtrList[arg1] = swapTemp;
                    arg0++;
                    arg1--;
                }
            }
            break;
    }
    return arg0;
}

/**
 * Takes every object and sorts the main object list by distance to the camera.
 */
void sort_objects_by_dist(s32 startIndex, s32 lastIndex) {
    s32 i;
    s32 didNotSwap;
    Object *obj;

    if (lastIndex < startIndex) {
        return;
    }

    for (i = startIndex; i <= lastIndex; i++) {
        obj = gObjPtrList[i];
        if (obj != NULL) {
            if (obj->trans.flags & OBJ_FLAGS_PARTICLE) {
                // get_distance_to_camera calculates the distance to the camera from a XYZ location.
                obj->distanceToCamera =
                    -get_distance_to_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
            } else if (obj->header->flags & HEADER_FLAGS_UNK_0080) {
#ifdef NATIVE_PORT
                /* The original ACCUMULATES: `distanceToCamera += -16000.0f`, once
                 * per sort, i.e. once per viewport per frame, forever. These objects
                 * are meant to sort behind everything ("draw me last"), and one pass
                 * of the original already achieves that -- the sentinel it lands on
                 * after the first pass is exactly -16000. Every pass after that is
                 * unbounded drift in a field SIMULATION reads (vehicle_tricky.c:428,
                 * racer.c:4881/9287, tracks.c:4377) and the authoritative hash
                 * covers. Store the sentinel instead of accumulating it: identical
                 * single-pass ordering (behind every real distance, tied among
                 * themselves so the bubble sort keeps their relative order), no
                 * drift, and no dependence on how many times the scene was drawn. */
                obj->distanceToCamera = -16000.0f;
#else
                obj->distanceToCamera += -16000.0f;
#endif
            } else {
                obj->distanceToCamera =
                    -get_distance_to_camera(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
            }
        } else {
            //!@bug obj is NULL here, so it would probably cause a crash. Thankfully, gObjPtrList shouldn't have NULL
            //! objects in it.
            obj->distanceToCamera = 0.0f;
        }
    }

    // Keep swapping until all objects are sorted by the distance to the camera.
    do {
        didNotSwap = TRUE;
        for (i = startIndex; i < lastIndex; i++) {
            if (gObjPtrList[i]->distanceToCamera < gObjPtrList[i + 1]->distanceToCamera) {
                obj = gObjPtrList[i];
                gObjPtrList[i] = gObjPtrList[i + 1];
                gObjPtrList[i + 1] = obj;
                didNotSwap = FALSE;
            }
        }
    } while (!didNotSwap);
}

#ifdef NATIVE_PORT
/* Results of the last obj_sort_tick(), consumed by render_level_geometry_and_objects
 * instead of it recomputing (and re-permuting) them per viewport. */
s32 gObjSortFirstActive;
s32 gObjSortObjCount;

/**
 * Authoritative object ordering, hoisted out of render, which used to REORDER
 * THE SIMULATION'S ITERATION ARRAY.
 *
 * get_first_active_object (objects.c:5850) partitions gObjPtrList and
 * sort_objects_by_dist (:6045) permutes it by distance to the CURRENT VIEWPORT's
 * camera -- once per viewport, inside render_level_geometry_and_objects. That same
 * array is what obj_update (:4176/4216/4225), process_object_interactions (:6081)
 * and checkpoint_update_all iterate, so how (and how many times) the scene was
 * drawn chose the next tick's collision and RNG order. The distances it writes are
 * gameplay-read too (vehicle_tricky.c:428, racer.c:4881/9287, tracks.c:4377).
 *
 * The permutation and the distance writes now happen exactly once per
 * authoritative tick, here, using the basis of the LAST viewport of the frame --
 * which is the ordering the simulation observed before this change, because the
 * last viewport's sort is the one that survived into the next tick.
 *
 * Render keeps correct per-viewport back-to-front ordering by sorting a PRIVATE
 * index array (render_level_geometry_and_objects), touching neither gObjPtrList
 * nor obj->distanceToCamera.
 *
 * Placed before obj_animate_tick, mirroring render's own order: the sort ran
 * before the object draw loop that animates.
 */
void obj_sort_tick(void) {
    extern LevelModel *gCurrentLevelModel; /* tracks.c */
    s32 savedCameraID;

    if (gCurrentLevelModel == NULL) {
        gObjSortFirstActive = 0;
        gObjSortObjCount = 0;
        return;
    }
    savedCameraID = get_current_viewport();
    scene_build_last_viewport_basis();
    gObjSortFirstActive = get_first_active_object(&gObjSortObjCount);
    sort_objects_by_dist(gObjSortFirstActive, gObjSortObjCount - 1);
    set_active_camera(savedCameraID);
}
#endif

/**
 * Go through each object and detect potential interactions between each.
 * Add candidates to a list and calcualte their distances.
 */
void process_object_interactions(void) {
    Object *obj2;
    Object *obj;
    ObjectInteraction *objInteract;
    ObjectInteraction *objInteract2;
    f32 xDiff;
    f32 zDiff;
    f32 radius;
    s32 j;
    s32 i;
    s32 objsWithInteractives;
    Object *objList[257]; // 257 seems random, but it works for now.

    objsWithInteractives = 0;
    for (i = gObjectListStart; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            objInteract = obj->interactObj;
            if (objInteract != NULL) {
                objList[objsWithInteractives] = obj;
                objsWithInteractives++;
                if (objInteract->unk11 != 2) {
                    objInteract->obj = NULL;
                    objInteract->flags &= ~(INTERACT_FLAGS_COLLIDED | INTERACT_FLAGS_PUSHING);
                    objInteract->distance = 255;
                }
            }
        }
    }

    gCollisionObjectCount = 0;

    for (i = 0; i < objsWithInteractives; i++) {
        obj = objList[i];
        objInteract = obj->interactObj;
        if (objInteract->unk11 == 2 && gCollisionObjectCount < 20) {
            gCollisionObjects[gCollisionObjectCount] = obj;
            gCollisionObjectCount++;
        }
        if (objInteract->flags & INTERACT_FLAGS_UNK_0004) {
            for (j = 0; j < objsWithInteractives; j++) {
                if (i != j) {
                    obj2 = objList[j];
                    objInteract2 = obj2->interactObj;
                    if (objInteract2->flags & (INTERACT_FLAGS_SOLID | INTERACT_FLAGS_TANGIBLE)) {
                        if (objInteract2->unk11 == 3) {
                            func_80016748(obj, obj2);
                        } else if (objInteract2->unk11 != 2) {
                            xDiff = obj->trans.x_position - obj2->trans.x_position;
                            zDiff = obj->trans.z_position - obj2->trans.z_position;
                            if (objInteract2->flags & INTERACT_FLAGS_UNK_0020) {
                                radius = 0x400000; // 4194304.0f;
                            } else {
                                radius = 0x40000; // 262144.0f;
                            }
                            if (((xDiff * xDiff) + (zDiff * zDiff)) < radius) {
                                func_800159C8(obj, obj2);
                            }
                        }
                    }
                }
            }
        }
        if (objInteract->flags & INTERACT_FLAGS_UNK_0100) {
            for (j = 0; j < objsWithInteractives; j++) {
                if (i != j) {
                    obj2 = objList[j];
                    objInteract2 = obj2->interactObj;
                    if (objInteract2->unk11 == 3) {
                        func_80016748(obj, obj2);
                    }
                }
            }
        }
    }

    for (i = 0; i < objsWithInteractives; i++) {
        obj = objList[i];
        objInteract = obj->interactObj;
        objInteract->x_position = obj->trans.x_position;
        objInteract->y_position = obj->trans.y_position;
        objInteract->z_position = obj->trans.z_position;
    }
}

void func_800159C8(Object *arg0, Object *arg1) {
    f32 sp9C;
    f32 sp98;
    f32 f14;
    f32 sp90;
    f32 sp8C;
    f32 f18;
    f32 sp84;
    f32 sp80;
    f32 sp7C;
    f32 f12;
    f32 sp74;
    f32 f2;
    f32 f0;
    f32 sp68;
    f32 sp64;
    f32 sp60;
    s32 var_v0;
    ObjectInteraction *sp58;
    ObjectInteraction *sp54;
    Object_Racer *sp50;
    Object_Racer *sp4C;

    sp68 = arg1->trans.x_position - arg0->trans.x_position;
    sp64 = arg1->trans.y_position - arg0->trans.y_position;
    sp60 = arg1->trans.z_position - arg0->trans.z_position;

    sp58 = arg0->interactObj;
    sp54 = arg1->interactObj;
    sp80 = 1 / gObjectUpdateRateF;

    if (sp54->unk11 == 1) {
        sp64 = -sp64;
        if (sp64 < sp54->unk16 * 10.0f || sp54->unk17 * 10.0f < sp64) {
            return;
        }
        sp64 = 0.0f;
    }

    if (sp54->unk11 == 4 && sp64 < 0.0f) {
        sp64 *= 0.3;
    }

    sp9C = sqrtf(sp68 * sp68 + sp64 * sp64 + sp60 * sp60);

#if VERSION == VERSION_80
    if (sp9C > 4000.0f) {
        return;
    }
    if (sp9C < -4000.0f) {
        return;
    }
#endif

    var_v0 = (s32) (f32) (s32) sp9C;
    if (sp58->flags & 0x20) {
        var_v0 >>= 3;
    }

    if (var_v0 > 255) {
        var_v0 = 255;
    }

    if (sp58->distance >= var_v0) {
        sp58->obj = arg1;
        sp58->distance = var_v0;
    }

    var_v0 = (s32) (f32) (s32) sp9C;
    if (sp54->flags & 0x20) {
        var_v0 >>= 3;
    }

    if (var_v0 > 255) {
        var_v0 = 255;
    }

    if (sp54->distance >= var_v0) {
        sp54->obj = arg0;
        sp54->distance = var_v0;
    }

    if (!(sp54->flags & 1)) {
        return;
    }

    sp98 = sp54->hitboxRadius + sp58->hitboxRadius;

    sp7C = arg0->trans.x_position - sp58->x_position;
    f12 = arg0->trans.y_position - sp58->y_position;
    sp74 = arg0->trans.z_position - sp58->z_position;

    if (sp54->unk11 == 1) {
        f12 = 0.0f;
    }

    f2 = sp7C * sp7C + f12 * f12 + sp74 * sp74;
    if (f2 > 1.0) {
        f2 = ((arg1->trans.x_position - sp58->x_position) * sp7C + (arg1->trans.y_position - sp58->y_position) * f12 +
              (arg1->trans.z_position - sp58->z_position) * sp74) /
             f2;
        if (f2 >= 0.0f && f2 <= 1.0) {
            sp8C = sp58->x_position + f2 * sp7C;
            f18 = sp58->y_position + f2 * f12;
            sp84 = sp58->z_position + f2 * sp74;
            sp9C = sqrtf((sp8C - arg1->trans.x_position) * (sp8C - arg1->trans.x_position) +
                         (f18 - arg1->trans.y_position) * (f18 - arg1->trans.y_position) +
                         (sp84 - arg1->trans.z_position) * (sp84 - arg1->trans.z_position));
        }
    }

    if (sp9C < sp98 && sp9C > 0.0f) {
        sp8C = sp54->x_position - sp58->x_position;
        f18 = sp54->y_position - sp58->y_position;
        sp84 = sp54->z_position - sp58->z_position;
        if (sp54->unk11 == 1) {
            f18 = 0.0f;
        }

        f0 = sqrtf(sp8C * sp8C + f18 * f18 + sp84 * sp84);
        if (f0 > 0.0f) {
            sp68 = sp8C / f0;
            sp64 = f18 / f0;
            sp60 = sp84 / f0;
        } else {
            sp68 /= sp9C;
            sp64 /= sp9C;
            sp60 /= sp9C;
        }
        sp9C = f0 - sp9C;
        if (sp9C < 0.0f) {
            sp9C = -sp9C;
        }

        sp68 *= sp9C;
        sp64 *= sp9C;
        sp60 *= sp9C;
        sp9C *= sp80;
        sp58->flags |= 8;
        sp54->flags |= 8;
        if (sp54->pushForce == 0) {
            arg0->trans.x_position -= sp68;
            arg0->trans.y_position -= sp64;
            arg0->trans.z_position -= sp60;

            sp68 *= sp80;
            sp60 *= sp80;
            if (arg0->behaviorId != 1) {
                return;
            }

            sp50 = arg0->racer;
            var_v0 = FALSE;
            if (sp50->vehicleID == 1) {
                if (sp9C > 0.3) {
                    if (sp9C > 1.0) {
                        var_v0 = TRUE;
                    }
                    if (var_v0) {
                        arg0->x_velocity *= 0.8;
                        arg0->z_velocity *= 0.8;
                    }
                    if (var_v0) {
                        sp54->flags |= 0x40;
                        f2 = (arg0->trans.x_position * arg0->z_velocity - arg0->trans.z_position * arg0->x_velocity);
                        f2 = (arg1->trans.x_position * arg0->z_velocity - arg1->trans.z_position * arg0->x_velocity) -
                             f2;
                        sp50->unk1D2 = 7;
                        if (f2 >= 0.0f) {
                            sp50->unk120 = arg0->x_velocity * 0.1;
                            sp50->unk11C = -arg0->z_velocity * 0.1;
                        } else {
                            sp50->unk120 = -arg0->x_velocity * 0.1;
                            sp50->unk11C = arg0->z_velocity * 0.1;
                        }
                    }
                }
            } else {
                if (sp9C > 0.3) {
                    if (sp9C > 1.0) {
                        var_v0 = 1;
                    }
                    arg0->x_velocity -= sp68;
                    arg0->z_velocity -= sp60;
                    sp50->velocity = sp9C * 0.25;
                    sp50->lateral_velocity = 0.0f;
                }
                if (var_v0) {
                    sp54->flags |= 0x40;
                    f2 = arg0->trans.x_position * arg0->z_velocity - arg0->trans.z_position * arg0->x_velocity;
                    f2 = arg1->trans.x_position * arg0->z_velocity - arg1->trans.z_position * arg0->x_velocity - f2;
                    if (f2 >= 0.0f) {
                        f2 = 2.0f;
                    } else {
                        f2 = -2.0f;
                    }
                    sp50->unk1D2 = 7;
                    sp50->unk11C = sp50->ox3 * f2 * sp50->velocity;
                    sp50->unk120 = sp50->oz3 * f2 * sp50->velocity;
                }
            }
            if (var_v0 && sp50->playerIndex != -1) {
                func_80016500(arg0, sp50);
            }
            return;
        }

        sp68 *= 0.5;
        sp64 *= 0.5;
        sp60 *= 0.5;
        if (sp58->pushForce != 0) {
            arg0->trans.x_position -= sp68;
            arg0->trans.y_position -= sp64;
            arg0->trans.z_position -= sp60;
        }
        arg1->trans.x_position += sp68;
        arg1->trans.y_position += sp64;
        arg1->trans.z_position += sp60;

        sp68 *= sp80;
        sp60 *= sp80;
        if (arg0->behaviorId == 1 && arg1->behaviorId == 1) {
            sp90 = 1.0;
            sp90 += (f32) (sp58->pushForce - sp54->pushForce) * 0.3;

            sp50 = arg0->racer;
            sp4C = arg1->racer;

            if (sp50->shieldType != 0 && sp4C->shieldType == 0) {
                sp4C->spinout_timer = sp50->shieldType;
            }
            if (sp4C->shieldType != 0 && sp50->shieldType == 0) {
                sp50->spinout_timer = sp4C->shieldType;
            }

            var_v0 = FALSE;
            if (sp50->vehicleID == 1) {
                var_v0 = TRUE;
            } else if (sp9C > 0.1) {
                var_v0 = TRUE;
                sp68 *= 0.5;
                sp60 *= 0.5;
            }

            if (var_v0) {
                if (sp58->pushForce != 0) {
                    f0 = 2.0 - sp90;
                    arg0->x_velocity -= sp68 * f0;
                    arg0->z_velocity -= sp60 * f0;
                    func_80016500(arg0, sp50);
                }
                arg1->x_velocity += sp68 * sp90;
                arg1->z_velocity += sp60 * sp90;
                func_80016500(arg1, sp4C);
            }
        }
    }
}

void func_80016500(Object *obj, Object_Racer *racer) {
    s32 shakeMagnitude;
    s32 volume;
    s32 angle;
    f32 startVelocity;
    f32 cosAngle;
    f32 sinAngle;

    startVelocity = racer->velocity;
    angle = racer->steerVisualRotation;
    if (racer->vehicleID == VEHICLE_CAR) {
        if (racer->drift_direction != 0) {
            angle += racer->unk10C;
            angle = (s16) angle;
        }
    }
    cosAngle = coss_f(-angle);
    sinAngle = sins_f(-angle);
    racer->lateral_velocity = (obj->x_velocity * cosAngle) + (obj->z_velocity * sinAngle);
    racer->velocity = (-obj->x_velocity * sinAngle) + (obj->z_velocity * cosAngle);
    if (racer->playerIndex != PLAYER_COMPUTER) {
        volume = (startVelocity - racer->velocity) * 14.0f;
        if (volume < 0) {
            volume = -volume;
        }
        volume += 35;
        if (volume > 127) {
            volume = 127;
        }
        if (racer->unk1F6 == 0) {
            sound_play(SOUND_CRASH_CHARACTER, &racer->unk220);
            sound_volume_set_relative(SOUND_CRASH_CHARACTER, racer->unk220, volume);
        }
        if (racer->unk1F6 == 0 && volume > 55) {
            if (!racer->raceFinished) {
                rumble_set(racer->playerIndex, RUMBLE_TYPE_18);
            }
            racer->unk1F3 |= 8;
        }
        if (volume > 55) {
            play_random_character_voice(obj, SOUND_VOICE_CHARACTER_NEGATIVE, 8, 1);
        }
        shakeMagnitude = (startVelocity - racer->velocity);
        if (shakeMagnitude < 0) {
            shakeMagnitude = -shakeMagnitude;
        }
        if (shakeMagnitude > 3) {
            shakeMagnitude = 3;
        }
        racer->unk1F6 = 30;
        set_active_camera(racer->playerIndex);
        cam_get_active_camera()->shakeMagnitude = shakeMagnitude;
    }
}

void func_80016748(Object *obj0, Object *obj1) {
    ObjectModel *objModel;
    s32 i;
    f32 temp;

#ifdef AVOID_UB
    MtxF obj1TransformMtx;
#else
    // THIS IS A HACK! Supposed to be a MtxF, but the stack ended up being too big.
    f32 pad[2];
    f32 obj1TransformMtx[4][3];
#endif

    f32 xDiff;
    f32 yDiff;
    f32 zDiff;
    ObjectInteraction *obj1Interact;
    ObjectInteraction *obj0Interact;
    Object_Racer *racer;
    f32 distance;
    f32 radius;
    ModelInstance *modInst;

    if (obj1->curVertData != NULL) {
        modInst = obj1->modelInstances[0];
        objModel = modInst->objModel;
        xDiff = obj0->trans.x_position - obj1->trans.x_position;
        yDiff = obj0->trans.y_position - obj1->trans.y_position;
        zDiff = obj0->trans.z_position - obj1->trans.z_position;
        if (!((objModel->unk3C + 50.0) < sqrtf((xDiff * xDiff) + (yDiff * yDiff) + (zDiff * zDiff)))) {
            obj0Interact = obj0->interactObj;
            obj1Interact = obj1->interactObj;
            mtxf_from_transform((MtxF *) obj1TransformMtx, &obj1->trans);
            for (i = 0; i < objModel->collisionSpheresSize; i += 2) {
                xDiff = obj1->curVertData[DKR_PTR(s16, objModel->collisionSpheres)[i]].x;
                yDiff = obj1->curVertData[DKR_PTR(s16, objModel->collisionSpheres)[i]].y;
                zDiff = obj1->curVertData[DKR_PTR(s16, objModel->collisionSpheres)[i]].z;
                mtxf_transform_point((float (*)[4]) obj1TransformMtx, xDiff, yDiff, zDiff, &xDiff, &yDiff, &zDiff);
                temp = (((f32) DKR_PTR(s16, objModel->collisionSpheres)[i + 1] / 64) * obj1->trans.scale) * 50.0;
                xDiff -= obj0->trans.x_position;
                yDiff -= obj0->trans.y_position;
                zDiff -= obj0->trans.z_position;
                distance = sqrtf((xDiff * xDiff) + (yDiff * yDiff) + (zDiff * zDiff));
                temp += obj1Interact->hitboxRadius;
                if (distance < temp && distance > 0.0f) {
                    obj0Interact->flags |= INTERACT_FLAGS_PUSHING;
                    obj1Interact->flags |= INTERACT_FLAGS_PUSHING;
                    obj0Interact->obj = obj1;
                    obj1Interact->obj = obj0;
                    obj0Interact->distance = 0;
                    obj1Interact->distance = 0;
                    radius = (temp - distance) / distance;
                    distance = 2; // Needed
                    radius /= distance;
                    xDiff *= radius;
                    yDiff *= radius;
                    zDiff *= radius;
                    obj0->trans.x_position -= xDiff;
                    obj0->trans.y_position -= yDiff;
                    obj0->trans.z_position -= zDiff;

                    if (obj0->behaviorId == BHV_RACER) {
                        racer = obj0->racer;
                        if (!racer->raceFinished) {
                            rumble_set(racer->playerIndex, RUMBLE_TYPE_18);
                        }
                        if (racer->vehicleID == VEHICLE_HOVERCRAFT) {
                            if (radius > 0.1) {
                                obj0->x_velocity -= xDiff;
                                obj0->z_velocity -= zDiff;
                            }
                        } else if (radius > 0.3) {
                            obj0->x_velocity -= xDiff;
                            obj0->z_velocity -= zDiff;
                            racer->velocity = radius * 4.0f;
                            racer->lateral_velocity = 0.0f;
                        }
                    }
                }
            }
        }
    }
}

void func_80016BC4(Object *obj) {
    s32 i;

    obj->collisionData->mtxFlip = 0;
    obj_collision_transform(obj);
    obj_collision_transform(obj); // Not sure why they do this a second time.
    for (i = 0; i < obj->header->numberOfModelIds; i++) {
        if (obj->modelInstances[i] != NULL) {
            model_init_collision(obj->modelInstances[i]->objModel);
        }
    }
}

/**
 * Find the first butterfly node within range and return a pointer to it.
 */
Object *obj_butterfly_node(f32 x, f32 y, f32 z, f32 maxDistCheck, s32 dontCheckYAxis) {
    f32 diffY;
    f32 diffZ;
    f32 diffX;
    f32 distance;
    s32 i;
    Object *curObj;

    for (i = 0; i < gObjectCount; i++) {
        curObj = gObjPtrList[i];
        if (!(curObj->trans.flags & OBJ_FLAGS_PARTICLE) && curObj->behaviorId == BHV_ANIMATED_OBJECT_3) {
            diffX = curObj->trans.x_position - x;
            diffZ = curObj->trans.z_position - z;
            if (!dontCheckYAxis) {
                diffY = curObj->trans.y_position - y;
                distance = sqrtf((diffX * diffX) + (diffY * diffY) + (diffZ * diffZ));
            } else {
                distance = sqrtf((diffX * diffX) + (diffZ * diffZ));
            }
            if (distance < maxDistCheck) {
                return curObj;
            }
        }
    }
    return NULL;
}

/**
 * Compare distance between itself and every racer.
 * Any racer within the radius is sorted from nearest to furthest.
 * Returns the number of racers within the radius.
 */
s32 obj_dist_racer(f32 x, f32 y, f32 z, f32 radius, s32 is2dCheck, Object **sortObj) {
    f32 distances[8];
    s32 i;
    s32 j;
    f32 xDiff;
    f32 yDiff;
    f32 zDiff;
    s32 result;
    Object *racerObj;
    Object_Racer *racer;
    Object *swapObj;
    f32 swapf;

    result = 0;
    if (gNumRacers > 0) {
        for (i = 0; i < gNumRacers; i++) {
            racerObj = (*gRacers)[i];
            racer = racerObj->racer;
            if (racer->playerIndex >= 0 && racer->playerIndex < 4) {
                if (is2dCheck) {
                    xDiff = racerObj->trans.x_position - x;
                    zDiff = racerObj->trans.z_position - z;
                    yDiff = sqrtf((xDiff * xDiff) + (zDiff * zDiff));
                } else {
                    xDiff = racerObj->trans.x_position - x;
                    yDiff = racerObj->trans.y_position - y;
                    zDiff = racerObj->trans.z_position - z;
                    yDiff = sqrtf((xDiff * xDiff) + (yDiff * yDiff) + (zDiff * zDiff));
                }
                if (yDiff < radius) {
                    distances[result] = yDiff;
                    sortObj[result] = (*gRacers)[i];
                    result++;
                }
            }
        }
        if (result >= 2) {
            for (i = result - 1; i > 0; i--) {
                for (j = 0; j < i; j++) {
                    if (distances[j + 1] < distances[j]) {
                        swapf = distances[j];
                        swapObj = sortObj[j];
                        distances[j] = distances[j + 1];
                        sortObj[j] = sortObj[j + 1];
                        distances[j + 1] = swapf;
                        sortObj[j + 1] = swapObj;
                    }
                }
            }
        }
    }
    return result;
}

/**
 * Updates the object's collision transformation matrices.
 */
void obj_collision_transform(Object *obj) {
    ObjectTransform trans;
    s32 i;
    f32 inverseScale;
    MtxF *curMtx;
    MtxF inverseMtx;
    ObjectCollision *colData;

    colData = obj->collisionData;
    colData->mtxFlip = (colData->mtxFlip + 1) & 1;
#ifdef AVOID_UB
    curMtx = &colData->matrices[colData->mtxFlip];
#else

    curMtx = (MtxF *) &colData->_matrices[colData->mtxFlip << 1];
#endif
    trans.rotation.y_rotation = -obj->trans.rotation.y_rotation;
    trans.rotation.x_rotation = -obj->trans.rotation.x_rotation;
    trans.rotation.z_rotation = -obj->trans.rotation.z_rotation;
    trans.scale = 1.0f;
    trans.x_position = -obj->trans.x_position;
    trans.y_position = -obj->trans.y_position;
    trans.z_position = -obj->trans.z_position;
    mtxf_from_inverse_transform(curMtx, &trans);
    inverseScale = 1.0 / obj->trans.scale;
    i = 0;
    // Zero out the matrix.
    while (i < 16) {
        ((f32 *) inverseMtx)[i] = 0.0f;
        i++;
    }
    inverseMtx[0][0] = inverseScale;
    inverseMtx[1][1] = inverseScale;
    inverseMtx[2][2] = inverseScale;
    inverseMtx[3][3] = 1.0f;
    mtxf_mul(curMtx, &inverseMtx, curMtx);
    trans.rotation.y_rotation = obj->trans.rotation.y_rotation;
    trans.rotation.x_rotation = obj->trans.rotation.x_rotation;
    trans.rotation.z_rotation = obj->trans.rotation.z_rotation;
    trans.scale = 1.0 / inverseScale;
    trans.x_position = obj->trans.x_position;
    trans.y_position = obj->trans.y_position;
    trans.z_position = obj->trans.z_position;
#ifdef AVOID_UB
    mtxf_from_transform((MtxF *) colData->matrices[(colData->mtxFlip + 2)], &trans);
#else
    mtxf_from_transform((MtxF *) colData->_matrices[(colData->mtxFlip + 2) << 1], &trans);
#endif
    colData->collidedObj = NULL;
}

s32 collision_objectmodel(Object *obj, s32 arg1, s32 *arg2, Vec3f *arg3, f32 *arg4, f32 *arg5, s8 *surface) {
    ModelInstance *modInst;
    s32 sp170;
    s32 sp16C;
    s32 sp168;
    s32 j;
    s32 sp160;
    f32 dist;
    Object *sp158;
    ObjectModel *sp154;
    f32 temp;
    unk800179D0 *sp14C;
    f32 sp13C[4];
    f32 sp12C[4];
    f32 sp11C[4];
    s32 i;
    ObjectCollision *collision;
    s32 tempv0;
    f32 sp100[4];
    f32 spF0[4];
    f32 spE0[4];
    MtxF *spDC;
    s32 spB4[20];
    f32 sp8C[20];

    sp160 = 0;

    for (sp170 = 0; sp170 < gCollisionObjectCount; sp170++) {
        sp158 = gCollisionObjects[sp170];
        modInst = sp158->modelInstances[sp158->modelIndex];
        sp154 = modInst->objModel;

        temp = sp158->trans.x_position - obj->trans.x_position;
        dist = sqrtf(
            (temp) * (temp) +
            (sp158->trans.y_position - obj->trans.y_position) * (sp158->trans.y_position - obj->trans.y_position) +
            (sp158->trans.z_position - obj->trans.z_position) * (sp158->trans.z_position - obj->trans.z_position));

        j = dist;
        if (sp158->interactObj->flags & 0x20) {
            j >>= 3;
        }
        if (j > 255) {
            j = 255;
        }
        if (sp158->interactObj->distance > j) {
            sp158->interactObj->distance = j;
            sp158->interactObj->obj = obj;
        }

        if (dist - 25.0f < sp154->unk3C * sp158->trans.scale) {
            spB4[sp160] = sp170;
            sp8C[sp160] = dist;

            // clang-format off
            for (i = sp160; i > 0 && sp8C[i - 1] < sp8C[i]; i--) {\
                temp = sp8C[i];\
                sp16C = spB4[i];\
                sp8C[i] = sp8C[i - 1];\
                spB4[i] = spB4[i - 1];\
                sp8C[i - 1] = temp;
                spB4[i - 1] = sp16C;
            }
            // clang-format on

            sp160++;
        }
    }

    sp168 = 0;
    sp170 = 0;
    while (sp170 < sp160) {
        sp158 = gCollisionObjects[spB4[sp170]];
        modInst = sp158->modelInstances[sp158->modelIndex];
        sp154 = modInst->objModel;
        collision = sp158->collisionData;
#ifdef AVOID_UB
        spDC = &collision->matrices[((sp158->collisionData->mtxFlip + 1) & 1)];
#else
        spDC = (MtxF *) &collision->_matrices[((sp158->collisionData->mtxFlip + 1) & 1) << 1];
#endif

        sp14C = func_8001790C(obj, sp158);
        if (sp14C != NULL) {
            for (i = 0, j = 0; j < arg1; j++, i += 3) {
                sp13C[j] = sp14C->unk0C[i + 0];
                sp12C[j] = sp14C->unk0C[i + 1];
                sp11C[j] = sp14C->unk0C[i + 2];
                mtxf_transform_point(*spDC, arg4[i], arg4[i + 1], arg4[i + 2], &sp100[j], &spF0[j], &spE0[j]);
            }
        } else {
            for (i = 0, j = 0; j < arg1; j++, i++) {
                mtxf_transform_point(*spDC, arg3[i].x, arg3[i].y, arg3[i].z, &sp13C[j], &sp12C[j], &sp11C[j]);
            }
        }

        for (i = 0, j = 0; j < arg1; j++, i += 3) {
            mtxf_transform_point(*spDC, arg4[i], arg4[i + 1], arg4[i + 2], &sp100[j], &spF0[j], &spE0[j]);
        }

        arg2[0] = 0;
        tempv0 = func_80017A18(sp154, arg1, arg2, sp13C, sp12C, sp11C, sp100, spF0, spE0, arg5, surface,
                               1.0 / sp158->trans.scale);
        if (tempv0 != 0) {

            // @fake
            if (!j) {}

            sp158->collisionData->collidedObj = obj;
#ifdef NATIVE_PORT
            /* [OBJCOLL] telemetry. This assignment is the ONLY non-NULL writer of
             * collidedObj in the whole game, and five interaction sites read it
             * (the door and T.T.-door balloon textboxes, the trophy cabinet, the
             * lighthouse signpost). It was unreachable while func_80017A18 was
             * stubbed, so counting it is what shows the gap is really closed. */
            mdkr_objcoll_hit();
#endif
        }

        if (D_8011AD24[0] == 0) {
            sp14C = func_80017978(obj, sp158);
        }

#ifdef AVOID_UB
        spDC = &sp158->collisionData->matrices[(sp158->collisionData->mtxFlip + 2)];
#else
        spDC = (MtxF *) &sp158->collisionData->_matrices[(sp158->collisionData->mtxFlip + 2) << 1];
#endif

        // @fake
        if (sp158) {}

        sp16C = 1;
        for (i = 0, j = 0; j < arg1; j++, i += 3, sp16C <<= 1) {
            if (sp14C != NULL) {
                sp14C->unk0C[i + 0] = sp100[j];
                sp14C->unk0C[i + 1] = spF0[j];
                sp14C->unk0C[i + 2] = spE0[j];
            }
            if (tempv0 & sp16C) {
                mtxf_transform_point(*spDC, sp100[j], spF0[j], spE0[j], &arg4[i + 0], &arg4[i + 1], &arg4[i + 2]);
            }
        }

        sp168 |= tempv0;
        sp170++;
    }

    arg2[0] = 0;
    for (i = 0; i < 4; i++) {
        if (sp168 & (1 << i)) {
            arg2[0]++;
        }
    }

    return sp168;
}

unk800179D0 *func_8001790C(Object *arg0, Object *arg1) {
    unk800179D0 *entry;
    s16 i;

    for (i = 0; i < 16; i++) {
        entry = &D_8011AFF4[i];
        if (entry->unk0 != 0 && entry->unk04 == arg0 && entry->unk08 == arg1) {
            entry->unk0 = 0;
            return entry;
        }
    }
    return NULL;
}

unk800179D0 *func_80017978(Object *obj1, Object *obj2) {
    unk800179D0 *entry;
    s16 i;

    for (i = 0; i < 16; i++) {
        entry = &D_8011AFF4[i];
        if (entry->unk0 == 0) {
            entry->unk04 = obj1;
            entry->unk08 = obj2;
            entry->unk0 = 2;
            return entry;
        }
    }
    return NULL;
}

u32 func_800179D0(void) {
    s16 i = 0;
    while (i < 16) {
        unk800179D0 *temp = &D_8011AFF4[i];
        if (temp->unk0 != 0) {
            temp->unk0--;
        }
        i++;
    }
#ifdef AVOID_UB
    return 0;
#endif
}

/* NATIVE_PORT: this body is the upstream decomp's matched func_80017A18, adopted
 * verbatim -- the port carries no patches of its own to it beyond the three
 * deltas below.
 *
 * THREE deltas from upstream, all of them NATIVE_PORT-only:
 *   1-2. `collisionPlanes` and `collisionFacets` are `dkrptr32` tokens here, so
 *        they are read through DKR_PTR() -- the same convention object_models.c
 *        already uses at every site that BUILDS this data. Both reduce to a
 *        plain cast on the N64 build (see dkr_native_ptr.h).
 *   3.   an `#ifdef NATIVE_PORT` early return at the top of the body, for
 *        MDKR_OBJCOLL=legacy. It exists because this fix's failure mode is
 *        silence, so the check needs both arms from one binary; it is inert
 *        unless that variable is set, and absent entirely from the N64 build.
 * Anything else appearing here on a future sync is upstream's, not ours.
 *
 * Until this landed, the link resolved to a `return 0` WEAK stub in
 * platform/hasm_stubs_temp.c, so object-model collision never reported a hit:
 * every collision-meshed object was intangible and `collidedObj` was permanently
 * NULL. See docs/OPEN_ITEMS.md wave "objcoll". */
s32 func_80017A18(ObjectModel *arg0, s32 arg1, s32 *arg2, f32 *arg3, f32 *arg4, f32 *arg5, f32 *arg6, f32 *arg7,
                  f32 *arg8, f32 *arg9, s8 *argA, f32 argB) {
    f32 *planes;
    s32 i, j, k;
    f32 sum1, sum2;
    f32 t;
    u32 var_a2; // u32 required here to force loading 1 instead of a3 into it
    s32 counter;
    s32 spF8;
    s32 var_s6;
    CollisionFacetPlanes *node;
    f32 pad; // unused; holds the stack slot at 0xEC
    s32 closestTri;
    f32 A;
    f32 B;
    f32 C;
    f32 D;
    f32 A1, B1, C1, D1;
    s32 redoLoop;
    f32 spC0;
    f32 x1;
    f32 y1;
    f32 z1;
    f32 x3, y3, z3;
    f32 x2;
    f32 y2;
    f32 z2;

#ifdef NATIVE_PORT
    /* Positive-control arm only: reproduce the removed WEAK stub's behaviour so
     * one binary can drive both sides of tests/check_door_blocks.py. Off unless
     * MDKR_OBJCOLL=legacy is set. */
    if (mdkr_objcoll_legacy()) {
        return 0;
    }
#endif

    spF8 = 0;
    planes = DKR_PTR(f32, arg0->collisionPlanes);
    var_s6 = 1;

    for (i = 0; i < arg1; i++) {
        x1 = arg6[i];
        y1 = arg7[i];
        z1 = arg8[i];
        x2 = arg3[i];
        y2 = arg4[i];
        z2 = arg5[i];
        spC0 = arg9[i] * argB;

        counter = 0;
        do {
            redoLoop = FALSE;
            j = 0;
            if (arg0->collisionFacetCount > 0) {
                do {
                    node = &DKR_PTR(CollisionFacetPlanes, arg0->collisionFacets)[j];
                    closestTri = node->basePlaneIndex;

                    A = planes[4 * closestTri + 0];
                    B = planes[4 * closestTri + 1];
                    C = planes[4 * closestTri + 2];
                    D = planes[4 * closestTri + 3];
                    sum1 = A * x2 + B * y2 + C * z2 + D;
                    sum2 = A * x1 + B * y1 + C * z1 + D;
                    sum1 -= spC0;
                    sum2 -= spC0;
                    if (sum1 >= -0.1 && sum2 < -0.1) {
                        x3 = (x1 - x2);
                        y3 = (y1 - y2);
                        z3 = (z1 - z2);
                        if (sum1 != sum2) {
                            t = sum1 / (sum1 - sum2);
                        } else {
                            t = 0.0f;
                        }

                        var_a2 = TRUE;
                        x3 = x3 * t + x2;
                        y3 = y3 * t + y2;
                        z3 = z3 * t + z2;

                        for (k = 0; k < 3 && var_a2 == TRUE; k++) {
                            closestTri = node->edgeBisectorPlane[k];

                            A1 = planes[4 * closestTri + 0];
                            B1 = planes[4 * closestTri + 1];
                            C1 = planes[4 * closestTri + 2];
                            D1 = planes[4 * closestTri + 3];
                            t = A1 * x3 + B1 * y3 + C1 * z3 + D1;
                            if (t > 4.0f) {
                                var_a2 = FALSE;
                            }
                        }

                        if (var_a2) {
                            redoLoop = TRUE;
                            if (B > 0.707) {
                                y1 = (spC0 - (A * x1 + C * z1 + D)) / B;
                            } else {
                                x1 -= sum2 * A;
                                y1 -= sum2 * B;
                                z1 -= sum2 * C;
                            }
                            counter++;
                            if (counter > 10) {
                                redoLoop = FALSE;
                                x1 = x2;
                                y1 = y2;
                                z1 = z2;
                            }
                            argA[i] = 0;
                            arg6[i] = x1;
                            arg7[i] = y1;
                            arg8[i] = z1;

                            j = arg0->collisionFacetCount; // break
                        }
                    }
                    j++;
                } while (j < arg0->collisionFacetCount);
            }
        } while (redoLoop);

        if (counter > 0) {
            arg2[0]++;
            spF8 |= var_s6;
        }
        var_s6 <<= 1;
    }

    return spF8;
}

/**
 * Sets the active Taj challenge.
 */
void set_taj_challenge_type(s32 vehicleID) {
    gTajChallengeType = vehicleID;
    gPathUpdateOff = FALSE;
}

/**
 * Returns which Taj challenge is currently active.
 */
UNUSED s16 get_taj_challenge_type(void) {
    return gTajChallengeType;
}

/**
 * Updates information pertaining to all checkpoints in the current race.
 */
void checkpoint_update_all(void) {
    f32 xDiff;
    f32 zDiff;
    f32 yDiff;
    s32 tempCheckpointID;
    s32 checkpointNum;
    s32 duplicateCheckpoint;
    s32 breakOut;
    s32 altRouteId;
    s32 i;
    s32 isSorted;
    s32 checkpointID;
    f32 ox;
    f32 oy;
    f32 oz;
    Object *obj;
    CheckpointNode *checkpoint;
    LevelObjectEntry_Checkpoint *checkpointEntry;
    MtxF mtx;
    ObjectTransform transform;
    s32 alternateCount;

    alternateCount = 0;
    gNumberOfMainCheckpoints = 0;
    for (i = 0; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && obj->behaviorId == BHV_CHECKPOINT &&
            gNumberOfMainCheckpoints < MAX_CHECKPOINTS) {
            checkpointEntry = &obj->level_entry->checkpoint;
            if (checkpointEntry->vehicleType == gTajChallengeType) {
                gTrackCheckpoints[gNumberOfMainCheckpoints].obj = obj;
                checkpointID = checkpointEntry->index;
                if (checkpointEntry->isAltCheckpoint) {
                    checkpointID += 255;
                    alternateCount++;
                }
                gTrackCheckpoints[gNumberOfMainCheckpoints].checkpointID = checkpointID;
                gTrackCheckpoints[gNumberOfMainCheckpoints].altRouteID = -1;
                gNumberOfMainCheckpoints++;
            }
        }
    }

    duplicateCheckpoint = FALSE;
    do {
        isSorted = TRUE;

        for (i = 0; i < gNumberOfMainCheckpoints - 1; i++) {
            if (gTrackCheckpoints[i].checkpointID == gTrackCheckpoints[i + 1].checkpointID) {
                duplicateCheckpoint = TRUE;
                checkpointNum = gTrackCheckpoints[i].checkpointID;
            }

            if (gTrackCheckpoints[i + 1].checkpointID < gTrackCheckpoints[i].checkpointID) {
                tempCheckpointID = gTrackCheckpoints[i].checkpointID;
                obj = gTrackCheckpoints[i].obj;
                gTrackCheckpoints[i].checkpointID = gTrackCheckpoints[i + 1].checkpointID;
                gTrackCheckpoints[i].obj = gTrackCheckpoints[i + 1].obj;
                gTrackCheckpoints[i + 1].checkpointID = tempCheckpointID;
                gTrackCheckpoints[i + 1].obj = obj;
                isSorted = FALSE;
            }
        }
    } while (!isSorted);
    gNumberOfTotalCheckpoints = gNumberOfMainCheckpoints;
    gNumberOfMainCheckpoints -= alternateCount;
    if (duplicateCheckpoint) {
        set_render_printf_position(20, 220);
        render_printf("Error: Multiple checkpoint no: %d !!\n", checkpointNum);
    }
    for (i = gNumberOfMainCheckpoints; i < gNumberOfTotalCheckpoints; i++) {
        tempCheckpointID = gTrackCheckpoints[i].checkpointID - 255;
        for (checkpointID = 0, breakOut = FALSE; checkpointID < gNumberOfMainCheckpoints && !breakOut; checkpointID++) {
            if (tempCheckpointID == gTrackCheckpoints[checkpointID].checkpointID) {
                gTrackCheckpoints[checkpointID].altRouteID = i;
                gTrackCheckpoints[i].altRouteID = checkpointID;
                breakOut = TRUE;
            }
        }
    }

    for (i = 0; i < gNumberOfTotalCheckpoints; i++) {
        checkpoint = &gTrackCheckpoints[i];
        obj = checkpoint->obj;
        checkpointEntry = &obj->level_entry->checkpoint;
        transform.rotation.y_rotation = obj->trans.rotation.y_rotation;
        transform.rotation.x_rotation = obj->trans.rotation.x_rotation;
        transform.rotation.z_rotation = obj->trans.rotation.z_rotation;
        transform.scale = 1.0f;
        transform.x_position = 0.0f;
        transform.y_position = 0.0f;
        transform.z_position = 0.0f;
        mtxf_from_transform(&mtx, &transform);
        mtxf_transform_point(mtx, 0.0f, 0.0f, 1.0f, &ox, &oy, &oz);
        checkpoint->rotationXFrac = ox;
        checkpoint->rotationYFrac = oy;
        checkpoint->rotationZFrac = oz;
        checkpoint->unkC =
            -(((obj->trans.x_position * ox) + (obj->trans.y_position * oy)) + (obj->trans.z_position * oz));
        checkpoint->x = obj->trans.x_position;
        checkpoint->y = obj->trans.y_position;
        checkpoint->z = obj->trans.z_position;
        checkpoint->scale = obj->trans.scale * 2;
        checkpoint->checkpointID = obj->trans.scale * 128.0f;
        checkpoint->altDistance = 0.0f;
        checkpoint->distance = 0.0f;
        if (i < gNumberOfMainCheckpoints) {
            tempCheckpointID = i + 1;
            if (tempCheckpointID == gNumberOfMainCheckpoints) {
                tempCheckpointID = 0;
            }
            xDiff = obj->trans.x_position - gTrackCheckpoints[tempCheckpointID].obj->trans.x_position;
            yDiff = obj->trans.y_position - gTrackCheckpoints[tempCheckpointID].obj->trans.y_position;
            zDiff = obj->trans.z_position - gTrackCheckpoints[tempCheckpointID].obj->trans.z_position;
            checkpoint->distance = sqrtf(((xDiff * xDiff) + (yDiff * yDiff)) + (zDiff * zDiff));
            altRouteId = gTrackCheckpoints[tempCheckpointID].altRouteID;
            if (altRouteId != -1) {
                xDiff = obj->trans.x_position - gTrackCheckpoints[altRouteId].obj->trans.x_position;
                yDiff = obj->trans.y_position - gTrackCheckpoints[altRouteId].obj->trans.y_position;
                zDiff = obj->trans.z_position - gTrackCheckpoints[altRouteId].obj->trans.z_position;
                checkpoint->altDistance = sqrtf(((xDiff * xDiff) + (yDiff * yDiff)) + (zDiff * zDiff));
            } else {
                checkpoint->altDistance = checkpoint->distance;
            }
        } else {
            tempCheckpointID = gTrackCheckpoints[i].altRouteID + 1;
            if (tempCheckpointID == gNumberOfMainCheckpoints) {
                tempCheckpointID = 0;
            }
            xDiff = obj->trans.x_position - gTrackCheckpoints[tempCheckpointID].obj->trans.x_position;
            yDiff = obj->trans.y_position - gTrackCheckpoints[tempCheckpointID].obj->trans.y_position;
            zDiff = obj->trans.z_position - gTrackCheckpoints[tempCheckpointID].obj->trans.z_position;
            checkpoint->distance = sqrtf(((xDiff * xDiff) + (yDiff * yDiff)) + (zDiff * zDiff));
            altRouteId = gTrackCheckpoints[tempCheckpointID].altRouteID;
            if (altRouteId != -1) {
                xDiff = obj->trans.x_position - gTrackCheckpoints[altRouteId].obj->trans.x_position;
                yDiff = obj->trans.y_position - gTrackCheckpoints[altRouteId].obj->trans.y_position;
                zDiff = obj->trans.z_position - gTrackCheckpoints[altRouteId].obj->trans.z_position;
                checkpoint->altDistance = sqrtf(((xDiff * xDiff) + (yDiff * yDiff)) + (zDiff * zDiff));
            } else {
                checkpoint->altDistance = checkpoint->distance;
            }
        }
        checkpoint->unk2E[0] = checkpointEntry->unkB;
        checkpoint->unk32[0] = checkpointEntry->unkF;
        checkpoint->unk36[0] = checkpointEntry->unk13;
        checkpoint->unk2E[1] = checkpointEntry->unkC;
        checkpoint->unk32[1] = checkpointEntry->unk10;
        checkpoint->unk36[1] = checkpointEntry->unk14;
        checkpoint->unk2E[2] = checkpointEntry->unkD;
        checkpoint->unk32[2] = checkpointEntry->unk11;
        checkpoint->unk36[2] = checkpointEntry->unk15;
        checkpoint->unk2E[3] = checkpointEntry->unkE;
        checkpoint->unk32[3] = checkpointEntry->unk12;
        checkpoint->unk36[3] = checkpointEntry->unk16;
        checkpoint->unk3B = checkpointEntry->unk19;
    }
}

/**
 * Checks to see whether a racer or homing missile passed a checkpoint.
 * Return values:
 * - 0: Homing missile passed the checkpoint.
 * - -100: Racer passed the checkpoint.
 * - Otherwise: No checkpoint has been passed.
 */
s32 checkpoint_is_passed(s32 checkpointIndex, Object *obj, f32 objX, f32 objY, f32 objZ, f32 *checkpointDistance,
                         u8 *isOnAlternateRoute) {
    s32 retLength;
    s32 isAltCheckpoint;
    f32 distX, distY, distZ;
    f32 length;
    f32 nextLength;
    f32 toNext;
    Object_Racer *racer;
    f32 fromPrev;
    CheckpointNode *currentCheckpoint;
    CheckpointNode *prevCheckpoint;
    CheckpointNode *altRouteCheckpoint;

    if (gNumberOfMainCheckpoints == 0) {
        return 1;
    }

    currentCheckpoint = &gTrackCheckpoints[checkpointIndex];
    if (checkpointIndex != 0) {
        prevCheckpoint = &gTrackCheckpoints[checkpointIndex - 1];
    } else {
        prevCheckpoint = &gTrackCheckpoints[gNumberOfMainCheckpoints - 1];
    }

    if (*isOnAlternateRoute) {
        if (currentCheckpoint->altRouteID != -1) {
            currentCheckpoint = &gTrackCheckpoints[currentCheckpoint->altRouteID];
        }
        if (prevCheckpoint->altRouteID != -1) {
            prevCheckpoint = &gTrackCheckpoints[prevCheckpoint->altRouteID];
        }
    }

    isAltCheckpoint = FALSE;
    if (!(*isOnAlternateRoute) && prevCheckpoint->altRouteID == -1 && currentCheckpoint->altRouteID != -1) {
        altRouteCheckpoint = &gTrackCheckpoints[currentCheckpoint->altRouteID];
        distX = altRouteCheckpoint->x - obj->trans.x_position;
        distY = altRouteCheckpoint->y - obj->trans.y_position;
        distZ = altRouteCheckpoint->z - obj->trans.z_position;
        if (sqrtf(distX * distX + distY * distY + distZ * distZ) < altRouteCheckpoint->checkpointID) {
            currentCheckpoint = altRouteCheckpoint;
            isAltCheckpoint = TRUE;
        }
    }

    distX = currentCheckpoint->x - prevCheckpoint->x;
    distY = currentCheckpoint->y - prevCheckpoint->y;
    distZ = currentCheckpoint->z - prevCheckpoint->z;
    length = sqrtf(distX * distX + distY * distY + distZ * distZ);
    if (length > 0.0) {
        distX *= 1.0f / length;
        distY *= 1.0f / length;
        distZ *= 1.0f / length;
    }

    toNext = currentCheckpoint->rotationXFrac * obj->trans.x_position +
             currentCheckpoint->rotationYFrac * obj->trans.y_position +
             currentCheckpoint->rotationZFrac * obj->trans.z_position + currentCheckpoint->unkC;

    nextLength = currentCheckpoint->rotationXFrac * distX + currentCheckpoint->rotationYFrac * distY +
                 currentCheckpoint->rotationZFrac * distZ;
    nextLength = -toNext / nextLength;

    fromPrev = prevCheckpoint->rotationXFrac * obj->trans.x_position +
               prevCheckpoint->rotationYFrac * obj->trans.y_position +
               prevCheckpoint->rotationZFrac * obj->trans.z_position + prevCheckpoint->unkC;

    length = prevCheckpoint->rotationXFrac * distX + prevCheckpoint->rotationYFrac * distY +
             prevCheckpoint->rotationZFrac * distZ;
    length = fromPrev / length;

    if (nextLength + length != 0.0) {
        length = nextLength / (nextLength + length);
    } else {
        length = 0.0f;
    }
    *checkpointDistance = length;

    if (obj->behaviorId == BHV_RACER) {
        racer = obj->racer;
        if (racer->playerIndex == PLAYER_COMPUTER) {
            if (length < -0.3) {
                return -100;
            }
            if (length > 1.3) {
                return -100;
            }
        }
    }

    if (nextLength <= 0) {
        if (isAltCheckpoint) {
            *isOnAlternateRoute = TRUE;
        } else if (currentCheckpoint->altRouteID == -1) {
            *isOnAlternateRoute = FALSE;
        }

        distY = currentCheckpoint->rotationXFrac * objX + currentCheckpoint->rotationYFrac * objY +
                currentCheckpoint->rotationZFrac * objZ + currentCheckpoint->unkC;
        if (distY > 0) {
            if (obj->behaviorId == BHV_RACER) {
                Object_Racer *objRacer = obj->racer;
                if (currentCheckpoint->unk3B != 0) {
                    objRacer->indicator_type = currentCheckpoint->unk3B;
                    objRacer->indicator_timer = 120;
                }
            }

            prevCheckpoint = currentCheckpoint;
            checkpointIndex++;
            if (checkpointIndex == gNumberOfMainCheckpoints) {
                checkpointIndex = 0;
            }

            currentCheckpoint = &gTrackCheckpoints[checkpointIndex];

            toNext = currentCheckpoint->rotationXFrac * obj->trans.x_position +
                     currentCheckpoint->rotationYFrac * obj->trans.y_position +
                     currentCheckpoint->rotationZFrac * obj->trans.z_position + currentCheckpoint->unkC;
            nextLength = currentCheckpoint->rotationXFrac * distX + currentCheckpoint->rotationYFrac * distY +
                         currentCheckpoint->rotationZFrac * distZ;
            nextLength = -toNext / nextLength;

            fromPrev = prevCheckpoint->rotationXFrac * obj->trans.x_position +
                       prevCheckpoint->rotationYFrac * obj->trans.y_position +
                       prevCheckpoint->rotationZFrac * obj->trans.z_position + prevCheckpoint->unkC;
            length = prevCheckpoint->rotationXFrac * distX + prevCheckpoint->rotationYFrac * distY +
                     prevCheckpoint->rotationZFrac * distZ;
            length = fromPrev / length;

            if (nextLength + length != 0.0) {
                length = nextLength / (nextLength + length);
            } else {
                length = 0.0;
            }
            *checkpointDistance = length;
        } else {
            *checkpointDistance = 0.0;
        }
        return 0;
    } else {
        retLength = length * 100.0f;
        if (retLength == 0) {
            retLength++;
        }
        return retLength;
    }
}

/**
 * Search and return Taj's overworld object.
 * Used for drawing his minimap position.
 */
Object *find_taj_object(void) {
    s32 i;
    Object *current_obj;
    for (i = gObjectListStart; i < gObjectCount; i++) {
        current_obj = gObjPtrList[i];
        if (!(current_obj->trans.flags & OBJ_FLAGS_PARTICLE) && (current_obj->behaviorId == BHV_PARK_WARDEN)) {
            return current_obj;
        }
    }
    return NULL;
}

// Handles MidiFadePoint, MidiFade, and MidiSetChannel objects?
void func_80018CE0(Object *racerObj, f32 xPos, f32 yPos, f32 zPos, s32 updateRate) {
    f32 temp_f0;
    s32 pad_spF8;
    s32 spF4;
    s32 pad_spF0;
    f32 temp_f22;
    s32 pad_spE8;
    s32 pad_spE4;
    s32 pad_spE0;
    f32 temp_f2;
    s32 pad_spD8;
    f32 tempF2;
    s32 temp_f10;
    s32 temp_t3_2;
    s32 i; // s1
    s32 var_s2;
    f32 spC0;
    Object_MidiFadePoint *midiFadePoint; // spBC
    f32 tempF;
    u16 temp_t4;
    s8 var_v0_2;
    u8 var_v0_u;
    s8 var_v1;
    Object *obj;
    Object_MidiFade *midiFade;
    Object_MidiChannelSet *midiChannelSet;
    Object_Racer *racer;

    racer = racerObj->racer;
    if (racer->playerIndex != 0) {
        return;
    }

    if (cam_get_viewport_layout() != 0) {
        return;
    }

    for (spF4 = gObjectListStart; spF4 < gObjectCount; spF4++) {
        obj = gObjPtrList[spF4];
        if (!(obj->trans.flags & 0x8000)) {
            if (obj->behaviorId == BHV_MIDI_FADE_POINT) {
                spC0 = sqrtf(((racerObj->trans.x_position - obj->trans.x_position) *
                              (racerObj->trans.x_position - obj->trans.x_position)) +
                             ((racerObj->trans.y_position - obj->trans.y_position) *
                              (racerObj->trans.y_position - obj->trans.y_position)) +
                             ((racerObj->trans.z_position - obj->trans.z_position) *
                              (racerObj->trans.z_position - obj->trans.z_position)));
                midiFadePoint = obj->midi_fade_point;
                if (spC0 < midiFadePoint->unk2) {
                    if (midiFadePoint->unk1C == music_current_sequence()) {
                        if (spC0 <= midiFadePoint->unk0) {
                            var_s2 = 0;
                        } else {
                            spC0 -= midiFadePoint->unk0;
                            temp_f0 = (midiFadePoint->unk2 - midiFadePoint->unk0);
                            var_s2 = (127.0f * spC0) / temp_f0;
                        }
                        for (i = 0; i < 16; i++) {
                            switch (midiFadePoint->unkC[i]) {
                                case 1:
                                    if (var_s2 >= 0x7B) {
                                        music_channel_off(i);
                                    } else {
                                        music_channel_fade_set(i, (0x7F - var_s2));
                                        music_channel_on(i);
                                    }
                                    break;
                                case 2:
                                    if ((music_channel_fade(i) > 0) && (music_channel_active(i) == 0)) {
                                        music_channel_fade_set(i, var_s2);
                                    }
                                    break;
                                default:
                                    stubbed_printf("ERROR Channel %d\n", i);
                                    break;
                            }
                        }
                    }
                }
            } else if (obj->behaviorId == BHV_MIDI_FADE) {
                midiFade = obj->midi_fade;

                temp_f0 =
                    (midiFade->unk8 * xPos) + (midiFade->unkC * yPos) + (midiFade->unk10 * zPos) + midiFade->unk14;
                temp_f2 = (midiFade->unk8 * racerObj->trans.x_position) +
                          (midiFade->unkC * racerObj->trans.y_position) +
                          (midiFade->unk10 * racerObj->trans.z_position) + midiFade->unk14;
                if (temp_f0 > 0.0f && temp_f2 <= 0.0f) {
                    var_v1 = 1;
                } else if (temp_f2 > 0.0f && temp_f0 <= 0.0f) {
                    var_v1 = -1;
                } else {
                    var_v1 = 0;
                }
                if (var_v1 != 0) {
                    temp_f0 = racerObj->trans.x_position - xPos;
                    temp_f2 = racerObj->trans.y_position - yPos;

                    temp_f22 =
                        (-midiFade->unk8 * xPos - midiFade->unkC * yPos - midiFade->unk10 * zPos - midiFade->unk14) /
                        (midiFade->unk8 * temp_f0 + midiFade->unkC * temp_f2 +
                         midiFade->unk10 * (racerObj->trans.z_position - zPos));
                    tempF = temp_f22 * temp_f0;
                    if ((midiFade->unk18 <= tempF + xPos) && (tempF + xPos <= midiFade->unk24)) {
                        tempF2 = racerObj->trans.z_position - zPos;
                        if ((midiFade->unk1C <= (temp_f22 * temp_f2) + yPos) &&
                            ((temp_f22 * temp_f2) + yPos <= midiFade->unk28)) {
                            if ((midiFade->unk20 <= (temp_f22 * (tempF2)) + zPos) &&
                                ((temp_f22 * (tempF2)) + zPos <= midiFade->unk2C)) {
                                midiFade->unk0 = var_v1;
                                midiFade->unk1 = 0;
                                midiFade->unk4 = 0;
                                D_8011AF60 = midiFade;
                            }
                        }
                    }
                }
            } else if (obj->behaviorId == BHV_MIDI_CHANNEL_SET) {
                midiChannelSet = obj->midi_channel_set;
                temp_f0 = sqrtf(((racerObj->trans.x_position - obj->trans.x_position) *
                                 (racerObj->trans.x_position - obj->trans.x_position)) +
                                ((racerObj->trans.y_position - obj->trans.y_position) *
                                 (racerObj->trans.y_position - obj->trans.y_position)) +
                                ((racerObj->trans.z_position - obj->trans.z_position) *
                                 (racerObj->trans.z_position - obj->trans.z_position)));
                var_v0_u = midiChannelSet->unk2;
                if ((temp_f0 < (var_v0_u * 4)) && (midiChannelSet->unk0 != music_channel_get_mask()) &&
                    (midiChannelSet->unk3 == music_current_sequence())) {
                    music_dynamic_set(midiChannelSet->unk0);
                }
            }
        }
    }

    if (D_8011AF60 == 0) {
        return;
    }

    if (D_8011AF60->unk40 == music_current_sequence()) {
        D_8011AF60->unk4 += updateRate;
        temp_t4 = D_8011AF60->unk2 * gVideoRefreshRate;
        if (temp_t4 < D_8011AF60->unk4) {
            D_8011AF60->unk4 = temp_t4;
        }
        temp_f10 = (D_8011AF60->unk4 * 254.0f) / temp_t4;
        if (temp_f10 < 0xFE) {
            D_8011AF60->unk1 = temp_f10;
        } else {
            D_8011AF60->unk1 = 0xFE;
        }
        for (i = 0; i < 16; i++) {
            var_v0_2 = D_8011AF60->unk2F[i];
            if (D_8011AF60->unk0 == -1) {
                var_v0_2 >>= 2;
            }
            var_v0_2 &= 3;
            switch (var_v0_2) {
                case 1:
                    music_channel_on(i);
                    music_channel_fade_set(i, 0x7FU);
                    break;
                case 0:
                    music_channel_off(i);
                    break;
                case 3:
                    if (D_8011AF60->unk1 >= 0x80) {
                        temp_t3_2 = (D_8011AF60->unk1 - 0x7F) & 0xFF;
                        music_channel_on(i);
                        if (music_channel_fade(i) < temp_t3_2) {
                            music_channel_fade_set(i, temp_t3_2);
                        }
                    }
                    break;
                case 2:
                    if (D_8011AF60->unk1 < 0x7F) {
                        temp_t3_2 = (0x7F - D_8011AF60->unk1) & 0xFF;
                        if (temp_t3_2 < music_channel_fade(i)) {
                            music_channel_fade_set(i, temp_t3_2);
                        }
                    } else {
                        music_channel_off(i);
                    }
                    break;
                default:
                    break;
            }
        }
    }
    if ((D_8011AF60->unk1 == 0xFE) && (D_8011AF60->unk40 == music_current_sequence())) {
        D_8011AF60 = 0;
    }
}

/**
 * Calculates the direction a homing rocket takes toward the next checkpoint.
 * Returns true if a direction was calculated, or false if there are no checkpoints.
 */
s32 homing_rocket_get_next_direction(Object *obj, s32 checkpoint, u8 isOnAlternateRoute, s32 arg3, s32 arg4,
                                     f32 checkpointDist, f32 *outX, f32 *outY, f32 *outZ) {
    s32 numCheckpoints;
    s32 checkpointIndex;
    s32 i;
    f32 xData[4];
    f32 yData[4];
    f32 zData[4];
    f32 xSpline;
    f32 temp2;
    f32 zSpline;
    f32 ySpline;
    f32 dx;
    f32 dy;
    f32 dz;
    CheckpointNode *checkpointNode;

    numCheckpoints = gNumberOfMainCheckpoints;
    if (numCheckpoints == 0) {
        return FALSE;
    }
    checkpointIndex = checkpoint - 2;
    if (checkpointIndex < 0) {
        checkpointIndex += numCheckpoints;
    }
    for (i = 0; i < 4; i++) {
        checkpointNode = find_next_checkpoint_node(checkpointIndex, isOnAlternateRoute);
        xData[i] = checkpointNode->x + (checkpointNode->scale * checkpointNode->rotationZFrac * arg3);
        yData[i] = checkpointNode->y + (checkpointNode->scale * arg4);
        zData[i] = checkpointNode->z + (checkpointNode->scale * -checkpointNode->rotationXFrac * arg3);
        checkpointIndex += 1;
        if (checkpointIndex == numCheckpoints) {
            checkpointIndex = 0;
        }
    }
    temp2 = (1.0 - checkpointDist);
    if (temp2 < 0.0f) {
        temp2 = 0.0f;
    }
    if (temp2 > 1.0) {
        temp2 = 1.0f;
    }
    xSpline = cubic_spline_interpolation(xData, 0, temp2, &dx);
    ySpline = cubic_spline_interpolation(yData, 0, temp2, &dy);
    zSpline = cubic_spline_interpolation(zData, 0, temp2, &dz);
    temp2 = sqrtf((dx * dx) + (dy * dy) + (dz * dz));
    if (temp2 != 0.0f) {
        temp2 = 500.0 / temp2;
        dx *= temp2;
        dy *= temp2;
        dz *= temp2;
    }
    *outX = (xSpline + dx) - obj->trans.x_position;
    *outY = (ySpline + dy) - obj->trans.y_position;
    *outZ = (zSpline + dz) - obj->trans.z_position;
    return TRUE;
}

// D_B0000574 is a direct read from the ROM as opposed to RAM
extern s32 D_B0000574;

/**
 * Check the win conditions of the current race.
 * This varies based on the race type, so this does a multitude of different possible things.
 * When the race is finished, it will then for the most part trigger the next menu,
 * but adventure mode will start the balloon cutscene if it has not yet been awarded.
 */
#ifdef NATIVE_PORT
static s32 sMdkrChallengeProbeCourse = -1;
static s32 sMdkrChallengeProbeTick;

static void mdkr_challenge_snapshot(
    MdkrChallengeProbe *probe, s8 raceType) {
    Settings *settings;
    s32 i;

    memset(probe, 0, sizeof(*probe));
    settings = get_settings();
    probe->course = settings != NULL ? settings->courseId : -1;
    probe->race_type = raceType;
    probe->tracks_mode = is_in_tracks_mode();
    probe->racer_count = gNumRacers;
    probe->tick = sMdkrChallengeProbeTick;
    if (settings != NULL) {
        probe->tt_amulet = settings->ttAmulet;
        if (settings->courseFlagsPtr != NULL && settings->courseId >= 0) {
            probe->course_flags =
                (u32) settings->courseFlagsPtr[settings->courseId];
        }
    }
    for (i = 0; i < gNumRacers && i < 4; i++) {
        Object *obj = (*gRacers)[i];
        Object_Racer *racer;
        MdkrChallengeRacerProbe *out;

        if (obj == NULL || obj->racer == NULL) {
            continue;
        }
        racer = obj->racer;
        out = &probe->racers[i];
        out->player_index = racer->playerIndex;
        out->racer_index = racer->racerIndex;
        out->x = obj->trans.x_position;
        out->y = obj->trans.y_position;
        out->z = obj->trans.z_position;
        out->score = racer->lap;
        out->health = racer->bananas;
        out->egg_count = racer->eggHudCounter;
        out->finished = racer->raceFinished;
        out->finish_position = racer->finishPosition;
    }
}

static Object_Racer *mdkr_challenge_human(void) {
    s32 i;

    for (i = 0; i < gNumRacers; i++) {
        Object_Racer *racer = (*gRacers)[i]->racer;
        if (racer != NULL && racer->playerIndex == PLAYER_ONE) {
            return racer;
        }
    }
    return NULL;
}

static Object_Racer *mdkr_challenge_ai(s32 ordinal) {
    s32 i;

    for (i = 0; i < gNumRacers; i++) {
        Object_Racer *racer = (*gRacers)[i]->racer;
        if (racer != NULL && racer->playerIndex == PLAYER_COMPUTER) {
            if (ordinal == 0) {
                return racer;
            }
            ordinal--;
        }
    }
    return NULL;
}

/*
 * Deliver one terminal gameplay event to the selected racer. This function
 * deliberately cannot assign a result: eggs and treasure call the same named
 * production predicates as their object loops; battle only removes one health
 * point and leaves elimination to race_check_finish() below.
 */
static void mdkr_challenge_drive_terminal_event(s8 raceType) {
    MdkrChallengeProbe probe;
    Object_Racer *target;
    s32 eventOrdinal;
    s32 outcome;
    s32 ready;

    outcome = mdkr_challenge_test_outcome();
    if (outcome == 0) {
        return;
    }
    if (sMdkrChallengeProbeCourse != get_settings()->courseId) {
        sMdkrChallengeProbeCourse = get_settings()->courseId;
        sMdkrChallengeProbeTick = 0;
        mdkr_challenge_snapshot(&probe, raceType);
        mdkr_challenge_probe_state("load", &probe);
    }
    ready = get_race_countdown() == 0 && get_race_start_timer() == 0;
    if (ready && !gRaceFinishTriggered) {
        sMdkrChallengeProbeTick++;
        if (sMdkrChallengeProbeTick == 1 ||
            (sMdkrChallengeProbeTick % 30) == 0) {
            mdkr_challenge_snapshot(&probe, raceType);
            mdkr_challenge_probe_state(
                sMdkrChallengeProbeTick == 1 ? "ready" : "state", &probe);
        }
    }
    if (!mdkr_challenge_test_event(
            raceType, ready, gRaceFinishTriggered, &eventOrdinal)) {
        return;
    }

    if (raceType == RACETYPE_CHALLENGE_BATTLE && outcome > 0) {
        target = mdkr_challenge_ai(eventOrdinal / 8);
    } else if (outcome > 0) {
        target = mdkr_challenge_human();
    } else {
        target = raceType == RACETYPE_CHALLENGE_BATTLE
                     ? mdkr_challenge_human()
                     : mdkr_challenge_ai(0);
    }
    if (target == NULL) {
        return;
    }
    if (raceType == RACETYPE_CHALLENGE_EGGS) {
        target->lap++;
        racer_challenge_egg_finish_if_complete(target);
    } else if (raceType == RACETYPE_CHALLENGE_BANANAS) {
        racer_challenge_banana_deposit(target);
    } else if (raceType == RACETYPE_CHALLENGE_BATTLE &&
               target->bananas > 0) {
        target->bananas--;
    }

    mdkr_challenge_snapshot(&probe, raceType);
    mdkr_challenge_probe_event(&probe, outcome, eventOrdinal);
    mdkr_challenge_probe_state("event", &probe);
}

static void mdkr_challenge_trace_phase(const char *phase, s8 raceType) {
    MdkrChallengeProbe probe;

    if (mdkr_challenge_test_outcome() == 0) {
        return;
    }
    mdkr_challenge_snapshot(&probe, raceType);
    mdkr_challenge_probe_state(phase, &probe);
}

static void mdkr_taj_trace_phase(
    const char *phase, s32 vehicle, const s8 *thresholds,
    s32 reason, s32 menu, s32 tick) {
    MdkrTajProbe probe = { 0 };
    Settings *settings;
    Object *humanObj;
    Object *aiObj;

    if (!mdkr_taj_probe_enabled()) {
        return;
    }
    settings = get_settings();
    probe.vehicle = vehicle;
    probe.flags = settings != NULL ? settings->tajFlags : 0;
    probe.balloons =
        settings != NULL && settings->balloonsPtr != NULL
            ? settings->balloonsPtr[0]
            : 0;
    if (thresholds != NULL) {
        probe.thresholds[0] = thresholds[0];
        probe.thresholds[1] = thresholds[1];
        probe.thresholds[2] = thresholds[2];
    }
    probe.racer_count = gNumRacers;
    probe.tick = tick;
    probe.reason = reason;
    probe.menu = menu;
    probe.completion_gate =
        vehicle >= VEHICLE_CAR && vehicle <= VEHICLE_PLANE
            ? mdkr_taj_completion_gate_enabled(vehicle)
            : 1;
    humanObj =
        gRacersByPort != NULL ? gRacersByPort[PLAYER_ONE] : NULL;
    if (humanObj != NULL && humanObj->racer != NULL) {
        Object_Racer *human = humanObj->racer;
        if (vehicle < VEHICLE_CAR || vehicle > VEHICLE_PLANE) {
            probe.vehicle = human->vehicleID;
        }
        probe.human_lap = human->lap;
        probe.human_finished = human->raceFinished;
        probe.human_place = human->finishPosition;
        probe.human_x = humanObj->trans.x_position;
        probe.human_y = humanObj->trans.y_position;
        probe.human_z = humanObj->trans.z_position;
    }
    aiObj =
        gNumRacers > 1 && gRacers != NULL ? (*gRacers)[1] : NULL;
    if (aiObj != NULL && aiObj->racer != NULL) {
        Object_Racer *ai = aiObj->racer;
        probe.ai_lap = ai->lap;
        probe.ai_finished = ai->raceFinished;
        probe.ai_place = ai->finishPosition;
        probe.ai_x = aiObj->trans.x_position;
        probe.ai_y = aiObj->trans.y_position;
        probe.ai_z = aiObj->trans.z_position;
    }
    mdkr_taj_probe_state(phase, &probe);
}
#endif

void race_check_finish(s32 updateRate) {
    s32 prevRacerPos;
    s32 i;
    s32 j;
    s32 racerPos;
    Settings *settings;
    s16 numHumanRacers;
    s16 numHumanRacersFinished;
    Object_Racer *curRacer2;
    Object_Racer *curRacer;
    s16 numFinishedRacers;
    s16 foundIndex;
    Object_Racer *racer[4];
    s16 racerIndex;
    /* Only the anti-tamper branch may raise this flag. Retail/non-anti-tamper
     * builds must carry the normal false value into flags[2]. */
    s8 raceType = FALSE;
    s8 someBool;
    LevelHeader *currentLevelHeader;
    s32 newStartingPosition;
    s8 sp5C[4];
    s8 someBool2;
    s8 flags[3];
    uintptr_t camera; /* LP64: must hold a full 64-bit Camera* (see ->mode write below) */

    currentLevelHeader = level_header();
    settings = get_settings();
#ifdef NATIVE_PORT
    if (gIsTajChallenge && gNumRacers >= 2 && gRacers != NULL &&
        gRacersByPort != NULL &&
        gRacersByPort[PLAYER_ONE] != NULL &&
        gRacersByPort[PLAYER_ONE]->racer != NULL &&
        (*gRacers)[1] != NULL && (*gRacers)[1]->racer != NULL) {
        Object_Racer *tajHuman = gRacersByPort[PLAYER_ONE]->racer;
        Object_Racer *tajAi = (*gRacers)[1]->racer;
        s32 tajTick = 0;
        s32 tajAction = mdkr_taj_test_action(
            TRUE, tajHuman->lap, tajHuman->raceFinished,
            tajAi->lap, tajAi->raceFinished, &tajTick);

        if (mdkr_taj_should_probe(tajTick)) {
            mdkr_taj_trace_phase(
                "race", tajHuman->vehicleID, NULL, -1, 0, tajTick);
        }
        if (tajAction == MDKR_TAJ_ACTION_ABORT) {
            mdkr_taj_trace_phase(
                "event_abort", tajHuman->vehicleID, NULL,
                CHALLENGE_END_QUIT, 0, tajTick);
            mode_end_taj_race(CHALLENGE_END_QUIT);
            return;
        }
        if (tajAction == MDKR_TAJ_ACTION_AI_FINISH) {
            /*
             * One final-lap progress event. Production below remains solely
             * responsible for raceFinished, place assignment, human loss,
             * dialogue choice, flags, and save behavior.
             */
            tajAi->lap = currentLevelHeader->laps;
            mdkr_taj_trace_phase(
                "event_ai_finish", tajHuman->vehicleID, NULL,
                CHALLENGE_END_FINISH, 4, tajTick);
        }
        if (tajAction == MDKR_TAJ_ACTION_AI_HOLD ||
            tajAction == MDKR_TAJ_ACTION_AI_HOLD_FIRST) {
            /*
             * Reject only the carpet's final-lap event. The human still has to
             * drive all three laps; production then detects that finish and
             * assigns first place without a verdict/position write here.
             */
            tajAi->lap = currentLevelHeader->laps - 1;
            if (tajAction == MDKR_TAJ_ACTION_AI_HOLD_FIRST) {
                mdkr_taj_trace_phase(
                    "event_ai_hold", tajHuman->vehicleID, NULL,
                    CHALLENGE_END_FINISH, 0, tajTick);
            }
        }
    }
#endif
    numHumanRacersFinished = 0;
    numHumanRacers = 0;
    someBool2 = currentLevelHeader->race_type;
    numFinishedRacers = 0;
    if (someBool2 != RACETYPE_DEFAULT && someBool2 != RACETYPE_HORSESHOE_GULCH && someBool2 != RACETYPE_BOSS) {
        if (someBool2 & RACETYPE_CHALLENGE) {
            if (someBool2 == RACETYPE_CHALLENGE_EGGS) {
                racer_update_eggs(*gRacers);
            }
#ifdef NATIVE_PORT
            mdkr_challenge_drive_terminal_event(someBool2);
#endif
            if (gRaceFinishTriggered == FALSE) {
                for (i = 0; i < gNumRacers; i++) {
                    racer[i] = (*gRacers)[i]->racer;
                    // Manage eliminated racers in deathmatch.
                    if (currentLevelHeader->race_type == RACETYPE_CHALLENGE_BATTLE && racer[i]->bananas <= 0 &&
#ifdef NATIVE_PORT
                        mdkr_challenge_terminal_gate_enabled(RACETYPE_CHALLENGE_BATTLE) &&
#endif
                        !racer[i]->raceFinished) {
                        racer[i]->raceFinished = TRUE;
                        racer[i]->balloon_quantity = 0;
                        racer_sound_free((*gRacers)[i]);
                        (*gRacers)[i]->trans.flags |= OBJ_FLAGS_INVISIBLE;
                        (*gRacers)[i]->interactObj->flags = INTERACT_FLAGS_NONE;
                        racer[i]->finishPosition = 5 - gNumFinishedRacers;
                        gNumFinishedRacers++;
                    }
                    if (racer[i]->playerIndex != PLAYER_COMPUTER) {
                        if (racer[i]->raceFinished) {
                            numHumanRacersFinished++;
                        }
                        numHumanRacers++;
                    }
                    if (racer[i]->raceFinished) {
                        numFinishedRacers++;
                        if (racer[i]->finishPosition == 0) {
                            racer[i]->finishPosition = gNumFinishedRacers;
                            gNumFinishedRacers++;
                        }
                    }
                }
                if ((currentLevelHeader->race_type != RACETYPE_CHALLENGE_BATTLE && numFinishedRacers > 0) ||
                    ((((numHumanRacers == 1 && numHumanRacersFinished == 1) ||
                       (numHumanRacers >= 2 && numHumanRacersFinished >= numHumanRacers)) ||
                      numFinishedRacers >= 3))) {
                    for (i = 0; i < gNumRacers; i++) {
                        if (currentLevelHeader->race_type == RACETYPE_CHALLENGE_BATTLE) {
                            sp5C[i] = 10 - racer[i]->bananas;
                        } else {
                            sp5C[i] = racer[i]->lap;
                            if (currentLevelHeader->race_type == RACETYPE_CHALLENGE_EGGS) {
                                sp5C[i] *= 3;
                                if (racer[i]->eggHudCounter != 0) {
                                    sp5C[i] += 2;
                                } else if (racer[i]->held_obj != NULL) {
                                    sp5C[i] += 1;
                                }
                            }
                        }
                        if (sp5C[i] > 10) {
                            sp5C[i] = 10;
                        }
                        if (sp5C[i] < 0) {
                            sp5C[i] = 0;
                        }
                    }

                    i = 0;
                    do {
                        racerIndex = -1;
                        foundIndex = -1;
                        for (i = 0; i < gNumRacers; i++) {
                            if (!racer[i]->raceFinished && sp5C[i] >= foundIndex) {
                                foundIndex = sp5C[i];
                                racerIndex = i;
                            }
                        }

                        if (racerIndex != -1) {
                            // In battle mode, last to finish wins, so flip the finish order.
                            if (currentLevelHeader->race_type == RACETYPE_CHALLENGE_BATTLE) {
                                racer[racerIndex]->finishPosition = 5 - gNumFinishedRacers;
                            } else {
                                racer[racerIndex]->finishPosition = gNumFinishedRacers;
                            }
                            gNumFinishedRacers++;
                            racer[racerIndex]->raceFinished = TRUE;
                        }
                        i = 0;
                    } while (racerIndex != -1);

                    gSwapLeadPlayer = FALSE;
                    // Award the winner a TT amulet if not in tracks mode.
                    if (!is_in_tracks_mode() &&
                        (racer[0]->finishPosition == 1 ||
                         (is_in_two_player_adventure() && racer[1]->finishPosition == 1)) &&
                        (!(settings->courseFlagsPtr[settings->courseId] & RACE_CLEARED))) {
                        settings->courseFlagsPtr[settings->courseId] |= RACE_CLEARED;
                        i = settings->ttAmulet + 1;
                        if (i > 4) {
                            i = 4;
                        }
                        settings->ttAmulet = i;
                    }
                    for (racerPos = 0; racerPos < 8;) {
                        settings->racers[racerPos++].starting_position = -1;
                    }

                    newStartingPosition = SEQUENCE_BATTLE_LOSE;
                    for (racerPos = 0; racerPos < gNumRacers; racerPos++) {
                        if (racer[racerPos]->playerIndex != PLAYER_COMPUTER && racer[racerPos]->finishPosition == 1) {
                            newStartingPosition = SEQUENCE_BATTLE_VICTORY;
                        }
                        settings->racers[racerPos].starting_position = racer[racerPos]->finishPosition - 1;
                    }

                    music_play(newStartingPosition);
                    newStartingPosition = 4;
                    for (prevRacerPos = 0; prevRacerPos < 8; prevRacerPos++) {
                        if (settings->racers[prevRacerPos].starting_position == -1) {
                            settings->racers[prevRacerPos].starting_position = newStartingPosition;
                            newStartingPosition++;
                        }
                    }

                    gSwapLeadPlayer = FALSE;
                    if (is_in_two_player_adventure() && settings->racers[PLAYER_TWO].starting_position <
                                                            settings->racers[PLAYER_ONE].starting_position) {
                        gSwapLeadPlayer = TRUE;
                    }
                    // i will be nonzero if any adventure mode award triggers happened.
                    if (i == 0) {
                        if (is_in_two_player_adventure()) {
                            if (gSwapLeadPlayer) {
                                gSwapLeadPlayer = FALSE;
                                swap_lead_player();
                                if (gLeadPlayerIndex != 0) {
                                    gIsP2LeadPlayer = TRUE;
                                }
                            } else if (gLeadPlayerIndex != 0) {
                                gIsP2LeadPlayer = TRUE;
                            }
                        }
                        postrace_start(0, 30);
#ifdef NATIVE_PORT
                        mdkr_challenge_trace_phase("postrace", someBool2);
#endif
                    } else {
                        level_properties_push(SPECIAL_MAP_ID_NO_LEVEL, 0, VEHICLE_CAR, CUTSCENE_ID_NONE);
                        level_properties_push(ASSET_LEVEL_TTAMULETSEQUENCE, 0, VEHICLE_NO_OVERRIDE,
                                              settings->ttAmulet - 1);
                        race_finish_adventure(TRUE);
#ifdef NATIVE_PORT
                        mdkr_challenge_trace_phase("adventure", someBool2);
#endif
                    }
                    gRaceFinishTriggered = TRUE;
#ifdef NATIVE_PORT
                    mdkr_challenge_trace_phase("verdict", someBool2);
#endif
                }
            }
        }
#ifdef NATIVE_PORT
        /* Challenge races return above the default-race assignment path. Keep
         * their trace coverage, but use the same stable controller mapping. */
        if (gNumRacers > 0 && gRacersByPort != NULL &&
            gRacersByPort[PLAYER_ONE] != NULL) {
            extern void mdkr_pace_probe_finish(int, int, int, int, int);
            Object_Racer *humanRacer = gRacersByPort[PLAYER_ONE]->racer;
            mdkr_pace_probe_finish(
                humanRacer->lap, humanRacer->raceFinished,
                humanRacer->finishPosition, humanRacer->racerIndex,
                humanRacer->playerIndex);
        }
#endif
        return;
    }

    i = 0;
    do {
        racerPos = 1;
        curRacer = (*gRacers)[i]->racer;
        prevRacerPos = curRacer->racerOrder;
        j = 0;
        do {
            if (j != i) {
                curRacer2 = (*gRacers)[j]->racer;
                if (curRacer->raceFinished == FALSE && curRacer2->raceFinished != FALSE) {
                    racerPos++;
                } else if (curRacer->courseCheckpoint < curRacer2->courseCheckpoint) {
                    racerPos++;
                } else if (curRacer->courseCheckpoint == curRacer2->courseCheckpoint) {
                    if (curRacer2->unk1A8 < curRacer->unk1A8) {
                        racerPos++;
                    }
                    if (curRacer->unk1A8 == curRacer2->unk1A8 && i < j) {
                        racerPos++;
                    }
                }
            }
            j++;
        } while (j < gNumRacers);

        curRacer->racerOrder = racerPos;
        if (curRacer->lap < currentLevelHeader->laps) {
            if (prevRacerPos == curRacer->racerOrder) {
                if (curRacer->unk1B0 < 2) {
                    if (curRacer->vehicleID != VEHICLE_LOOPDELOOP) {
                        curRacer->unk1B0++;
                    }
                } else if (curRacer->racerOrder != curRacer->racePosition) {
                    curRacer->unk1B2 = 10;
                    curRacer->racePosition = curRacer->racerOrder;
                }
            } else {
                curRacer->unk1B0 = 0;
            }
        }
        i++;
    } while (i < gNumRacers);

#ifdef NATIVE_PORT
    /*
     * Optional trophy-fixture completion control. It advances lap counters only
     * after a real trophy race has run for the configured number of frames;
     * the production block immediately below still assigns finish positions,
     * builds the post-race order and owns every menu/points/award decision.
     */
    mdkr_trophy_complete_race(*gRacers, gNumRacers,
                              currentLevelHeader->laps);
#endif

    i = 0;
    do {
        curRacer = (*gRacers)[i]->racer;
        if (curRacer->lap >= currentLevelHeader->laps && curRacer->raceFinished == FALSE) {
            if (get_game_mode() != GAMEMODE_UNUSED_4) {
                curRacer->raceFinished = TRUE;
                curRacer->finishPosition = gNumFinishedRacers;
                if (gNumFinishedRacers == 1 && curRacer->playerIndex == PLAYER_COMPUTER) {
                    sound_play(SOUND_WHOOSH5, NULL);
                }
                gNumFinishedRacers++;
            }
        }
        if (curRacer->playerIndex != PLAYER_COMPUTER) {
            numHumanRacers++;
            if (curRacer->raceFinished) {
                // clang-format off
                numHumanRacersFinished++;\
                numFinishedRacers++;
                // clang-format on
            }
        } else if (curRacer->raceFinished) {
            numFinishedRacers++;
        }
        i++;
    } while (i < gNumRacers);

#ifdef NATIVE_PORT
    /*
     * Native regression verdict control. It is deliberately after production
     * lap/finish assignment and before the position table + Adventure reward
     * decision. With no MDKR_ADVENTURE_WIN/LOSS environment variable this is
     * a constant-time no-op; see platform/mdkr_adventure.c.
     */
    if (gNumRacers > 0 && gRacersByPort != NULL &&
        gRacersByPort[PLAYER_ONE] != NULL) {
        mdkr_adventure_force_verdict(
            gRacersByPort[PLAYER_ONE], *gRacers, gNumRacers);
    }

    /*
     * Publish the exact post-assignment state consumed below. This cannot live
     * in update_player_racer(), which stops updating a racer once it finishes,
     * or at race_check_finish() entry: a winning Adventure result can be
     * consumed in this same call, leaving no next call in which entry could
     * observe fin=1.
     *
     * gRacers is starting-grid order, not player order; its slot zero can be a
     * CPU. The stable controller mapping plus identity fields prove that tests
     * are following the port-1 human.
     */
    if (gNumRacers > 0 && gRacersByPort != NULL &&
        gRacersByPort[PLAYER_ONE] != NULL) {
        extern void mdkr_pace_probe_finish(int, int, int, int, int);
        Object_Racer *humanRacer = gRacersByPort[PLAYER_ONE]->racer;
        mdkr_pace_probe_finish(
            humanRacer->lap, humanRacer->raceFinished,
            humanRacer->finishPosition, humanRacer->racerIndex,
            humanRacer->playerIndex);
    }
#endif

    i = 0;
    do {
        gRacersByPosition[i] = 0;
        i++;
    } while (i < gNumRacers);

    i = 0;
    do {
        curRacer = (*gRacers)[i]->racer;
        if (curRacer->raceFinished) {
            racerPos = curRacer->finishPosition - 1;
        } else {
            racerPos = curRacer->racerOrder - 1;
        }
        gRacersByPosition[racerPos] = (*gRacers)[i];
        i++;
    } while (i < gNumRacers);

    i = 0;
    j = 0;
    do {
        // @fake
        if (1) {}

        someBool = FALSE;

        for (; j < gNumRacers; j++) {
            if (gRacersByPosition[j] == (*gRacers)[i]) {
                someBool = TRUE;
                if (!curRacer) {}
                j = gNumRacers;
            }
        }

        j = 0;
        if (someBool == FALSE) {
            for (; j < gNumRacers; j++) {
                if (gRacersByPosition[j] == 0) {
                    gRacersByPosition[j] = (*gRacers)[i];
                    j = gNumRacers;
                }
            }
        }
        j = 0;
        i++;
    } while (i < gNumRacers);

    j = 0;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        // @fake
        if (1) {}
        if (1) {}
        if (1) {}
        if (1) {}
        j |= input_pressed(i);
    }

    if (gIsTajChallenge && numHumanRacersFinished != 0) {
        mode_end_taj_race(CHALLENGE_END_FINISH);
    } else if (D_8011AD3C != 0 && numFinishedRacers != 0) {
        curRacer = (*gRacers)[0]->racer;
        if (!curRacer->raceFinished) {
            curRacer->raceFinished = TRUE;
            curRacer->finishPosition = gNumFinishedRacers;
            gNumFinishedRacers++;
        }
    } else if (gRaceFinishTriggered == FALSE) {
        someBool2 = FALSE;
        if (is_in_two_player_adventure() && numHumanRacersFinished > 0 && get_trophy_race_world_id() == 0 &&
            set_course_finish_flags(settings) != 0) {
            someBool2 = TRUE;
        }
        if ((numHumanRacersFinished == numHumanRacers ||
             (numHumanRacers >= 2 && numFinishedRacers >= (gNumRacers - 1))) ||
            someBool2) {
            if (numHumanRacersFinished != numHumanRacers) {
                i = 0;
                do {
                    curRacer = gRacersByPosition[i]->racer;
                    if (curRacer->raceFinished == FALSE) {
                        if (curRacer->playerIndex >= 0) {
                            set_active_camera(curRacer->playerIndex);
                            camera = (uintptr_t) cam_get_active_camera_no_cutscenes();
                            // we need the camera to be a s32 for the WAIT_ON_IOBUSY anti tamper call to work
                            // but we *know* that cam_get_active_camera_no_cutscenes returns a Camera pointer so this
                            // should be safe
                            // LP64: hold the full pointer via uintptr_t; (s32) truncated+sign-extended
                            // the Camera* -> wild ->mode write at the race-finish challenge.
                            ((Camera *) camera)->mode = CAMERA_FINISH_CHALLENGE;
                        }
                        curRacer->raceFinished = TRUE;
                        curRacer->finishPosition = gNumFinishedRacers;
                        gNumFinishedRacers++;
                    }
                    i++;
                } while (i < gNumRacers);
            }

            raceType = FALSE;
#ifdef ANTI_TAMPER
            // Anti-Piracy check
            // passing in camera here is probably a fake
            WAIT_ON_IOBUSY(camera);
            // D_B0000574 is a direct read from the ROM as opposed to RAM
            if (((D_B0000574 & 0xFFFF) & 0xFFFF) != 0x6C07) {
                raceType = TRUE;
            }
#endif

            if (!gIsTimeTrial) {
                i = 0;
                do {
                    curRacer = gRacersByPosition[i]->racer;
                    racerPos = curRacer->racerIndex;
                    settings->racers[racerPos].starting_position = i;
                    i++;
                } while (i < gNumRacers);
            }
            gSwapLeadPlayer = FALSE;
            flags[2] = raceType;
            if (is_in_two_player_adventure() &&
                (settings->racers[PLAYER_TWO].starting_position < settings->racers[PLAYER_ONE].starting_position)) {
                gSwapLeadPlayer = TRUE;
            }
            curRacer = (*gRacersByPosition)->racer;
            gFirstTimeFinish = FALSE;
            if ((settings->gNumRacers == 1 || is_in_two_player_adventure()) &&
                curRacer->playerIndex != PLAYER_COMPUTER && !is_in_tracks_mode() && get_trophy_race_world_id() == 0) {
                gFirstTimeFinish = TRUE;
            }
            i = FALSE;
            if (gFirstTimeFinish && !someBool2) {
                i = set_course_finish_flags(settings);
            }
            if (someBool2) {
                gFirstTimeFinish = TRUE;
                i = TRUE;
            }

#ifdef ANTI_TAMPER
            if (flags[2]) {
                i = FALSE;
                gFirstTimeFinish = FALSE;
            }
#endif

            if (!i) {
                if (is_in_two_player_adventure()) {
                    if (gSwapLeadPlayer) {
                        gSwapLeadPlayer = FALSE;
                        swap_lead_player();
                        if (gLeadPlayerIndex) {
                            gIsP2LeadPlayer = TRUE;
                        }
                    } else if (gLeadPlayerIndex) {
                        gIsP2LeadPlayer = TRUE;
                    }
                }
                postrace_start(gFirstTimeFinish, 30);
            } else {
                settings->balloonsPtr[settings->worldId]++;
                if (settings->worldId != 0) {
                    settings->balloonsPtr[0]++;
                }
                race_finish_adventure(TRUE);
            }
            gRaceFinishTriggered = -1; // -1 doesn't do anything different.
            if (get_number_of_active_players() == 1) {
                race_finish_time_trial();
            }
        }
    }
}

/**
 * Mark the course as finished for the appropriate mode.
 * Check if it's the first race or the silver coin race before deciding which flag to write.
 * Return whether something was written.
 */
s8 set_course_finish_flags(Settings *settings) {
    Object_Racer *racer;

    racer = gRacersByPosition[PLAYER_ONE]->racer;
    if (racer->playerIndex == PLAYER_COMPUTER) {
        return FALSE;
    }
    gFirstTimeFinish = FALSE;
    if (!(settings->courseFlagsPtr[settings->courseId] & RACE_CLEARED)) {
        if (gIsTimeTrial == FALSE) {
            gFirstTimeFinish = TRUE;
            settings->courseFlagsPtr[settings->courseId] |= RACE_CLEARED;
        }
    } else if (gIsSilverCoinRace && racer->silverCoinCount >= 8 && gIsTimeTrial == FALSE) {
        gFirstTimeFinish = TRUE;
        settings->courseFlagsPtr[settings->courseId] |= RACE_CLEARED_SILVER_COINS;
    }
    return gFirstTimeFinish;
}

/**
 * Sets the countdown ready for the level to fade out.
 * Begins the timer that exits the level in 5 seconds.
 */
void race_finish_adventure(UNUSED s32 unusedArg) {
    gRaceEndTimer = 300;
    gRaceEndStage = 0;
    unused_D_8011AD52 = unusedArg;
}

/**
 * Begins counting down, once it reaches 0, start stopping all the race behaviours.
 * This function is also where the Taj Balloon cutscene is also handled.
 * After that's done, write to save and send the player back to the hub.
 */
void race_transition_adventure(s32 updateRate) {
    s32 i;
    Object_Racer *racer;
    Object *prevPort0Racer;
    s32 sp30;
    Object *prevRacer0Obj;
    Settings *settings;
    u32 cutsceneTimerLimit;

    set_pause_lockout_timer(1);
    sp30 = gRaceEndTimer;
    gRaceEndTimer -= updateRate;
    if (gRaceEndTimer <= 0) {
        gRaceEndTimer = -1;
    }
    if (sp30 > 50 && gRaceEndTimer <= 50) {
        transition_begin(&gRaceEndFade);
    }
    sp30 = 0;
    if (gRaceEndStage == 0 && gRaceEndTimer == -1) {
        for (i = 0; i < gNumRacers; i++) {
            racer = (*gRacers)[i]->racer;
            racer->magnetTargetObj = NULL;
            if (racer->playerIndex != PLAYER_COMPUTER && racer->finishPosition == 1) {
                sp30 = i;
            }
            if (racer->magnetSoundMask != NULL) {
                sndp_stop(racer->magnetSoundMask);
            }
            if (racer->shieldSoundMask != NULL) {
                audspat_point_stop(racer->shieldSoundMask);
            }
        }
        prevPort0Racer = (*gRacers)[0];
        (*gRacers)[0] = (*gRacers)[sp30];
        (*gRacers)[sp30] = prevPort0Racer;
        racer_sound_free((*gRacers)[0]);
        hud_audio_init();
        reset_rocket_sound_timer();
        sndp_stop_all_looped();
        if (is_in_two_player_adventure()) {
            set_scene_viewport_num(0);
            cam_set_layout(VIEWPORT_LAYOUT_1_PLAYER);
            prevRacer0Obj = (*gRacers)[0];
            prevPort0Racer = gRacersByPort[0];
            racer = prevRacer0Obj->racer;
            gRacersByPort[0] = prevRacer0Obj;
            gRacersByPort[1] = prevPort0Racer;
            if (gSwapLeadPlayer != 0) {
                gSwapLeadPlayer = 0;
                swap_lead_player();
                racer->playerIndex = 0;
            }
        }
        gNumRacersSaved = gNumRacers;
        gRaceEndStage = 1;
    }
    if (gRaceEndStage == 1) {
        hud_visibility(FALSE);
        gNumRacersSaved--;
        if (gNumRacersSaved > 0) {
            i = 0;
            while (i < gNumRacers && gRacersByPosition[i] != (*gRacers)[gNumRacersSaved]) {
                i++;
            }
            if (i < gNumRacers) {
                for (; i < (gNumRacers - 1); i++) {
                    gRacersByPosition[i] = gRacersByPosition[i + 1];
                }
            }
            free_object((*gRacers)[gNumRacersSaved]);
            (*gRacers)[gNumRacersSaved] = NULL;
            gNumRacers--;

        } else {
            gRaceEndStage = 2;
        }
    }
    if (gRaceEndStage == 2) {
        prevPort0Racer = (*gRacers)[0];
        racer = prevPort0Racer->racer;
        func_800230D0(prevPort0Racer, racer);
        racer->raceFinished = FALSE;
        gRaceEndStage = 3;
        func_8001E45C(CUTSCENE_ID_UNK_A);
        gBalloonCutsceneTimer = 0;
        func_8001E93C();
    }
    if (gRaceEndStage == 3) {
        transition_begin(&gRaceEndTransition);
        gRaceEndStage = 4;
        set_anti_aliasing(TRUE);
    }
    if (gRaceEndStage == 4) {
        set_anti_aliasing(TRUE);
        disable_racer_input();
        if (!(level_type() & RACETYPE_CHALLENGE_BATTLE)) {
            if (osTvType == OS_TV_TYPE_PAL) {
                cutsceneTimerLimit = 415;
            } else {
                cutsceneTimerLimit = 540;
            }
            gBalloonCutsceneTimer += updateRate;
            if (gBalloonCutsceneTimer < cutsceneTimerLimit) {
                minimap_fade(1);
            } else {
                hud_visibility(1);
            }
        }
        i = input_pressed(PLAYER_ONE) & A_BUTTON;
        settings = get_settings();
        if (!(settings->cutsceneFlags & 0x40000)) {
            i = 0;
        }
        if (func_800214C4() != 0 || (i != 0 && check_fadeout_transition() == 0)) {
            if (i != 0) {
                transition_begin(&gBalloonCutsceneTransition);
            }
            level_transition_begin(2);
            gRaceEndStage = 5;
            settings->cutsceneFlags |= 0x40000;
        }
    }
}

/**
 * Returns the race finish timer.
 */
s16 race_finish_timer(void) {
    return gRaceEndTimer;
}

/**
 * Return the timer used for the collectable balloon cutscene.
 */
u32 get_balloon_cutscene_timer(void) {
    return gBalloonCutsceneTimer;
}

/**
 * Checks if the fastest lap or the race time is faster than the current record.
 * Save those if they are, and if the staff ghost is not enabled, enable it for the next attempt.
 * The staff ghost comparison is also called here.
 */
void race_finish_time_trial(void) {
    s32 bestCourseTime;
    s32 bestRacerTime;
    s32 i;
    s32 courseTime;
    s32 vehicleID;
    s32 curRacerLapTime;
    s32 j;
    Object_Racer *curRacer;
    Object_Racer *bestRacer;
    Settings *settings;
    LevelHeader *levelHeader;
#ifdef NATIVE_PORT
    s32 tajTimeTrial = FALSE;
#endif

    levelHeader = level_header();
    settings = get_settings();
    settings->timeTrialRacer = 0;
    settings->unk115[1] = 0;
    settings->unk115[0] = 0;
    bestCourseTime = 36001;
    bestRacerTime = 36001;
    bestRacer = gRacersByPosition[0]->racer;
#ifdef NATIVE_PORT
    for (i = 0; i < gNumRacers; i++) {
        if (gRacersByPosition[i] != NULL && gRacersByPosition[i]->racer != NULL &&
            !taj_physics_canonical_records_allowed(gRacersByPosition[i]->racer)) {
            tajTimeTrial = TRUE;
        }
    }
#endif
    for (i = 0; i < gNumRacers; i++) {
        curRacer = gRacersByPosition[i]->racer;
        if (curRacer->racerIndex >= 0) {
            if (curRacer->racerIndex < get_number_of_active_players()) {
                settings->racers[curRacer->racerIndex].best_times = 0;
                vehicleID = curRacer->vehicleIDPrev;
                if (vehicleID >= VEHICLE_CAR && vehicleID < NUMBER_OF_PLAYER_VEHICLES) {
                    courseTime = 0;
                    for (j = 0; j < levelHeader->laps && j < 5; j++) {
                        settings->racers[curRacer->racerIndex].lap_times[j] = curRacer->lap_times[j];
                        curRacerLapTime = curRacer->lap_times[j];
                        courseTime += curRacerLapTime;
                        if (curRacerLapTime < bestRacerTime) {
                            settings->unk115[1] = j;
                            settings->unk115[0] = curRacer->racerIndex;
                            bestRacerTime = curRacerLapTime;
                        }
                    }
                    settings->racers[curRacer->racerIndex].course_time = courseTime;
                    if (courseTime < bestCourseTime) {
                        bestCourseTime = courseTime;
                        settings->timeTrialRacer = curRacer->racerIndex;
                        bestRacer = curRacer;
                    }
                }
            }
        }
    }
    settings->display_times = FALSE;
    if (gIsTimeTrial) {
        vehicleID = gPrevTimeTrialVehicle;
        if (vehicleID >= NUMBER_OF_PLAYER_VEHICLES || vehicleID < VEHICLE_CAR) {
            vehicleID = VEHICLE_CAR;
        }
        settings->display_times = TRUE;
#ifdef NATIVE_PORT
        if (!tajTimeTrial && settings->unk115[0] == 0) {
#else
        if (settings->unk115[0] == 0) {
#endif
            if ((settings->flapTimesPtr[vehicleID][settings->courseId] == 0) ||
                (bestRacerTime < settings->flapTimesPtr[vehicleID][settings->courseId])) {
                settings->flapTimesPtr[vehicleID][settings->courseId] = bestRacerTime;
                settings->racers[settings->unk115[0]].best_times |= 1 << settings->unk115[1];
            }
        }
#ifdef NATIVE_PORT
        if (!tajTimeTrial && settings->timeTrialRacer == 0) {
#else
        if (settings->timeTrialRacer == 0) {
#endif
            if ((settings->courseTimesPtr[vehicleID][settings->courseId] == 0) ||
                (bestCourseTime < settings->courseTimesPtr[vehicleID][settings->courseId])) {
                settings->courseTimesPtr[vehicleID][settings->courseId] = bestCourseTime;
                settings->racers[settings->timeTrialRacer].best_times |= 0x80;
            }
        }
        if (((!vehicleID) && (!vehicleID)) && (!vehicleID)) {} // Fakematch
#ifdef NATIVE_PORT
        if (!tajTimeTrial && settings->timeTrialRacer == 0) {
#else
        if (settings->timeTrialRacer == 0) {
#endif
            if (bestCourseTime < 10800 && (vehicleID != gTimeTrialVehicle || timetrial_map_id() != level_id() ||
                                           bestCourseTime < gTimeTrialTime)) {
                gTimeTrialTime = bestCourseTime;
                gTimeTrialVehicle = gPrevTimeTrialVehicle;
                gTimeTrialCharacter = settings->racers[0].character;
                timetrial_swap_player_ghost(level_id());
                gHasGhostToSave = TRUE;
            }
            if (osTvType == OS_TV_TYPE_PAL) {
                bestCourseTime = (bestCourseTime * 6) / 5;
            }
            if (bestCourseTime < gTTGhostTimeToBeat) {
                if (gTimeTrialStaffGhost) {
                    tt_ghost_beaten(level_id(), &bestRacer->playerIndex);
                } else {
                    hud_time_trial_message(&bestRacer->playerIndex);
                }
            } else {
                hud_time_trial_message(&bestRacer->playerIndex);
            }
        }
#ifdef NATIVE_PORT
        if (tajTimeTrial) {
            gHasGhostToSave = FALSE;
            taj_physics_trace_record_suppressed(bestRacer);
        }
#endif
    }
}

/**
 * Returns true if the player ghost data is valid for playback.
 */
s32 timetrial_valid_player_ghost(void) {
    if (timetrial_map_id() != level_id()) {
        return FALSE;
    } else {
        if (gTimeTrialVehicle != gPrevTimeTrialVehicle) {
            return FALSE;
        } else {
            return TRUE;
        }
    }
}

/**
 * Return the player ghost object.
 */
Object *timetrial_player_ghost(void) {
    return gGhostObjPlayer;
}

/**
Pretty sure this determines whether or not you're eligible to race TT ghost in track select
when TT is on. It looks like it checks some ghost data makes sure you've got a ghost for that level
with the default vehicle,
Returns 0 if TT ghost was loaded successfully.
*/
s32 timetrial_load_staff_ghost(s32 mapId) {
    TTGhostTable *ghostTable;
    TTGhostTable *prevGhostTable;
    s32 ret;
    TTGhostTable *nextGhostTable;

    gMapDefaultVehicle = leveltable_vehicle_default(mapId);
    ghostTable = (TTGhostTable *) asset_table_load(ASSET_TTGHOSTS_TABLE);

    nextGhostTable = ghostTable;
    do {
        prevGhostTable = nextGhostTable;
        if ((prevGhostTable->mapId == mapId) && (prevGhostTable->defaultVehicleId == gMapDefaultVehicle)) {
            break;
        }
        nextGhostTable++;
    } while (prevGhostTable->mapId != 0xFF);

    ret = 1;

    if (prevGhostTable->mapId != 0xFF) {
        ret = load_tt_ghost(nextGhostTable->ghostOffset, nextGhostTable[1].ghostOffset - nextGhostTable->ghostOffset,
                            &gTTGhostTimeToBeat);
    }

    mempool_free(ghostTable);
    return ret;
}

/**
 * Return true if this object is the time trial ghost.
 */
s32 timetrial_staff_ghost_check(Object *obj) {
    return obj == gGhostObjStaff;
}

/**
 * Free ghost data then save the players victory.
 * Check if the player has beaten every time and unlock TT.
 * Otherwise, tell them to try another track.
 */
void tt_ghost_beaten(s32 arg0, s16 *playerId) {
    s32 trackIdCount;
    s8 *mainTrackIds;

    gGhostObjStaff = NULL;
    timetrial_free_staff_ghost();
    gTimeTrialStaffGhost = FALSE;
    mainTrackIds = (s8 *) get_misc_asset(ASSET_MISC_MAIN_TRACKS_IDS);
    trackIdCount = 0;
    while (mainTrackIds[trackIdCount] != -1 && mainTrackIds[trackIdCount] != arg0) {
        trackIdCount++;
    }
    if (gBeatStaffGhost) {
        // Save that TT has been beaten for this track.
        set_eeprom_settings_value(16 << trackIdCount);
        // Check if TT has been beaten for all tracks.
        if ((get_eeprom_settings() & 0xFFFFF0) == 0xFFFFF0) {
            set_magic_code_flags(CHEAT_CONTROL_TT);
            sound_play(SOUND_VOICE_TT_BEAT_ALL_TIMES, NULL);
            sound_play_delayed(SOUND_VOICE_TT_UNLOCKED, NULL, 1.5f);
            set_current_text(
                ASSET_GAME_TEXT_84); // Text for "You have beaten all my times!" and then "Now you can PICK me!"
        } else {
            sound_play(SOUND_VOICE_TT_WELL_DONE, NULL);
            sound_play_delayed(SOUND_VOICE_TT_TRY_ANOTHER_TRACK, NULL, 1.0f);
            set_current_text(ASSET_GAME_TEXT_83); // Text for "Well Done! Now try another track."
        }
        gBeatStaffGhost = FALSE;
        return;
    }
    hud_time_trial_message(playerId);
}

/**
 * Compare if the course record is enough to unlock the staff ghost.
 * Also check if the ghost tiself has been beaten.
 * Store both results and return if there should be a ghost.
 */
u8 timetrial_init_staff_ghost(s32 trackId) {
    s32 i;
    s8 *mainTrackIds;
    u16 *staffTime;
    Settings *settings;

    gBeatStaffGhost = FALSE;
    gTimeTrialStaffGhost = FALSE;
    settings = get_settings();
    if (leveltable_vehicle_default(trackId) == (Vehicle) gPrevTimeTrialVehicle) {
        mainTrackIds = (s8 *) get_misc_asset(ASSET_MISC_MAIN_TRACKS_IDS);
        staffTime = (u16 *) get_misc_asset(ASSET_MISC_GHOST_UNLOCK_TIMES);
        for (i = 0; mainTrackIds[i] != -1 && trackId != mainTrackIds[i]; i++) {}
        if (mainTrackIds[i] != -1) {
            if (staffTime[i] >= settings->courseTimesPtr[gPrevTimeTrialVehicle][trackId]) {
                // Check if TT has been beaten?
                if (!(get_eeprom_settings() & ((1 << 4) << i))) {
                    gBeatStaffGhost = TRUE;
                }
                if (timetrial_load_staff_ghost(trackId) == 0) {
                    gTimeTrialStaffGhost = TRUE;
                }
            }
        }
    }
    return gTimeTrialStaffGhost;
}

/**
 * Return the time trial staff ghost object.
 */
Object *timetrial_ghost_staff(void) {
    return gGhostObjStaff;
}

/**
 * Return true if the tt ghost is unbeaten for this track.
 */
s32 timetrial_staff_unbeaten(void) {
    return gBeatStaffGhost == FALSE;
}

/**
 * Calls a function to start loading the player ghost data from the controller pak.
 * Returns the controller pak status. 0 means good.
 */
s32 timetrial_init_player_ghost(s32 playerID) {
    s16 characterID;
    s16 time;
    s32 cpakStatus;
    s32 ghostMapID;

    ghostMapID = timetrial_map_id();
    if (level_id() != ghostMapID || gTimeTrialVehicle != gPrevTimeTrialVehicle) {
        cpakStatus = timetrial_load_player_ghost(playerID, level_id(), gPrevTimeTrialVehicle, &characterID, &time);
        if (cpakStatus == CONTROLLER_PAK_GOOD) {
            gTimeTrialVehicle = gPrevTimeTrialVehicle;
            gTimeTrialCharacter = characterID;
            gTimeTrialTime = time;
        }
        return cpakStatus;
    }
    return timetrial_load_player_ghost(playerID, level_id(), gPrevTimeTrialVehicle, NULL, NULL);
}

/**
 * Call a function to write the ghost data to the controller pak.
 * Returns the controller pak status. 0 is good.
 */
SIDeviceStatus timetrial_save_player_ghost(s32 controllerIndex) {
#ifdef NATIVE_PORT
    if (taj_physics_run_is_noncanonical()) {
        taj_physics_trace_record_suppressed(NULL);
        return CONTROLLER_PAK_BAD_DATA;
    }
#endif
    return timetrial_write_player_ghost(controllerIndex, timetrial_map_id(), gTimeTrialVehicle, gTimeTrialCharacter,
                                        gTimeTrialTime);
}

/**
 * Returns whether there's valid ghost data to save.
 */
u8 has_ghost_to_save(void) {
    return gHasGhostToSave;
}

/**
 * Resets the variables used for ghost data saving.
 */
void set_ghost_none(void) {
    gGhostMapID = -1;
    gHasGhostToSave = FALSE;
}

/**
 * Finds the opponent to this racer in a relative position to them and calculates the distance to them.
 * The position argument is relative to the racer's current position and represents the number of
 * places ahead (positive) or behind (negative) the opponent is. So, for instance, if position is 1,
 * find the opponent one place ahead of the racer; if it's -1, find the opponent one place behind.
 */
Object *racer_find_nearest_opponent_relative(Object_Racer *racer, s32 position, f32 *distance) {
    UNUSED s32 pad;
    Object *tempRacerObj;
    position = (racer->racerOrder - position) - 1;
    if (position < 0 || position >= gNumRacers) {
        return NULL;
    }
    tempRacerObj = gRacersByPosition[position];
    if (tempRacerObj == NULL) {
        return NULL;
    }
    *distance = racer_calc_distance_to_opponent(racer, tempRacerObj->racer);
    return tempRacerObj;
}

/**
 * Calculates the distance between two racers. Traverses through the checkpoints
 * between the two racers to calculate the total distance.
 * If the second racer is ahead, the distance is positive, otherwise negative.
 */
f32 racer_calc_distance_to_opponent(Object_Racer *racer1, Object_Racer *racer2) {
    Object_Racer *temp_racer;
    f32 totalDistance;
    s32 r1_ccp;
    UNUSED s32 pad;
    s32 reverseOrder;
    s32 checkpointID;

    if (gNumberOfMainCheckpoints <= 0) {
        return 0.0f;
    }
    totalDistance = 0.0f;
    reverseOrder = FALSE;
    if (racer2->courseCheckpoint < racer1->courseCheckpoint) {
        temp_racer = racer1;
        racer1 = racer2;
        racer2 = temp_racer;
        reverseOrder = TRUE;
    }
    checkpointID = racer1->nextCheckpoint;
    for (r1_ccp = racer1->courseCheckpoint; r1_ccp < racer2->courseCheckpoint; r1_ccp++) {
        totalDistance += gTrackCheckpoints[checkpointID++].distance;
        if (checkpointID == gNumberOfMainCheckpoints) {
            checkpointID = 0;
        }
    }
    checkpointID = racer1->nextCheckpoint - 1;
    if (checkpointID < 0) {
        checkpointID = gNumberOfMainCheckpoints - 1;
    }
    totalDistance += (gTrackCheckpoints[checkpointID].distance * racer1->checkpoint_distance);
    checkpointID = racer2->nextCheckpoint - 1;
    if (checkpointID < 0) {
        checkpointID = gNumberOfMainCheckpoints - 1;
    }
    totalDistance -= (gTrackCheckpoints[checkpointID].distance * racer2->checkpoint_distance);
    if (reverseOrder) {
        totalDistance = -totalDistance;
    }
    return totalDistance;
}

/**
 * Traverses from the racer's position through the upcoming checkpoints
 * to the starting line and calculates the total distance.
 */
UNUSED f32 race_calc_distance_to_start_line(Object_Racer *racer) {
    f32 distLeft;
    s32 checkpointID;

    if (gNumberOfMainCheckpoints <= 0) {
        return 0.0f;
    }
    distLeft = 0.0f;
    for (checkpointID = racer->nextCheckpoint; checkpointID < gNumberOfMainCheckpoints; checkpointID++) {
        distLeft += gTrackCheckpoints[checkpointID].distance;
    }
    checkpointID = racer->nextCheckpoint - 1;
    if (checkpointID < 0) {
        checkpointID = gNumberOfMainCheckpoints - 1;
    }
    distLeft += (gTrackCheckpoints[checkpointID].distance * racer->checkpoint_distance);
    return distLeft;
}

/**
 * Returns a pointer to a specific checkpoint.
 */
CheckpointNode *get_checkpoint_node(s32 checkpointID) {
    return &gTrackCheckpoints[checkpointID];
}

/**
 * Takes the position along the checkpoint path, and finds the next applicable node.
 * If an alternative path is available, use that node instead.
 */
CheckpointNode *find_next_checkpoint_node(s32 splinePos, s32 isAlternate) {
    CheckpointNode *checkpointNode = &gTrackCheckpoints[splinePos];
    if (isAlternate != 0 && checkpointNode->altRouteID != -1) {
        checkpointNode = &gTrackCheckpoints[checkpointNode->altRouteID];
    }
    return checkpointNode;
}

/**
 * Returns the number of active checkpoints in the current level.
 */
s32 get_checkpoint_count(void) {
    return gNumberOfMainCheckpoints;
}

/**
 * Returns the group of racer objects.
 * Official Name: objGetPlayerlist
 */
Object **get_racer_objects(s32 *numRacers) {
    *numRacers = gNumRacers;
    return *gRacers;
}

/**
 * Returns the group of racer objects, ordered by player index.
 */
Object **get_racer_objects_by_port(s32 *numRacers) {
    *numRacers = gNumRacers;
    return gRacersByPort;
}

/**
 * Returns the group of racer objects, ordered by current race position.
 */
Object **get_racer_objects_by_position(s32 *numRacers) {
    *numRacers = gNumRacers;
    return gRacersByPosition;
}

/**
 * Returns the racer object specified by the ID
 */
Object *get_racer_object(s32 index) {
    if (gNumRacers == 0) {
        return NULL;
    }
    if (index < 0 || index >= gNumRacers) {
        return NULL;
    }
    return (*gRacers)[index];
}

/**
 * Returns the racer object specified by the player ID.
 */
Object *get_racer_object_by_port(s32 index) {
    if (gNumRacers == 0) {
        return NULL;
    }
    if (index < 0 || index >= gNumRacers) {
        return NULL;
    }
    return gRacersByPort[index];
}

/**
 * Unused function that would've iterated through all active checkpoints to render their visual nodes.
 * The function it calls is completely stubbed out.
 */
UNUSED void debug_render_checkpoints(Gfx **dList, Mtx **mtx, Vertex **vtx) {
    s32 i;

    material_set_no_tex_offset(dList, NULL, RENDER_Z_COMPARE);
    if (gNumberOfMainCheckpoints > 3) {
        for (i = 0; i < gNumberOfMainCheckpoints; i++) {
            // Ground path
            debug_render_checkpoint_node(i, 0, dList, mtx, vtx);
        }
        for (i = 0; i < gNumberOfMainCheckpoints; i++) {
            // Air path
            debug_render_checkpoint_node(i, 1, dList, mtx, vtx);
        }
    }
}

/**
 * Would've rendered an individual checkpoint node. On https://noclip.website, with dev objects enabled, you can see a
 * visual representation of what these checkpoints would've looked like ingame.
 */
UNUSED void debug_render_checkpoint_node(UNUSED s32 checkpointID, UNUSED s32 pathID, UNUSED Gfx **dList,
                                         UNUSED Mtx **mtx, UNUSED Vertex **vtx) {
}

/**
 * Loop through every existing spectate camera and sort them by index.
 */
void spectate_update(void) {
    Object *objPtr;
    Object *temp;
    s32 continueLoop;
    s32 i;

    gCameraObjCount = 0;
    for (i = 0; i < gObjectCount; i++) {
        objPtr = gObjPtrList[i];
        if (!(objPtr->trans.flags & OBJ_FLAGS_PARTICLE)) {
            if (objPtr->behaviorId == BHV_CAMERA_CONTROL) {
                if (gCameraObjCount < CAMCONTROL_COUNT) {
                    (*gCameraObjList)[gCameraObjCount] = objPtr;
                    gCameraObjCount++;
                }
            }
        }
    }

    do {
        continueLoop = TRUE;
        for (i = 0; i < gCameraObjCount - 1; i++) {
            objPtr = (*gCameraObjList)[i + 1];
            temp = (*gCameraObjList)[i];
            if (temp->properties.camControl.cameraID > objPtr->properties.camControl.cameraID) {
                (*gCameraObjList)[i] = (*gCameraObjList)[i + 1];
                (*gCameraObjList)[i + 1] = temp;
                continueLoop = FALSE;
            }
        }
    } while (!continueLoop);
}

Object *spectate_object(s32 cameraIndex) {
    if (cameraIndex < 0 || cameraIndex >= gCameraObjCount) {
        return NULL;
    }
    return (*gCameraObjList)[cameraIndex];
}

/**
 * Take the current camera passed through the function and compare distances between the next and previous camera.
 * Set the camera to be whichever's closest to the object.
 */
Object *spectate_nearest(Object *obj, s32 *cameraId) {
    Object *nextCamera;
    Object *prevCamera;
    Object *currCamera;
    s32 *cameraIndex;
    f32 x;
    f32 y;
    f32 z;
    f32 prevCameraXYZ;
    f32 currCameraXYZ;
    f32 nextCameraXYZ;
    s32 cameraIndex_Curr;
    s32 cameraIndex_Prev;
    s32 cameraIndex_Next;
    cameraIndex = cameraId;
    if (gCameraObjCount == 0) {
        return NULL;
    }
    cameraIndex_Next = *cameraIndex + 1;
    cameraIndex_Curr = *cameraIndex;
    cameraIndex_Prev = *cameraIndex - 1;
    if (cameraIndex_Next >= gCameraObjCount) {
        cameraIndex_Next = 0;
    }
    if (cameraIndex_Prev < 0) {
        cameraIndex_Prev = gCameraObjCount - 1;
    }
    currCamera = (*gCameraObjList)[cameraIndex_Curr];
    nextCamera = (*gCameraObjList)[cameraIndex_Next];
    prevCamera = (*gCameraObjList)[cameraIndex_Prev];
    x = currCamera->trans.x_position - obj->trans.x_position;
    y = currCamera->trans.y_position - obj->trans.y_position;
    z = currCamera->trans.z_position - obj->trans.z_position;
    currCameraXYZ = (x * x) + (y * y) + (z * z);
    x = nextCamera->trans.x_position - obj->trans.x_position;
    y = nextCamera->trans.y_position - obj->trans.y_position;
    z = nextCamera->trans.z_position - obj->trans.z_position;
    nextCameraXYZ = (x * x) + (y * y) + (z * z);
    x = prevCamera->trans.x_position - obj->trans.x_position;
    y = prevCamera->trans.y_position - obj->trans.y_position;
    z = prevCamera->trans.z_position - obj->trans.z_position;
    prevCameraXYZ = (x * x) + (y * y) + (z * z);

    if (nextCameraXYZ < currCameraXYZ) {
        *cameraId = cameraIndex_Next;
        currCamera = nextCamera;
        currCameraXYZ = nextCameraXYZ;
    }
    if (prevCameraXYZ < currCameraXYZ) {
        *cameraId = cameraIndex_Prev;
        currCamera = prevCamera;
    }
    return currCamera;
}

/**
 * Take every existing AI node and find each neighbouring node.
 * Afterwards, sort them by height so the game can generate elevation thresholds.
 */
void ainode_update(void) {
    LevelObjectEntry_AiNode *aiNodeEntry;
    Object *obj;
    s16 nodeCount;
    Object *nextAiNodeObj;
    f32 diffX;
    f32 diffZ;
    f32 diffY;
    s32 i;
    s32 j;
    s8 index;  // Must be an s8
    u8 index2; // Must be an u8
    s16 swap;
    Object_AiNode *aiNodeObj64;
    s8 nodeIDs[AINODE_COUNT];
    s8 elevations[AINODE_COUNT];

    if (gInitAINodes == FALSE) {
        return;
    }
    gInitAINodes = FALSE;
    for (i = 0; i < AINODE_COUNT; i++) {
        (*gAINodes)[i] = NULL;
    }
    nodeCount = 0;
    // Store each existing node ID in the temporary vars.
    for (i = 0; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE) && obj->behaviorId == BHV_AINODE) {
            aiNodeEntry = &obj->level_entry->aiNode;
            index2 = aiNodeEntry->nodeID;
            if (!(index2 & AINODE_COUNT)) {
                (*gAINodes)[index2] = obj;
                nodeIDs[nodeCount] = aiNodeEntry->nodeID;
                elevations[nodeCount] = aiNodeEntry->elevation & 3;
                nodeCount++;
            }
        }
    }
    if (nodeCount == 0) {
        return;
    }
    // Find and set each neighbouring node and distance for each existing node.
    for (i = 0; i < AINODE_COUNT; i++) {
        obj = (*gAINodes)[i];
        if (obj != NULL) {
            aiNodeObj64 = obj->ai_node;
            aiNodeEntry = &obj->level_entry->aiNode;
            for (j = 0; j < 4; j++) {
                index2 = aiNodeEntry->adjacent[j];
                if (!(index2 & AINODE_COUNT)) {
                    nextAiNodeObj = (*gAINodes)[index2];
                    aiNodeObj64->nodeObj[j] = nextAiNodeObj;
                    if (nextAiNodeObj == NULL) {
                        aiNodeEntry->adjacent[j] = -1;
                    } else {
                        diffX = obj->trans.x_position - nextAiNodeObj->trans.x_position;
                        diffY = obj->trans.y_position - nextAiNodeObj->trans.y_position;
                        diffZ = obj->trans.z_position - nextAiNodeObj->trans.z_position;
                        aiNodeObj64->distToNode[j] = sqrtf((diffX * diffX) + (diffY * diffY) + (diffZ * diffZ));
                    }
                }
            }
        }
    }
    // Sort them by height
    do {
        j = TRUE;
        for (i = 0; i < nodeCount - 1; i++) {
            if ((*gAINodes)[nodeIDs[i + 1]]->trans.y_position < (*gAINodes)[nodeIDs[i]]->trans.y_position) {
                swap = nodeIDs[i];
                nodeIDs[i] = nodeIDs[i + 1];
                nodeIDs[i + 1] = swap;
                swap = elevations[i];
                elevations[i] = elevations[i + 1];
                elevations[i + 1] = swap;
                j = FALSE;
            }
        }
    } while (!j); // Keep doing this until no more swaps are needed.

    if (1) {} // Fakematch

    for (i = 0; i < 5; i++) {
        gElevationHeights[i] = -20000.0f;
    }

    index = elevations[0];
    for (i = 0; i < nodeCount - 1;) {
        while (i < nodeCount - 1 && index >= elevations[i]) {
            i++;
        }
        if (index < elevations[i]) {
            index = elevations[i];
            gElevationHeights[index] =
                ((*gAINodes)[nodeIDs[i]]->trans.y_position + (*gAINodes)[nodeIDs[i - 1]]->trans.y_position) * 0.5;
        } else {
            i = nodeCount;
        }
    }
    gElevationHeights[0] = -10000.0f;
    gElevationHeights[4] = -gElevationHeights[0];
}

/**
 * Compare heights against the thresholds.
 * Elevation level is set based on position.
 */
s16 obj_elevation(f32 yPos) {
    s16 i = 0;
    s16 elevation = 0;
    for (; i < 4; i++) {
        if (gElevationHeights[i] != -20000.0f && gElevationHeights[i] < yPos) {
            elevation = i;
        }
    }
    return elevation;
}

/**
 * Loop through the AI Node list and add this new object to the list if it does not already exist.
 */
s32 ainode_register(Object *obj) {
    s32 i;
    for (i = 0; i < AINODE_COUNT; i++) {
        if ((*gAINodes)[i] == NULL) {
            (*gAINodes)[i] = obj;
            return i;
        }
    }
    return -1;
}

/**
 * Search through each AI node and find the one closest to the coordinates given.
 * Can choose to include or ignore elevation.
 */
s32 ainode_find_nearest(f32 diffX, f32 diffY, f32 diffZ, s32 useElevation) {
    UNUSED f32 pad[6];
    s32 elevation;
    f32 len;
    f32 x;
    f32 z;
    f32 y;
    f32 dist;
    s32 findDist;
    s32 numSteps;
    s32 result;
    Object *obj;
    LevelObjectEntry_AiNode *levelObj;

    if (useElevation) {
        elevation = obj_elevation(diffY);
    }
    dist = 50000.0;
    result = 0xFF;
    for (numSteps = 0; numSteps != AINODE_COUNT; numSteps++) {
        obj = (*gAINodes)[numSteps];
        if (obj) {
            levelObj = &((obj->level_entry)->aiNode);
            findDist = TRUE;
            if (useElevation && elevation != levelObj->elevation) {
                findDist = FALSE;
            }
            if (useElevation == 2 && levelObj->unk8 != 3) {
                findDist = FALSE;
            }
            if (findDist) {
                x = obj->trans.x_position - diffX;
                y = obj->trans.y_position - diffY;
                z = obj->trans.z_position - diffZ;
                len = sqrtf((x * x) + (y * y) + (z * z));
                if (len < dist) {
                    dist = len;
                    result = numSteps;
                }
            }
        }
    }
    return result;
}

// Updated Object_NPC
f32 func_8001C6C4(Object_NPC *npc, Object *npcParentObj, f32 updateRateF, f32 speedF, s32 direction) {
    f32 xPositions[5];
    f32 yPositions[5];
    f32 zPositions[5];
    f32 xDiff2;
    f32 yDiff2;
    f32 zDiff2;
    Object *aiNode;
    f32 dist;
    f32 xDiff;
    f32 yDiff;
    f32 zDiff;
    f32 tempYDiff;
    UNUSED s32 pad_sp84;
    s32 i;
    f32 var_f20_2;
    s32 var_s0;
    s32 someBool;

    if (osTvType == OS_TV_TYPE_PAL) {
        updateRateF *= 1.2;
    }

    for (i = 0; i < 5; i++) {
        if (npc->nodeData[i] == 0xFF) {
            return 0.0f;
        }

        aiNode = ainode_get(npc->nodeData[i]);
        if (aiNode == NULL) {
            return 0.0f;
        }

        xPositions[i] = aiNode->trans.x_position;
        yPositions[i] = aiNode->trans.y_position;
        zPositions[i] = aiNode->trans.z_position;
    }

    xDiff2 = catmull_rom_interpolation(xPositions, 0, npc->unk0);
    yDiff2 = catmull_rom_interpolation(yPositions, 0, npc->unk0);
    zDiff2 = catmull_rom_interpolation(zPositions, 0, npc->unk0);
    someBool = FALSE;
    if (npc->unk8 == 0.0f) {
        npc->unk8 = 0.01f;
    }

    for (var_s0 = 0; var_s0 != 2; var_s0++) {
        var_f20_2 = npc->unk0 + (npc->unk8 * updateRateF);
        if (var_f20_2 >= 1.0) {
            someBool = TRUE;
            var_f20_2 -= 1.0;
        }
        xDiff = catmull_rom_interpolation(xPositions, someBool, var_f20_2);
        yDiff = catmull_rom_interpolation(yPositions, someBool, var_f20_2);
        zDiff = catmull_rom_interpolation(zPositions, someBool, var_f20_2);
        xDiff -= xDiff2;
        yDiff -= yDiff2;
        zDiff -= zDiff2;
        if (var_s0 == 0) {
            someBool = FALSE;
            dist = sqrtf((xDiff * xDiff) + (yDiff * yDiff) + (zDiff * zDiff)) / updateRateF;
            if (dist != 0.0f) {
                npc->unk8 *= (speedF / dist);
            }
        }
    }
    npc->unk0 = var_f20_2;
    xDiff2 = xDiff + xDiff2;
    tempYDiff = yDiff2 = yDiff + yDiff2;
    zDiff2 = zDiff + zDiff2;

    xDiff = xDiff2 - npcParentObj->trans.x_position;
    yDiff = yDiff2 - npcParentObj->trans.y_position;
    zDiff = zDiff2 - npcParentObj->trans.z_position;

    xDiff2 = xDiff;
    yDiff2 = yDiff;
    zDiff2 = zDiff;

    dist = sqrtf((xDiff * xDiff) + (yDiff * yDiff) + (zDiff * zDiff));
    if (dist != 0.0f) {
        dist = 255.0 / dist;
        xDiff *= dist;
        yDiff *= dist;
        zDiff *= dist;
    }
    dist = sqrtf((xDiff2 * xDiff2) + (yDiff2 * yDiff2) + (zDiff2 * zDiff2)) / 16;
    if (speedF < dist) {
        dist = speedF;
    }
    if (dist >= 1.0) {
        var_s0 = (arctan2_f(xDiff, zDiff) - (npcParentObj->trans.rotation.y_rotation & 0xFFFF)) - 0x8000;
        if (var_s0 > 0x8000) {
            var_s0 -= 0xFFFF;
        }
        if (var_s0 < -0x8000) {
            var_s0 += 0xFFFF;
        }
        npcParentObj->trans.rotation.y_rotation += ((var_s0 * (s32) updateRateF)) >> 4;
        var_s0 = arctan2_f(yDiff, 255.0f) - (npcParentObj->trans.rotation.x_rotation & 0xFFFF);
        if (var_s0 > 0x8000) {
            var_s0 -= 0xFFFF;
        }
        if (var_s0 < -0x8000) {
            var_s0 += 0xFFFF;
        }
        npcParentObj->trans.rotation.x_rotation += ((var_s0 * (s32) updateRateF)) >> 4;
    }

    npcParentObj->trans.rotation.z_rotation = 0;
    xDiff = sins_f((s16) (npcParentObj->trans.rotation.y_rotation + 0x8000)) * dist;
    move_object(npcParentObj, xDiff * updateRateF, 0.0f,
                coss_f((npcParentObj->trans.rotation.y_rotation + 0x8000)) * dist * updateRateF);
    npcParentObj->trans.y_position = tempYDiff;
    dist = dist * updateRateF * 2;
    if (someBool != 0) {
        npc->nodeData[0] = npc->nodeData[1];
        npc->nodeData[1] = npc->nodeData[2];
        npc->nodeData[2] = npc->nodeData[3];
        npc->nodeData[3] = npc->nodeData[4];
        npc->nodeData[4] = ainode_find_next(npc->nodeData[3], npc->nodeData[2], direction);
    }

    return dist;
}

s32 ainode_find_next(s32 nodeId, s32 nextNodeId, s32 direction) {
    Object *aiNodeObj;
    LevelObjectEntry_AiNode *entry;
    Object_AiNode *aiNode;
    s32 nextIndex;
    s32 i;
    s32 someCount;

    if (nodeId < -1 || nodeId >= AINODE_COUNT) {
        return NODE_NONE;
    }
    aiNodeObj = (*gAINodes)[nodeId];
    if (aiNodeObj == NULL) {
        return NODE_NONE;
    }

    entry = &aiNodeObj->level_entry->aiNode;
    aiNode = aiNodeObj->ai_node;
    direction = direction & 3;
    someCount = 0;
    nextIndex = (aiNode->directions[direction] + 1) & 3;

    for (i = 0; i < 4; i++) {
        if (entry->adjacent[nextIndex] != NODE_NONE && entry->adjacent[nextIndex] != nextNodeId) {
            aiNode->directions[direction] = nextIndex;
            i = 4; // break
            someCount++;
        }
        nextIndex = (nextIndex + 1) & 3;
    }

    if (someCount == 0) {
        return NODE_NONE;
    } else {
        return entry->adjacent[aiNode->directions[direction]];
    }
}

s16 func_8001CD28(s32 arg0, s32 arg1, s32 arg2, s32 arg3) {
    s16 result;
    s32 sp370;
    s16 var_t1;
    s16 sp36C;
    s16 temp2;
    s16 i;
    LevelObjectEntry_AiNode *aiNodeEntry;
    s8 someBool;
    s8 var_s3;
    s8 var_s5;
    s8 temp;
    s32 var_ra;
    Object_AiNode *aiNode;
    Object *aiNodeObj;
    s32 sp154[AINODE_COUNT];
    s8 spD4[AINODE_COUNT];
    s8 sp54[AINODE_COUNT];

    // Only matches with do {} while?
    i = 0;
    do {
        sp154[i] = 0;
        i++;
    } while (i < AINODE_COUNT);

    aiNodeObj = (*gAINodes)[arg0];
    aiNode = aiNodeObj->ai_node;
    aiNodeEntry = &aiNodeObj->level_entry->aiNode;
    var_t1 = 0;
    var_ra = 0;
    result = NODE_NONE;
    var_s3 = 0;
    someBool = 1;
    var_s5 = 0;
    do {
        for (i = 0; i < 4; i++) {
            if (aiNode->nodeObj[i] != 0) {
                temp2 = aiNodeEntry->adjacent[i];
                temp = 1;
                if (((arg0 == aiNodeEntry->nodeID) && (temp2 == arg2)) ||
                    ((arg2 == aiNodeEntry->nodeID) && (temp2 == arg0))) {
                    temp = 0;
                    if (var_s5 != 0) {
                        temp = 1;
                    }
                    var_s5++;
                }
                if (temp != 0) {
                    sp36C = 0;
                    temp = 0;
                    while ((sp36C < var_t1) && (temp2 != spD4[sp36C])) {
                        sp36C++;
                    }

                    sp370 = aiNode->distToNode[i] + var_ra;
                    if (sp36C == var_t1) {
                        temp = 2;
                    } else if (sp370 < sp154[temp2]) {
                        temp = 1;
                    }
                }
                if (temp != 0) {
                    while (sp36C < var_t1 - 1) {
                        spD4[sp36C] = spD4[sp36C + 1];
                        sp54[sp36C] = sp54[sp36C + 1];
                        sp36C++;
                    }
                    if (temp == 2) {
                        var_t1++;
                    }
                    sp154[temp2] = sp370;

                    sp36C = var_t1 - 1;

                    sp54[sp36C] = (someBool) ? aiNodeEntry->adjacent[i] : var_s3;
                    spD4[sp36C] = aiNodeEntry->adjacent[i];

                    while ((sp36C > 0) && (sp154[spD4[sp36C - 1]] < sp154[spD4[sp36C]])) {
                        temp = spD4[sp36C];
                        spD4[sp36C] = spD4[sp36C - 1];
                        spD4[sp36C - 1] = temp;
                        temp = sp54[sp36C];
                        sp54[sp36C] = sp54[sp36C - 1] & 0xFF & 0xFF & 0xFF;
                        sp54[sp36C - 1] = temp & 0xFF;
                        sp36C--;
                    }
                }
            }
        }

        if (var_t1 > 0) {
            var_t1--;
            aiNodeObj = (*gAINodes)[spD4[var_t1]];
            aiNodeEntry = &aiNodeObj->level_entry->aiNode;
            var_s3 = sp54[var_t1];
            var_ra = sp154[aiNodeEntry->nodeID];
            aiNode = aiNodeObj->ai_node;
            someBool = 0;
            if (arg1 & 0x100) {
                if ((arg1 & 0x7F) == spD4[var_t1]) {
                    result = var_s3;
                }
            } else if (arg1 == aiNodeEntry->unk8) {
                result = var_s3;
            }
            if ((var_t1 == 0) && (result == 0xFF)) {
                result = var_s3;
            }
        }
    } while ((result == NODE_NONE) && (var_t1 > 0));

    if (result != 0xFF) {
        aiNodeObj = (*gAINodes)[arg0];
        aiNodeEntry = &aiNodeObj->level_entry->aiNode;
        aiNode = aiNodeObj->ai_node;
        for (i = 0; (i < 4) && (result != aiNodeEntry->adjacent[i]); i++) {}
        if (i < 4) {
            aiNode->directions[arg3] = i;
        }
    }
    return result;
}

/**
 * Signal that AI nodes exist, so the game knows to initialise them.
 */
void ainode_enable(void) {
    gInitAINodes = TRUE;
}

/**
 * If the node ID is new, set the tail ID to it.
 */
void ainode_tail_set(s32 nodeID) {
    if (nodeID != gAINodeTail[0]) {
        gAINodeTail[1] = gAINodeTail[0];
        gAINodeTail[0] = nodeID;
    }
}

/**
 * Return the last created AI node.
 */
UNUSED Object *ainode_tail(s32 *nodeID) {
    *nodeID = gAINodeTail[1];
    return gAINodes[0][gAINodeTail[1]];
}

/**
 * Return the AI node assigned to the given ID.
 */
Object *ainode_get(s32 nodeID) {
    if (nodeID >= 0 && nodeID < AINODE_COUNT) {
        return gAINodes[0][nodeID];
    }
    return NULL;
}

UNUSED void func_8001D248(UNUSED s32 arg0, UNUSED s32 arg1, UNUSED s32 arg2) {
}

/**
 * Applies shading properties to a global variable.
 * Presumably intended for level geometry, which supports shading, but never uses it.
 */
void set_world_shading(f32 ambient, f32 diffuse, s16 angleX, s16 angleY, s16 angleZ) {
#ifdef NATIVE_PORT
    set_shading_properties(
        &gWorldShading, ambient, diffuse, angleX, angleY, angleZ);
#else
    set_shading_properties((ShadeProperties *) &gWorldShading, ambient, diffuse, angleX, angleY, angleZ);
#endif
}

/**
 * Add values onto the existing properties of an objects shading.
 * Resets the shading based off the new values.
 */
UNUSED void add_shading_properties(Object *obj, f32 ambientChange, f32 diffuseChange, s16 angleX, s16 angleY,
                                   s16 angleZ) {
    if (obj->shading != NULL) {
        obj->shading->ambient += ambientChange;
        if (obj->shading->ambient < 0.0f) {
            obj->shading->ambient = 0.0f;
        } else if (obj->shading->ambient > 1.0f) {
            obj->shading->ambient = 1.0f;
        }
        obj->shading->diffuse += diffuseChange;
        if (obj->shading->diffuse < 0.0f) {
            obj->shading->diffuse = 0.0f;
        }
        if (obj->shading->diffuse >= 2.0f) {
            obj->shading->diffuse = 1.99f;
        }
        set_shading_properties(obj->shading, obj->shading->ambient, obj->shading->diffuse,
                               (obj->shading->unk22 + angleX), (obj->shading->unk24 + angleY),
                               (obj->shading->unk26 + angleZ));
        if (obj->header->shadeIntensityy != 0) {
            obj->shading->lightR = obj->header->unk3A;
            obj->shading->lightG = obj->header->unk3B;
            obj->shading->lightB = obj->header->unk3C;
            obj->shading->lightIntensity = obj->header->shadeIntensityy;
            obj->shading->lightDirX = -(obj->shading->shadowDirX >> 1);
            obj->shading->lightDirY = -(obj->shading->shadowDirY >> 1);
            obj->shading->lightDirZ = -(obj->shading->shadowDirZ >> 1);
        }
    }
}

void set_shading_properties(ShadeProperties *arg0, f32 ambient, f32 diffuse, s16 angleX, s16 angleY, s16 angleZ) {
    Vec3s angle;
    Vec3f velocityPos;

    arg0->unk22 = angleX;
    arg0->ambient = ambient;
    arg0->diffuse = diffuse;
    arg0->unk0 = 1.0f;
    arg0->unk24 = angleY;
    arg0->unk26 = angleZ;
    angle.z_rotation = angleX;
    angle.y_rotation = angleZ;
    angle.x_rotation = angleY;
    velocityPos.z = -16384.0f;
    velocityPos.x = 0.0f;
    velocityPos.y = 0.0f;
    vec3f_rotate(&angle, &velocityPos);
    arg0->shadowDirX = -velocityPos.x;
    arg0->shadowDirY = -velocityPos.y;
    arg0->shadowDirZ = -velocityPos.z;
    arg0->shadowR = 0;
    arg0->shadowG = 0;
    arg0->shadowB = 0;
}

/**
 * Take the normalised length of the position set by the perspective and set the world angle for the envmap.
 * Official name: setObjectViewNormal
 */
void update_envmap_position(f32 x, f32 y, f32 z) {
    f32 vecLength = sqrtf((x * x) + (y * y) + (z * z));
    f32 normalizedLength;
    if (vecLength != 0.0f) {
        normalizedLength = -8192.0f / vecLength;
        x *= normalizedLength;
        y *= normalizedLength;
        z *= normalizedLength;
    }
    gEnvmapPos[0].x = dkr_f32_to_s16_wrap(x);
    gEnvmapPos[0].y = dkr_f32_to_s16_wrap(y);
    gEnvmapPos[0].z = dkr_f32_to_s16_wrap(z);
}

/**
 * If the triangle batch allows for it, compute envmap normals for the mesh.
 * Some objects will prefer some extra additions on top before calculating, like light intensity.
 */
void obj_shade_fancy(ObjectModel *model, Object *object, s32 arg2, f32 intensity) {
    s16 environmentMappingEnabled;
    s32 dynamicLightingEnabled;
    s16 i;

    dynamicLightingEnabled = 0;
    environmentMappingEnabled = 0;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (DKR_PTR(TriangleBatchInfo, model->batches)[i].miscData != BATCH_VTX_COL) {
            dynamicLightingEnabled = -1; // This is a bit weird, but I guess it works.
        }
        if (DKR_PTR(TriangleBatchInfo, model->batches)[i].flags & RENDER_ENVMAP) {
            environmentMappingEnabled = -1;
        }
    }

    if (dynamicLightingEnabled) {
        // Calculates dynamic lighting for the object
        if (object->header->directionalPointLighting) {
            // Dynamic directional lighting for some objects (Intro diddy, Taj, T.T., Bosses)
            calc_dynamic_lighting_for_object_1(object, model, arg2, object, intensity, 1.0f);
        } else {
            // Dynamic ambient lighting for other objects (Racers, Rare logo, Wizpig face, etc.)
            calc_dynamic_lighting_for_object_2(object, model, arg2, intensity);
        }
    }

    if (environmentMappingEnabled) {
        // Calculates environment mapping for the object
        calc_env_mapping_for_object(model, object->trans.rotation.z_rotation, object->trans.rotation.x_rotation,
                                    object->trans.rotation.y_rotation);
    }
}

void calc_dynamic_lighting_for_object_1(Object *object, ObjectModel *model, s16 arg2, Object *anotherObject,
                                        f32 intensity, f32 arg5) {
    s16 normIdx;
    s16 j;
    s16 i;
    Vec3s objRot;
    s32 s6;
    s32 lightDirX, lightDirY, lightDirZ;    // 16.16 fixed point, normalized
    s32 shadowDirX, shadowDirY, shadowDirZ; // 16.16 fixed point, normalized
    s32 diffuseFactor;
    s32 ambientFactor;
    s32 lightIntensity;
    s32 shadeStrength;
    Vec3f direction;
    Vertex *vertices;
    Vec3s *normals;

    if (object->shading == NULL) {
        return;
    }

    vertices = object->curVertData;
    normals = DKR_PTR(Vec3s, model->normals);
    normIdx = 0;

    direction.x = -(f32)object->shading->lightDirX * 8.0f;
    direction.y = -(f32)object->shading->lightDirY * 8.0f;
    direction.z = -(f32)object->shading->lightDirZ * 8.0f;
    objRot.y_rotation = -object->trans.rotation.y_rotation;
    objRot.x_rotation = -object->trans.rotation.x_rotation;
    objRot.z_rotation = -object->trans.rotation.z_rotation;
    vec3f_rotate_ypr(&objRot, &direction);

    if (object->header->shadeIntensityy != 0 && arg2) {
        mtxf_transform_dir(get_projection_matrix_f32(), &direction, &direction);
    }

    lightDirX = -direction.x;
    lightDirY = -direction.y;
    lightDirZ = -direction.z;
    s6 = object->shading->lightIntensity;

    direction.x = (f32)object->shading->shadowDirX * 4.0f;
    direction.y = (f32)object->shading->shadowDirY * 4.0f;
    direction.z = (f32)object->shading->shadowDirZ * 4.0f;

    if (arg2) {
        mtxf_transform_dir(get_projection_matrix_f32(), &direction, &direction);
    }
    vec3f_rotate_ypr(&objRot, &direction);

    shadowDirX = direction.x;
    shadowDirY = direction.y;
    shadowDirZ = direction.z;

    ambientFactor = object->shading->ambient * object->shading->unk0 * 255.0f * intensity;
    diffuseFactor = object->shading->diffuse * object->shading->unk0 * 255.0f * intensity;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (DKR_PTR(TriangleBatchInfo, model->batches)[i].miscData != BATCH_VTX_COL) { // 0xFF means use vertex colors
            for (j = DKR_PTR(TriangleBatchInfo, model->batches)[i].verticesOffset; j < DKR_PTR(TriangleBatchInfo, model->batches)[i + 1].verticesOffset; j++) {
                // calculate lighting
                lightIntensity = (normals[normIdx].x * lightDirX + normals[normIdx].y * lightDirY +
                                  normals[normIdx].z * lightDirZ) >>
                                 13;
                if (lightIntensity > 0) {
                    lightIntensity = (lightIntensity * s6) >> 16;
                    if (lightIntensity > 255) {
                        lightIntensity = 255;
                    }
                } else {
                    lightIntensity = 0;
                }

                // calculate shading
                shadeStrength = (normals[normIdx].x * shadowDirX + normals[normIdx].y * shadowDirY +
                                 normals[normIdx].z * shadowDirZ) >>
                                13;
                if (shadeStrength > 0) {
                    shadeStrength = (shadeStrength * diffuseFactor) >> 16;
                    shadeStrength += ambientFactor;
                    if (shadeStrength > 255) {
                        shadeStrength = 255;
                    }
                } else {
                    shadeStrength = ambientFactor;
                }

                vertices[j].r = lightIntensity;
                vertices[j].g = lightIntensity;
                vertices[j].b = lightIntensity;
                vertices[j].a = shadeStrength;
                normIdx++;
            }
        } else if (DKR_PTR(TriangleBatchInfo, model->batches)[i].flags & RENDER_ENVMAP) {
            normIdx += DKR_PTR(TriangleBatchInfo, model->batches)[i + 1].verticesOffset - DKR_PTR(TriangleBatchInfo, model->batches)[i].verticesOffset;
        }
    }
}

void calc_env_mapping_for_object(ObjectModel *model, s16 zRot, s16 xRot, s16 yRot) {
    MtxS objRotMtxS32;
    MtxF objRotMtxF32;
    ObjectTransform objTrans;
    s16 k;
    s16 count;
    Triangle *triangles;
    Vec3s *model40Entries;
    s32 sp70;
    TextureHeader *tex;
    s16 shiftS;
    s16 maskS;
    s16 shiftT;
    s16 maskT;
    s16 i;
    s16 j;
    s16 var_v0;
    s16 var_v1;

    count = 0;
    triangles = DKR_PTR(Triangle, model->triangles);
    model40Entries = DKR_PTR(Vec3s, model->normals);
    objTrans.rotation.z_rotation = zRot;
    objTrans.rotation.x_rotation = xRot;
    objTrans.rotation.y_rotation = yRot;
    objTrans.x_position = 0.0f;
    objTrans.y_position = 0.0f;
    objTrans.z_position = 0.0f;
    objTrans.scale = 1.0f;
    mtxf_from_transform(&objRotMtxF32, &objTrans);
    mtxf_to_mtxs(&objRotMtxF32, &objRotMtxS32);

    for (i = 0; i < model->numberOfBatches; i++) {
        if (DKR_PTR(TriangleBatchInfo, model->batches)[i].flags & RENDER_ENVMAP) {
            sp70 = ((DKR_PTR(TriangleBatchInfo, model->batches)[i].flags & RENDER_UNK_0020000) | RENDER_ENVMAP) ^ RENDER_ENVMAP;
            tex = DKR_PTR(TextureHeader, DKR_PTR(TextureInfo, model->textures)[DKR_PTR(TriangleBatchInfo, model->batches)[i].textureIndex].texture);
            k = 0;

            switch (tex->width) {
                case 0x80:
                    SET_SHIFT_AND_MASK(shiftS, maskS, 4);
                    break;
                case 0x40:
                    SET_SHIFT_AND_MASK(shiftS, maskS, 5);
                    break;
                case 0x20:
                    SET_SHIFT_AND_MASK(shiftS, maskS, 6);
                    break;
                default:
                    SET_SHIFT_AND_MASK(shiftS, maskS, 7);
                    break;
            }
            switch (tex->height) {
                case 0x80:
                    SET_SHIFT_AND_MASK(shiftT, maskT, 4);
                    break;
                case 0x40:
                    SET_SHIFT_AND_MASK(shiftT, maskT, 5);
                    break;
                case 0x20:
                    SET_SHIFT_AND_MASK(shiftT, maskT, 6);
                    break;
                default:
                    SET_SHIFT_AND_MASK(shiftT, maskT, 7);
                    break;
            }

            for (j = DKR_PTR(TriangleBatchInfo, model->batches)[i].verticesOffset; j < DKR_PTR(TriangleBatchInfo, model->batches)[i + 1].verticesOffset; j++, k++) {
                gEnvmapPos[1].x = model40Entries[count].x;
                gEnvmapPos[1].y = model40Entries[count].y;
                gEnvmapPos[1].z = model40Entries[count].z;
                count++;
                mtxs_transform_dir(&objRotMtxS32, &gEnvmapPos[1]);
                if (sp70 == 0) {
                    vec3s_reflect(&gEnvmapPos[0], &gEnvmapPos[1]);
                }
                var_v0 = gEnvmapPos[1].x;
                var_v1 = gEnvmapPos[1].y;
                if (var_v0 > 0) {
                    var_v0--;
                }
                if (var_v1 > 0) {
                    var_v1--;
                }
                var_v0 = (var_v0 * 4) + 0x8000;
                var_v1 = (var_v1 * 4) + 0x8000;
                D_8011AF68[k].u = (var_v0 >> shiftS) & maskS;
                D_8011AF68[k].v = (var_v1 >> shiftT) & maskT;
            }

            for (j = DKR_PTR(TriangleBatchInfo, model->batches)[i].facesOffset; j < DKR_PTR(TriangleBatchInfo, model->batches)[i + 1].facesOffset; j++) {
                triangles[j].uv0.u = D_8011AF68[triangles[j].vi0].u;
                triangles[j].uv0.v = D_8011AF68[triangles[j].vi0].v;
                triangles[j].uv1.u = D_8011AF68[triangles[j].vi1].u;
                triangles[j].uv1.v = D_8011AF68[triangles[j].vi1].v;
                triangles[j].uv2.u = D_8011AF68[triangles[j].vi2].u;
                triangles[j].uv2.v = D_8011AF68[triangles[j].vi2].v;
            }
        } else if (DKR_PTR(TriangleBatchInfo, model->batches)[i].miscData < BATCH_VTX_COL) {
            count += DKR_PTR(TriangleBatchInfo, model->batches)[i + 1].verticesOffset - DKR_PTR(TriangleBatchInfo, model->batches)[i].verticesOffset;
        }
    }
}

/**
 * Find the racer object representing the player and directly set position and angle to new values.
 */
UNUSED void set_racer_position_and_angle(s16 player, s16 *x, s16 *y, s16 *z, s16 *angleZ, s16 *angleX, s16 *angleY) {
    Object *obj;
    Object_Racer *racer;
    s32 i;

    for (i = 0; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (!(obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            if (obj->behaviorId == BHV_RACER) {
                racer = obj->racer;
                if (player == racer->playerIndex) {
                    *x = obj->trans.x_position;
                    *y = obj->trans.y_position;
                    *z = obj->trans.z_position;
                    *angleZ = obj->trans.rotation.z_rotation;
                    *angleX = obj->trans.rotation.x_rotation;
                    *angleY = obj->trans.rotation.y_rotation;
                    i = gObjectCount; // Feels like it should be a break instead.
                }
            }
        }
    }
}

/**
 * Returns a pointer to the asset in the misc. section. If index is out of range, then this
 * function just returns the pointer to gAssetsMiscSection.
 * Official name: objGetTable
 */
s32 *get_misc_asset(s32 index) {
    if (index < 0 || index >= gAssetsMiscTableLength) {
        return gAssetsMiscSection;
    }
    return (s32 *) &gAssetsMiscSection[gAssetsMiscTable[index]];
}

#ifdef NATIVE_PORT
/**
 * Byte length of an ASSET_MISC sub-asset. gAssetsMiscTable entries are s32-WORD
 * offsets into gAssetsMiscSection (see get_misc_asset), so the byte length of
 * sub-asset `index` is (table[index + 1] - table[index]) * sizeof(s32).
 * Returns 0 when the index is out of range, mirroring get_misc_asset's
 * defensive contract. Used by the on-demand asset_swap_misc_* converters to
 * bound their walk to the blob instead of trusting a count field.
 */
s32 get_misc_asset_size(s32 index) {
    s32 words;

    if (index < 0 || index + 1 > gAssetsMiscTableLength) {
        return 0;
    }
    words = gAssetsMiscTable[index + 1] - gAssetsMiscTable[index];
    if (words <= 0) {
        return 0;
    }
    return words * (s32) sizeof(s32);
}
#endif

#ifdef NATIVE_PORT
/*
 * The ASSET_MISC section is heterogeneous and deliberately left un-normalized by
 * asset_swap (its sub-assets have different types). Several sub-assets are, however,
 * big-endian 32-bit word (f32/s32) arrays that the game reads directly — notably the
 * per-vehicle wheel-collision points (ObjectHeader.unk5C/unk5D) and the stone-surface
 * grip table (ASSET_MISC_32). Read little-endian, a value like -10.0f (0xC1200000)
 * becomes 0x000020C1 — a ~0 denormal — so wheel offsets/radii come out all zero and a
 * racer's wheels never ground, leaving it unable to accelerate. Byte-swap such a
 * word-array sub-asset in place, once (dedup keyed on the section pointer so it resets
 * if the section is ever reloaded). Note gAssetsMiscTable offsets are s32-word indices
 * into gAssetsMiscSection (see get_misc_asset), not byte offsets.
 */
static s32 *sMiscSwapSection = NULL;
static u8 sMiscSwapDone[512];

/*
 * Locate a sub-asset without claiming it. Returns NULL when the index is out of
 * range or the table entry is the terminator / an implausible range. Used by the
 * per-field swizzles and by the post-swap verification, neither of which wants
 * the one-shot dedup.
 */
static u8 *dkr_misc_subasset(s32 index, s32 *byteLenOut) {
    s32 wstart, wend;

    if (gAssetsMiscSection == NULL || index < 0 || index + 1 > gAssetsMiscTableLength) {
        return NULL;
    }
    wstart = gAssetsMiscTable[index];
    wend = gAssetsMiscTable[index + 1];
    if (wstart < 0 || wend <= wstart) {
        return NULL;
    }
    *byteLenOut = (wend - wstart) * (s32) sizeof(s32);
    return (u8 *) &gAssetsMiscSection[wstart];
}

/*
 * Claim a sub-asset for a one-shot in-place byteswap. Returns FALSE if the index
 * is unusable or has already been swapped (by ANY of the swap kinds — the flag is
 * per index, not per kind, so a sub-asset is swapped exactly once in exactly one
 * way; two kinds claiming the same index is a bug in the lists, not a double
 * swap). On TRUE, [*wstartOut, *wendOut) is the sub-asset's s32-WORD range.
 */
static s32 dkr_misc_swap_claim(s32 index, s32 *wstartOut, s32 *wendOut) {
    s32 wstart, wend;

    if (index < 0 || index + 1 > gAssetsMiscTableLength || index >= (s32) sizeof(sMiscSwapDone)) {
        return FALSE;
    }
    if (sMiscSwapSection != gAssetsMiscSection) {
        s32 k;
        sMiscSwapSection = gAssetsMiscSection;
        for (k = 0; k < (s32) sizeof(sMiscSwapDone); k++) {
            sMiscSwapDone[k] = 0;
        }
    }
    if (sMiscSwapDone[index]) {
        return FALSE;
    }
    sMiscSwapDone[index] = 1;

    wstart = gAssetsMiscTable[index];
    wend = gAssetsMiscTable[index + 1];
    if (wend <= wstart || wend < 0) {
        return FALSE; /* terminator / implausible range — leave untouched */
    }
    *wstartOut = wstart;
    *wendOut = wend;
    return TRUE;
}

void dkr_misc_swap_words(s32 index) {
    s32 wstart, wend, w;
    u32 *base;

    if (!dkr_misc_swap_claim(index, &wstart, &wend)) {
        return;
    }
    base = (u32 *) gAssetsMiscSection;
    for (w = wstart; w < wend; w++) {
        u32 v = base[w];
        base[w] = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
                  ((v & 0xFF000000u) >> 24);
    }
}

/*
 * The 16-bit sibling of dkr_misc_swap_words(), for sub-assets the game reads only
 * as s16[]/u16[]. A 32-bit word swap is WRONG for these: besides reversing each
 * halfword it also transposes the two halfwords within every word, which for
 * ASSET_MISC_RUMBLE_DATA would swap strength with duration in each {strength,
 * duration} pair. Sub-asset byte lengths are always a multiple of 4 (the MISC
 * table stores s32 word offsets), so the halfword walk never has a partial tail.
 */
void dkr_misc_swap_halfwords(s32 index) {
    s32 wstart, wend, h;
    u16 *base;

    if (!dkr_misc_swap_claim(index, &wstart, &wend)) {
        return;
    }
    base = (u16 *) gAssetsMiscSection;
    for (h = wstart * 2; h < wend * 2; h++) { /* s32-word range -> halfword range */
        u16 v = base[h];
        base[h] = (u16) ((v << 8) | (v >> 8));
    }
}

/*
 * ===================== ASSET_MISC endianness normalization =====================
 *
 * ASSET_MISC is one section holding many unrelated sub-assets, so
 * asset_swap_normalize() deliberately punts it — a single blanket rule is unsafe.
 * Instead every sub-asset whose type is known is normalized ONCE at section-load
 * time, from the explicit lists in dkr_misc_normalize_tables() below, by the swap
 * kind its layout requires:
 *
 *   kind             primitive                        covers
 *   32-bit words     dkr_misc_swap_words()            f32[]/s32[] arrays
 *   16-bit halfwords dkr_misc_swap_halfwords()        s16[]/u16[] arrays
 *   per-field, in place  dkr_misc_swap_shield_records()   SHIELD_DATA (s16+f32 mix)
 *   per-field + stride copy  asset_swap_misc_boost()      ASSET_MISC_20 (see below)
 *
 * Choosing the wrong kind is silently destructive, so the kinds are not
 * interchangeable: a 32-bit swap on a 16-bit array also transposes the two
 * halfwords inside every word, and a 16-bit swap on a float array byte-reverses
 * each half of it. The shared sMiscSwapDone[] flag is per INDEX, not per kind, so
 * a sub-asset is swapped exactly once in exactly one way.
 *
 * Why explicit lists and not lazy per-call-site swaps: this used to be done ad hoc
 * at the few call sites that had been noticed (racer weight/handling and the
 * per-vehicle unk5C/unk5D arrays). Every table nobody had happened to look at
 * stayed big-endian, and a BE f32 read natively is a *denormal*, not an obvious
 * zero or garbage — e.g. 600.0f (0x44160000) reads back as 0x00001644 =
 * 7.99e-42. Denormals mostly behave like 0.0, so a wrong table is silent right
 * up until something DIVIDES by it: func_80050A28() (racer.c) computes
 *     racer->lateral_velocity += (velocity * gCurrentStickX) / MISC_8[characterId];
 * which stays 0/denormal = 0 while the stick is centred and then overflows to
 * ±inf on the first steering input at speed. That inf reaches obj->trans, the
 * racer leaves the world, the BSP walk returns no visible segments and the whole
 * scene stops drawing — with no crash. See docs/OPEN_ITEMS.md.
 *
 * So: add the sub-asset to the right list here rather than swapping at a consumer,
 * and give it a plausibility bound in dkr_misc_verify_tables() — several of these
 * tables are on paths no test fixture reaches, where a bound is the only thing
 * standing between a wrong swap and a silent wrong table.
 *
 * Every entry was verified by dumping the raw section (tools/dump_misc_asset.py)
 * and checking that the swapped values decode to plausible ones for the consumer.
 */
/*
 * ASSET_MISC_SHIELD_DATA (21) — RacerShieldGfx[30], the per-character shield
 * effect placement, read by render_racer_shield() as
 * `shield[vehicleID * NUMBER_OF_CHARACTERS + racerIndex]`.
 *
 * Needs a PER-FIELD swizzle: the record packs four s16 followed by two f32, so a
 * blanket 32-bit word swap (dkr_misc_swap_words) would transpose x<->y and
 * z<->y_offset, and a blanket 16-bit swap (dkr_misc_swap_halfwords) would split
 * both floats into byte-reversed halves.
 *
 * Unlike the boost table (ASSET_MISC_20) this one needs NO stride conversion and
 * no copy out: the record is four s16 + two f32 with no trailing pointer fields,
 * so sizeof(RacerShieldGfx) == the 0x10 on-disk stride on the N64 and on LP64
 * alike (asserted below). The swizzle is therefore in place, like the word and
 * halfword lists.
 *
 * Read in the wrong byte order this one is NOT the usual silent denormal: `scale`
 * decodes to about -4.29e8 (0x3ECCCCCD read little-endian is 0xCDCCCC3E), so an
 * active shield would scale the effect object by a huge NEGATIVE factor;
 * turnSpeed does go denormal (1.0f -> 4.6e-41) and y_position/y_offset read
 * 3072/1024 instead of 12/4.
 */
#define DKR_SHIELD_ROM_STRIDE 0x10
#define DKR_SHIELD_ENTRIES (NUMBER_OF_PLAYER_VEHICLES * NUMBER_OF_CHARACTERS)

_Static_assert(sizeof(RacerShieldGfx) == DKR_SHIELD_ROM_STRIDE,
               "RacerShieldGfx must match its 0x10 on-disk stride (no padding, no pointers)");
_Static_assert(offsetof(RacerShieldGfx, x_position) == 0x0, "RacerShieldGfx.x_position@0x0");
_Static_assert(offsetof(RacerShieldGfx, y_position) == 0x2, "RacerShieldGfx.y_position@0x2");
_Static_assert(offsetof(RacerShieldGfx, z_position) == 0x4, "RacerShieldGfx.z_position@0x4");
_Static_assert(offsetof(RacerShieldGfx, y_offset) == 0x6, "RacerShieldGfx.y_offset@0x6");
_Static_assert(offsetof(RacerShieldGfx, scale) == 0x8, "RacerShieldGfx.scale@0x8");
_Static_assert(offsetof(RacerShieldGfx, turnSpeed) == 0xC, "RacerShieldGfx.turnSpeed@0xC");

static void dkr_misc_swap_shield_records(void) {
    s32 byteLen = 0;
    s32 wstart, wend, i, n;
    u8 *base = dkr_misc_subasset(ASSET_MISC_SHIELD_DATA, &byteLen);

    if (base == NULL) {
        return;
    }
    /* Claim through the shared dedup so the record swizzle can never run twice
     * (and so a stray dkr_misc_swap_words on this index is a no-op, not a
     * scramble on top of a correct swizzle). */
    if (!dkr_misc_swap_claim(ASSET_MISC_SHIELD_DATA, &wstart, &wend)) {
        return;
    }
    if ((byteLen % DKR_SHIELD_ROM_STRIDE) != 0) {
        fprintf(stderr, "[FATAL] ASSET_MISC_SHIELD_DATA length %d is not a multiple of the 0x%X record stride\n",
                (int) byteLen, DKR_SHIELD_ROM_STRIDE);
        abort();
    }
    n = byteLen / DKR_SHIELD_ROM_STRIDE;
    if (n != DKR_SHIELD_ENTRIES) {
        fprintf(stderr, "[FATAL] ASSET_MISC_SHIELD_DATA has %d records, expected %d (%d vehicles x %d characters)\n",
                (int) n, (int) DKR_SHIELD_ENTRIES, (int) NUMBER_OF_PLAYER_VEHICLES, (int) NUMBER_OF_CHARACTERS);
        abort();
    }
    for (i = 0; i < n; i++) {
        u8 *rec = base + (i * DKR_SHIELD_ROM_STRIDE);
        s32 k;
        for (k = 0; k < 4; k++) { /* x_position, y_position, z_position, y_offset */
            u16 *h = (u16 *) (rec + (k * 2));
            u16 v = *h;
            *h = (u16) ((v << 8) | (v >> 8));
        }
        for (k = 0; k < 2; k++) { /* scale, turnSpeed */
            u32 *w = (u32 *) (rec + 0x8 + (k * 4));
            u32 v = *w;
            *w = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
                 ((v & 0xFF000000u) >> 24);
        }
    }
}

/*
 * Post-swap plausibility verification.
 *
 * None of the four sub-assets normalized by the halfword list and the shield
 * swizzle is reached by any fixture in the regression matrix (the shield needs a
 * racer to actually hold a shield, RUMBLE_DATA needs a rumble pak, and MISC_23 /
 * GHOST_UNLOCK_TIMES sit on save-file and time-trial-record paths). "0 crashes
 * across the matrix" therefore proves nothing about them, which is exactly the
 * trap that let ASSET_MISC_8 stay big-endian for three waves. So each table is
 * checked against a bound that the CORRECT decode satisfies with room to spare
 * and the byte-reversed decode violates outright — measured on us.v80:
 *
 *   table          invariant                 correct   byte-reversed
 *   SHIELD_DATA    0 < scale,turnSpeed < 100   0.4/1.0  scale -4.29e8      -> trips
 *   RUMBLE_DATA    every u16 <= 255            max 100  max 25600          -> trips
 *   GHOST_TIMES    every u16 <= 36000          max 8520 10 of 20 over      -> trips
 *   MISC_23        time fields <= 36000        max 10800 48 of 288 over    -> trips
 *
 * MISC_23's initials fields are deliberately NOT bounded — they are packed
 * character triples, so no range test discriminates them (byte-reversed they peak
 * at 20749, under any useful bound). The time fields alone are a strong enough
 * discriminator. A comparison against hardcoded expected VALUES was rejected as
 * ROM-version specific; these bounds hold for any plausible table.
 */
#define DKR_MISC_MAX_TIME_FRAMES 36000 /* 10 minutes at 60 Hz */
#define DKR_MISC_MAX_RUMBLE 255        /* strength is an amplitude, duration is in frames */

static void dkr_misc_verify_halfword_bound(s32 index, const char *name, u32 bound, s32 stride, s32 phase) {
    s32 byteLen = 0;
    const u8 *base = dkr_misc_subasset(index, &byteLen);
    s32 n, i;

    if (base == NULL) {
        return;
    }
    n = byteLen / 2;
    for (i = 0; i < n; i++) {
        u16 v;
        if (stride > 1 && (i % stride) != phase && (i % stride) != phase + 2) {
            continue; /* MISC_23: only the two time fields of each 4-entry group */
        }
        v = *(const u16 *) (base + (i * 2));
        if ((u32) v > bound) {
            fprintf(stderr,
                    "[FATAL] %s[%d] = %u exceeds %u - the sub-asset looks byte-reversed "
                    "(is it missing from the swap list in dkr_misc_normalize_tables?)\n",
                    name, (int) i, (unsigned) v, (unsigned) bound);
            abort();
        }
    }
}

static void dkr_misc_verify_tables(void) {
    s32 byteLen = 0;
    const u8 *base;
    s32 i, n;

    /* SHIELD_DATA — the two f32 fields must be sane positive scales. NaN and inf
     * both fail these comparisons, so no <math.h> isfinite() is needed. */
    base = dkr_misc_subasset(ASSET_MISC_SHIELD_DATA, &byteLen);
    if (base != NULL && (byteLen % DKR_SHIELD_ROM_STRIDE) == 0) {
        n = byteLen / DKR_SHIELD_ROM_STRIDE;
        for (i = 0; i < n; i++) {
            const RacerShieldGfx *r = (const RacerShieldGfx *) (base + (i * DKR_SHIELD_ROM_STRIDE));
            if (!(r->scale > 0.0f && r->scale < 100.0f) || !(r->turnSpeed > 0.0f && r->turnSpeed < 100.0f)) {
                fprintf(stderr,
                        "[FATAL] SHIELD_DATA[%d]: scale=%g turnSpeed=%g is not a plausible shield record "
                        "- the sub-asset looks byte-reversed\n",
                        (int) i, (double) r->scale, (double) r->turnSpeed);
                abort();
            }
        }
    }

    dkr_misc_verify_halfword_bound(ASSET_MISC_RUMBLE_DATA, "RUMBLE_DATA", DKR_MISC_MAX_RUMBLE, 1, 0);
    dkr_misc_verify_halfword_bound(ASSET_MISC_GHOST_UNLOCK_TIMES, "GHOST_UNLOCK_TIMES",
                                   DKR_MISC_MAX_TIME_FRAMES, 1, 0);
    /* MISC_23 is groups of 4: {courseTime, courseInitials, flapTime, flapInitials};
     * bound entries 0 and 2 of each group (see clear_lap_records). */
    dkr_misc_verify_halfword_bound(ASSET_MISC_23, "MISC_23", DKR_MISC_MAX_TIME_FRAMES, 4, 0);
}

/* One bounded line per newly-normalized table, so a future regression is visible
 * in a trace rather than silent. Matches the boost_table: line. */
static void dkr_misc_trace_tables(void) {
    s32 byteLen = 0;
    const u8 *base;
    s32 i, n;

    if (!mdkr_trace_enabled()) {
        return;
    }
    base = dkr_misc_subasset(ASSET_MISC_SHIELD_DATA, &byteLen);
    if (base != NULL && (byteLen % DKR_SHIELD_ROM_STRIDE) == 0) {
        const RacerShieldGfx *r = (const RacerShieldGfx *) base;
        n = byteLen / DKR_SHIELD_ROM_STRIDE;
        fprintf(stderr, "[TRACE] shield_table: %d records; [0] pos=(%d,%d,%d) y_off=%d scale=%.4f turnSpeed=%.4f\n",
                (int) n, (int) r->x_position, (int) r->y_position, (int) r->z_position, (int) r->y_offset,
                (double) r->scale, (double) r->turnSpeed);
    }
    base = dkr_misc_subasset(ASSET_MISC_RUMBLE_DATA, &byteLen);
    if (base != NULL) {
        n = byteLen / 4; /* {strength, duration} pairs */
        fprintf(stderr, "[TRACE] rumble_table: %d pairs;", (int) n);
        for (i = 0; i < n && i < 6; i++) {
            fprintf(stderr, " %u/%u", (unsigned) *(const u16 *) (base + (i * 4)),
                    (unsigned) *(const u16 *) (base + (i * 4) + 2));
        }
        fprintf(stderr, "%s\n", (n > 6) ? " ..." : "");
    }
    base = dkr_misc_subasset(ASSET_MISC_23, &byteLen);
    if (base != NULL) {
        n = byteLen / 2;
        fprintf(stderr, "[TRACE] misc23_table: %d u16 (%d levels x 12); [0] course=%u/%u lap=%u/%u\n", (int) n,
                (int) (n / 12), (unsigned) *(const u16 *) (base + 0), (unsigned) *(const u16 *) (base + 2),
                (unsigned) *(const u16 *) (base + 4), (unsigned) *(const u16 *) (base + 6));
    }
    base = dkr_misc_subasset(ASSET_MISC_GHOST_UNLOCK_TIMES, &byteLen);
    if (base != NULL) {
        n = byteLen / 2;
        fprintf(stderr, "[TRACE] ghost_unlock_times: %d entries;", (int) n);
        for (i = 0; i < n && i < 8; i++) {
            fprintf(stderr, " %u", (unsigned) *(const u16 *) (base + (i * 2)));
        }
        fprintf(stderr, "%s\n", (n > 8) ? " ..." : "");
    }
    fflush(stderr);
}

void dkr_misc_normalize_tables(void) {
    static const s16 sWordArrayMiscIds[] = {
        ASSET_MISC_4,  /* f32[10] per-character model-scale multiplier (objects.c) */
        ASSET_MISC_8,  /* f32[10] per-character steer-slide divisor (racer.c func_80050A28) */
        ASSET_MISC_RACER_WEIGHT,     /* f32[10] */
        ASSET_MISC_RACER_HANDLING,   /* f32[10] */
        ASSET_MISC_RACER_UNUSED_11,  /* f32[10] */
        ASSET_MISC_17, /* f32[3]  taj-challenge unk124 values (racer.c) */
        ASSET_MISC_18, /* f32[10] challenge unk124 values (racer.c) */
        ASSET_MISC_MAGNET_DATA, /* f32[3][5] magnet gfx x/y/z/scale/shear (objects.c) */
        ASSET_MISC_32, /* f32[5]  stone-surface top-speed multiplier by wheel count */
        /* f32[16] per-vehicle acceleration curves, reached via ObjectHeader.unk5C */
        ASSET_MISC_RACERACCELERATION_UNKNOWN0, ASSET_MISC_RACERACCELERATION_DIDDY,
        ASSET_MISC_RACERACCELERATION_TT, ASSET_MISC_RACERACCELERATION_UNKNOWN1,
        ASSET_MISC_RACERACCELERATION_KRUNCH, ASSET_MISC_RACERACCELERATION_BUMPER,
        ASSET_MISC_RACERACCELERATION_TIPTUP, ASSET_MISC_RACERACCELERATION_CONKER,
        ASSET_MISC_RACERACCELERATION_TIMBER, ASSET_MISC_RACERACCELERATION_BANJO,
        ASSET_MISC_RACERACCELERATION_DRUMSTICK, ASSET_MISC_RACERACCELERATION_PIPSY,
        ASSET_MISC_RACERACCELERATION_UNKNOWN2,
        /* f32[4][4] per-vehicle wheel-collision points, reached via ObjectHeader.unk5D */
        ASSET_MISC_51, ASSET_MISC_52, ASSET_MISC_53, ASSET_MISC_54, ASSET_MISC_55,
        ASSET_MISC_RACER_HITBOX_SIZE, /* f32[14] collision radius by vehicle id (racer.c) */
    };
    /*
     * Sub-assets the game reads ONLY as s16[]/u16[]. These must not go in the word
     * list above: a 32-bit swap additionally transposes the two halfwords in every
     * word (for RUMBLE_DATA that swaps strength with duration in each pair).
     */
    static const s16 sHalfWordArrayMiscIds[] = {
        ASSET_MISC_RUMBLE_DATA, /* u16[38] = 19 {strength, duration} pairs (save_data.c) */
        ASSET_MISC_23,          /* u16[576] = 48 levels x 3 files x {courseTime, initials,
                                 * flapTime, initials} default records (thread3_main.c
                                 * clear_lap_records) */
        ASSET_MISC_GHOST_UNLOCK_TIMES, /* u16[20] staff times, 1:1 with the 20
                                        * MAIN_TRACKS_IDS entries (objects.c) */
    };
    s32 i;

    for (i = 0; i < (s32) (sizeof(sWordArrayMiscIds) / sizeof(sWordArrayMiscIds[0])); i++) {
        dkr_misc_swap_words(sWordArrayMiscIds[i]);
    }
    for (i = 0; i < (s32) (sizeof(sHalfWordArrayMiscIds) / sizeof(sHalfWordArrayMiscIds[0])); i++) {
        dkr_misc_swap_halfwords(sHalfWordArrayMiscIds[i]);
    }
    /* Records needing a per-field swizzle. ASSET_MISC_20 (boost) is NOT here — it
     * also needs an LP64 stride conversion, so it is converted out to a native
     * array by dkr_boost_table() instead of in place. */
    dkr_misc_swap_shield_records();

    dkr_misc_verify_tables();
    dkr_misc_trace_tables();
}

/*
 * ASSET_MISC_20 — the boost / exhaust graphics table (Object_Boost[10]).
 *
 * The raw sub-asset cannot be used directly on this host, for two independent
 * reasons (both measured; see docs/OPEN_ITEMS.md and the field map in
 * platform/asset_swap.c):
 *
 *  1. ENDIANNESS. ASSET_MISC is heterogeneous and therefore punted by
 *     asset_swap_normalize(), so the record is still big-endian. Read natively,
 *     every f32 is a ~0 denormal and spriteId/textureId are byte-reversed.
 *     dkr_misc_swap_words() cannot be used because the record packs an s16 pair
 *     at 0x6C and four u8/s8 at 0x70 — a blanket 32-bit word swap scrambles
 *     both. asset_swap_misc_boost() does the per-field swizzle instead.
 *
 *  2. STRIDE. The ON-DISK stride is 0x80; sizeof(Object_Boost) is 0x88 on LP64
 *     because `Sprite *sprite` (0x78) and `TextureHeader *tex` (0x7C on disk)
 *     are 4-byte fields on the N64 and 8-byte host pointers here. So
 *     ((Object_Boost *) rawBlob)[i] is misaligned for every i > 0 even after a
 *     correct byteswap. The blob lives inside gAssetsMiscSection and cannot be
 *     expanded in place, so the table is copied out to this native array.
 *
 * Before this converter existed the decoded spriteIds were literally halves of
 * neighbouring float words (12032, 0, 0, -16319, -32705, 66, 16451, 0, 16450,
 * 0), tex_load_sprite() returned NULL for them, and render_sprite_billboard()
 * segfaulted on sprite->numberOfFrames the first time a racer boosted.
 *
 * The game WRITES into this table (runtime animation state at 0x70..0x77 plus
 * the two loaded resource pointers), so the native array is the single source
 * of truth from here on and is built exactly once per gAssetsMiscSection load —
 * rebuilding it mid-session would wipe that state. Call it via GET_BOOST_TABLE()
 * (objects.h), which is a plain get_misc_asset() cast on the N64 build.
 */
#define DKR_BOOST_ROM_STRIDE 0x80 /* on-disk record size (see asset_swap.c) */
#define DKR_BOOST_MAX_ENTRIES NUMBER_OF_CHARACTERS /* us.v80 ships exactly 10 */

/* Lock the host layout this conversion depends on. The 0x00..0x77 prefix must
 * stay byte-for-byte offset-identical to the on-disk record — asset_swap_misc_boost()
 * memcpy()s it wholesale and then swaps at ON-DISK offsets. Only the two
 * trailing pointer fields are allowed to move (that is the whole point). */
_Static_assert(sizeof(Object_Boost_Inner) == 0x24, "Object_Boost_Inner must be 9 f32 (0x24)");
_Static_assert(offsetof(Object_Boost, carBoostData) == 0x00, "Object_Boost.carBoostData@0x00");
_Static_assert(offsetof(Object_Boost, hovercraftBoostData) == 0x24, "Object_Boost.hovercraftBoostData@0x24");
_Static_assert(offsetof(Object_Boost, flyingBoostData) == 0x48, "Object_Boost.flyingBoostData@0x48");
_Static_assert(offsetof(Object_Boost, spriteId) == 0x6C, "Object_Boost.spriteId@0x6C");
_Static_assert(offsetof(Object_Boost, textureId) == 0x6E, "Object_Boost.textureId@0x6E");
_Static_assert(offsetof(Object_Boost, unk70) == 0x70, "Object_Boost.unk70@0x70");
_Static_assert(offsetof(Object_Boost, unk71) == 0x71, "Object_Boost.unk71@0x71");
_Static_assert(offsetof(Object_Boost, unk72) == 0x72, "Object_Boost.unk72@0x72");
_Static_assert(offsetof(Object_Boost, unk73) == 0x73, "Object_Boost.unk73@0x73");
_Static_assert(offsetof(Object_Boost, unk74) == 0x74, "Object_Boost.unk74@0x74");
_Static_assert(offsetof(Object_Boost, sprite) == 0x78, "Object_Boost.sprite@0x78 (end of the copied prefix)");
/* On the N64 the host struct IS the on-disk record; on LP64 it is 8 bytes larger.
 * Both are fine — but nothing between 0x00 and 0x78 may ever gain padding. */
_Static_assert(sizeof(Object_Boost) >= DKR_BOOST_ROM_STRIDE, "Object_Boost smaller than its on-disk record");

static Object_Boost sBoostTable[DKR_BOOST_MAX_ENTRIES];
static s32 *sBoostTableSection = NULL;

Object_Boost *dkr_boost_table(void) {
    const void *raw;
    s32 byteLen;
    u32 entries;

    if (gAssetsMiscSection == NULL || gAssetsMiscTableLength <= ASSET_MISC_20 + 1) {
        return sBoostTable; /* zeroed; caller-safe */
    }
    if (sBoostTableSection == gAssetsMiscSection) {
        return sBoostTable; /* already converted — preserve runtime state */
    }
    sBoostTableSection = gAssetsMiscSection;

    /* gAssetsMiscTable entries are s32-WORD offsets into gAssetsMiscSection. */
    raw = (const void *) &gAssetsMiscSection[gAssetsMiscTable[ASSET_MISC_20]];
    byteLen = (gAssetsMiscTable[ASSET_MISC_20 + 1] - gAssetsMiscTable[ASSET_MISC_20]) * (s32) sizeof(s32);
    if (byteLen < 0 || (byteLen % DKR_BOOST_ROM_STRIDE) != 0) {
        fprintf(stderr, "[FATAL] ASSET_MISC_20 length %d is not a multiple of the 0x%X boost record stride\n",
                (int) byteLen, DKR_BOOST_ROM_STRIDE);
        abort();
    }
    entries = asset_swap_misc_boost(sBoostTable, (u32) sizeof(Object_Boost), DKR_BOOST_MAX_ENTRIES, raw,
                                    (u32) byteLen);
    if (entries != DKR_BOOST_MAX_ENTRIES) {
        fprintf(stderr, "[FATAL] ASSET_MISC_20 decoded %u boost entries, expected %d\n", (unsigned) entries,
                (int) DKR_BOOST_MAX_ENTRIES);
        abort();
    }
    if (mdkr_trace_enabled()) {
        /* Host stdio only: the game's own sprintf() (printf.c) is the N64
         * implementation and is not usable for host diagnostics. */
        u32 i;
        fprintf(stderr, "[TRACE] boost_table: %u entries; spriteId/textureId:", (unsigned) entries);
        for (i = 0; i < entries; i++) {
            fprintf(stderr, " %d/%d", (int) sBoostTable[i].spriteId, (int) sBoostTable[i].textureId);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return sBoostTable;
}
#endif

/**
 * If the bridge is raised, decrement its timer and return the remaining time.
 */
s32 is_bridge_raised(s32 index) {
    if (index >= 0 && index < 8) {
        if (gDrawbridgeTimers[0][index] > 0) {
            gDrawbridgeTimers[0][index]--;
        }
        return gDrawbridgeTimers[0][index];
    }
    return 0;
}

/**
 * Starts the bridge timer when a racer hits the bell switch.
 */
void start_bridge_timer(s32 index) {
    if (index >= 0 && index < 8) {
        gDrawbridgeTimers[0][index] = 8;
    }
}

/**
 * When the sound timer hits the correct value, write the objects position to the arguments.
 */
void obj_bridge_pos(s32 timing, f32 *x, f32 *y, f32 *z) {
    s32 i;
    Object *current_obj;
    *x = -32000.0f;
    *y = -32000.0f;
    *z = -32000.0f;
    for (i = 0; i < gObjectCount; i++) {
        current_obj = gObjPtrList[i];

        if (current_obj != NULL && !(current_obj->trans.flags & OBJ_FLAGS_PARTICLE) &&
            current_obj->behaviorId == BHV_RAMP_SWITCH && current_obj->properties.common.unk0 == timing) {
            *x = current_obj->trans.x_position;
            *y = current_obj->trans.y_position;
            *z = current_obj->trans.z_position;
        }
    }
}

/**
 * Return the index of the currently active cutscene.
 */
s16 cutscene_id(void) {
    return gCutsceneID;
}

/**
 * Set the current cutscene index.
 */
void cutscene_id_set(s32 cutsceneID) {
    gCutsceneID = cutsceneID;
}

void func_8001E45C(s32 cutsceneID) {
    if (cutsceneID != gCutsceneID) {
        gCutsceneID = cutsceneID;
        gPathUpdateOff = FALSE;
        D_8011AE7E = TRUE;
        if (get_game_mode() == GAMEMODE_MENU) {
            set_frame_blackout_timer();
        }
    }
}

/**
 * Returns the index of the standard object list.
 * Goes unused, since objGetObjList exists
 */
UNUSED s32 get_object_list_index(void) {
    return gObjectListStart;
}

void func_8001E4C4(void) {
    Object *obj;
    s32 lastObjCount;
    s32 curObjCount;
    s32 i;
    s32 j;
    LevelObjectEntry_Animation *entryAnimation;

    for (i = 0; i < gObjectCount; i++) {
        gObjPtrList[i]->trans.flags &= ~OBJ_FLAGS_UNK_2000;
    }
    for (i = 0; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (obj != NULL && !(obj->trans.flags & OBJ_FLAGS_PARTICLE) && obj->behaviorId == BHV_ANIMATION) {
            entryAnimation = &obj->level_entry->animation;
            if (entryAnimation->channel != gCutsceneID && entryAnimation->channel != 20) {
                obj->trans.flags |= OBJ_FLAGS_UNK_2000;
                if (obj->animTarget != NULL) {
                    obj->animTarget->trans.flags |= OBJ_FLAGS_UNK_2000;
                }
            }
        }
    }
    curObjCount = gObjectCount - 1;
    lastObjCount = curObjCount;
    for (i = 0; curObjCount >= i;) {
        for (j = 0; lastObjCount >= i && (j == 0);) {
            if (gObjPtrList[i]->trans.flags & OBJ_FLAGS_UNK_2000) {
                i++;
            } else {
                j = -1;
            }
        }
        for (j = 0; curObjCount >= 0 && j == 0;) {
            if (gObjPtrList[curObjCount]->trans.flags & OBJ_FLAGS_UNK_2000) {
                j = -1;
            } else {
                curObjCount--;
            }
        }
        if (i < curObjCount) {
            obj = gObjPtrList[i];
            gObjPtrList[i] = gObjPtrList[curObjCount];
            gObjPtrList[curObjCount] = obj;
            i++;
            curObjCount--;
        }
    }
    gObjectListStart = i;
    gFirstActiveObjectId = 0;
}

void func_8001E6EC(s8 arg0) {
    LevelObjectEntry_OverridePos *overridePosEntry;
    Object *overridePosObj;
    Object_OverridePos *overridePos;
    s32 i;
    s32 j;
    s32 someBool;
    Object *animTarget;

    for (i = 0; i < D_8011AE00; i++) {
        overridePosObj = D_8011ADD8[i];
        overridePosEntry = &overridePosObj->level_entry->overridePos;
        overridePos = overridePosObj->override_pos;
        if ((overridePosEntry->cutsceneId == gCutsceneID) || ((overridePosEntry->cutsceneId == 20))) {
            for (j = 0;
                 (j < D_8011AE78) && (overridePosEntry->behaviorId != D_8011AE74[j]->properties.animation.behaviourID);
                 j++) {}
            if (j != D_8011AE78 && D_8011AE74[j]->animTarget != NULL) {
                someBool = (D_8011AE74[j]->animTarget->collisionData != NULL) ? FALSE : TRUE;
                if (arg0 != someBool) {
                    animTarget = D_8011AE74[j]->animTarget;
                    overridePos->x = animTarget->trans.x_position;
                    overridePos->y = animTarget->trans.y_position;
                    overridePos->z = animTarget->trans.z_position;
                    overridePos->anim = animTarget;
                    animTarget->trans.x_position = overridePosObj->trans.x_position;
                    animTarget->trans.y_position = overridePosObj->trans.y_position;
                    animTarget->trans.z_position = overridePosObj->trans.z_position;
                }
            } else {
                overridePos->anim = NULL;
            }
        }
    }
    D_8011AE01 = FALSE;
}

void func_8001E89C(void) {
    s32 i;
    Object *obj;
    Object_OverridePos *obj64;

    // some flag, flips to 1 when loading a new zone
    if (D_8011AE01 != FALSE) {
        D_8011AE01 = FALSE;
        return;
    }

    // loading (boss) cutscene
    for (i = 0; i < D_8011AE00; i++) {
        obj = D_8011ADD8[i];
        obj64 = obj->override_pos;

        if (obj64->anim != NULL) {
            obj64->anim->trans.x_position = obj64->x;
            obj64->anim->trans.y_position = obj64->y;
            obj64->anim->trans.z_position = obj64->z;
        }
    }
}

void func_8001E93C(void) {
    s32 pad[3];
    LevelObjectEntry_OverridePos *overridePos;
    Object *obj;
    s32 numOfObjs;
    s32 pad2;
    s32 i;
    s32 stopLooping;
    s32 sp28 = 0;
    s16 animActorIndex1;
    s16 animActorIndex2;
    s32 var_a0;
    Object *animObj1;
    LevelObjectEntry_Animation *animation1;
    LevelObjectEntry_Animation *animation2;

    if (D_8011AE7E) {
        for (numOfObjs = 0; numOfObjs < D_8011AE78; numOfObjs++) {
            obj = D_8011AE74[numOfObjs];
            animation1 = &obj->level_entry->animation;
            if (obj->animTarget != NULL && animation1->channel != 20) {
                animObj1 = obj->animTarget;
                free_object(animObj1);
                obj->animTarget = NULL;
            }
        }
    }
    if (D_8011AD3E > 20) {
        D_8011AD3E = 0;
    }
    func_8001E4C4();
    numOfObjs = 0;
    for (i = 0; i < gObjectCount; i++) {
        if (gObjPtrList[i] != NULL) {
            if (!(gObjPtrList[i]->trans.flags & OBJ_FLAGS_PARTICLE)) {
                if (gObjPtrList[i]->behaviorId == BHV_OVERRIDE_POS) {
                    overridePos = &gObjPtrList[i]->level_entry->overridePos;
                    if (overridePos->cutsceneId == gCutsceneID ||
                        overridePos->cutsceneId == (CUTSCENE_SHERBET_ISLAND_BOSS | CUTSCENE_ADVENTURE_TWO)) {
                        D_8011ADD8[numOfObjs] = gObjPtrList[i];
                        numOfObjs++;
                    }
                }
            }
        }
    }
    D_8011AE00 = numOfObjs;
    D_8011AE01 = TRUE;

    D_8011AE78 = 0;
    numOfObjs = 0;
    for (i = gObjectListStart; i < gObjectCount; i++) {
        if (gObjPtrList[i] != NULL) {
            if (!(gObjPtrList[i]->trans.flags & OBJ_FLAGS_PARTICLE)) {
                if (gObjPtrList[i]->behaviorId == BHV_ANIMATION) {
                    D_8011AE74[numOfObjs] = gObjPtrList[i];
                    numOfObjs++;
                }
            }
        }
    }

    do {
        stopLooping = TRUE;
        for (i = 0; i < numOfObjs - 1; i++) {
            animation1 = &D_8011AE74[i]->level_entry->animation;
            animation2 = &D_8011AE74[i + 1]->level_entry->animation;
            animActorIndex1 = animation1->actorIndex;
            animActorIndex2 = animation2->actorIndex;

            if (animation1->channel == 20) {
                animActorIndex1 -= 400;
            }
            if (animation2->channel == 20) {
                animActorIndex2 -= 400;
            }

            if (animActorIndex2 < animActorIndex1) {
                animObj1 = D_8011AE74[i];
                D_8011AE74[i] = D_8011AE74[i + 1];
                D_8011AE74[i + 1] = animObj1;
                stopLooping = FALSE;
            } else if (animActorIndex1 == animActorIndex2) {
                if (animation2->order < animation1->order) {
                    animObj1 = D_8011AE74[i];
                    D_8011AE74[i] = D_8011AE74[i + 1];
                    D_8011AE74[i + 1] = animObj1;
                    stopLooping = FALSE;
                } else if (animation1->order == animation2->order &&
                           (D_8011AE74[i + 1]->properties.animation.action == 1 ||
                            D_8011AE74[i]->properties.animation.action == 2)) {
                    animObj1 = D_8011AE74[i];
                    D_8011AE74[i] = D_8011AE74[i + 1];
                    D_8011AE74[i + 1] = animObj1;
                    stopLooping = FALSE;
                }
            }
        }
    } while (stopLooping == FALSE);

    var_a0 = -101;
    for (i = 0; i < numOfObjs; i += 1) {
        animation1 = &D_8011AE74[i]->level_entry->animation;
        if (animation1->actorIndex != var_a0) {
            var_a0 = animation1->actorIndex;
            sp28 = 0;
        }
        animation1->order = sp28++; // It is possible that sp28 could not be initalized?
        D_8011AE74[i]->properties.animation.action = 0;
    }

    D_8011AE78 = numOfObjs;
    if (D_8011AE7E) {
        func_8001EE74();
    }
    D_8011AE7E = FALSE;
}

void func_8001EE74(void) {
    LevelObjectEntry_Animation *animation;
    Object *obj;
    s32 i;

    for (i = 0; i < D_8011AE78; i++) {
        obj = D_8011AE74[i];
        animation = &obj->level_entry->animation;
        if (obj->animTarget == NULL && animation->order == 0 && animation->objectIdToSpawn != -1) {
            func_8001F23C(obj, animation);
        }
        if (D_8011AD26 || animation->channel != 20) {
            if (obj->animTarget != NULL) {
                obj_init_animobject(obj, obj->animTarget);
            }
        }
    }
    D_8011AD26 = FALSE;
}

#ifdef NATIVE_PORT
static u32 sMdkrAnimTargetInitCount;
static u32 sMdkrAnimTargetInvalidCount;

static s32 mdkr_is_animated_behavior(s32 behaviorId) {
    switch (behaviorId) {
        case BHV_DINO_WHALE:
        case BHV_ANIMATED_OBJECT:
        case BHV_CAMERA_ANIMATION:
        case BHV_CAR_ANIMATION:
        case BHV_CHARACTER_SELECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_PARK_WARDEN_2:
        case BHV_ANIMATED_OBJECT_2:
        case BHV_WIZPIG_SHIP:
        case BHV_ANIMATED_OBJECT_3:
        case BHV_ANIMATED_OBJECT_4:
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
        case BHV_HIT_TESTER_3:
        case BHV_HIT_TESTER_4:
        case BHV_DOOR_OPENER:
        case BHV_PIG_ROCKETEER:
        case BHV_WIZPIG_GHOSTS:
            return TRUE;
        default:
            return FALSE;
    }
}

static s32 mdkr_anim_target_is_valid(const Object *obj, const Object_AnimatedObject *anim) {
    uintptr_t address = (uintptr_t) anim;
    uintptr_t arenaStart = (uintptr_t) g_dkrArenaBase;
    uintptr_t arenaEnd = arenaStart + g_dkrArenaSize;

    return obj != NULL && obj->header != NULL &&
           mdkr_is_animated_behavior(obj->behaviorId) &&
           obj->header->behaviorId == obj->behaviorId &&
           address >= arenaStart &&
           address <= arenaEnd - sizeof(*anim);
}

__attribute__((destructor))
static void mdkr_anim_target_report(void) {
    const char *trace = getenv("MDKR_TRACE");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        fprintf(stderr, "[TRACE] [ANIMTARGET] init=%u invalid=%u\n",
                sMdkrAnimTargetInitCount, sMdkrAnimTargetInvalidCount);
    }
}
#endif

void obj_init_animobject(Object *animationObj, Object *animatedObj) {
    LevelObjectEntry_Animation *animEntry;
    Object_AnimatedObject *anim;
    f32 scale;

    animEntry = &animationObj->level_entry->animation;
    anim = animatedObj->animatedObject;
#ifdef NATIVE_PORT
    sMdkrAnimTargetInitCount++;
    if (!mdkr_anim_target_is_valid(animatedObj, anim)) {
        sMdkrAnimTargetInvalidCount++;
        fprintf(stderr,
                "[ANIMTARGET] invalid level=%d spawnId=%d targetObject=%d "
                "runtimeBehavior=%d headerBehavior=%d data=%p\n",
                level_id(), animEntry->objectIdToSpawn,
                animatedObj != NULL ? animatedObj->objectID : -1,
                animatedObj != NULL ? animatedObj->behaviorId : -1,
                animatedObj != NULL && animatedObj->header != NULL
                    ? animatedObj->header->behaviorId : -1,
                (void *) anim);
        fprintf(stderr, "[FATAL] invalid animated-object target binding\n");
        abort();
    }
#endif
    scale = animEntry->scale & 0xFF;
    if (scale < 1.0f) {
        scale = 1.0f;
    }
    scale /= 64;
    animatedObj->trans.scale = animatedObj->header->scale * scale;
    animatedObj->properties.animatedObj.unk0 = 0;
    animatedObj->properties.animatedObj.unk4 = 0;
    if (animEntry->unk22 >= 2 && animEntry->unk22 < 10) {
        animatedObj->properties.animatedObj.unk0 = animEntry->unk22 - 1;
    }
    if (animEntry->unk22 >= 10 && animEntry->unk22 < 18) {
        animatedObj->properties.animatedObj.unk0 = animEntry->unk22 - 9;
    }
    animatedObj->trans.x_position = animationObj->trans.x_position;
    animatedObj->trans.y_position = animationObj->trans.y_position;
    animatedObj->trans.z_position = animationObj->trans.z_position;
    animatedObj->trans.rotation.y_rotation = animationObj->trans.rotation.y_rotation;
    animatedObj->trans.rotation.z_rotation = animationObj->trans.rotation.z_rotation;
    animatedObj->trans.rotation.x_rotation = animationObj->trans.rotation.x_rotation;
    anim->unk26 = 0;
    anim->unk3D = animEntry->channel;
    anim->actorIndex = animEntry->actorIndex;
    anim->unk8 = (f32) animEntry->nodeSpeed * 0.1;
    anim->startDelay = normalise_time(animEntry->animationStartDelay);
    animatedObj->animationID = animEntry->objAnimIndex;
    animatedObj->animFrame = animEntry->unk16;
    anim->z = animEntry->objAnimSpeed;
    anim->y = 0;
    anim->loopType = animEntry->objAnimLoopType;
    anim->unk2E = animEntry->rotateType;
    anim->unk3E = animEntry->nextAnim;
    anim->unk3F = animEntry->unk2D;
    anim->unk31 = animEntry->yawSpinSpeed;
    anim->unk32 = animEntry->rollSpinSpeed;
    anim->unk33 = animEntry->pitchSpinSpeed;
    anim->unk34 = animEntry->unk20;
    anim->unk2D = 0;
    anim->unk4 = 0;
    anim->unk0 = 0;
    animationObj->particleEmitter = NULL;
    anim->pauseCounter = normalise_time(animEntry->pauseFrameCount);
    anim->unk3A = animEntry->specialHide;
    if (animEntry->unk13 >= 0) {
        anim->unk2F = animEntry->unk13;
    }
    anim->unk39 = animEntry->unk1F;
    anim->soundID = animEntry->unk1E;
    anim->unk3B = animEntry->unk29;
    anim->unk40 = animEntry->soundEffect;
    anim->unk41 = animEntry->fadeOptions;
    anim->unk3C = animEntry->fadeAlpha;
    anim->unk42 = 0xFF;
    if (anim->unk18 != NULL) {
        sndp_stop(anim->unk18);
    }
    anim->unk18 = NULL;
    anim->unk43 = animEntry->unk30;
    anim->unk1C = animationObj;
    anim->unk45 = 0;
}

void func_8001F23C(Object *obj, LevelObjectEntry_Animation *animEntry) {
    s32 i;
    LevelObjectEntryCommon newObjEntry;
    Object *newObj;
    Object_AnimatedObject *camera;
    s32 viewportCount;

    NEW_OBJECT_ENTRY(newObjEntry, animEntry->objectIdToSpawn, 8, animEntry->common.x, animEntry->common.y,
                     animEntry->common.z);

    obj->animTarget = spawn_object(&newObjEntry, OBJECT_SPAWN_UNK01);
    newObj = obj->animTarget;
    // (newObj->behaviorId == BHV_DINO_WHALE) is Dinosaur1, Dinosaur2, Dinosaur3, Whale, and Dinoisle
    if (obj->animTarget != NULL && newObj->behaviorId == BHV_DINO_WHALE && gTimeTrialEnabled) {
        free_object(newObj);
        obj->animTarget = NULL;
        newObj = NULL;
    }
    if (newObj != NULL) {
        newObj->level_entry = NULL;
        obj_init_animobject(obj, newObj);
        if (newObj->header->behaviorId == BHV_CAMERA_ANIMATION) {
            camera = newObj->animatedObject;
            camera->unk44 = D_8011AD3E;
            viewportCount = cam_get_viewport_layout();
            if (race_is_adventure_2P()) {
                viewportCount = VIEWPORT_LAYOUT_2_PLAYERS;
            }
            for (i = 0; i < viewportCount;) {
                newObj = spawn_object(&newObjEntry, OBJECT_SPAWN_UNK01);
                if (newObj != NULL) {
                    newObj->level_entry = NULL;
                    obj_init_animobject(obj, newObj);
                    camera = newObj->animatedObject;
                    i++;
                    camera->cameraID = i;
                    camera->unk44 = D_8011AD3E;
                }
            }
            D_8011AD3E++;
        }
    }
}

s8 func_8001F3B8(void) {
    return D_8011ADD4;
}

void func_8001F3C8(s32 arg0) {
    if (arg0 != D_8011ADD4) {
        D_8011AE78 = 0;
    }
    D_8011ADD4 = arg0;
}

s32 func_8001F3EC(s32 arg0) {
    s32 i;
    s32 count;
    if (D_8011AE78 == 0) {
        return -1;
    }

    count = 0;
    for (i = 0; i < D_8011AE78; i++) {
        if (D_8011AE74[i]->properties.animation.behaviourID == arg0) {
            count++;
        }
    }

    return count;
}

void func_8001F450(void) {
    D_8011AD53 = 1;
}

s32 func_8001F460(Object *arg0, s32 arg1, Object *arg2) {
    f32 var_f2;
    f32 var_f0;
    f32 var_f20;
    s32 var_s0;
    s32 var_s2;
    s32 var_s4;
    s32 var_s5;
    s32 q;
    s32 var_t0;
    ObjectTransform *trans;
    Object_AnimatedObject *obj64;
    s32 sp168;
    f32 sp154[5];
    f32 sp140[5];
    f32 sp12C[5];
    LevelObjectEntry_Animation *temp_s1;
    f32 sp124;
    f32 sp120;
    f32 sp11C;
    Object *otherObj64;
    f32 sp114;
    ObjectModel *temp_a0_3;
    s32 temp_a1_2;
    s32 var_v0_2;

    f32 spF4[5];
    f32 spE0[5];
    f32 spCC[5];
    f32 spB8[5];
    f32 spB4;
    s8 *miscAsset;
    s32 pad;
    FadeTransition fadeTransition;

    if (gCutsceneID < 0) {
        return 1;
    }

    if (arg1 >= 9) {
        arg1 = 8;
    }

    sp114 = arg1;

    obj64 = arg0->animatedObject;
    if (osTvType == 0) {
        sp114 *= 1.2;
    }
    if (obj64->startDelay < 0) {
        var_t0 = 0;
        if (obj64->unk34 & 1) {
            var_t0 = A_BUTTON;
        }
        if (obj64->unk34 & 2) {
            var_t0 |= B_BUTTON;
        }
        if (obj64->unk34 & 4) {
            var_t0 |= CONT_START;
        }

        var_s2 = 0;

        // clang-format off
        for (var_s0 = 0; var_s0 < MAXCONTROLLERS; var_s0++) { var_s2 |= input_pressed(var_s0); }
        // clang-format on

        if (var_s2 & var_t0) {
            obj64->startDelay = 1;
        }
    }

    if ((obj64->startDelay >= 0) && (obj64->unk45 == 0)) {
        obj64->startDelay -= arg1;
        if (obj64->startDelay <= 0) {
            obj64->unk45 = 1;
            temp_s1 = &obj64->unk1C->level_entry->animation;
            func_80021104(arg0, obj64, temp_s1);
            obj64->startDelay = 0;
            func_8002125C(arg0, temp_s1, obj64, -1);
        }
    }
    if (obj64->startDelay != 0) {
        if (obj64->unk3A != 0) {
            arg0->trans.flags |= OBJ_FLAGS_INVISIBLE;
            obj64->unk42 = 0;
            return 1;
        } else {
            return 0;
        }
    }

    arg0->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
    if (obj64->unk39 > 0) {
        if (obj64->unk39 != music_current_sequence()) {
            music_play(obj64->unk39);
            music_change_off();
        }
        obj64->unk39 = -2;
        music_volume_reset();
    } else if (obj64->unk39 == -2) {
        music_change_on();
        obj64->unk39 = -1;
    }
    if (obj64->soundID != 0) {
        var_v0_2 = obj64->soundID & 0xFF;
        if (obj64->currentSound == 0) {
            if (var_v0_2 == 0xFF) {
                if (obj64->unk18 != NULL) {
                    sndp_stop(obj64->unk18);
                }
            } else {
                if (obj64->unk18 != NULL) {
                    sndp_stop(obj64->unk18);
                    var_v0_2 = obj64->soundID & 0xFF;
                }
                sound_play(var_v0_2, &obj64->unk18);
            }
            obj64->soundID = 0;
        }
    }

    if (obj64->unk43 != 0) {
        music_fade(obj64->unk43 * 256);
        obj64->unk43 = 0;
    }

    var_t0 = (u8) obj64->unk42;
    if (obj64->unk41 & 1) {
        if (obj64->unk41 & 2) {
            var_t0 = 0;
        }
        if ((arg1 * 8) < var_t0) {
            var_t0 -= (arg1 * 8);
        } else {
            var_t0 = 0;
            arg0->trans.flags |= OBJ_FLAGS_INVISIBLE;
        }
    } else {
        if (obj64->unk41 & 2) {
            var_t0 = 0xFF;
        }
        var_t0 += arg1 * 8;
        if (var_t0 > 0xFF) {
            var_t0 = 0xFF;
        }
        arg0->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
    }

    var_s2 = obj64->unk3B & 0x7F;
    obj64->unk42 = var_t0;
    if (var_s2 != 0x7F) {
        if (var_s2 >= 8) {
            miscAsset = ((s8 *) get_misc_asset(ASSET_MISC_13) + (var_s2 * 5));
            miscAsset -= 0x28;
            var_t0 = (miscAsset[0] & 0xFF) + 900;
            var_s0 = (miscAsset[1] & 0xFF) + 900;
            slowly_change_fog(0, miscAsset[2] & 0xFF, miscAsset[3] & 0xFF, miscAsset[4] & 0xFF, var_t0, var_s0,
                              normalise_time(6) * obj64->unk3C);
        } else if (var_s2 >= 6) {
            fadeTransition.type = FADE_FLAG_INVERT;
            // clang-format off
            if (var_s2 == 7) {
                fadeTransition.red = 200; fadeTransition.green = 200; fadeTransition.blue = 255; 
            } else {
                fadeTransition.red = 255; fadeTransition.green = 255; fadeTransition.blue = 255;
            }
            // clang-format on
            fadeTransition.duration = 7;
            fadeTransition.endTimer = 3;
            transition_begin(&fadeTransition);
        } else {
            fadeTransition.type = obj64->unk3B;
            miscAsset = (s8 *) get_misc_asset(ASSET_MISC_14) + (obj64->unk40 * 3);
            fadeTransition.red = miscAsset[0];
            fadeTransition.green = miscAsset[1];
            fadeTransition.blue = miscAsset[2];
            if (obj64->unk3B & 0x80) {
                fadeTransition.endTimer = 0;
            } else {
                fadeTransition.endTimer = 0xFFFF;
            }
            fadeTransition.duration = normalise_time(6) * obj64->unk3C;
            if (check_fadeout_transition() == 0 || (fadeTransition.type & 0x80)) {
                transition_begin(&fadeTransition);
            }
        }
        obj64->unk3B = 0xFF;
    }

    if (obj64->unk2E == 1) {
        arg0->trans.rotation.y_rotation += (s16) (obj64->unk31 * (f32) (sp114 * 8.0));
        arg0->trans.rotation.x_rotation += (s16) (obj64->unk32 * (f32) (sp114 * 8.0));
        arg0->trans.rotation.z_rotation += (s16) (obj64->unk33 * (f32) (sp114 * 8.0));
    }

    if (arg2 != NULL && arg2->header->modelType == 0) {
        arg2->animationID = arg0->animationID;
        if ((s16) obj64->y != arg2->animFrame) {
            obj64->y = arg2->animFrame;
        }
        if (arg2->modelInstances[arg2->modelIndex] != NULL) {
            temp_a0_3 = arg2->modelInstances[arg2->modelIndex]->objModel;
            if (arg2->animationID >= 0 && arg2->animationID < temp_a0_3->numberOfAnimations) {
                var_s5 = (DKR_PTR(ObjectModel_44, temp_a0_3->animations)[arg2->animationID].animLength - 1) << 4;
                switch (obj64->loopType) {
                    case 0:
                        obj64->y += obj64->z * sp114;
                        if (obj64->y >= var_s5) {
                            obj64->y -= var_s5;
                        }
                        break;
                    case 2:
                        obj64->y += obj64->z * sp114;
                        if (obj64->y >= var_s5) {
                            obj64->y = var_s5 - 1.0f;
                        }
                        break;
                    case 1:
                        if (obj64->unk2D == 0) {
                            obj64->y += obj64->z * sp114;
                            if (obj64->y >= var_s5) {
                                obj64->unk2D = 1;
                                obj64->y = var_s5 - 1.0f;
                            }
                        } else {
                            obj64->y -= obj64->z * sp114;
                            if (obj64->y <= 0) {
                                obj64->y = 0;
                                obj64->unk2D = 0;
                            }
                        }
                        break;
                    case 3:
                        if (obj64->unk2D == 0) {
                            obj64->y += obj64->z * sp114;
                            if (obj64->y >= var_s5) {
                                obj64->unk2D = 1;
                                obj64->y = var_s5 - 1.0f;
                            }
                        } else {
                            obj64->y -= obj64->z * sp114;
                            if (obj64->y <= 0) {
                                obj64->y = 0;
                            }
                        }
                        break;
                }
            }
        }
        arg2->animFrame = obj64->y;
    }

    if (obj64->unk8 <= 0.0) {
        return func_800214E4(arg0, arg1);
    }

    temp_a1_2 = obj64->actorIndex;
    for (var_s4 = 0; var_s4 < D_8011AE78 && temp_a1_2 != D_8011AE74[var_s4]->properties.animation.behaviourID;
         var_s4++) {}
    if (var_s4 >= D_8011AE78) {
        return func_800214E4(arg0, arg1);
    }

    for (var_s5 = 1;
         (var_s4 + var_s5) < D_8011AE78 && temp_a1_2 == D_8011AE74[var_s4 + var_s5]->properties.animation.behaviourID;
         var_s5++) {}
    if (var_s5 < 2) {
        return func_800214E4(arg0, arg1);
    }

    sp168 = -1;
    spB4 = 1.0 / D_8011AE74[0]->header->scale;

    temp_s1 = &D_8011AE74[var_s4 + var_s5 - 1]->level_entry->animation;
    if (var_s5 > 2) {
        if (temp_s1->goToNode >= 0 && temp_s1->goToNode < var_s5 - 1) {
            sp168 = temp_s1->goToNode;
        }
    }

    if (sp168 == -1 && obj64->unk26 >= var_s5 - 1) {
        return func_800214E4(arg0, arg1);
    }

    if (obj64->pauseCounter >= 0) {
        if (D_8011AD53 == 0) {
            obj64->pauseCounter -= arg1;
        }
        return 0;
    }

    var_s0 = obj64->unk26 - 1;
    var_s2 = 0;
    for (var_t0 = 0; var_t0 != 5; var_s2++, var_t0++, var_s0++) {
        if (var_s0 == -1) {
            if (sp168 != 0) {
                sp154[var_s2] = D_8011AE74[var_s4]->trans.x_position +
                                (D_8011AE74[var_s4]->trans.x_position - D_8011AE74[var_s4 + 1]->trans.x_position);
                sp140[var_s2] = D_8011AE74[var_s4]->trans.y_position +
                                (D_8011AE74[var_s4]->trans.y_position - D_8011AE74[var_s4 + 1]->trans.y_position);
                sp12C[var_s2] = D_8011AE74[var_s4]->trans.z_position +
                                (D_8011AE74[var_s4]->trans.z_position - D_8011AE74[var_s4 + 1]->trans.z_position);
                spE0[var_t0] = D_8011AE74[var_s4]->trans.rotation.y_rotation;
                spCC[var_t0] = D_8011AE74[var_s4]->trans.rotation.x_rotation;
                spB8[var_t0] = D_8011AE74[var_s4]->trans.rotation.z_rotation;
                spF4[var_t0] = D_8011AE74[var_s4]->trans.scale;
            } else {
                q = var_s4 + var_s5 - 1;
                sp154[var_s2] = D_8011AE74[q]->trans.x_position;
                sp140[var_s2] = D_8011AE74[q]->trans.y_position;
                sp12C[var_s2] = D_8011AE74[q]->trans.z_position;
                spE0[var_t0] = D_8011AE74[q]->trans.rotation.y_rotation;
                spCC[var_t0] = D_8011AE74[q]->trans.rotation.x_rotation;
                spB8[var_t0] = D_8011AE74[q]->trans.rotation.z_rotation;
                spF4[var_t0] = D_8011AE74[q]->trans.scale;
            }
        } else if (var_s0 >= var_s5) {
            if (sp168 == -1) {
                var_s0 = var_s5 - 1;
                q = var_s5 + var_s4 - 1;
                temp_s1 = &D_8011AE74[q]->level_entry->animation;
                if (temp_s1->unk22 == 1) {
                    set_active_camera(obj64->cameraID);
                    trans = &cam_get_active_camera_no_cutscenes()->trans;
                } else {
                    trans = &D_8011AE74[q]->trans;
                }
                sp154[var_s2] = trans->x_position + (trans->x_position - D_8011AE74[q - 1]->trans.x_position);
                sp140[var_s2] = trans->y_position + (trans->y_position - D_8011AE74[q - 1]->trans.y_position);
                sp12C[var_s2] = trans->z_position + (trans->z_position - D_8011AE74[q - 1]->trans.z_position);
                spCC[var_t0] = trans->rotation.x_rotation;
                spB8[var_t0] = trans->rotation.z_rotation;
                trans = &D_8011AE74[q]->trans;
                spE0[var_t0] = trans->rotation.y_rotation;
                spF4[var_t0] = D_8011AE74[q]->trans.scale;
            } else {
                q = var_s4 + sp168 + var_s0 - var_s5;
                sp154[var_s2] = D_8011AE74[q]->trans.x_position;
                sp140[var_s2] = D_8011AE74[q]->trans.y_position;
                sp12C[var_s2] = D_8011AE74[q]->trans.z_position;
                spE0[var_t0] = D_8011AE74[q]->trans.rotation.y_rotation;
                spCC[var_t0] = D_8011AE74[q]->trans.rotation.x_rotation;
                spB8[var_t0] = D_8011AE74[q]->trans.rotation.z_rotation;
                spF4[var_t0] = D_8011AE74[q]->trans.scale;
            }
        } else {
            q = var_s0 + var_s4;
            if (obj64->z) {} // @fake
            temp_s1 = &D_8011AE74[q]->level_entry->animation;

            if (temp_s1->unk22 == 1) {
                set_active_camera(obj64->cameraID);
                trans = &cam_get_active_camera_no_cutscenes()->trans;
            } else {
                trans = &D_8011AE74[q]->trans;
            }
            sp154[var_s2] = trans->x_position;
            sp140[var_s2] = trans->y_position;
            sp12C[var_s2] = trans->z_position;
            spCC[var_t0] = trans->rotation.x_rotation;
            spB8[var_t0] = trans->rotation.z_rotation;
            trans = &D_8011AE74[q]->trans;
            spE0[var_t0] = trans->rotation.y_rotation;
            if (temp_s1->unk22 == 1) {
                spCC[var_t0] = -spCC[var_t0];
            }
            spF4[var_t0] = D_8011AE74[q]->trans.scale;
        }
    }

    if (obj64->unk4 == 0) {
        obj64->unk4 = 0.01f;
    }

    for (var_s2 = 0, var_s0 = 0; var_s0 < 2; var_s0++) {
        var_f20 = obj64->unk0 + (obj64->unk4 * sp114);
        if (var_f20 >= 1.00) {
            var_s2 = 1;
            var_f20 -= 1.0;
        }
        if (obj64->unk3F == 0) {
            sp124 = catmull_rom_interpolation(sp154, var_s2, var_f20);
            sp120 = catmull_rom_interpolation(sp140, var_s2, var_f20);
            sp11C = catmull_rom_interpolation(sp12C, var_s2, var_f20);
        } else {
            sp124 = lerp(sp154, var_s2, var_f20);
            sp120 = lerp(sp140, var_s2, var_f20);
            sp11C = lerp(sp12C, var_s2, var_f20);
        }
        sp124 -= arg0->trans.x_position;
        sp120 -= arg0->trans.y_position;
        sp11C -= arg0->trans.z_position;
        if (var_s0 != 1) {
            var_s2 = 0;
            var_f2 = sqrtf((sp124 * sp124) + (sp120 * sp120) + (sp11C * sp11C)) / sp114;
            if (var_f2 != 0) {
                obj64->unk4 *= obj64->unk8 / var_f2;
            }
        }
    }

    arg0->trans.scale = catmull_rom_interpolation(spF4, var_s2, var_f20) * spB4 * arg0->header->scale;
    if (var_s2 != 0 && sp168 == -1 && var_s5 == obj64->unk26 + 2) {
        sp124 = catmull_rom_interpolation(sp154, 0, 1.0f);
        sp120 = catmull_rom_interpolation(sp140, 0, 1.0f);
        sp11C = catmull_rom_interpolation(sp12C, 0, 1.0f);
        sp124 -= arg0->trans.x_position;
        sp120 -= arg0->trans.y_position;
        sp11C -= arg0->trans.z_position;
    }

    arg0->x_velocity = sp124 / sp114;
    arg0->y_velocity = sp120 / sp114;
    arg0->z_velocity = sp11C / sp114;
    move_object(arg0, sp124, sp120, sp11C);

    switch (obj64->unk2E) {
        case 1:
            break;
        case 2:
            if (obj64->unk3F == 0) {
                cubic_spline_interpolation(sp154, var_s2, var_f20, &sp124);
                cubic_spline_interpolation(sp140, var_s2, var_f20, &sp120);
                cubic_spline_interpolation(sp12C, var_s2, var_f20, &sp11C);
            } else {
                lerp_and_get_derivative(sp154, var_s2, var_f20, &sp124);
                lerp_and_get_derivative(sp140, var_s2, var_f20, &sp120);
                lerp_and_get_derivative(sp12C, var_s2, var_f20, &sp11C);
            }
            var_f2 = sqrtf((sp124 * sp124) + (sp120 * sp120) + (sp11C * sp11C));
            if (var_f2 != 0) {
                var_f2 = 100.0 / var_f2;
                sp124 *= var_f2;
                sp120 *= var_f2;
                sp11C *= var_f2;
            }
            arg0->trans.rotation.y_rotation = arctan2_f(sp124, sp11C) - 0x8000;
            arg0->trans.rotation.x_rotation = arctan2_f(sp120, 100.0f);
            break;
        case 3:
            break;
        default:
            for (var_t0 = 1; var_t0 < 5; var_t0++) {
                f32 temp = 0;
                f32 delta;

                delta = spE0[var_t0] - spE0[var_t0 - 1];
                if (delta > 32768.0) {
                    temp -= 65536.0;
                } else if (delta < -32768.0) {
                    temp += 65536.0;
                }
                // clang-format off
                for (var_s0 = var_t0; var_s0 < 5; var_s0++) { spE0[var_s0] += temp; }
                // clang-format on

                temp = 0;
                delta = spCC[var_t0] - spCC[var_t0 - 1];
                if (delta > 32768.0) {
                    temp -= 65536.0;
                } else if (delta < -32768.0) {
                    temp += 65536.0;
                }
                // clang-format off
                for (var_s0 = var_t0; var_s0 < 5; var_s0++) {spCC[var_s0] += temp; }
                // clang-format on

                temp = 0;
                delta = spB8[var_t0] - spB8[var_t0 - 1];
                if (delta > 32768.0) {
                    temp -= 65536.0;
                } else if (delta < -32768.0) {
                    temp += 65536.0;
                }
                // clang-format off
                for (var_s0 = var_t0; var_s0 < 5; var_s0++) { spB8[var_s0] += temp; }
                // clang-format on
            }
            if (obj64->unk3F == 0) {
                arg0->trans.rotation.s[0] =
                    dkr_f32_to_s16_wrap(catmull_rom_interpolation(spE0, var_s2, var_f20));
                arg0->trans.rotation.s[1] =
                    dkr_f32_to_s16_wrap(catmull_rom_interpolation(spCC, var_s2, var_f20));
                arg0->trans.rotation.s[2] =
                    dkr_f32_to_s16_wrap(catmull_rom_interpolation(spB8, var_s2, var_f20));
            } else {
                arg0->trans.rotation.s[0] = dkr_f32_to_s16_wrap(lerp(spE0, var_s2, var_f20));
                arg0->trans.rotation.s[1] = dkr_f32_to_s16_wrap(lerp(spCC, var_s2, var_f20));
                arg0->trans.rotation.s[2] = dkr_f32_to_s16_wrap(lerp(spB8, var_s2, var_f20));
            }
            break;
    }

    var_t0 = obj64->unk26;
    obj64->unk0 = var_f20;
    if (sp168 != -1 && var_t0 >= var_s5) {
        var_t0 = (var_t0 - var_s5) + sp168;
    }

    temp_s1 = &D_8011AE74[var_s4 + var_t0]->level_entry->animation;
    sp124 = (f32) temp_s1->nodeSpeed * 0.1;
    if (sp124 < 0) {
        sp124 = obj64->x;
    } else {
        obj64->x = sp124;
    }
    if (sp124 >= 0) {
        if (var_s2 == 0) {
            var_s0 = var_t0 + 1;
            if (sp168 != -1 && var_s0 >= var_s5) {
                var_s0 = var_s0 - var_s5 + sp168;
            }
            if (var_s0 < var_s5) {
                temp_s1 = &D_8011AE74[var_s4 + var_s0]->level_entry->animation;
                if (temp_s1->nodeSpeed >= 0) {
                    if (0) {} // @fake
                    sp11C = (f32) temp_s1->nodeSpeed * 0.1;
                } else {
                    sp11C = sp124;
                }
            }
            obj64->unk8 = ((sp11C - sp124) * var_f20) + sp124;
        }
    }

    if (var_s2 != 0) {
        obj64->unk26 += 1;
        if (sp168 == -1) {
            if (obj64->unk26 >= var_s5) {
                obj64->unk26 = var_s5 - 1;
            } else {
                temp_s1 = &D_8011AE74[var_s4 + obj64->unk26]->level_entry->animation;
                func_8002125C(arg0, temp_s1, obj64, var_s4);
            }
        } else {
            if (var_s5 < obj64->unk26) {
                obj64->unk26 = sp168 + 1;
            }
            var_s2 = obj64->unk26;
            if (obj64->unk26 >= var_s5) {
                var_s2 = (obj64->unk26 - var_s5) + sp168;
            }
            var_s2 += var_s4;
            temp_s1 = &D_8011AE74[var_s2]->level_entry->animation;
            func_8002125C(arg0, temp_s1, obj64, var_s4);
        }
    }
    if (obj64->unk2E == 3) {
        for (var_t0 = 0; var_t0 < D_8011AE78 && obj64->unk3E != D_8011AE74[var_t0]->properties.animation.behaviourID;
             var_t0++) {}
        if (var_t0 != D_8011AE78) {
            trans = &D_8011AE74[var_t0]->animTarget->trans;
            if (trans != NULL) {
                sp124 = trans->x_position - arg0->trans.x_position;
                sp120 = trans->y_position - arg0->trans.y_position;
                sp11C = trans->z_position - arg0->trans.z_position;
                var_f0 = sqrtf((sp124 * sp124) + (sp120 * sp120) + (sp11C * sp11C));
                if (var_f0 > 0) {
                    arg0->trans.rotation.y_rotation = arctan2_f(sp124, sp11C) - 0x8000;
                    arg0->trans.rotation.x_rotation = arctan2_f(sp120, var_f0);
                }
            }
        }
    }
    arg0->particleEmittersEnabled = obj64->unk2F;
    obj_spawn_particle(arg0, arg1);
    return 0;
}

s32 func_800210CC(s8 arg0) {
    if (arg0 >= D_8011AD3D) {
        D_8011AD3D = arg0;
        return TRUE;
    }
    return FALSE;
}

void func_80021104(Object *obj, Object_AnimatedObject *animObj, LevelObjectEntry_Animation *entry) {
    Camera *camera;
    ObjectTransform *animObjTrans;

    animObjTrans = &animObj->unk1C->trans;
    if (obj->behaviorId == BHV_CAMERA_ANIMATION) {
        animObj->unk44 = D_8011AD3E;
        D_8011AD3E++;
    }
    if (entry->unk22 == 18) {
        set_active_camera(animObj->cameraID);
        camera = cam_get_active_camera_no_cutscenes();
        animObjTrans->x_position = camera->trans.x_position;
        animObjTrans->y_position = camera->trans.y_position;
        animObjTrans->z_position = camera->trans.z_position;
        animObjTrans->rotation.y_rotation = (0x8000 - camera->trans.rotation.y_rotation);
        animObjTrans->rotation.x_rotation = -camera->trans.rotation.x_rotation;
        animObjTrans->rotation.z_rotation = camera->trans.rotation.z_rotation;
    }
    if ((entry->unk22 >= 10) && (entry->unk22 < 18)) {
        Object *seg = (*gRacers)[entry->unk22 - 10];
        if (seg != NULL) {
            animObjTrans->x_position = seg->trans.x_position;
            animObjTrans->y_position = seg->trans.y_position;
            animObjTrans->z_position = seg->trans.z_position;
            animObjTrans->rotation.y_rotation = seg->trans.rotation.y_rotation;
            animObjTrans->rotation.x_rotation = seg->trans.rotation.x_rotation;
            animObjTrans->rotation.z_rotation = seg->trans.rotation.z_rotation;
        }
    }
}

void func_8002125C(Object *obj, LevelObjectEntry_Animation *entry, Object_AnimatedObject *animObj, UNUSED s32 index) {
    s32 initialAnimFrame;

    initialAnimFrame = entry->objAnimIndex;
    if (initialAnimFrame >= 0) {
        if (initialAnimFrame != obj->animationID) {
            obj->animFrame = entry->unk16;
        }
        obj->animationID = entry->objAnimIndex;
        animObj->z = entry->objAnimSpeed;
        animObj->loopType = entry->objAnimLoopType;
    }
    if (entry->unk13 >= 0) {
        animObj->unk2F = entry->unk13;
    }
    animObj->pauseCounter = normalise_time(entry->pauseFrameCount);
    animObj->unk3F = entry->unk2D;
    animObj->unk3A = entry->specialHide;
    animObj->unk39 = entry->unk1F;
    animObj->unk43 = entry->unk30;
    animObj->soundID = entry->unk1E;
    animObj->unk3B = entry->unk29;
    animObj->unk40 = entry->soundEffect;
    animObj->unk41 = entry->fadeOptions;
    animObj->unk3C = entry->fadeAlpha;
    if (entry->messageId != 255) {
        set_current_text(entry->messageId);
    }
    if (entry->unk2A >= 0) {
        func_8001E45C(entry->unk2A);
        return;
    }
    if (entry->unk15 >= 0) {
        func_80021400(entry->unk15);
    }
    if (entry->unk28 >= 0) {
        D_8011AD22[D_8011AD21]++;
    }
}

void func_80021400(s32 arg0) {
    s32 i;
    arg0 &= 0xFF; //?

    for (i = 0; i < D_8011AE78 && (arg0 != (D_8011AE74[i]->properties.animation.behaviourID & 0xFF)); i++) {}

    if (i < D_8011AE78) {
        if (D_8011AE74[i]->animTarget != NULL) {
            if (D_8011AE74[i]->animTarget->animatedObject->startDelay < 0) {
                D_8011AE74[i]->animTarget->animatedObject->startDelay = 1;
            }
        }
    }
}

s8 func_800214C4(void) {
    return D_8011AD22[1 - D_8011AD21];
}

s8 func_800214E4(Object *obj, s32 updateRate) {
    s32 i;
    Object_AnimatedObject *animObj;

    animObj = obj->animatedObject;
    if (animObj->unk3A != 0) {
        obj->trans.flags |= OBJ_FLAGS_INVISIBLE;
    }
    if (animObj->pauseCounter == -1) {
        return animObj->unk3A;
    }
    if (animObj->pauseCounter >= 0) {
        animObj->pauseCounter -= updateRate;
    }
    if (animObj->pauseCounter == -1) {
        animObj->pauseCounter = -2;
    }
    if (animObj->pauseCounter <= 0) {
        obj->trans.flags |= OBJ_FLAGS_INVISIBLE;
        for (i = 0; (i < D_8011AE78 && animObj->actorIndex != D_8011AE74[i]->properties.animation.behaviourID); i++) {
            if (FALSE) {} // FAKEMATCH
        }
        obj_init_animobject(D_8011AE74[i], obj);
        return 1;
    }
    return 0;
}

s32 func_80021600(s32 arg0) {
    Object_AnimatedObject *objAnim;
    s32 i;
    Object *sp154;
    LevelObjectEntry_Animation *levelObjAnim;
    s32 count;
    s32 s0;
    s32 j;
    ObjectTransform *objTransform;
    f32 f0;
    s32 sp138;
    f32 xPositions[5];
    f32 yPositions[5];
    f32 zPositions[5];
    f32 spF8;
    f32 spF4;
    f32 spF0;
    f32 spEC;
    f32 delta;
    s32 q;
    f32 scales[5];
    f32 yRotations[5];
    f32 xRotations[5];
    f32 zRotations[5];
    f32 sp90;

    if (gCutsceneID < 0) {
        return TRUE;
    }

    for (i = 0; i < D_8011AE78 && arg0 != D_8011AE74[i]->properties.animation.behaviourID; i++) {}
    if (i >= D_8011AE78) {
        return TRUE;
    }

    for (count = 1; i + count < D_8011AE78 && arg0 == D_8011AE74[i + count]->properties.animation.behaviourID;
         count++) {}
    if (count < 2) {
        return TRUE;
    }

    sp154 = D_8011AE74[i]->animTarget;
    if (sp154 == NULL) {
        return TRUE;
    }

    objAnim = sp154->animatedObject;
    sp138 = -1;
    sp90 = 1.0 / D_8011AE74[i]->header->scale;

    levelObjAnim = &D_8011AE74[i + count - 1]->level_entry->animation;
    if (count > 2) {
        if (levelObjAnim->goToNode >= 0 && levelObjAnim->goToNode < count - 1) {
            sp138 = levelObjAnim->goToNode;
        }
    }

    s0 = objAnim->unk26 - 1;
    for (j = 0; j < 5; j++, s0++) {
        if (s0 == -1) {
            if (sp138 != 0) {
                xPositions[j] = D_8011AE74[i]->trans.x_position +
                                (D_8011AE74[i]->trans.x_position - D_8011AE74[i + 1]->trans.x_position);
                yPositions[j] = D_8011AE74[i]->trans.y_position +
                                (D_8011AE74[i]->trans.y_position - D_8011AE74[i + 1]->trans.y_position);
                zPositions[j] = D_8011AE74[i]->trans.z_position +
                                (D_8011AE74[i]->trans.z_position - D_8011AE74[i + 1]->trans.z_position);
                yRotations[j] = D_8011AE74[i]->trans.rotation.y_rotation;
                xRotations[j] = D_8011AE74[i]->trans.rotation.x_rotation;
                zRotations[j] = D_8011AE74[i]->trans.rotation.z_rotation;
                scales[j] = D_8011AE74[i]->trans.scale;
            } else {
                q = i + count - 1;
                xPositions[j] = D_8011AE74[q]->trans.x_position;
                yPositions[j] = D_8011AE74[q]->trans.y_position;
                zPositions[j] = D_8011AE74[q]->trans.z_position;
                yRotations[j] = D_8011AE74[q]->trans.rotation.y_rotation;
                xRotations[j] = D_8011AE74[q]->trans.rotation.x_rotation;
                zRotations[j] = D_8011AE74[q]->trans.rotation.z_rotation;
                scales[j] = D_8011AE74[q]->trans.scale;
            }
        } else if (s0 >= count) {
            if (sp138 == -1) {
                s0 = count - 1;
                q = s0 + i;
                levelObjAnim = &D_8011AE74[q]->level_entry->animation;
                if (levelObjAnim->unk22 == 1) {
                    set_active_camera(objAnim->cameraID);
                    objTransform = &cam_get_active_camera_no_cutscenes()->trans;
                } else {
                    objTransform = &D_8011AE74[q]->trans;
                }

                xPositions[j] =
                    (objTransform->x_position - D_8011AE74[q - 1]->trans.x_position) + objTransform->x_position;
                yPositions[j] =
                    (objTransform->y_position - D_8011AE74[q - 1]->trans.y_position) + objTransform->y_position;
                zPositions[j] =
                    (objTransform->z_position - D_8011AE74[q - 1]->trans.z_position) + objTransform->z_position;
                xRotations[j] = objTransform->rotation.x_rotation;
                zRotations[j] = objTransform->rotation.z_rotation;
                objTransform = &D_8011AE74[q]->trans;
                yRotations[j] = objTransform->rotation.y_rotation;
                scales[j] = D_8011AE74[q]->trans.scale;
            } else {
                q = i + sp138 + s0 - count;
                xPositions[j] = D_8011AE74[q]->trans.x_position;
                yPositions[j] = D_8011AE74[q]->trans.y_position;
                zPositions[j] = D_8011AE74[q]->trans.z_position;
                yRotations[j] = D_8011AE74[q]->trans.rotation.y_rotation;
                xRotations[j] = D_8011AE74[q]->trans.rotation.x_rotation;
                zRotations[j] = D_8011AE74[q]->trans.rotation.z_rotation;
                scales[j] = D_8011AE74[q]->trans.scale;
            }
        } else {
            q = s0 + i;
            if (1) {} // Fake
            levelObjAnim = &D_8011AE74[q]->level_entry->animation;
            if (levelObjAnim->unk22 == 1) {
                set_active_camera(objAnim->cameraID);
                objTransform = &cam_get_active_camera_no_cutscenes()->trans;
            } else {
                objTransform = &D_8011AE74[q]->trans;
            }
            xPositions[j] = objTransform->x_position;
            yPositions[j] = objTransform->y_position;
            zPositions[j] = objTransform->z_position;
            xRotations[j] = objTransform->rotation.x_rotation;
            zRotations[j] = objTransform->rotation.z_rotation;
            objTransform = &D_8011AE74[q]->trans;
            yRotations[j] = objTransform->rotation.y_rotation;
            if (levelObjAnim->unk22 == 1) {
                xRotations[j] = -xRotations[j];
            }
            scales[j] = D_8011AE74[q]->trans.scale;
        }
    }

    spEC = objAnim->unk0;
    if (objAnim->unk3F == 0) {
        spF8 = catmull_rom_interpolation(xPositions, 0, spEC);
        spF4 = catmull_rom_interpolation(yPositions, 0, spEC);
        spF0 = catmull_rom_interpolation(zPositions, 0, spEC);
    } else {
        spF8 = lerp(xPositions, 0, spEC);
        spF4 = lerp(yPositions, 0, spEC);
        spF0 = lerp(zPositions, 0, spEC);
    }

    spF8 -= sp154->trans.x_position;
    spF4 -= sp154->trans.y_position;
    spF0 -= sp154->trans.z_position;

    move_object(sp154, spF8, spF4, spF0);
    sp154->trans.scale = catmull_rom_interpolation(scales, 0, spEC) * sp90 * sp154->header->scale;

    switch (objAnim->unk2E) {
        case 1:
            break;
        case 2:
            if (objAnim->unk3F == 0) {
                cubic_spline_interpolation(xPositions, 0, spEC, &spF8);
                cubic_spline_interpolation(yPositions, 0, spEC, &spF4);
                cubic_spline_interpolation(zPositions, 0, spEC, &spF0);
            } else {
                lerp_and_get_derivative(xPositions, 0, spEC, &spF8);
                lerp_and_get_derivative(yPositions, 0, spEC, &spF4);
                lerp_and_get_derivative(zPositions, 0, spEC, &spF0);
            }

            spEC = sqrtf(spF8 * spF8 + spF4 * spF4 + spF0 * spF0);
            if (spEC != 0.0f) {
                delta = spEC;
                spEC = spF8;
                delta = 100.0 / delta;

                spF8 *= delta;
                spF4 *= delta;
                spF0 *= delta;
            }

            sp154->trans.rotation.y_rotation = arctan2_f(spF8, spF0) - 0x8000;
            sp154->trans.rotation.x_rotation = arctan2_f(spF4, 100.0f);
            break;
        case 3:
            for (j = 0; j < D_8011AE78 && objAnim->unk3E != D_8011AE74[j]->properties.animation.behaviourID; j++) {}

            if (j != D_8011AE78) {
                objTransform = &D_8011AE74[j]->animTarget->trans;
                if (objTransform != NULL) {
                    spF8 = objTransform->x_position - sp154->trans.x_position;
                    spF4 = objTransform->y_position - sp154->trans.y_position;
                    spF0 = objTransform->z_position - sp154->trans.z_position;
                    spEC = sqrtf(spF8 * spF8 + spF4 * spF4 + spF0 * spF0);
                    if (spEC > 0.0f) {
                        sp154->trans.rotation.y_rotation = arctan2_f(spF8, spF0) - 0x8000;
                        sp154->trans.rotation.x_rotation = arctan2_f(spF4, spEC);
                    }
                }
            }
            break;
        default:
            for (j = 1; j < 5; j++) {
                f0 = 0.0f;
                delta = yRotations[j] - yRotations[j - 1];
                if (delta > 32768.0) {
                    f0 -= 65536.0;
                } else if (delta < -32768.0) {
                    f0 += 65536.0;
                }

                // clang-format off
                for (s0 = j; s0 < 5; s0++) { yRotations[s0] += f0; }
                // clang-format on

                f0 = 0.0f;
                delta = xRotations[j] - xRotations[j - 1];
                if (delta > 32768.0) {
                    f0 -= 65536.0;
                } else if (delta < -32768.0) {
                    f0 += 65536.0;
                }

                // clang-format off
                for (s0 = j; s0 < 5; s0++) { xRotations[s0] += f0; }
                // clang-format on

                f0 = 0.0f;
                delta = zRotations[j] - zRotations[j - 1];
                if (delta > 32768.0) {
                    f0 -= 65536.0;
                } else if (delta < -32768.0) {
                    f0 += 65536.0;
                }

                // clang-format off
                for (s0 = j; s0 < 5; s0++) { zRotations[s0] += f0; }
                // clang-format on
            }

            if (objAnim->unk3F == 0) {
                sp154->trans.rotation.y_rotation = catmull_rom_interpolation(yRotations, 0, spEC);
                sp154->trans.rotation.x_rotation = catmull_rom_interpolation(xRotations, 0, spEC);
                sp154->trans.rotation.z_rotation = catmull_rom_interpolation(zRotations, 0, spEC);
            } else {
                sp154->trans.rotation.y_rotation = lerp(yRotations, 0, spEC);
                sp154->trans.rotation.x_rotation = lerp(xRotations, 0, spEC);
                sp154->trans.rotation.z_rotation = lerp(zRotations, 0, spEC);
            }
            break;
    }

    return FALSE;
}

f32 catmull_rom_interpolation(f32 *data, s32 index, f32 x) {
    f32 ret;
    f32 c, b, a;

    a = (-0.5 * data[index]) + (1.5 * data[index + 1]) + (-1.5 * data[index + 2]) + (0.5 * data[index + 3]);
    b = (1.0 * data[index]) + (-2.5 * data[index + 1]) + (2.0 * data[index + 2]) + (-0.5 * data[index + 3]);
    c = (data[index + 2] * 0.5) + (0.0 * data[index + 1]) + (-0.5 * data[index]) + (0.0 * data[index + 3]);

    ret = (1.0 * data[index + 1]);
    ret = (((((a * x) + b) * x) + c) * x) + ret;

    return ret;
}

/**
 * Interpolates x along a spline and returns the resultant progress along the spline.
 */
f32 cubic_spline_interpolation(f32 *data, s32 index, f32 x, f32 *derivative) {
    f32 ret;
    f32 c, b, a;

    a = (-0.5 * data[index]) + (1.5 * data[index + 1]) + (-1.5 * data[index + 2]) + (0.5 * data[index + 3]);
    b = (1.0 * data[index]) + (-2.5 * data[index + 1]) + (2.0 * data[index + 2]) + (-0.5 * data[index + 3]);
    c = (data[index + 2] * 0.5) + (0.0 * data[index + 1]) + (-0.5 * data[index]) + (0.0 * data[index + 3]);

    ret = (1.0 * data[index + 1]);
    *derivative = (((a * 3 * x) + (2 * b)) * x) + c;
    ret = (((((a * x) + b) * x) + c) * x) + ret;

    return ret;
}

f32 catmull_rom_derivative(f32 *data, s32 index, f32 x) {
    f32 derivative;
    f32 c, b, a;

    a = (-0.5 * data[index]) + (1.5 * data[index + 1]) + (-1.5 * data[index + 2]) + (0.5 * data[index + 3]);
    b = (1.0 * data[index]) + (-2.5 * data[index + 1]) + (2.0 * data[index + 2]) + (-0.5 * data[index + 3]);
    c = (data[index + 2] * 0.5) + (0.0 * data[index + 1]) + (-0.5 * data[index]) + (0.0 * data[index + 3]);

    derivative = (((a * 3 * x) + (2 * b)) * x) + c;

    return derivative;
}

/**
 * Imprecise method, which does not guarantee v = v1 when t = 1. (From Wikipedia)
 */
f32 lerp(f32 *data, u32 index, f32 t) {
    f32 result = data[index + 1] + t * ((data[index + 2] - data[index + 1]));
    return result;
}

/**
 * Peforms the lerp, and also returns the distance between the two points.
 */
f32 lerp_and_get_derivative(f32 *data, u32 index, f32 t, f32 *derivative) {
    f32 lerp;
    f32 vector;
    vector = data[index + 2] - data[index + 1];
    lerp = data[index + 1] + (vector * t);
    *derivative = vector;
    return lerp;
}

UNUSED void func_800228DC(UNUSED s32 arg0, UNUSED s32 arg1, UNUSED s32 arg2) {
}

/**
 * Prepares the player racer for a Taj Challenge race.
 * Prevents them from pausing for 10 frames.
 */
void init_racer_for_challenge(s32 vehicleID) {
    Object_Racer *racer;

    gTajRaceInit = 3;
    racer = get_racer_object(PLAYER_ONE)->racer;
    racer->courseCheckpoint = 0;
    racer->nextCheckpoint = 0;
    racer->lap = 0;
    racer->unk1BA = 0;
    set_taj_challenge_type(vehicleID);
    set_pause_lockout_timer(10);
#ifdef NATIVE_PORT
    mdkr_taj_trace_phase("accept", vehicleID, NULL, -1, 0, 0);
#endif
}

/**
 * Initialise the relevant variables related to races.
 * These are usually done on level load, but Taj challenges don't reset the level.
 */
void mode_init_taj_race(void) {
    CheckpointNode *checkpointNode;
    UNUSED s32 pad;
    s32 j;
    s32 i;
    s32 lvlSeg;
    LevelHeader *levelHeader;
    LevelObjectEntry_Racer newRacerEntry;
    Settings *settings;
    Object *racerObj;
    Object_Racer *racer;
#ifdef NATIVE_PORT
    f32 yOut[COLLISION_Y_QUERY_CAPACITY];
#else
    f32 yOut[8];
#endif

    gTajRaceInit -= 1;
    if (gTajRaceInit == 0) {
        levelHeader = level_header();
        gChallengePrevMusic = levelHeader->music;
        gChallengePrevInstruments = levelHeader->instruments;
        levelHeader->music = SEQUENCE_TAJS_RACES;
        levelHeader->instruments = 0xFFFF;
        racerObj = get_racer_object(PLAYER_ONE);
        racer = racerObj->racer;
        gIsTajChallenge = racer->vehicleID + 1;
        checkpointNode = func_800230D0(racerObj, racer);
        racer->cameraYaw = 0x8000 - racer->steerVisualRotation;
        racer->wrongWayCounter = 0;
        racer->startInput = 0;
        racer->courseCheckpoint = 0;
        racer->nextCheckpoint = 0;
        racer->lap = 0;
        racer->countLap = 0;
        racer->lap_times[0] = 0;
        racer->lap_times[1] = 0;
        racer->lap_times[2] = 0;
        racer->unk1BA = 0;
        settings = get_settings();
        gEventCountdown = 80;
        gRaceFinishTriggered = FALSE;
        gNumFinishedRacers = 1;
        levelHeader->laps = 3;
        levelHeader->race_type = RACETYPE_DEFAULT;
        hud_init_element();
        // clang-format off
        for (i = 0; i < ARRAY_COUNT(racer->lap_times); i++) { racer->lap_times[i] = 0; } // Must be a single line.
        // clang-format on
        newRacerEntry.common.x = (checkpointNode->x + (checkpointNode->rotationZFrac * 35.0f));
        newRacerEntry.common.z = (checkpointNode->z - (checkpointNode->rotationXFrac * 35.0f));
        lvlSeg =
            get_level_segment_index_from_position(newRacerEntry.common.x, checkpointNode->y, newRacerEntry.common.z);
        newRacerEntry.common.y =
#ifdef NATIVE_PORT
            collision_get_y(lvlSeg, newRacerEntry.common.x, newRacerEntry.common.z, yOut, ARRAY_COUNT(yOut))
                ? yOut[0]
                : checkpointNode->y;
#else
            collision_get_y(lvlSeg, newRacerEntry.common.x, newRacerEntry.common.z, yOut) ? yOut[0] : checkpointNode->y;
#endif
        newRacerEntry.common.size = 16;
        newRacerEntry.angleY = racer->steerVisualRotation;
        newRacerEntry.angleX = 0;
        newRacerEntry.angleZ = 0;
        newRacerEntry.playerIndex = 4;
        newRacerEntry.common.objectID = ASSET_OBJECT_ID_FLYINGCARPET;
        model_anim_offset(0);
        racerObj = spawn_object(&newRacerEntry.common, OBJECT_SPAWN_UNK01);
        (*gRacers)[1] = racerObj;
        gRacersByPosition[1] = racerObj;
        gRacersByPort[1] = racerObj;
        racerObj->level_entry = NULL;
        gNumRacers = 2;
        racer = racerObj->racer;
        i = 0; // Fakematch
        racer->vehicleID = VEHICLE_CARPET;
        racer->vehicleIDPrev = racer->vehicleID;
        racer->racerIndex = 1;
        racer->characterId = settings->racers[0].character;
        racer->stretch_height_cap = 1.0f;
        racer->stretch_height = 1.0f;
        racer->transparency = 0xFF;
        racer->vehicleSound = NULL;
        racerObj->interactObj->pushForce = 2;

        for (j = gObjectListStart; j < gObjectCount; j++) {
            if (!(gObjPtrList[j]->trans.flags & OBJ_FLAGS_PARTICLE) && gObjPtrList[j]->behaviorId == BHV_PARK_WARDEN) {
                racer->unk154 = gObjPtrList[j];
            }
        }
        set_pause_lockout_timer(20);
        transition_begin(&gTajChallengeTransition);
#ifdef NATIVE_PORT
        mdkr_taj_trace_phase(
            "start", racer->vehicleID == VEHICLE_CARPET
                         ? gIsTajChallenge - 1
                         : racer->vehicleID,
            NULL, -1, 0, 0);
#endif
    }
}

/**
 * Finds which golden balloon should be moved, and move it onto Taj's current position.
 * Sets opacity to 0, so the balloon with smoothly fade in.
 */
void obj_taj_create_balloon(s32 blockID, f32 x, f32 y, f32 z) {
    s32 i;
    Object *obj;
    Settings *settings = get_settings();

    for (i = 0; i < gObjectCount; i++) {
        obj = gObjPtrList[i];
        if (obj->behaviorId == BHV_GOLDEN_BALLOON && obj->level_entry != NULL &&
            obj->level_entry->goldenBalloon.challengeID >= 1 &&
            obj->level_entry->goldenBalloon.challengeID <= 3) {
            if (settings->tajFlags &&
                settings->tajFlags &
                    (UINT32_C(1) <<
                     (obj->level_entry->goldenBalloon.challengeID + 2))) {
                obj->trans.x_position = x;
                obj->trans.y_position = y + 10.0;
                obj->trans.z_position = z;
                obj->segmentID = blockID;
                obj->properties.common.unk0 = 0;
                obj->opacity = 0;
            }
        }
    }
}

/**
 * Revert changes set by the game when starting the Taj challenge.
 * This includes the music set, as well as the extra racer spawned.
 * Has a different response depending on whether the challenge was aborted or finished.
 */
void mode_end_taj_race(s32 reason) {
    s32 flags;
    s32 i;
    s32 menu;
    s32 vehicle;
    Object_Racer *racer;
    Settings *settings;
    Object *obj;
    LevelHeader *levelHeader;

    racer = (*gRacers)[0]->racer;
    vehicle = racer->vehicleID;
    menu = 0;
#ifdef NATIVE_PORT
    mdkr_taj_trace_phase(
        "end_before", vehicle, NULL, reason, 0, 0);
#endif
    levelHeader = level_header();
    levelHeader->race_type = RACETYPE_HUBWORLD;
    levelHeader->music = gChallengePrevMusic;
    levelHeader->instruments = gChallengePrevInstruments;
    minimap_opacity_set(1);

    // Only works with do {} while?
    i = 0;
    do {
        racer = (*gRacers)[i]->racer;
        if (!racer) {}
        racer->raceFinished = 0;
        racer->lap = 0;
        racer->nextCheckpoint = 0;
        racer->courseCheckpoint = 0;
    } while (++i < gNumRacers);

    free_object((*gRacers)[1]);
    gRacersByPosition[0] = (*gRacers)[0];
    gNumRacers = 1;
    obj = NULL;
    for (i = gObjectListStart; i < gObjectCount; i++) {
        if (!(gObjPtrList[i]->trans.flags & OBJ_FLAGS_PARTICLE) && gObjPtrList[i]->behaviorId == BHV_PARK_WARDEN) {
            obj = gObjPtrList[i];
        }
    }
    racer = (*gRacers)[0]->racer;
    if (racer->challengeMarker != NULL) {
        free_object(racer->challengeMarker);
        racer->challengeMarker = NULL;
    }
    if (reason == CHALLENGE_END_FINISH) {
        if (racer->finishPosition == 1) {
            settings = get_settings();
            flags = racer->vehicleID;
            if (flags < VEHICLE_CAR || flags > VEHICLE_PLANE) {
                menu = 4;
            } else {
                flags = (s32) (UINT32_C(1) << (flags + 3));
                if ((settings->tajFlags & flags) ||
#ifdef NATIVE_PORT
                    !mdkr_taj_completion_gate_enabled(racer->vehicleID)
#else
                    FALSE
#endif
                ) {
                    menu = 5;
                } else {
                    menu = racer->vehicleID + 6;
                    settings->tajFlags |= flags;
                    safe_mark_write_save_file(get_save_file_index());
                }
#ifdef NATIVE_PORT
                /* Also recover a global sidecar lost after an imported 0x38 save. */
                if (taj_mod_unlock_from_taj_flags(settings->tajFlags)) {
                    MDKR_TRACE("taj_mod_unlock: route=taj_challenges flags=0x%x",
                               settings->tajFlags);
                    sound_play(SOUND_VOICE_TAJ_WAHEY, NULL);
                }
#endif
            }
        } else {
            menu = 4;
        }
        set_next_taj_challenge_menu(menu);
        if (obj != NULL) {
            obj->properties.common.unk0 = 31;
        }
        set_taj_status(TAJ_TELEPORT);
    } else {
        music_change_on();
        music_stop();
        set_next_taj_challenge_menu(0);
        audspat_jingle_on();
        if (reason == CHALLENGE_END_OOB) {
            set_current_text(ASSET_GAME_TEXT_0);
        }
        gEventCountdown = 0;
        gEventStartTimer = 0;
        if (obj != NULL) {
            obj->properties.common.unk0 = 20;
        }
    }
    music_change_on();
    hud_audio_init();
    level_music_start(1.0f);
    gIsTajChallenge = FALSE;
#ifdef NATIVE_PORT
    mdkr_taj_trace_phase(
        "end_after", vehicle, NULL, reason, menu, 0);
#endif
}

CheckpointNode *func_800230D0(Object *obj, Object_Racer *racer) {
    CheckpointNode *lastCheckpointNode;
    Camera *activeCameraSegment;
    s32 yOutCount;
#ifdef NATIVE_PORT
    f32 yOut[COLLISION_Y_QUERY_CAPACITY];
#else
    f32 yOut[9];
#endif
    Object *ptrList = NULL;
    s32 i;

    if (gNumberOfMainCheckpoints == 0) {
        lastCheckpointNode = NULL;
        for (i = 0; i < gObjectCount; i++) {
            ptrList = gObjPtrList[i];
            if (ptrList == NULL) {
                continue;
            }
            if (!(ptrList->trans.flags & OBJ_FLAGS_PARTICLE) && (ptrList->behaviorId == BHV_SETUP_POINT)) {
                if (ptrList->properties.setupPoint.racerIndex == 0) {
                    obj->trans.x_position = ptrList->trans.x_position;
                    obj->trans.y_position = ptrList->trans.y_position;
                    obj->trans.z_position = ptrList->trans.z_position;
                    obj->segmentID = ptrList->segmentID;
                    i = gObjectCount;
                }
            }
        }
    } else {
        lastCheckpointNode = &gTrackCheckpoints[gNumberOfMainCheckpoints - 1];
        obj->trans.x_position = lastCheckpointNode->x - (lastCheckpointNode->rotationZFrac * 35.0f);
        obj->trans.y_position = lastCheckpointNode->y;
        obj->trans.z_position = lastCheckpointNode->z + (lastCheckpointNode->rotationXFrac * 35.0f);
        obj->segmentID =
            get_level_segment_index_from_position(obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
    }
#ifdef NATIVE_PORT
    yOutCount = collision_get_y(obj->segmentID, obj->trans.x_position, obj->trans.z_position, yOut,
                                ARRAY_COUNT(yOut));
#else
    yOutCount = collision_get_y(obj->segmentID, obj->trans.x_position, obj->trans.z_position, yOut);
#endif
    if (yOutCount != 0) {
        obj->trans.y_position = yOut[yOutCount - 1];
    }
    racer->prev_x_position = obj->trans.x_position;
    racer->prev_y_position = obj->trans.y_position;
    racer->prev_z_position = obj->trans.z_position;
    if (lastCheckpointNode != NULL) {
        racer->steerVisualRotation = arctan2_f(lastCheckpointNode->rotationXFrac, lastCheckpointNode->rotationZFrac);
    } else if (ptrList != NULL) {
        racer->steerVisualRotation = ptrList->trans.rotation.y_rotation;
    } else {
        /* A malformed level without checkpoints/setup points retains heading. */
        racer->steerVisualRotation = obj->trans.rotation.y_rotation;
    }
    racer->nextCheckpoint = 0;
    racer->courseCheckpoint = racer->lap * gNumberOfMainCheckpoints;
    obj->trans.rotation.y_rotation = racer->steerVisualRotation;
    racer->unkD8[0] = obj->trans.x_position;
    racer->unkD8[1] = obj->trans.y_position + 15.0f;
    racer->unkD8[2] = obj->trans.z_position;
    racer->unkD8[3] = obj->trans.x_position;
    racer->unkD8[4] = obj->trans.y_position + 15.0f;
    racer->unkD8[5] = obj->trans.z_position;
    racer->unkD8[6] = obj->trans.x_position;
    racer->unkD8[7] = obj->trans.y_position + 15.0f;
    racer->unkD8[8] = obj->trans.z_position;
    racer->unkD8[9] = obj->trans.x_position;
    racer->unkD8[10] = obj->trans.y_position + 15.0f;
    racer->unkD8[11] = obj->trans.z_position;
    obj->interactObj->x_position = obj->trans.x_position;
    obj->interactObj->y_position = obj->trans.y_position;
    obj->interactObj->z_position = obj->trans.z_position;
    // fake
    if (1) {}
    if (1) {}
    racer->velocity = 0.0f;
    racer->lateral_velocity = 0.0f;
    obj->x_velocity = 0.0f;
    obj->z_velocity = 0.0f;
    racer->vehicleID = racer->vehicleIDPrev;
    if (racer->playerIndex != -1) {
        set_active_camera(racer->playerIndex);
        activeCameraSegment = cam_get_active_camera_no_cutscenes();
        activeCameraSegment->trans.x_position = obj->trans.x_position;
        activeCameraSegment->trans.y_position = obj->trans.y_position;
        activeCameraSegment->trans.z_position = obj->trans.z_position;
    }
    return lastCheckpointNode;
}

/**
 * Returns true if a taj challenge is currently active.
 */
s8 is_taj_challenge(void) {
    return gIsTajChallenge;
}

/**
 * Searches for the furthest teleport anchor and returns it.
 */
Object *find_furthest_telepoint(f32 x, f32 z) {
    Object *tempObj;
    Object *bestObj;
    f32 diffX;
    f32 diffZ;
    f32 distance;
    f32 bestDist;
    s32 i;

    bestDist = 0.0f;
    i = 0;
    bestObj = NULL;
    if (gObjectCount > 0) {
        do {
            tempObj = gObjPtrList[i];
            if (!(tempObj->trans.flags & OBJ_FLAGS_PARTICLE) && tempObj->behaviorId == BHV_TAJ_TELEPOINT) {
                diffX = tempObj->trans.x_position - x;
                diffZ = tempObj->trans.z_position - z;
                tempObj = gObjPtrList[i]; // fakematch
                distance = sqrtf((diffX * diffX) + (diffZ * diffZ));
                if (bestDist < distance) {
                    bestDist = distance;
                    bestObj = tempObj;
                }
            }
            i += 1;
        } while (i < gObjectCount);
    }
    return bestObj;
}

s32 func_80023568(void) {
    if (D_8011AD3C != 0) {
        return D_8011AD24[1] + 1;
    } else if (level_type() == RACETYPE_BOSS) {
        return D_8011AD24[1] + 1;
    }
    return 0;
}

/**
 * Return whether doors can be forced open.
 */
s8 obj_door_override(void) {
    return gOverrideDoors;
}

/**
 * Set a value that decides whether doors can be forced open.
 */
void obj_door_open(s32 setting) {
    gOverrideDoors = setting;
}

/**
 * Return the size of the object property struct intended to be used with the object.
 */
s32 get_object_property_size(Object *obj, void *obj64) {
#ifndef NATIVE_PORT
    s32 temp_v0;
#endif
    s32 ret = 0;

#ifdef NATIVE_PORT
    /*
     * Native spawn asks for the size first, then lets the checked layout cursor
     * choose a 16-byte-aligned address and assigns the union pointer there. Do
     * not derive alignment by truncating a host pointer or add N64-only slack.
     */
    obj->anyBehaviorData = NULL;
#else
    obj->anyBehaviorData = obj64;
#endif

    switch (obj->header->behaviorId) {
        case BHV_RACER:
            ret = sizeof(Object_Racer);
            break;
        case BHV_DOOR:
        case BHV_TT_DOOR:
            ret = sizeof(Object_Door);
            break;
        case BHV_EXIT:
            ret = sizeof(Object_Exit);
            break;
        case BHV_ANIMATOR:
            ret = sizeof(Object_Animator);
            break;
        case BHV_AUDIO:
            ret = sizeof(Object_Audio);
            break;
        case BHV_AUDIO_LINE:
        case BHV_AUDIO_LINE_2:
            ret = sizeof(Object_AudioLine);
            break;
        case BHV_AINODE:
            ret = sizeof(Object_AiNode);
            break;
        case BHV_MODECHANGE:
        case BHV_BONUS:
        case BHV_TRIGGER:
            ret = sizeof(Object_Trigger);
            break;
        case BHV_AUDIO_REVERB:
            ret = sizeof(Object_AudioReverb);
            break;
        case BHV_TEXTURE_SCROLL:
            ret = sizeof(Object_TexScroll);
            break;
        case BHV_WEAPON:
        case BHV_WEAPON_2:
            ret = sizeof(Object_Weapon);
            break;
        case BHV_WEAPON_BALLOON:
            ret = sizeof(Object_WeaponBalloon);
            break;
        case BHV_BANANA:
            ret = sizeof(Object_Banana);
            break;
        case BHV_BRIDGE_WHALE_RAMP:
            ret = sizeof(Object_Bridge_WhaleRamp);
            break;
        case BHV_SEA_MONSTER:
            ret = 0x18;
            break;
        case BHV_COLLECT_EGG:
            ret = sizeof(Object_CollectEgg);
            break;
        case BHV_STOPWATCH_MAN:
        case BHV_PARK_WARDEN:
        case BHV_GOLDEN_BALLOON:
            ret = sizeof(Object_NPC);
            break;
        case BHV_LASER_GUN:
            ret = sizeof(Object_LaserGun);
            break;
        case BHV_OVERRIDE_POS:
            ret = sizeof(Object_OverridePos);
            break;
        case BHV_DINO_WHALE:
        case BHV_ANIMATED_OBJECT:
        case BHV_CAMERA_ANIMATION:
        case BHV_CAR_ANIMATION:
        case BHV_CHARACTER_SELECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_PARK_WARDEN_2:
        case BHV_ANIMATED_OBJECT_2:
        case BHV_WIZPIG_SHIP:
        case BHV_ANIMATED_OBJECT_3:
        case BHV_ANIMATED_OBJECT_4:
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
        case BHV_HIT_TESTER_3:
        case BHV_HIT_TESTER_4:
        case BHV_DOOR_OPENER:
        case BHV_PIG_ROCKETEER:
        case BHV_WIZPIG_GHOSTS:
            ret = sizeof(Object_AnimatedObject);
            break;
        case BHV_MIDI_FADE:
            ret = sizeof(Object_MidiFade);
            break;
        case BHV_MIDI_FADE_POINT:
            ret = sizeof(Object_MidiFadePoint);
            break;
        case BHV_MIDI_CHANNEL_SET:
            ret = sizeof(Object_MidiChannelSet);
            break;
        case BHV_BUTTERFLY:
#ifdef NATIVE_PORT
            ret = sizeof(Object_Butterfly);
#else
            temp_v0 = 0x10 - ((s32) obj64 & 0xF);
            obj->butterfly = (Object_Butterfly *) ((u8 *) obj64 + temp_v0);
            ret = (temp_v0 + sizeof(Object_Butterfly));
#endif
            break;
        case BHV_FISH:
#ifdef NATIVE_PORT
            ret = sizeof(Object_Fish);
#else
            temp_v0 = 0x10 - ((s32) obj64 & 0xF);
            obj->fish = (Object_Fish *) ((u8 *) obj64 + temp_v0);
            ret = (temp_v0 + sizeof(Object_Fish));
#endif
            break;
        case BHV_CHARACTER_FLAG:
#ifdef NATIVE_PORT
            ret = sizeof(CharacterFlagModel);
#else
            temp_v0 = 0x10 - ((s32) obj64 & 0xF);
            obj->characterFlagModel = (CharacterFlagModel *) ((u8 *) obj64 + temp_v0);
            ret = (temp_v0 + sizeof(CharacterFlagModel));
#endif
            break;
        case BHV_UNK_5E:
            ret = 0x60;
            break;
        case BHV_TROPHY_CABINET:
            ret = sizeof(JingleState);
            break;
        case BHV_FROG:
            ret = sizeof(Object_Frog);
            break;
        case BHV_FIREBALL_OCTOWEAPON_2:
            ret = sizeof(Object_Weapon);
            break;
        default:
            obj->anyBehaviorData = NULL;
            break;
    }

#ifdef NATIVE_PORT
    return ret;
#else
    return (ret & ~3) + 4;
#endif
}

/**
 * Run when an object is created.
 * Used to do one-time things like initialising variables
 * Arg2 is always zero. Effectively unused.
 */
void run_object_init_func(Object *obj, void *entry, s32 param) {
    obj->behaviorId = obj->header->behaviorId;
#ifdef NATIVE_PORT
    if (taj_visual_claim_spawned_object(obj)) {
        return;
    }
#endif
    switch (obj->behaviorId) {
        case BHV_RACER:
            obj_init_racer(obj, (LevelObjectEntry_Racer *) entry);
            break;
        case BHV_SCENERY:
            obj_init_scenery(obj, (LevelObjectEntry_Scenery *) entry);
            break;
        case BHV_FISH:
            obj_init_fish(obj, (LevelObjectEntry_Fish *) entry, param);
            break;
        case BHV_ANIMATOR:
            obj_init_animator(obj, (LevelObjectEntry_Animator *) entry, param);
            break;
        case BHV_SMOKE:
            obj_init_smoke(obj, (LevelObjectEntry_Smoke *) entry);
            break;
        case BHV_UNK_19:
            obj_init_unknown25(obj, (LevelObjectEntry_Unknown25 *) entry);
            break;
        case BHV_BOMB_EXPLOSION:
            obj_init_bombexplosion(obj, (LevelObjectEntry_BombExplosion *) entry);
            break;
        case BHV_EXIT:
            obj_init_exit(obj, (LevelObjectEntry_Exit *) entry);
            break;
        case BHV_AUDIO:
            obj_init_audio(obj, (LevelObjectEntry_Audio *) entry);
            break;
        case BHV_AUDIO_LINE:
        case BHV_AUDIO_LINE_2:
            obj_init_audioline(obj, (LevelObjectEntry_AudioLine *) entry);
            break;
        case BHV_AUDIO_REVERB:
            obj_init_audioreverb(obj, (LevelObjectEntry_AudioReverb *) entry);
            break;
        case BHV_CAMERA_CONTROL:
            obj_init_cameracontrol(obj, (LevelObjectEntry_CameraControl *) entry);
            break;
        case BHV_SETUP_POINT:
            obj_init_setuppoint(obj, (LevelObjectEntry_SetupPoint *) entry);
            break;
        case BHV_DINO_WHALE:
            obj_init_dino_whale(obj, (LevelObjectEntry_Dino_Whale *) entry);
            break;
        case BHV_CHECKPOINT:
            obj_init_checkpoint(obj, (LevelObjectEntry_Checkpoint *) entry, param);
            break;
        case BHV_MODECHANGE:
            obj_init_modechange(obj, (LevelObjectEntry_ModeChange *) entry);
            break;
        case BHV_BONUS:
            obj_init_bonus(obj, (LevelObjectEntry_Bonus *) entry);
            break;
        case BHV_DOOR:
            obj_init_door(obj, (LevelObjectEntry_Door *) entry);
            break;
        case BHV_TT_DOOR:
            obj_init_ttdoor(obj, (LevelObjectEntry_TTDoor *) entry);
            break;
        case BHV_FOG_CHANGER:
            obj_init_fogchanger(obj, (LevelObjectEntry_FogChanger *) entry);
            break;
        case BHV_AINODE:
            obj_init_ainode(obj, (LevelObjectEntry_AiNode *) entry);
            break;
        case BHV_WEAPON_BALLOON:
            obj_init_weaponballoon(obj, (LevelObjectEntry_WeaponBalloon *) entry);
            break;
        case BHV_BALLOON_POP:
            obj_init_wballoonpop(obj, (LevelObjectEntry_WBalloonPop *) entry);
            break;
        case BHV_WEAPON:
        case BHV_WEAPON_2:
            obj_init_weapon(obj, (LevelObjectEntry_Weapon *) entry);
            break;
        case BHV_SKY_CONTROL:
            obj_init_skycontrol(obj, (LevelObjectEntry_SkyControl *) entry);
            break;
        case BHV_TORCH_MIST:
            obj_init_torch_mist(obj, (LevelObjectEntry_Torch_Mist *) entry);
            break;
        case BHV_TEXTURE_SCROLL:
            obj_init_texscroll(obj, (LevelObjectEntry_TexScroll *) entry, param);
            break;
        case BHV_STOPWATCH_MAN:
            obj_init_stopwatchman(obj, (LevelObjectEntry_StopWatchMan *) entry);
            break;
        case BHV_BANANA:
            obj_init_banana(obj, (LevelObjectEntry_Banana *) entry);
            break;
        case BHV_LIGHT_RGBA:
            obj_init_rgbalight(obj, (LevelObjectEntry_RgbaLight *) entry, param);
            break;
        case BHV_BUOY_PIRATE_SHIP:
            obj_init_buoy_pirateship(obj, (LevelObjectEntry_Buoy_PirateShip *) entry, param);
            break;
        case BHV_LOG:
            obj_init_log(obj, (LevelObjectEntry_Log *) entry, param);
            break;
        case BHV_WEATHER:
            obj_init_weather(obj, (LevelObjectEntry_Weather *) entry);
            break;
        case BHV_BRIDGE_WHALE_RAMP:
            obj_init_bridge_whaleramp(obj, (LevelObjectEntry_Bridge_WhaleRamp *) entry);
            break;
        case BHV_RAMP_SWITCH:
            obj_init_rampswitch(obj, (LevelObjectEntry_RampSwitch *) entry);
            break;
        case BHV_SEA_MONSTER:
            obj_init_seamonster(obj, (LevelObjectEntry_SeaMonster *) entry);
            break;
        case BHV_LENS_FLARE:
            obj_init_lensflare(obj, (LevelObjectEntry_LensFlare *) entry);
            break;
        case BHV_LENS_FLARE_SWITCH:
            obj_init_lensflareswitch(obj, (LevelObjectEntry_LensFlareSwitch *) entry, param);
            break;
        case BHV_COLLECT_EGG:
            obj_init_collectegg(obj, (LevelObjectEntry_CollectEgg *) entry);
            break;
        case BHV_EGG_CREATOR:
            obj_init_eggcreator(obj, (LevelObjectEntry_EggCreator *) entry);
            break;
        case BHV_CHARACTER_FLAG:
            obj_init_characterflag(obj, (LevelObjectEntry_CharacterFlag *) entry);
            break;
        case BHV_ANIMATION:
            obj_init_animation(obj, (LevelObjectEntry_Animation *) entry, param);
            break;
        case BHV_INFO_POINT:
            obj_init_infopoint(obj, (LevelObjectEntry_InfoPoint *) entry);
            break;
        case BHV_TRIGGER:
            obj_init_trigger(obj, (LevelObjectEntry_Trigger *) entry);
            break;
        case BHV_ZIPPER_WATER:
        case BHV_ZIPPER_AIR:
            obj_init_airzippers_waterzippers(obj, (LevelObjectEntry_AirZippers_WaterZippers *) entry);
            break;
        case BHV_TIMETRIAL_GHOST:
            obj_init_timetrialghost(obj, (LevelObjectEntry_TimeTrial_Ghost *) entry);
            break;
        case BHV_WAVE_GENERATOR:
            obj_init_wavegenerator(obj, (LevelObjectEntry_WaveGenerator *) entry, param);
            break;
        case BHV_BUTTERFLY:
            obj_init_butterfly(obj, (LevelObjectEntry_Butterfly *) entry, param);
            break;
        case BHV_PARK_WARDEN:
            obj_init_parkwarden(obj, (LevelObjectEntry_Parkwarden *) entry);
            break;
        case BHV_WORLD_KEY:
            obj_init_worldkey(obj, (LevelObjectEntry_WorldKey *) entry);
            break;
        case BHV_BANANA_SPAWNER:
            obj_init_bananacreator(obj, (LevelObjectEntry_BananaCreator *) entry);
            break;
        case BHV_TREASURE_SUCKER:
            obj_init_treasuresucker(obj, (LevelObjectEntry_TreasureSucker *) entry);
            break;
        case BHV_LAVA_SPURT:
            obj_init_lavaspurt(obj, (LevelObjectEntry_LavaSpurt *) entry);
            break;
        case BHV_POS_ARROW:
            obj_init_posarrow(obj, (LevelObjectEntry_PosArrow *) entry);
            break;
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_3:
            obj_init_hittester(obj, (LevelObjectEntry_HitTester *) entry);
            break;
        case BHV_HIT_TESTER_2:
        case BHV_DYNAMIC_LIGHT_OBJECT_2:
        case BHV_HIT_TESTER_4:
            obj_init_dynamic_lighting_object(obj, (LevelObjectEntry_DynamicLightingObject *) entry);
            break;
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_3:
            obj_init_unknown96(obj, (LevelObjectEntry_Unknown96 *) entry);
            break;
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_4:
            obj_init_snowball(obj, (LevelObjectEntry_Snowball *) entry);
            break;
        case BHV_MIDI_FADE:
            obj_init_midifade(obj, (LevelObjectEntry_MidiFade *) entry);
            break;
        case BHV_MIDI_FADE_POINT:
            obj_init_midifadepoint(obj, (LevelObjectEntry_MidiFadePoint *) entry);
            break;
        case BHV_MIDI_CHANNEL_SET:
            obj_init_midichset(obj, (LevelObjectEntry_Midichset *) entry);
            break;
        case BHV_EFFECT_BOX:
            obj_init_effectbox(obj, (LevelObjectEntry_EffectBox *) entry);
            break;
        case BHV_TROPHY_CABINET:
            obj_init_trophycab(obj, (LevelObjectEntry_TrophyCab *) entry);
            break;
        case BHV_BUBBLER:
            obj_init_bubbler(obj, (LevelObjectEntry_Bubbler *) entry);
            break;
        case BHV_FLY_COIN:
            obj_init_flycoin(obj, (LevelObjectEntry_FlyCoin *) entry);
            break;
        case BHV_GOLDEN_BALLOON:
            obj_init_goldenballoon(obj, (LevelObjectEntry_GoldenBalloon *) entry);
            break;
        case BHV_LASER_BOLT:
            obj_init_laserbolt(obj, (LevelObjectEntry_Laserbolt *) entry);
            break;
        case BHV_LASER_GUN:
            obj_init_lasergun(obj, (LevelObjectEntry_Lasergun *) entry);
            break;
        case BHV_ZIPPER_GROUND:
            obj_init_groundzipper(obj, (LevelObjectEntry_GroundZipper *) entry);
            break;
        case BHV_OVERRIDE_POS:
            obj_init_overridepos(obj, (LevelObjectEntry_OverridePos *) entry);
            break;
        case BHV_WIZPIG_SHIP:
            obj_init_wizpigship(obj, (LevelObjectEntry_WizpigShip *) entry);
            break;
        case BHV_BOOST:
            obj_init_boost(obj, (LevelObjectEntry_Boost2 *) entry);
            break;
        case BHV_SILVER_COIN:
            obj_init_silvercoin(obj, (LevelObjectEntry_SilverCoin *) entry);
            break;
        case BHV_WARDEN_SMOKE:
            obj_init_wardensmoke(obj, (LevelObjectEntry_WardenSmoke *) entry);
            break;
        case BHV_UNK_5E:
            obj_init_unknown94(obj, (LevelObjectEntry_Unknown94 *) entry, param);
            break;
        case BHV_TELEPORT:
            obj_init_teleport(obj, (LevelObjectEntry_Teleport *) entry);
            break;
        case BHV_ROCKET_SIGNPOST:
        case BHV_ROCKET_SIGNPOST_2:
            obj_init_lighthouse_rocketsignpost(obj, (LevelObjectEntry_Lighthouse_RocketSignpost *) entry);
            break;
        case BHV_RANGE_TRIGGER:
            obj_init_rangetrigger(obj, (LevelObjectEntry_RangeTrigger *) entry);
            break;
        case BHV_FIREBALL_OCTOWEAPON:
        case BHV_FIREBALL_OCTOWEAPON_2:
            obj_init_fireball_octoweapon(obj, (LevelObjectEntry_Fireball_Octoweapon *) entry);
            break;
        case BHV_FROG:
            obj_init_frog(obj, (LevelObjectEntry_Frog *) entry);
            break;
        case BHV_SILVER_COIN_2:
            obj_init_silvercoin_adv2(obj, (LevelObjectEntry_SilverCoinAdv2 *) entry);
            break;
        case BHV_LEVEL_NAME:
            obj_init_levelname(obj, (LevelObjectEntry_LevelName *) entry);
            break;
    }
}

/**
 * Set initialisation property flags based off object ID.
 * This includes things like shadow data, interaction and visuals.
 */
s32 obj_init_property_flags(s32 behaviorId) {
    s32 flags = OBJECT_BEHAVIOUR_NONE;
    switch (behaviorId) {
        case BHV_RACER:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_WATER_EFFECT |
                    OBJECT_BEHAVIOUR_ANIMATION | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_SCENERY:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_WEAPON:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_WATER_EFFECT | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_DINO_WHALE:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION |
                    OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_DOOR:
        case BHV_TT_DOOR:
            flags = OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_WEAPON_BALLOON:
        case BHV_GOLDEN_BALLOON:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION |
                    OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
        case BHV_HIT_TESTER_3:
        case BHV_HIT_TESTER_4:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION | OBJECT_BEHAVIOUR_INTERACTIVE |
                    OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_UNK_18:
            flags = OBJECT_BEHAVIOUR_WATER_EFFECT;
            break;
        case BHV_STOPWATCH_MAN:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION |
                    OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_BANANA:
        case BHV_WORLD_KEY:
        case BHV_SILVER_COIN:
        case BHV_SILVER_COIN_2:
            flags = OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_SHADOW;
            break;
        case BHV_LOG:
            flags = OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_BRIDGE_WHALE_RAMP:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_ANIMATION | OBJECT_BEHAVIOUR_INTERACTIVE |
                    OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_RAMP_SWITCH:
            flags = OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_SHADOW;
            break;
        case BHV_SEA_MONSTER:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_COLLECT_EGG:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_UNK_30:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_UNK_3F:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_ANIMATED_OBJECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_PARK_WARDEN_2:
        case BHV_WIZPIG_SHIP:
        case BHV_ANIMATED_OBJECT_4:
        case BHV_PIG_ROCKETEER:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_CHARACTER_SELECT:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_TROPHY_CABINET:
        case BHV_DYNAMIC_LIGHT_OBJECT_2:
        case BHV_ROCKET_SIGNPOST:
        case BHV_ROCKET_SIGNPOST_2:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_INTERACTIVE | OBJECT_BEHAVIOUR_COLLIDABLE;
            break;
        case BHV_UNK_5B:
            flags = OBJECT_BEHAVIOUR_SHADED;
            break;
        case BHV_ANIMATED_OBJECT_2:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_EXIT:
        case BHV_CHECKPOINT:
        case BHV_WEAPON_2:
        case BHV_SKY_CONTROL:
        case BHV_MODECHANGE:
        case BHV_BUOY_PIRATE_SHIP:
        case BHV_BONUS:
        case BHV_INFO_POINT:
        case BHV_TRIGGER:
        case BHV_ZIPPER_WATER:
        case BHV_LAVA_SPURT:
        case BHV_LASER_BOLT:
        case BHV_LASER_GUN:
        case BHV_ZIPPER_AIR:
        case BHV_TELEPORT:
        case BHV_FIREBALL_OCTOWEAPON:
        case BHV_FIREBALL_OCTOWEAPON_2:
            flags = OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_ZIPPER_GROUND:
            flags = OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_ANIMATION:
        case BHV_CAMERA_ANIMATION:
        case BHV_BUTTERFLY:
            flags = OBJECT_BEHAVIOUR_SHADOW;
            break;
        case BHV_PARK_WARDEN:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION |
                    OBJECT_BEHAVIOUR_INTERACTIVE;
            break;
        case BHV_FROG:
            flags = OBJECT_BEHAVIOUR_SHADED | OBJECT_BEHAVIOUR_SHADOW | OBJECT_BEHAVIOUR_ANIMATION;
            break;
        case BHV_UNK_72:
            flags = OBJECT_BEHAVIOUR_SHADED;
            break;
    }
    return flags;
}

/**
 * Run every frame for most objects with set behaviours.
 * One big switch statement for whichever object.
 */
void run_object_loop_func(Object *obj, s32 updateRate) {
    update_object_stack_trace(OBJECT_UPDATE, obj->objectID);
    switch (obj->behaviorId) {
        case BHV_SCENERY:
            obj_loop_scenery(obj, updateRate);
            break;
        case BHV_FISH:
            obj_loop_fish(obj, updateRate);
            break;
        case BHV_ANIMATOR:
            obj_loop_animator(obj, updateRate);
            break;
        case BHV_SMOKE:
            obj_loop_smoke(obj, updateRate);
            break;
        case BHV_UNK_19:
            obj_loop_unknown25(obj, updateRate);
            break;
        case BHV_BOMB_EXPLOSION:
            obj_loop_bombexplosion(obj, updateRate);
            break;
        case BHV_EXIT:
            obj_loop_exit(obj, updateRate);
            break;
        case BHV_CAMERA_CONTROL:
            obj_loop_cameracontrol(obj, updateRate);
            break;
        case BHV_SETUP_POINT:
            obj_loop_setuppoint(obj, updateRate);
            break;
        case BHV_DINO_WHALE:
            obj_loop_dino_whale(obj, updateRate);
            break;
        case BHV_CHECKPOINT:
            obj_loop_checkpoint(obj, updateRate);
            break;
        case BHV_MODECHANGE:
            obj_loop_modechange(obj, updateRate);
            break;
        case BHV_BONUS:
            obj_loop_bonus(obj, updateRate);
            break;
        case BHV_DOOR:
            obj_loop_door(obj, updateRate);
            break;
        case BHV_FOG_CHANGER:
            obj_loop_fogchanger(obj);
            break;
        case BHV_AINODE:
            obj_loop_ainode(obj, updateRate);
            break;
        case BHV_WEAPON_BALLOON:
            obj_loop_weaponballoon(obj, updateRate);
            break;
        case BHV_BALLOON_POP:
            obj_loop_wballoonpop(obj, updateRate);
            break;
        case BHV_WEAPON:
        case BHV_WEAPON_2:
            obj_loop_weapon(obj, updateRate);
            break;
        case BHV_SKY_CONTROL:
            obj_loop_skycontrol(obj, updateRate);
            break;
        case BHV_TORCH_MIST:
            obj_loop_torch_mist(obj, updateRate);
            break;
        case BHV_TEXTURE_SCROLL:
            obj_loop_texscroll(obj, updateRate);
            break;
        case BHV_STOPWATCH_MAN:
            obj_loop_stopwatchman(obj, updateRate);
            break;
        case BHV_BANANA:
            obj_loop_banana(obj, updateRate);
            break;
        case BHV_BUOY_PIRATE_SHIP:
            obj_loop_buoy_pirateship(obj, updateRate);
            break;
        case BHV_LOG:
            obj_loop_log(obj, updateRate);
            break;
        case BHV_WEATHER:
            obj_loop_weather(obj, updateRate);
            break;
        case BHV_BRIDGE_WHALE_RAMP:
            obj_loop_bridge_whaleramp(obj, updateRate);
            break;
        case BHV_RAMP_SWITCH:
            obj_loop_rampswitch(obj, updateRate);
            break;
        case BHV_SEA_MONSTER:
            obj_loop_seamonster(obj, updateRate);
            break;
        case BHV_COLLECT_EGG:
            obj_loop_collectegg(obj, updateRate);
            break;
        case BHV_EGG_CREATOR:
            obj_loop_eggcreator(obj, updateRate);
            break;
        case BHV_CHARACTER_FLAG:
            obj_loop_characterflag(obj, updateRate);
            break;
        case BHV_ANIMATED_OBJECT:
        case BHV_ANIMATED_OBJECT_2:
        case BHV_ANIMATED_OBJECT_3:
        case BHV_ANIMATED_OBJECT_4:
            obj_loop_animobject(obj, updateRate);
            break;
        case BHV_WIZPIG_GHOSTS:
            obj_loop_wizghosts(obj, updateRate);
            break;
        case BHV_CAMERA_ANIMATION:
            obj_loop_animcamera(obj, updateRate);
            break;
        case BHV_INFO_POINT:
            obj_loop_infopoint(obj, updateRate);
            break;
        case BHV_CAR_ANIMATION:
            obj_loop_animcar(obj, updateRate);
            break;
        case BHV_CHARACTER_SELECT:
            obj_loop_char_select(obj, updateRate);
            break;
        case BHV_TRIGGER:
            obj_loop_trigger(obj, updateRate);
            break;
        case BHV_VEHICLE_ANIMATION:
            obj_loop_vehicleanim(obj, updateRate);
            break;
        case BHV_ZIPPER_WATER:
        case BHV_ZIPPER_AIR:
            obj_loop_airzippers_waterzippers(obj, updateRate);
            break;
        case BHV_TIMETRIAL_GHOST:
            obj_loop_timetrialghost(obj, updateRate);
            break;
        case BHV_WAVE_POWER:
            obj_loop_wavepower(obj);
            break;
        case BHV_BUTTERFLY:
            obj_loop_butterfly(obj, updateRate);
            break;
        case BHV_PARK_WARDEN:
            obj_loop_parkwarden(obj, updateRate);
            break;
        case BHV_WORLD_KEY:
            obj_loop_worldkey(obj, updateRate);
            break;
        case BHV_BANANA_SPAWNER:
            obj_loop_bananacreator(obj, updateRate);
            break;
        case BHV_TREASURE_SUCKER:
            obj_loop_treasuresucker(obj, updateRate);
            break;
        case BHV_LAVA_SPURT:
            obj_loop_lavaspurt(obj, updateRate);
            break;
        case BHV_POS_ARROW:
            obj_loop_posarrow(obj, updateRate);
            break;
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_HIT_TESTER_3:
        case BHV_HIT_TESTER_4:
            obj_loop_hittester(obj, updateRate);
            break;
        case BHV_SNOWBALL:
        case BHV_SNOWBALL_2:
        case BHV_SNOWBALL_3:
        case BHV_SNOWBALL_4:
            obj_loop_snowball(obj, updateRate);
            break;
        case BHV_EFFECT_BOX:
            obj_loop_effectbox(obj, updateRate);
            break;
        case BHV_TROPHY_CABINET:
            obj_loop_trophycab(obj, updateRate);
            break;
        case BHV_BUBBLER:
            obj_loop_bubbler(obj, updateRate);
            break;
        case BHV_FLY_COIN:
            obj_loop_flycoin(obj, updateRate);
            break;
        case BHV_GOLDEN_BALLOON:
            obj_loop_goldenballoon(obj, updateRate);
            break;
        case BHV_LASER_BOLT:
            obj_loop_laserbolt(obj, updateRate);
            break;
        case BHV_LASER_GUN:
            obj_loop_lasergun(obj, updateRate);
            break;
        case BHV_PARK_WARDEN_2:
            obj_loop_gbparkwarden(obj, updateRate);
            break;
        case BHV_ZIPPER_GROUND:
            obj_loop_groundzipper(obj, updateRate);
            break;
        case BHV_WIZPIG_SHIP:
            obj_loop_wizpigship(obj, updateRate);
            break;
        case BHV_SILVER_COIN:
        case BHV_SILVER_COIN_2:
            obj_loop_silvercoin(obj, updateRate);
            break;
        case BHV_WARDEN_SMOKE:
            obj_loop_wardensmoke(obj, updateRate);
            break;
        case BHV_UNK_5E:
            obj_loop_unknown94(obj, updateRate);
            break;
        case BHV_TELEPORT:
            obj_loop_teleport(obj, updateRate);
            break;
        case BHV_RANGE_TRIGGER:
            obj_loop_rangetrigger(obj, updateRate);
            break;
        case BHV_ROCKET_SIGNPOST_2:
            obj_loop_rocketsignpost(obj, updateRate);
            break;
        case BHV_FIREBALL_OCTOWEAPON:
        case BHV_FIREBALL_OCTOWEAPON_2:
            obj_loop_fireball_octoweapon(obj, updateRate);
            break;
        case BHV_FROG:
            obj_loop_frog(obj, updateRate);
            break;
        case BHV_TT_DOOR:
            obj_loop_ttdoor(obj, updateRate);
            break;
        case BHV_DOOR_OPENER:
            obj_loop_dooropener(obj, updateRate);
            break;
        case BHV_PIG_ROCKETEER:
            obj_loop_pigrocketeer(obj, updateRate);
            break;
        case BHV_LEVEL_NAME:
            obj_loop_levelname(obj, updateRate);
            break;
    }
    update_object_stack_trace(OBJECT_UPDATE, OBJECT_CLEAR);
}

UNUSED void func_8002458C(UNUSED s32 arg0) {
}

s16 *func_80024594(s32 *currentCount, s32 *maxCount) {
    *currentCount = D_800DC700;
    *maxCount = ARRAY_COUNT(D_8011AC20);
    return D_8011AC20;
}

void func_800245B4(s16 arg0) {
    D_8011AC20[D_800DC700++] = arg0;
    if (D_800DC700 >= ARRAY_COUNT(D_8011AC20)) {
        D_800DC700 = 0;
    }
}

UNUSED const char sReadOutErrorString[] = "RO error %d!!\n";
UNUSED const char sPureAnguishString[] = "ARGHHHHHHHHH\n";
