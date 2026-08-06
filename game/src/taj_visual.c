#include "taj_visual.h"
#include "taj_select_layout.h"

#ifdef NATIVE_PORT

#include "asset_enums.h"
#include "audio.h"
#include "game.h"
#include "macros.h"
#include "math_util.h"
#include "object_models.h"
#include "objects.h"
#include "taj_mod.h"
#include "taj_physics.h"
#include "textures_sprites.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This is presentation-only.  It never writes Object_Racer::unk154, never
 * joins gRacers, and never changes the donor's physics/collision state. */
#define TAJ_VISUAL_MAX_RACERS 4
#define TAJ_VISUAL_CARPET_Y 5.5f
#define TAJ_VISUAL_BOB_AMPLITUDE 1.15f
#define TAJ_VISUAL_BOB_RATE 0x0180
#define TAJ_VISUAL_ROLL_AMPLITUDE 0x0080
#define TAJ_VISUAL_ANIMATION_WITNESS_FIELDS 31
#define TAJ_VISUAL_ANIMATION_WITNESS_LIMIT 8
#define TAJ_VISUAL_RIDER_ANIMATION 6
#define TAJ_SELECT_IDLE_ANIMATION 0
#define TAJ_SELECT_HOVER_ANIMATION 1
#define TAJ_SELECT_CONFIRM_ANIMATION 5
#define TAJ_SELECT_CONFIRM_TICKS 60
#define TAJ_SELECT_RETRY_TICKS 15
#define TAJ_SELECT_RETRY_LIMIT 8
#define TAJ_VISUAL_RECOVERY_RETRY_LIMIT 8
#define TAJ_SELECT_SCALE 0.20f
#define TAJ_SELECT_YAW 0x7F00

typedef enum TajVisualState {
    TAJ_VISUAL_NONE,
    TAJ_VISUAL_COMPOSED,
    TAJ_VISUAL_FALLBACK,
} TajVisualState;

typedef enum TajVisualSpawnLease {
    TAJ_VISUAL_SPAWN_NONE,
    TAJ_VISUAL_SPAWN_CARPET,
    TAJ_VISUAL_SPAWN_RIDER,
    TAJ_VISUAL_SPAWN_SELECT_RIDER,
    TAJ_VISUAL_SPAWN_SELECT_SIGN,
} TajVisualSpawnLease;

typedef enum TajVisualFaultTarget {
    TAJ_VISUAL_FAULT_NONE,
    TAJ_VISUAL_FAULT_CARPET,
    TAJ_VISUAL_FAULT_RIDER,
    TAJ_VISUAL_FAULT_SIGN,
} TajVisualFaultTarget;

typedef struct TajVisualSlot {
    Object *donor;
    Object *carpet;
    Object *rider;
    TajVisualState state;
    u16 bobPhase;
    u16 animationWitnessTimer;
    u16 animationWitnessCount;
    s32 animationWitnessPending;
    f32 carpetBaseScale;
    f32 riderBaseScale;
    s32 multiplayerShadowTraced;
    s32 riderShadowSuppressionTraced;
    s32 donorSuppressionTraced;
    s32 effectAnchorTraced;
    s32 dashActive;
    s32 dashPulseTraced;
    s32 recoveryAttempts;
    u32 generation;
} TajVisualSlot;

typedef struct TajSelectVisual {
    TajSelectLayout layout;
    Object *authoredActors[11];
    f32 authoredScales[11];
    Object *source;
    Object *rider;
    Object *sign;
    u32 hoverMask;
    u32 confirmedMask;
    u32 previousConfirmedMask;
    s32 confirmTimer;
    s32 signCycleTimer;
    s32 signCycleOffset;
    s32 active;
    s32 actorRetryTimer;
    s32 signRetryTimer;
    s32 actorAttempts;
    s32 signAttempts;
    s32 unavailable;
    s32 faultSpawnsRemaining;
    s32 faultSignSpawnsRemaining;
    s32 pairRecoveryAttempts;
    s32 faultDropsRemaining;
    TajVisualFaultTarget faultDropTarget;
    u32 generation;
} TajSelectVisual;

static TajVisualSlot sSlots[TAJ_VISUAL_MAX_RACERS];
/* Bumped by every teardown (free_all_objects -> taj_visual_reset). The
 * "log this once" latches scattered across the menu/HUD/select code are
 * per-scene facts, not per-process ones; without a boundary to compare against
 * they fired once for the lifetime of the executable, so a second race or a
 * second visit to the results screen logged nothing and the traces the Taj
 * gates assert on were only ever present on the first pass. */
static u32 sTraceEpoch = 1;
static TajVisualRacerPredicate sRacerPredicate;
static TajVisualSpawnLease sSpawnLease;
static TajSelectVisual sSelect;
static s32 sTraceEnabled = -1;
static s32 sSelectUnscaledShadow = -1;
static s32 sRaceFreezeCarpet = -1;
static s32 sRaceUnscaledCarpet = -1;
static s32 sRaceDisableMultiplayerShadow = -1;
static s32 sRaceFaultDropsRemaining = -1;
static TajVisualFaultTarget sRaceFaultDropTarget;

static ObjectModel *taj_visual_model_find(const Object *obj,
                                          s32 expectedModel);

static s32 taj_select_trace_enabled(void) {
    const char *value = getenv("MDKR_TAJ_SELECT_TRACE");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

/* Native test hooks deliberately use a loss after a complete composition,
 * rather than corrupting an object or retaining a pointer after free_object().
 * The normal free callback sees the still-live address, detaches ownership,
 * and the deferred allocator reclaims it on its ordinary path. */
static TajVisualFaultTarget taj_visual_fault_target(const char *value,
                                                    s32 picker) {
    if (value == NULL) {
        return TAJ_VISUAL_FAULT_NONE;
    }
    if (!strcmp(value, "rider")) {
        return TAJ_VISUAL_FAULT_RIDER;
    }
    if (!picker && !strcmp(value, "carpet")) {
        return TAJ_VISUAL_FAULT_CARPET;
    }
    if (picker && !strcmp(value, "sign")) {
        return TAJ_VISUAL_FAULT_SIGN;
    }
    return TAJ_VISUAL_FAULT_NONE;
}

static const char *taj_visual_fault_name(TajVisualFaultTarget target) {
    switch (target) {
        case TAJ_VISUAL_FAULT_CARPET:
            return "carpet";
        case TAJ_VISUAL_FAULT_RIDER:
            return "rider";
        case TAJ_VISUAL_FAULT_SIGN:
            return "sign";
        default:
            return "none";
    }
}

static s32 taj_visual_traced_model_id(const Object *obj) {
    if (obj == NULL || obj->header == NULL ||
        obj->header->numberOfModelIds <= 0 || obj->modelIndex < 0 ||
        obj->modelIndex >= obj->header->numberOfModelIds) {
        return -1;
    }
    return DKR_PTR(s32, obj->header->modelIds)[obj->modelIndex];
}

static void taj_select_trace(const char *event) {
    s32 model = -1;
    s32 signModel = -1;
    s32 signPlayer = -1;
    TajSelectRow row = TAJ_SELECT_ROW_NONE;
    s32 slot = -1;
    f32 x = 0.0f;
    if (!taj_select_trace_enabled()) {
        return;
    }
    /* modelIndex is behaviour/LOD-owned and is not a checked subscript on its
     * own; a trace must never be the thing that reads past modelIds[]. */
    model = taj_visual_traced_model_id(sSelect.rider);
    signModel = taj_visual_traced_model_id(sSelect.sign);
    if (sSelect.hoverMask != 0) {
        s32 selected = 0;
        s32 player;
        for (player = 0; player < TAJ_VISUAL_MAX_RACERS; player++) {
            if (sSelect.hoverMask & taj_mod_player_bit(player)) {
                if (selected == sSelect.signCycleOffset) {
                    signPlayer = player;
                    break;
                }
                selected++;
            }
        }
    }
    taj_select_layout_position(&sSelect.layout, sSelect.layout.tajIndex, &row,
                               &slot, &x);
    fprintf(stderr,
            "[TAJSELECT] event=%s index=%d actor=%p model=%d sign_model=%d "
            "row=%d slot=%d "
            "x=%.2f hover=0x%X confirmed=0x%X sign_player=%d "
            "generation=%u recovery=%d animation=%d frame=%d shadow=%.6f\n",
            event, sSelect.layout.tajIndex, (void *)sSelect.rider, model,
            signModel, row, slot, (double)x, sSelect.hoverMask,
            sSelect.confirmedMask, signPlayer, sSelect.generation,
            sSelect.pairRecoveryAttempts,
            sSelect.rider != NULL ? sSelect.rider->animationID : -1,
            sSelect.rider != NULL ? sSelect.rider->animFrame : -1,
            (double)(sSelect.rider != NULL && sSelect.rider->shadow != NULL
                         ? sSelect.rider->shadow->scale : 0.0f));
}

static s32 taj_visual_trace_enabled(void) {
    const char *value;
    if (sTraceEnabled >= 0) {
        return sTraceEnabled;
    }
    value = getenv("MDKR_TAJ_VISUAL_TRACE");
    sTraceEnabled = value != NULL && value[0] != '\0' && value[0] != '0';
    return sTraceEnabled;
}

static s32 taj_visual_test_flag(const char *name, s32 *cached) {
    const char *value;
    if (*cached >= 0) {
        return *cached;
    }
    value = getenv(name);
    *cached = value != NULL && value[0] != '\0' && value[0] != '0';
    return *cached;
}

/* Shadows carry an absolute header-authored size; they do not inherit the
 * object's transform scale. Presentation companions deliberately rescale the
 * retail models, so keep their grounding decals in the same proportion. */
static void taj_visual_scale_shadow(Object *obj) {
    f32 scale;
    if (obj == NULL || obj->header == NULL || obj->shadow == NULL) {
        return;
    }
    if (!isfinite(obj->trans.scale) || !isfinite(obj->header->scale) ||
        !isfinite(obj->header->shadowScale) || obj->trans.scale <= 0.0f ||
        obj->header->scale <= 0.0f) {
        obj->shadow->scale = 0.0f;
        return;
    }
    scale = obj->header->shadowScale *
            (obj->trans.scale / obj->header->scale);
    obj->shadow->scale = isfinite(scale) && scale > 0.0f ? scale : 0.0f;
}

static u64 taj_visual_vertex_hash(const Object *obj) {
    const Vertex *vertices;
    ObjectModel *model;
    u64 hash = UINT64_C(1469598103934665603);
    s32 i;

    if (obj == NULL || obj->curVertData == NULL) {
        return 0;
    }
    model = taj_visual_model_find(obj, ASSET_OBJECTMODEL_MAGICCARPET);
    if (model == NULL || model->numberOfVertices <= 0) {
        return 0;
    }
    /* taj_visual_model_find() locates the carpet model by ASSET ID, but
     * curVertData is the buffer published for whichever LOD modelIndex
     * currently selects. Those are only the same model when the active
     * instance IS the carpet, so require that rather than walking one model's
     * vertex count across another model's buffer. */
    if (obj->modelInstances == NULL || obj->modelIndex < 0 ||
        obj->header == NULL ||
        obj->modelIndex >= obj->header->numberOfModelIds ||
        obj->modelInstances[obj->modelIndex] == NULL ||
        obj->modelInstances[obj->modelIndex]->objModel != model) {
        return 0;
    }
    vertices = obj->curVertData;
    for (i = 0; i < model->numberOfVertices; i++) {
        hash ^= (u16)vertices[i].x;
        hash *= UINT64_C(1099511628211);
        hash ^= (u16)vertices[i].y;
        hash *= UINT64_C(1099511628211);
        hash ^= (u16)vertices[i].z;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void taj_visual_trace(const char *event, const TajVisualSlot *slot) {
    if (taj_visual_trace_enabled()) {
        s32 carpetModel = -1;
        s32 riderModel = -1;
        s32 carpetTask = -1;
        s32 carpetAnimations = -1;
        u64 vertexHash = 0;
        f32 authoredRatio = 0.0f;
        f32 observedRatio = 0.0f;
        if (slot != NULL && slot->carpet != NULL &&
            slot->carpet->header != NULL) {
            carpetModel = DKR_PTR(s32, slot->carpet->header->modelIds)[0];
        }
        if (slot != NULL && slot->rider != NULL &&
            slot->rider->header != NULL) {
            riderModel = DKR_PTR(s32, slot->rider->header->modelIds)[0];
        }
        if (slot != NULL && slot->carpet != NULL &&
            slot->carpet->modelInstances != NULL &&
            slot->carpet->header != NULL &&
            slot->carpet->header->numberOfModelIds > 0) {
            s32 modelIndex = slot->carpet->modelIndex;
            ModelInstance *instance;
            if (modelIndex < 0) {
                modelIndex = 0;
            } else if (modelIndex >=
                       slot->carpet->header->numberOfModelIds) {
                modelIndex = slot->carpet->header->numberOfModelIds - 1;
            }
            instance = slot->carpet->modelInstances[modelIndex];
            if (instance != NULL && instance->objModel != NULL) {
                carpetTask = instance->animationTaskNum;
                carpetAnimations = instance->objModel->numberOfAnimations;
            }
            vertexHash = taj_visual_vertex_hash(slot->carpet);
        }
        if (slot != NULL && slot->riderBaseScale > 0.0f) {
            authoredRatio = slot->carpetBaseScale / slot->riderBaseScale;
        }
        if (slot != NULL && slot->carpet != NULL && slot->rider != NULL &&
            slot->rider->trans.scale > 0.0f) {
            observedRatio =
                slot->carpet->trans.scale / slot->rider->trans.scale;
        }
        fprintf(stderr,
                "[TAJVIS] event=%s donor=%p carpet=%p rider=%p state=%d "
                "generation=%u recovery=%d carpet_model=%d rider_model=%d "
                "carpet_animations=%d carpet_frame=%d carpet_task=%d "
                "vertex_hash=%016llx carpet_scale=%.6f rider_scale=%.6f "
                "carpet_base_scale=%.6f rider_base_scale=%.6f "
                "authored_ratio=%.6f observed_ratio=%.6f dash_active=%d\n",
                event, (void *)(slot != NULL ? slot->donor : NULL),
                (void *)(slot != NULL ? slot->carpet : NULL),
                (void *)(slot != NULL ? slot->rider : NULL),
                slot != NULL ? slot->state : TAJ_VISUAL_NONE,
                slot != NULL ? slot->generation : 0,
                slot != NULL ? slot->recoveryAttempts : 0, carpetModel,
                riderModel, carpetAnimations,
                slot != NULL && slot->carpet != NULL
                    ? slot->carpet->animFrame : -1,
                carpetTask, (unsigned long long)vertexHash,
                (double)(slot != NULL && slot->carpet != NULL
                             ? slot->carpet->trans.scale : 0.0f),
                (double)(slot != NULL && slot->rider != NULL
                             ? slot->rider->trans.scale : 0.0f),
                (double)(slot != NULL ? slot->carpetBaseScale : 0.0f),
                (double)(slot != NULL ? slot->riderBaseScale : 0.0f),
                (double)authoredRatio, (double)observedRatio,
                slot != NULL ? slot->dashActive : FALSE);
    }
}

static TajVisualSlot *taj_visual_slot_for(const Object *obj) {
    s32 i;
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].donor == obj) {
            return &sSlots[i];
        }
    }
    return NULL;
}

static TajVisualSlot *taj_visual_slot_allocate(Object *donor) {
    s32 i;
    TajVisualSlot *slot = taj_visual_slot_for(donor);
    if (slot != NULL) {
        return slot;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].donor == NULL) {
            sSlots[i].donor = donor;
            return &sSlots[i];
        }
    }
    return NULL;
}

static ObjectModel *taj_visual_model_find(const Object *obj,
                                          s32 expectedModel) {
    s32 i;
    ModelInstance *expectedInstance = NULL;
    if (obj == NULL || obj->header == NULL || obj->modelInstances == NULL ||
        obj->header->modelType != OBJECT_MODEL_TYPE_3D_MODEL ||
        obj->header->numberOfModelIds <= 0) {
        return NULL;
    }
    for (i = 0; i < obj->header->numberOfModelIds; i++) {
        ModelInstance *instance = obj->modelInstances[i];
        if (DKR_PTR(s32, obj->header->modelIds)[i] == expectedModel) {
            expectedInstance = instance;
        }
        if (instance == NULL || instance->objModel == NULL) {
            return NULL;
        }
    }
    return expectedInstance != NULL ? expectedInstance->objModel : NULL;
}

static s32 taj_visual_model_is_ready(const Object *obj, s32 expectedModel,
                                     s32 minimumAnimations) {
    ObjectModel *model = taj_visual_model_find(obj, expectedModel);
    return model != NULL && (minimumAnimations <= 0 ||
                             model->numberOfAnimations >= minimumAnimations);
}

static s32 taj_visual_sign_schema_is_ready(const Object *obj) {
    ObjectModel *model = taj_visual_model_find(
        obj, ASSET_OBJECTMODEL_DIDDYSELECT);
    TriangleBatchInfo *batches;
    s32 i;

    if (model == NULL || model->numberOfAnimations < 2 ||
        model->numberOfTextures != 20 || model->numberOfVertices != 343 ||
        model->numberOfTriangles != 297 || model->numberOfBatches != 28 ||
        model->batches == 0) {
        return FALSE;
    }
    batches = DKR_PTR(TriangleBatchInfo, model->batches);
    /* Supported US/PAL 1.1 share this exact asset. Batch zero is the complete
     * numbered placard (four vertices, two triangles); every later batch is
     * Diddy's body. Texture zero may already have been changed to P1..P4 by
     * the authored character-select actor sharing this cached model. */
    if (batches[0].textureIndex >= 4 || batches[0].verticesOffset != 0 ||
        batches[0].facesOffset != 0 || batches[1].verticesOffset != 4 ||
        batches[1].facesOffset != 2) {
        return FALSE;
    }
    for (i = 1; i < model->numberOfBatches; i++) {
        if (batches[i].textureIndex < 4) {
            return FALSE;
        }
    }
    return TRUE;
}

static void taj_visual_queue_free(Object *obj) {
    if (obj != NULL) {
        free_object(obj);
    }
}

static void taj_visual_clear_slot(TajVisualSlot *slot, s32 freeCompanions) {
    Object *carpet;
    Object *rider;
    if (slot == NULL) {
        return;
    }
    carpet = slot->carpet;
    rider = slot->rider;
    slot->carpet = NULL;
    slot->rider = NULL;
    slot->state = TAJ_VISUAL_NONE;
    slot->bobPhase = 0;
    slot->animationWitnessTimer = 0;
    slot->animationWitnessCount = 0;
    slot->animationWitnessPending = FALSE;
    slot->carpetBaseScale = 0.0f;
    slot->riderBaseScale = 0.0f;
    slot->multiplayerShadowTraced = FALSE;
    /* taj_visual_on_object_destroy() already resets this one; clear_slot and
     * race_pair_lost did not, so after a recompose the rider's shadow
     * suppression was never re-traced and the lifecycle log lost an event. */
    slot->riderShadowSuppressionTraced = FALSE;
    slot->donorSuppressionTraced = FALSE;
    slot->effectAnchorTraced = FALSE;
    slot->dashActive = FALSE;
    slot->dashPulseTraced = FALSE;
    slot->recoveryAttempts = 0;
    slot->generation = 0;
    if (freeCompanions) {
        taj_visual_queue_free(carpet);
        taj_visual_queue_free(rider);
    }
    slot->donor = NULL;
}

/* Called from free_object's pre-destruction hook. `lost` is intentionally used
 * only for address comparison; the object may be queued for destruction as
 * soon as this function returns. Clear the pair before freeing its survivor so
 * re-entrant lifecycle callbacks cannot observe or free an orphan twice. */
static void taj_visual_race_pair_lost(TajVisualSlot *slot, Object *lost,
                                      TajVisualFaultTarget target) {
    Object *survivor;

    if (slot == NULL || (lost != slot->carpet && lost != slot->rider)) {
        return;
    }
    survivor = lost == slot->carpet ? slot->rider : slot->carpet;
    slot->carpet = NULL;
    slot->rider = NULL;
    slot->state = TAJ_VISUAL_NONE;
    slot->bobPhase = 0;
    slot->animationWitnessTimer = 0;
    slot->animationWitnessCount = 0;
    slot->animationWitnessPending = FALSE;
    slot->carpetBaseScale = 0.0f;
    slot->riderBaseScale = 0.0f;
    slot->multiplayerShadowTraced = FALSE;
    slot->riderShadowSuppressionTraced = FALSE;
    slot->donorSuppressionTraced = FALSE;
    slot->effectAnchorTraced = FALSE;
    slot->dashActive = FALSE;
    slot->dashPulseTraced = FALSE;
    slot->recoveryAttempts++;
    taj_visual_trace(target == TAJ_VISUAL_FAULT_CARPET
                         ? "pair_lost_carpet"
                         : "pair_lost_rider", slot);
    if (survivor != NULL) {
        taj_visual_trace("orphan_cleanup", slot);
        taj_visual_queue_free(survivor);
    }
    /* One budget rule for every recovery path: `>=` means the limit is the
     * number of attempts allowed, matching taj_visual_race_retry_or_fallback()
     * and the two TAJ_SELECT_RETRY_LIMIT sites. `>` here quietly bought this
     * path one extra compose that the others did not get. */
    if (slot->recoveryAttempts >= TAJ_VISUAL_RECOVERY_RETRY_LIMIT) {
        slot->state = TAJ_VISUAL_FALLBACK;
        taj_visual_trace("fallback_recovery_limit", slot);
    }
}

/* This hook is armed only by test environment variables. It never stores or
 * reads the selected component after `free_object()`; recovery happens through
 * taj_visual_on_object_free(), exactly as it would for a real lifecycle loss. */
static void taj_visual_maybe_drop_race_pair(TajVisualSlot *slot) {
    Object *lost = NULL;
    const char *value;

    if (sRaceFaultDropsRemaining < 0) {
        value = getenv("MDKR_TAJ_VISUAL_DROP_AFTER_COMPOSE");
        sRaceFaultDropTarget = taj_visual_fault_target(value, FALSE);
        sRaceFaultDropsRemaining = sRaceFaultDropTarget == TAJ_VISUAL_FAULT_NONE
            ? 0 : 1;
    }
    if (slot == NULL || slot->state != TAJ_VISUAL_COMPOSED ||
        sRaceFaultDropsRemaining <= 0) {
        return;
    }
    if (sRaceFaultDropTarget == TAJ_VISUAL_FAULT_CARPET) {
        lost = slot->carpet;
    } else if (sRaceFaultDropTarget == TAJ_VISUAL_FAULT_RIDER) {
        lost = slot->rider;
    }
    if (lost == NULL) {
        return;
    }
    sRaceFaultDropsRemaining--;
    fprintf(stderr, "[TAJVIS] event=test_drop component=%s generation=%u\n",
            taj_visual_fault_name(sRaceFaultDropTarget), slot->generation);
    taj_visual_queue_free(lost);
}

static void taj_visual_race_retry_or_fallback(TajVisualSlot *slot,
                                              const char *event) {
    if (slot == NULL) {
        return;
    }
    slot->recoveryAttempts++;
    if (slot->recoveryAttempts >= TAJ_VISUAL_RECOVERY_RETRY_LIMIT) {
        slot->state = TAJ_VISUAL_FALLBACK;
        taj_visual_trace("fallback_recovery_limit", slot);
    } else {
        slot->state = TAJ_VISUAL_NONE;
        taj_visual_trace(event, slot);
    }
}

static Object *taj_visual_spawn(TajVisualSpawnLease lease, s32 objectID,
                                const Object *source) {
    LevelObjectEntry_Racer racerEntry;
    LevelObjectEntry_Parkwarden riderEntry;
    LevelObjectEntry_AnimatedObject animatedEntry;
    Object *obj;

    if (sSpawnLease != TAJ_VISUAL_SPAWN_NONE || source == NULL) {
        return NULL;
    }
    if ((lease == TAJ_VISUAL_SPAWN_SELECT_RIDER ||
         lease == TAJ_VISUAL_SPAWN_SELECT_SIGN) &&
        sSelect.faultSpawnsRemaining > 0) {
        sSelect.faultSpawnsRemaining--;
        return NULL;
    }
    if (lease == TAJ_VISUAL_SPAWN_SELECT_SIGN &&
        sSelect.faultSignSpawnsRemaining > 0) {
        sSelect.faultSignSpawnsRemaining--;
        return NULL;
    }
    sSpawnLease = lease;
    if (lease == TAJ_VISUAL_SPAWN_CARPET) {
        memset(&racerEntry, 0, sizeof(racerEntry));
        racerEntry.common.objectID = (u8)objectID;
        racerEntry.common.size =
            (u8)(sizeof(racerEntry) | ((objectID & 0x100) >> 1));
        racerEntry.common.x = (s16)source->trans.x_position;
        racerEntry.common.y = (s16)source->trans.y_position;
        racerEntry.common.z = (s16)source->trans.z_position;
        racerEntry.angleZ = 0;
        racerEntry.angleX = 0;
        racerEntry.angleY = 0;
        racerEntry.playerIndex = 4; /* no camera even if a claim regresses */
        obj = spawn_object(&racerEntry.common, OBJECT_SPAWN_UNK01);
    } else if (lease == TAJ_VISUAL_SPAWN_SELECT_SIGN) {
        memset(&animatedEntry, 0, sizeof(animatedEntry));
        animatedEntry.common.objectID = (u8)objectID;
        animatedEntry.common.size =
            (u8)(sizeof(animatedEntry) | ((objectID & 0x100) >> 1));
        animatedEntry.common.x = (s16)source->trans.x_position;
        animatedEntry.common.y = (s16)source->trans.y_position;
        animatedEntry.common.z = (s16)source->trans.z_position;
        obj = spawn_object(&animatedEntry.common, OBJECT_SPAWN_UNK01);
    } else {
        memset(&riderEntry, 0, sizeof(riderEntry));
        riderEntry.common.objectID = (u8)objectID;
        riderEntry.common.size =
            (u8)(sizeof(riderEntry) | ((objectID & 0x100) >> 1));
        riderEntry.common.x = (s16)source->trans.x_position;
        riderEntry.common.y = (s16)source->trans.y_position;
        riderEntry.common.z = (s16)source->trans.z_position;
        obj = spawn_object(&riderEntry.common, OBJECT_SPAWN_UNK01);
    }
    /* A claimed spawn clears this inside run_object_init_func.  Clear anyway
     * on allocation failure so a later unrelated object can never be claimed.
     */
    sSpawnLease = TAJ_VISUAL_SPAWN_NONE;
    return obj;
}

void taj_visual_set_racer_predicate(TajVisualRacerPredicate predicate) {
    sRacerPredicate = predicate;
    if (predicate == NULL) {
        taj_visual_reset();
    }
}

void taj_visual_select_begin(const TajSelectLayout *layout) {
    const char *faultSpawns;
    const char *faultSignSpawns;
    const char *faultDrop;
    taj_visual_select_end();
    if (layout == NULL || layout->tajIndex < 0) {
        return;
    }
    sSelect.layout = *layout;
    sSelect.active = TRUE;
    faultSpawns = getenv("MDKR_TAJ_SELECT_FAIL_SPAWNS");
    if (faultSpawns != NULL) {
        sSelect.faultSpawnsRemaining = atoi(faultSpawns);
        if (sSelect.faultSpawnsRemaining < 0) {
            sSelect.faultSpawnsRemaining = 0;
        } else if (sSelect.faultSpawnsRemaining > 1000) {
            sSelect.faultSpawnsRemaining = 1000;
        }
    }
    faultSignSpawns = getenv("MDKR_TAJ_SELECT_FAIL_SIGN_SPAWNS");
    if (faultSignSpawns != NULL) {
        sSelect.faultSignSpawnsRemaining = atoi(faultSignSpawns);
        if (sSelect.faultSignSpawnsRemaining < 0) {
            sSelect.faultSignSpawnsRemaining = 0;
        } else if (sSelect.faultSignSpawnsRemaining > 1000) {
            sSelect.faultSignSpawnsRemaining = 1000;
        }
    }
    faultDrop = getenv("MDKR_TAJ_SELECT_DROP_AFTER_COMPOSE");
    sSelect.faultDropTarget = taj_visual_fault_target(faultDrop, TRUE);
    sSelect.faultDropsRemaining =
        sSelect.faultDropTarget == TAJ_VISUAL_FAULT_NONE ? 0 : 1;
    taj_select_trace("begin");
}

void taj_visual_select_set_state(u32 hoverMask, u32 confirmedMask) {
    u32 newlyConfirmed;
    if (!sSelect.active) {
        return;
    }
    newlyConfirmed = confirmedMask & ~sSelect.previousConfirmedMask;
    if (hoverMask != sSelect.hoverMask) {
        sSelect.signCycleTimer = 0;
        sSelect.signCycleOffset = 0;
    }
    sSelect.hoverMask = hoverMask;
    sSelect.confirmedMask = confirmedMask;
    if (newlyConfirmed != 0) {
        sSelect.confirmTimer = TAJ_SELECT_CONFIRM_TICKS;
        taj_select_trace("confirm");
    }
    sSelect.previousConfirmedMask = confirmedMask;
}

void taj_visual_select_apply_authored_actor(Object *obj, s32 characterIndex) {
    TajSelectRow row;
    TajSelectRow tajRow;
    f32 xPosition;
    if (!sSelect.active || obj == NULL ||
        !taj_select_layout_position(&sSelect.layout, characterIndex, &row, NULL,
                                    &xPosition) ||
        !taj_select_layout_position(&sSelect.layout, sSelect.layout.tajIndex,
                                    &tajRow, NULL, NULL)) {
        return;
    }
    /* Keep the ROM-authored animation, depth, elevation, scale and lighting;
     * spacing changes to make room for the new actor. The eleven-character
     * lower row receives one uniform safe-area scale adjustment. */
    obj->trans.x_position = xPosition;
    if (characterIndex >= 0 && characterIndex < sSelect.layout.characterCount) {
        if (sSelect.authoredActors[characterIndex] != obj) {
            sSelect.authoredActors[characterIndex] = obj;
            sSelect.authoredScales[characterIndex] = obj->trans.scale;
        }
        obj->trans.scale =
            sSelect.authoredScales[characterIndex] *
            taj_select_layout_scale(&sSelect.layout, characterIndex);
    }
    /* Taj belongs to the lower row in every supported 8/9/10 retail roster.
     * Take an authored actor from that same row as the depth/elevation anchor,
     * rather than assuming one fixed table index has the right transform.
     * A guessed depth can put the spawned Taj behind the retail lineup even
     * though allocation and model probes all succeed. */
    if (sSelect.source == NULL && row == tajRow) {
        sSelect.source = obj;
    }
}

void taj_visual_select_end(void) {
    Object *rider = sSelect.rider;
    Object *sign = sSelect.sign;
    if (rider != NULL || sign != NULL) {
        taj_select_trace("end");
        sSelect.rider = NULL;
        sSelect.sign = NULL;
        taj_visual_queue_free(rider);
        taj_visual_queue_free(sign);
    }
    memset(&sSelect, 0, sizeof(sSelect));
    sSelect.layout.tajIndex = -1;
}

/* Same atomic ownership rule as the race pair. This is called before the lost
 * object is queued by free_object(), so it cannot inspect object contents.
 * Keeping either half alive would leave a floating sign or actor during the
 * retry window. */
static void taj_visual_select_pair_lost(Object *lost,
                                        TajVisualFaultTarget target) {
    Object *survivor;

    if (lost == NULL || (lost != sSelect.rider && lost != sSelect.sign)) {
        return;
    }
    survivor = lost == sSelect.rider ? sSelect.sign : sSelect.rider;
    sSelect.rider = NULL;
    sSelect.sign = NULL;
    sSelect.confirmTimer = 0;
    sSelect.actorAttempts = 0;
    sSelect.signAttempts = 0;
    sSelect.actorRetryTimer = TAJ_SELECT_RETRY_TICKS;
    sSelect.signRetryTimer = 0;
    sSelect.unavailable = FALSE;
    sSelect.pairRecoveryAttempts++;
    taj_select_trace(target == TAJ_VISUAL_FAULT_SIGN ? "pair_lost_sign"
                                                       : "pair_lost_rider");
    if (survivor != NULL) {
        taj_select_trace("orphan_cleanup");
        taj_visual_queue_free(survivor);
    }
    if (sSelect.pairRecoveryAttempts >= TAJ_VISUAL_RECOVERY_RETRY_LIMIT) {
        sSelect.unavailable = TRUE;
        taj_select_trace("presentation_unavailable");
    }
}

TajSelectVisualStatus taj_visual_select_status(void) {
    if (!sSelect.active) {
        return TAJ_SELECT_VISUAL_INACTIVE;
    }
    if (sSelect.unavailable) {
        return TAJ_SELECT_VISUAL_UNAVAILABLE;
    }
    if (sSelect.rider != NULL && sSelect.sign != NULL) {
        return TAJ_SELECT_VISUAL_READY;
    }
    return TAJ_SELECT_VISUAL_LOADING;
}

s32 taj_visual_select_sign_visible(void) {
    return taj_visual_select_status() == TAJ_SELECT_VISUAL_READY &&
           sSelect.hoverMask != 0;
}

s32 taj_visual_select_sign_object(const Object *obj) {
    return obj != NULL && obj == sSelect.sign;
}

s32 taj_visual_select_sign_player(const Object *obj) {
    s32 player;
    s32 selected = 0;
    if (taj_visual_select_status() != TAJ_SELECT_VISUAL_READY ||
        !taj_visual_select_sign_object(obj) || sSelect.hoverMask == 0) {
        return -1;
    }
    for (player = 0; player < TAJ_VISUAL_MAX_RACERS; player++) {
        if (sSelect.hoverMask & taj_mod_player_bit(player)) {
            if (selected == sSelect.signCycleOffset) {
                return player;
            }
            selected++;
        }
    }
    return -1;
}

s32 taj_visual_select_sign_batch(const Object *obj, s32 batchIndex) {
    /* Schema validation at composition proves batch zero is only the numbered
     * placard. Never infer presentation ownership from a mutable texture ID. */
    return taj_visual_select_sign_object(obj) && batchIndex == 0;
}

s32 taj_visual_spawn_lease_active(void) {
    return sSpawnLease != TAJ_VISUAL_SPAWN_NONE;
}

/* Every object this module composes is presentation-only: it owns no racer,
 * AI, collision, progression or input state, the simulation never reads it
 * back, and its transform/animation clock are written each tick from a bob
 * phase, a dash pulse and a menu hover state -- some of them behind
 * MDKR_TAJ_VISUAL_* / MDKR_TAJ_SELECT_* environment flags.  The authoritative
 * state hash asks this so it can skip them whole; see the PRESENTATION-OBJECT
 * EXCLUSION note in platform/sim_hash.c.  Ownership is answered from the live
 * slot pointers, so an object stops being ours the instant we release it. */
s32 taj_visual_is_presentation_object(const Object *obj) {
    s32 i;

    if (obj == NULL) {
        return FALSE;
    }
    if (obj == sSelect.rider || obj == sSelect.sign) {
        return TRUE;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].carpet == obj || sSlots[i].rider == obj) {
            return TRUE;
        }
    }
    return FALSE;
}

s32 taj_visual_claim_spawned_object(Object *obj) {
    TajVisualSpawnLease lease = sSpawnLease;
    if (lease == TAJ_VISUAL_SPAWN_NONE || obj == NULL) {
        return FALSE;
    }
    sSpawnLease = TAJ_VISUAL_SPAWN_NONE;
    /* Do not enter either retail behaviour: PARK_WARDEN owns dialogue/input,
     * and FLYINGCARPET is a challenge racer with camera/physics side effects.
     */
    obj->behaviorId = BHV_NONE;
    /* spawn_object retains the caller's entry pointer. These entries are
     * intentionally stack-local because the claimed objects have no authored
     * level entry, so sever that pointer before the stack frame disappears. */
    obj->level_entry = NULL;
    obj->animationID = 0;
    obj->animFrame = 0;
    if (obj->interactObj != NULL) {
        obj->interactObj->flags = INTERACT_FLAGS_NONE;
        obj->interactObj->pushForce = 0;
        obj->interactObj->hitboxRadius = 0;
    }
    return TRUE;
}

static s32 taj_visual_owner_is_eligible(const Object *obj) {
    TajVisualSlot *slot;

    if (obj == NULL || obj->header == NULL || obj->behaviorId != BHV_RACER ||
        obj->racer == NULL) {
        return FALSE;
    }
    /* Render indexes segmentID + 1.  Keep an invalid/tearing-down owner out
     * of composition rather than creating companions that the renderer cannot
     * admit safely.  The level loader enforces the upper 128-segment bound. */
    if (obj->segmentID < -1 || obj->segmentID >= 128) {
        return FALSE;
    }
    if (obj->racer->playerIndex < PLAYER_ONE ||
        obj->racer->playerIndex > PLAYER_FOUR) {
        /* update_player_racer() hands a finished human to PLAYER_COMPUTER.
         * Keep an already-composed Taj bound to the same stable racer object
         * for the finish camera/results transition; never use this latch to
         * promote an ordinary AI racer into Taj. */
        slot = taj_visual_slot_for(obj);
        if (obj->racer->raceFinished && slot != NULL &&
            slot->state == TAJ_VISUAL_COMPOSED) {
            return TRUE;
        }
        return FALSE;
    }
    if (sRacerPredicate != NULL) {
        return sRacerPredicate(obj->racer->playerIndex) != FALSE;
    }
    return FALSE;
}

/* Drop the composed picker actor without re-arming composition.
 *
 * Ownership is cleared BEFORE free_object() on purpose: the free hook routes
 * through taj_visual_select_pair_lost(), which treats a loss as a recoverable
 * fault and clears `unavailable` so the next tick recomposes. That is right for
 * a spontaneous loss and wrong here, where the teardown IS the consequence of
 * going unavailable. With the pointers already released the hook's identity
 * check no longer matches and the latch stands. */
static void taj_visual_select_release_actor(void) {
    Object *rider = sSelect.rider;
    Object *sign = sSelect.sign;

    if (rider == NULL && sign == NULL) {
        return;
    }
    sSelect.rider = NULL;
    sSelect.sign = NULL;
    sSelect.confirmTimer = 0;
    taj_select_trace("unavailable_teardown");
    taj_visual_queue_free(rider);
    taj_visual_queue_free(sign);
}

static void taj_visual_select_tick(s32 updateRate) {
    Object *rider;
    Object *sign;
    s32 animation;
    f32 xPosition;

    if (!sSelect.active || sSelect.source == NULL) {
        return;
    }
    if (sSelect.hoverMask != 0) {
        s32 selectedPlayers = 0;
        s32 player;
        for (player = 0; player < TAJ_VISUAL_MAX_RACERS; player++) {
            if (sSelect.hoverMask & taj_mod_player_bit(player)) {
                selectedPlayers++;
            }
        }
        sSelect.signCycleTimer += updateRate;
        if (sSelect.signCycleTimer >= 16 && selectedPlayers > 0) {
            sSelect.signCycleTimer &= 0xF;
            sSelect.signCycleOffset =
                (sSelect.signCycleOffset + 1) % selectedPlayers;
        }
    }
    if (sSelect.rider == NULL && !sSelect.unavailable) {
        if (sSelect.actorRetryTimer > 0) {
            sSelect.actorRetryTimer -= updateRate;
            if (sSelect.actorRetryTimer > 0) {
                return;
            }
            sSelect.actorRetryTimer = 0;
        }
        rider = taj_visual_spawn(TAJ_VISUAL_SPAWN_SELECT_RIDER,
                                 ASSET_OBJECT_ID_PARKWARDEN, sSelect.source);
        if (rider == NULL ||
            !taj_visual_model_is_ready(rider, ASSET_OBJECTMODEL_PARKWARDEN,
                                       TAJ_VISUAL_RIDER_ANIMATION + 1)) {
            taj_visual_queue_free(rider);
            sSelect.actorAttempts++;
            if (sSelect.actorAttempts >= TAJ_SELECT_RETRY_LIMIT) {
                sSelect.unavailable = TRUE;
                taj_select_trace("presentation_unavailable");
            } else {
                sSelect.actorRetryTimer = TAJ_SELECT_RETRY_TICKS;
                taj_select_trace("compose_retry");
            }
            return;
        }
        sSelect.rider = rider;
        sSelect.rider->animationID = TAJ_SELECT_IDLE_ANIMATION;
        sSelect.rider->animFrame = 0;
        taj_select_trace("compose");
    }
    if (sSelect.rider == NULL) {
        return;
    }

    if (sSelect.sign == NULL && !sSelect.unavailable) {
        if (sSelect.signRetryTimer > 0) {
            sSelect.signRetryTimer -= updateRate;
            if (sSelect.signRetryTimer > 0) {
                goto update_actor;
            }
            sSelect.signRetryTimer = 0;
        }
        sign = taj_visual_spawn(TAJ_VISUAL_SPAWN_SELECT_SIGN,
                                ASSET_OBJECT_ID_DIDDYSELECT, sSelect.source);
        if (sign == NULL || !taj_visual_sign_schema_is_ready(sign)) {
            taj_visual_queue_free(sign);
            sSelect.signAttempts++;
            if (sSelect.signAttempts >= TAJ_SELECT_RETRY_LIMIT) {
                sSelect.unavailable = TRUE;
                taj_select_trace("presentation_unavailable");
                /* UNAVAILABLE is what makes Taj unselectable, so the composed
                 * actor must go with it. The rider survived this fault (only
                 * the placard failed), and the update_actor tail below
                 * unconditionally clears OBJ_FLAGS_INVISIBLE -- so falling
                 * through here left a fully lit, correctly posed Taj standing
                 * in the line-up that the player cannot choose. Tear the pair
                 * down instead and let the status say what the scene shows. */
                taj_visual_select_release_actor();
                return;
            }
            sSelect.signRetryTimer = TAJ_SELECT_RETRY_TICKS;
            taj_select_trace("sign_retry");
            goto update_actor;
        }
        sSelect.sign = sign;
        sSelect.sign->animationID = 0;
        sSelect.sign->animFrame = 0;
        if (sSelect.sign->shadow != NULL) {
            /* The borrowed placard must not cast a second character shadow. */
            sSelect.sign->shadow->scale = 0.0f;
        }
        taj_select_trace("sign_schema_verified");
        taj_select_trace("sign_compose");
        sSelect.generation++;
        taj_select_trace(sSelect.pairRecoveryAttempts != 0 ? "recompose"
                                                      : "pair_compose");
        if (sSelect.faultDropsRemaining > 0) {
            Object *lost = sSelect.faultDropTarget == TAJ_VISUAL_FAULT_SIGN
                ? sSelect.sign : sSelect.rider;
            sSelect.faultDropsRemaining--;
            fprintf(stderr,
                    "[TAJSELECT] event=test_drop component=%s generation=%u\n",
                    taj_visual_fault_name(sSelect.faultDropTarget),
                    sSelect.generation);
            taj_visual_queue_free(lost);
            return;
        }
    }

update_actor:
    rider = sSelect.rider;
    if (!taj_select_layout_position(&sSelect.layout, sSelect.layout.tajIndex,
                                    NULL, NULL, &xPosition)) {
        return;
    }
    rider->trans.x_position = xPosition;
    /* The anchor is a live lower-row actor, so these values remain correct for
     * all unlock-dependent roster layouts and PAL/NTSC scene transforms. */
    rider->trans.y_position = sSelect.source->trans.y_position;
    rider->trans.z_position = sSelect.source->trans.z_position;
    rider->trans.rotation.x_rotation = 0;
    rider->trans.rotation.y_rotation = (s16)TAJ_SELECT_YAW;
    rider->trans.rotation.z_rotation = 0;
    rider->trans.scale =
        TAJ_SELECT_SCALE *
        taj_select_layout_scale(&sSelect.layout, sSelect.layout.tajIndex);
    if (taj_visual_test_flag("MDKR_TAJ_SELECT_UNSCALED_SHADOW",
                             &sSelectUnscaledShadow)) {
        if (rider->shadow != NULL) {
            rider->shadow->scale = rider->header->shadowScale;
        }
    } else {
        taj_visual_scale_shadow(rider);
    }
    rider->segmentID = sSelect.source->segmentID;
    rider->opacity = sSelect.source->opacity;
    rider->trans.flags &= ~OBJ_FLAGS_INVISIBLE;

    if (sSelect.confirmTimer > 0) {
        animation = TAJ_SELECT_CONFIRM_ANIMATION;
        sSelect.confirmTimer -= updateRate;
        if (sSelect.confirmTimer < 0) {
            sSelect.confirmTimer = 0;
        }
    } else if (sSelect.hoverMask != 0) {
        animation = TAJ_SELECT_HOVER_ANIMATION;
    } else {
        animation = TAJ_SELECT_IDLE_ANIMATION;
    }
    if (rider->animationID != animation) {
        rider->animationID = animation;
        rider->animFrame = 0;
        taj_select_trace("animation");
    }
    rider->animFrame += (s16)updateRate;
    func_80061C0C(rider);

    sign = sSelect.sign;
    if (sign != NULL) {
        sign->trans = rider->trans;
        sign->segmentID = rider->segmentID;
        sign->opacity = rider->opacity;
        sign->animationID = 0;
        if (sSelect.hoverMask != 0) {
            sign->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
        } else {
            sign->trans.flags |= OBJ_FLAGS_INVISIBLE;
        }
        sign->animFrame += (s16)updateRate;
        func_80061C0C(sign);
    }
}

static void taj_visual_sync(TajVisualSlot *slot, s32 updateRate) {
    Object *donor = slot->donor;
    Object *carpet = slot->carpet;
    Object *rider = slot->rider;
    s32 animationRate =
        donor->racer->raceFinished != 0 ? updateRate * 2 : updateRate;
    s32 dashTicks = taj_physics_dash_ticks_remaining(donor->racer);

    if (dashTicks > 0 && !slot->dashActive) {
        slot->dashActive = TRUE;
        sound_play_spatial(SOUND_WHOOSH4, donor->trans.x_position,
                           donor->trans.y_position, donor->trans.z_position,
                           NULL);
        taj_visual_trace("dash_fx_start", slot);
    } else if (dashTicks <= 0 && slot->dashActive) {
        slot->dashActive = FALSE;
        taj_visual_trace("dash_fx_end", slot);
    }

    slot->bobPhase += (u16)(updateRate * TAJ_VISUAL_BOB_RATE);
    carpet->trans = donor->trans;
    if (!taj_visual_test_flag("MDKR_TAJ_VISUAL_UNSCALED_CARPET",
                              &sRaceUnscaledCarpet)) {
        carpet->trans.scale *=
            slot->carpetBaseScale / slot->riderBaseScale;
    }
    carpet->trans.y_position +=
        TAJ_VISUAL_CARPET_Y +
        sins_f((s16)slot->bobPhase) * TAJ_VISUAL_BOB_AMPLITUDE;
    carpet->trans.rotation.z_rotation +=
        (s16)(sins_f((s16)(slot->bobPhase + 0x4000)) *
              TAJ_VISUAL_ROLL_AMPLITUDE);
    if (dashTicks > 0) {
        /* The dash is authoritative in taj_physics; this is a presentation-only
         * magic-carpet pulse and never borrows the retail boost timer/RNG. */
        carpet->trans.scale *=
            1.08f + sins_f((s16)(dashTicks * 0x1800)) * 0.03f;
    }
    carpet->segmentID = donor->segmentID;
    carpet->opacity = donor->opacity;
    taj_visual_scale_shadow(carpet);
    /* Emit one post-transform witness for the test-only deterministic dash
     * route.  The regular animation witnesses intentionally stop early, so
     * this proves the pulse operates on the live, scaled carpet rather than
     * merely reporting its state transition. */
    if (dashTicks > 0 && !slot->dashPulseTraced) {
        slot->dashPulseTraced = TRUE;
        taj_visual_trace("dash_pulse", slot);
    }
    carpet->animationID = 0;
    if (!taj_visual_test_flag("MDKR_TAJ_VISUAL_FREEZE_CARPET",
                              &sRaceFreezeCarpet)) {
        /* This is the presentation-only fragment of retail update_carpet().
         * obj_animate_tick consumes the resulting clock and deforms the ROM's
         * animated vertex stream. Never call update_carpet() here: the claimed
         * companion intentionally owns no racer/AI/camera state. */
        carpet->animFrame += (s16)updateRate;
        func_80061C0C(carpet);
    }

    /* The retail Taj challenge keeps Park Warden and the flying carpet at the
     * same transform.  Their model-local origins provide the seat relationship;
     * independently lifting the rider leaves Taj visibly standing on the road.
     */
    rider->trans = carpet->trans;
    /* Preserve the currently accepted playable-Taj size, then derive the
     * carpet from the ROM-authored carpet:rider ratio above. Retail's challenge
     * synchronises position and rotation but deliberately leaves the two
     * object scales independent. */
    rider->trans.scale = donor->trans.scale;
    rider->segmentID = carpet->segmentID;
    rider->opacity = carpet->opacity;
    taj_visual_scale_shadow(rider);
    /* Park Warden animation 6 is the authored Taj challenge carpet pose. */
    rider->animationID = TAJ_VISUAL_RIDER_ANIMATION;
    rider->animFrame += (s16)animationRate;
    func_80061C0C(rider);

    if (taj_visual_trace_enabled() && slot->animationWitnessCount <
        TAJ_VISUAL_ANIMATION_WITNESS_LIMIT) {
        slot->animationWitnessTimer += (u16)updateRate;
        if (slot->animationWitnessTimer >=
            TAJ_VISUAL_ANIMATION_WITNESS_FIELDS) {
            slot->animationWitnessTimer -=
                TAJ_VISUAL_ANIMATION_WITNESS_FIELDS;
            /* obj_animate_tick() runs after object updates and owns the
             * published buffer. Defer the witness until that exact boundary so
             * carpet_frame, task number, and vertex hash describe one mesh. */
            slot->animationWitnessPending = TRUE;
        }
    }
}

void taj_visual_animation_witness_tick(void) {
    s32 i;
    if (!taj_visual_trace_enabled()) {
        return;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        TajVisualSlot *slot = &sSlots[i];
        if (slot->animationWitnessPending &&
            slot->state == TAJ_VISUAL_COMPOSED && slot->carpet != NULL) {
            slot->animationWitnessPending = FALSE;
            slot->animationWitnessCount++;
            taj_visual_trace("carpet_animation", slot);
        }
    }
}

void taj_visual_tick(s32 updateRate) {
    s32 i;
    s32 numRacers;
    Object **racers;

    taj_visual_select_tick(updateRate);
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        TajVisualSlot *slot = &sSlots[i];
        if (slot->donor != NULL && !taj_visual_owner_is_eligible(slot->donor)) {
            taj_visual_trace("owner_ineligible", slot);
            taj_visual_clear_slot(slot, TRUE);
            continue;
        }
    }

    /* gRacers is authoritative for live racers.  A future taj_mod predicate
     * chooses the subset; this loop never infers Taj from a donor character. */
    racers = get_racer_objects(&numRacers);
    for (i = 0; racers != NULL && i < numRacers; i++) {
        Object *owner = racers[i];
        TajVisualSlot *slot;
        Object *carpet;
        Object *rider;
        if (!taj_visual_owner_is_eligible(owner)) {
            continue;
        }
        slot = taj_visual_slot_allocate(owner);
        if (slot == NULL || slot->state != TAJ_VISUAL_NONE) {
            if (slot != NULL && slot->state == TAJ_VISUAL_COMPOSED) {
                taj_visual_sync(slot, updateRate);
            }
            continue;
        }

        carpet = taj_visual_spawn(TAJ_VISUAL_SPAWN_CARPET,
                                  ASSET_OBJECT_ID_FLYINGCARPET, owner);
        if (carpet == NULL) {
            taj_visual_race_retry_or_fallback(slot, "retry_carpet");
            continue;
        }
        rider = taj_visual_spawn(TAJ_VISUAL_SPAWN_RIDER,
                                 ASSET_OBJECT_ID_PARKWARDEN, owner);
        if (!taj_visual_model_is_ready(carpet, ASSET_OBJECTMODEL_MAGICCARPET,
                                       1) ||
            rider == NULL ||
            !taj_visual_model_is_ready(rider, ASSET_OBJECTMODEL_PARKWARDEN,
                                       TAJ_VISUAL_RIDER_ANIMATION + 1) ||
            !isfinite(carpet->trans.scale) || carpet->trans.scale <= 0.0f ||
            !isfinite(rider->trans.scale) || rider->trans.scale <= 0.0f) {
            taj_visual_queue_free(carpet);
            taj_visual_queue_free(rider);
            taj_visual_race_retry_or_fallback(slot, "retry_probe");
            continue;
        }
        slot->carpet = carpet;
        slot->rider = rider;
        /* Capture both header-authored scales before the first synchronisation
         * overwrites either transform with the playable donor transform. */
        slot->carpetBaseScale = carpet->trans.scale;
        slot->riderBaseScale = rider->trans.scale;
        slot->state = TAJ_VISUAL_COMPOSED;
        slot->generation++;
        taj_visual_sync(slot, updateRate);
        taj_visual_trace(slot->recoveryAttempts != 0 ? "recompose" : "compose",
                         slot);
        taj_visual_maybe_drop_race_pair(slot);
    }
}

s32 taj_visual_suppress_donor_draw(const Object *obj) {
    TajVisualSlot *slot = taj_visual_slot_for(obj);
    s32 suppress = slot != NULL && slot->state == TAJ_VISUAL_COMPOSED &&
                   taj_visual_owner_is_eligible(obj);
    if (suppress && !slot->donorSuppressionTraced) {
        slot->donorSuppressionTraced = TRUE;
        taj_visual_trace("donor_suppressed", slot);
    }
    return suppress;
}

Object *taj_visual_effect_anchor(Object *obj) {
    TajVisualSlot *slot = taj_visual_slot_for(obj);

    if (slot == NULL || slot->state != TAJ_VISUAL_COMPOSED ||
        slot->carpet == NULL || !taj_visual_owner_is_eligible(obj)) {
        return obj;
    }
    if (!slot->effectAnchorTraced) {
        slot->effectAnchorTraced = TRUE;
        taj_visual_trace("effect_anchor", slot);
    }
    return slot->carpet;
}

s32 taj_visual_multiplayer_shadow_object(const Object *obj) {
    s32 i;
    if (obj == NULL ||
        taj_visual_test_flag("MDKR_TAJ_VISUAL_DISABLE_MULTIPLAYER_SHADOW",
                             &sRaceDisableMultiplayerShadow)) {
        return FALSE;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        TajVisualSlot *slot = &sSlots[i];
        if (slot->state == TAJ_VISUAL_COMPOSED && slot->carpet == obj &&
            slot->donor != NULL) {
            if (!slot->multiplayerShadowTraced) {
                slot->multiplayerShadowTraced = TRUE;
                taj_visual_trace("multiplayer_shadow", slot);
            }
            return TRUE;
        }
    }
    return FALSE;
}

s32 taj_visual_suppress_companion_shadow(const Object *obj) {
    s32 i;

    if (obj == NULL) {
        return FALSE;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        TajVisualSlot *slot = &sSlots[i];
        if (slot->state == TAJ_VISUAL_COMPOSED && slot->rider == obj &&
            slot->carpet != NULL && slot->donor != NULL) {
            if (!slot->riderShadowSuppressionTraced) {
                slot->riderShadowSuppressionTraced = TRUE;
                taj_visual_trace("rider_shadow_suppressed", slot);
            }
            return TRUE;
        }
    }
    return FALSE;
}

void taj_visual_on_object_free(Object *obj) {
    TajVisualSlot *slot = taj_visual_slot_for(obj);
    s32 i;
    if (sSelect.source == obj) {
        sSelect.source = NULL;
    }
    if (sSelect.rider == obj || sSelect.sign == obj) {
        taj_visual_select_pair_lost(
            obj, sSelect.sign == obj ? TAJ_VISUAL_FAULT_SIGN
                                      : TAJ_VISUAL_FAULT_RIDER);
    }
    if (slot != NULL) {
        taj_visual_trace("owner_free", slot);
        taj_visual_clear_slot(slot, TRUE);
        return;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].carpet == obj || sSlots[i].rider == obj) {
            taj_visual_race_pair_lost(
                &sSlots[i], obj,
                sSlots[i].carpet == obj ? TAJ_VISUAL_FAULT_CARPET
                                         : TAJ_VISUAL_FAULT_RIDER);
            return;
        }
    }
}

void taj_visual_on_object_destroy(Object *obj) {
    TajVisualSlot *slot = taj_visual_slot_for(obj);
    s32 i;
    if (sSelect.source == obj) {
        sSelect.source = NULL;
    }
    if (sSelect.rider == obj || sSelect.sign == obj) {
        /* Direct teardown must never queue another object. free_object()
         * already performed the transactional cleanup before obj_destroy(). */
        sSelect.rider = NULL;
        sSelect.sign = NULL;
    }
    /* obj_destroy is also the direct level-teardown walk.  Never queue here:
     * free_object already performed companion cleanup in ordinary transitions,
     * while free_all_objects reset+flushed companions before its direct walk.
     */
    if (slot != NULL) {
        slot->donor = NULL;
        slot->carpet = NULL;
        slot->rider = NULL;
        slot->state = TAJ_VISUAL_NONE;
        slot->bobPhase = 0;
        slot->animationWitnessTimer = 0;
        slot->animationWitnessCount = 0;
        slot->animationWitnessPending = FALSE;
        slot->carpetBaseScale = 0.0f;
        slot->riderBaseScale = 0.0f;
        slot->multiplayerShadowTraced = FALSE;
        slot->donorSuppressionTraced = FALSE;
        slot->effectAnchorTraced = FALSE;
        slot->dashActive = FALSE;
        slot->dashPulseTraced = FALSE;
        return;
    }
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].carpet == obj || sSlots[i].rider == obj) {
            /* Direct level teardown cannot queue the sibling, but ownership
             * still disappears atomically so no stale pointer can be reused. */
            sSlots[i].carpet = NULL;
            sSlots[i].rider = NULL;
            sSlots[i].state = TAJ_VISUAL_NONE;
            sSlots[i].bobPhase = 0;
            sSlots[i].animationWitnessTimer = 0;
            sSlots[i].animationWitnessCount = 0;
            sSlots[i].animationWitnessPending = FALSE;
            sSlots[i].carpetBaseScale = 0.0f;
            sSlots[i].riderBaseScale = 0.0f;
            sSlots[i].multiplayerShadowTraced = FALSE;
            sSlots[i].riderShadowSuppressionTraced = FALSE;
            sSlots[i].donorSuppressionTraced = FALSE;
            sSlots[i].effectAnchorTraced = FALSE;
            sSlots[i].dashActive = FALSE;
            sSlots[i].dashPulseTraced = FALSE;
            return;
        }
    }
}

u32 taj_visual_trace_epoch(void) {
    return sTraceEpoch;
}

void taj_visual_reset(void) {
    s32 i;
    sTraceEpoch++;
    taj_visual_select_end();
    for (i = 0; i < TAJ_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].donor != NULL) {
            taj_visual_trace("reset", &sSlots[i]);
            taj_visual_clear_slot(&sSlots[i], TRUE);
        }
    }
    sSpawnLease = TAJ_VISUAL_SPAWN_NONE;
}

#endif
