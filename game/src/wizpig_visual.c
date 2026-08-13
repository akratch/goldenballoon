#include "wizpig_visual.h"
#include "bonus_character_visual.h"
#include "taj_select_layout.h"

#ifdef NATIVE_PORT

#include "asset_enums.h"
#include "decomp_names.h"
#include "game.h"
#include "macros.h"
#include "math_util.h"
#include "mdkr_trace.h"
#include "object_models.h"
#include "objects.h"
#include "structs.h"
#include "textures_sprites.h"

#include <math.h>
#include <string.h>

#define WIZPIG_VISUAL_MAX_RACERS 4
#define WIZPIG_VISUAL_RETRY_LIMIT 8
#define WIZPIG_SELECT_RETRY_TICKS 15
#define WIZPIG_SELECT_CONFIRM_TICKS 60
#define WIZPIG_SELECT_SCALE 0.22f
#define WIZPIG_SELECT_Y_OFFSET 8.0f
#define WIZPIG_VEHICLE_SCALE 0.30f
#define WIZPIG_ROCKET_SCALE 0.28f
#define WIZPIG_VEHICLE_SEAT_Y 4.0f
#define WIZPIG_HOVER_SEAT_Y 14.0f
#define WIZPIG_SELECT_YAW 0x7F00

enum {
    WIZPIG_ANIM_IDLE = 0,
    WIZPIG_ANIM_WALK = 1,
    WIZPIG_ANIM_JUMP = 3,
};

typedef enum WizpigVisualLease {
    WIZPIG_LEASE_NONE,
    WIZPIG_LEASE_RACE,
    WIZPIG_LEASE_SELECT,
    WIZPIG_LEASE_SIGN,
} WizpigVisualLease;

typedef struct WizpigVisualSlot {
    Object *owner;
    Object *rider;
    s32 objectID;
    s32 attempts;
    s32 retryTimer;
    s32 composed;
    u32 animationWitness;
} WizpigVisualSlot;

typedef struct WizpigSelectVisual {
    TajSelectLayout layout;
    Object *authoredActors[13];
    f32 authoredScales[13];
    Object *source;
    Object *actor;
    Object *sign;
    u32 hoverMask;
    u32 confirmedMask;
    u32 previousConfirmedMask;
    s32 confirmTimer;
    s32 retryTimer;
    s32 attempts;
    s32 unavailable;
    s32 active;
    s32 signCycleTimer;
    s32 signCycleOffset;
    u32 animationWitness;
} WizpigSelectVisual;

static WizpigVisualIdentityPredicate sIdentityPredicate;
static WizpigVisualLease sLease;
static WizpigVisualSlot sSlots[WIZPIG_VISUAL_MAX_RACERS];
static WizpigSelectVisual sSelect;
static s32 sSelectPoseTraced;

static ObjectModel *wizpig_model_find(const Object *obj, s32 expectedModel) {
    s32 i;
    s32 *ids;
    if (obj == NULL || obj->header == NULL || obj->modelInstances == NULL ||
        obj->header->numberOfModelIds <= 0)
        return NULL;
    ids = DKR_PTR(s32, obj->header->modelIds);
    for (i = 0; i < obj->header->numberOfModelIds; i++) {
        if (ids[i] == expectedModel && obj->modelInstances[i] != NULL) {
            return obj->modelInstances[i]->objModel;
        }
    }
    return NULL;
}

/* Every animationID this module drives must exist on the model that will play it.
 * obj_clamp_model_animation() silently clamps an out-of-range animationID down to
 * numberOfAnimations - 1 (object_models.c), so a short asset would play a different clip forever
 * with no failure signal. Fold the bound into the schema instead, so a short asset fails
 * composition and the complete donor is drawn -- the fail-visible contract every other check here
 * follows.
 *
 * A lower bound rather than an exact count: an asset carrying EXTRA animations still plays every
 * clip we ask for correctly, so pinning the count would reject assets that are actually fine. */
static s32 wizpig_animations_ready(const ObjectModel *model) {
    return model != NULL && model->numberOfAnimations > WIZPIG_ANIM_JUMP;
}

static s32 wizpig_race_schema_ready(const Object *obj, s32 objectID) {
    ObjectModel *model;
    if (objectID == ASSET_OBJECT_ID_WIZPIG) {
        model = wizpig_model_find(obj, ASSET_OBJECTMODEL_WIZPIG);
        return model != NULL && model->numberOfTextures == 17 &&
               model->numberOfVertices == 740 &&
               model->numberOfTriangles == 561 &&
               model->numberOfBatches == 58 && wizpig_animations_ready(model);
    }
    if (objectID != ASSET_OBJECT_ID_WIZPIGROCKET)
        return FALSE;
    model = wizpig_model_find(obj, ASSET_OBJECTMODEL_WIZPIGROCKET);
    return model != NULL && model->numberOfTextures == 20 &&
           model->numberOfVertices == 887 && model->numberOfTriangles == 654 &&
           model->numberOfBatches == 68 && wizpig_animations_ready(model);
}

static s32 wizpig_donor_model_schema_ready(const Object *obj, s32 lod,
                                           const ObjectModel *model) {
    static const s16 carVertices[6] = {309, 258, 192, 129, 121, 24};
    static const s16 carTriangles[6] = {240, 168, 122, 72, 64, 13};
    static const s16 carBatches[6] = {29, 30, 24, 17, 13, 2};
    static const s16 hoverVertices[6] = {323, 285, 234, 196, 143, 14};
    static const s16 hoverTriangles[6] = {237, 182, 137, 100, 78, 14};
    static const s16 hoverBatches[6] = {30, 30, 29, 24, 16, 2};
    const s16 *vertices;
    const s16 *triangles;
    const s16 *batches;

    if (obj == NULL || obj->racer == NULL || model == NULL || lod < 0 ||
        lod >= 6)
        return FALSE;
    if (obj->racer->vehicleIDPrev == VEHICLE_CAR) {
        vertices = carVertices;
        triangles = carTriangles;
        batches = carBatches;
    } else if (obj->racer->vehicleIDPrev == VEHICLE_HOVERCRAFT) {
        vertices = hoverVertices;
        triangles = hoverTriangles;
        batches = hoverBatches;
    } else if (obj->racer->vehicleIDPrev == VEHICLE_PLANE) {
        /* Whole-plane hiding consumes no batch index, so there is nothing for a per-LOD geometry
         * fingerprint to protect here. Identity is still asserted by the modelIds check in the
         * caller. */
        return TRUE;
    } else {
        return FALSE;
    }
    return model->numberOfVertices == vertices[lod] &&
           model->numberOfTriangles == triangles[lod] &&
           model->numberOfBatches == batches[lod];
}

static s32 wizpig_donor_schema_ready(const Object *obj) {
    s32 firstModel;
    s32 *ids;
    s32 i;
    s32 loaded = 0;

    if (obj == NULL || obj->racer == NULL || obj->header == NULL ||
        obj->modelInstances == NULL || obj->header->numberOfModelIds < 6) {
        return FALSE;
    }
    if (obj->racer->vehicleIDPrev == VEHICLE_CAR) {
        firstModel = ASSET_OBJECTMODEL_KREMCAR_0;
    } else if (obj->racer->vehicleIDPrev == VEHICLE_HOVERCRAFT) {
        firstModel = ASSET_OBJECTMODEL_KREMLINHOVER_0;
    } else if (obj->racer->vehicleIDPrev == VEHICLE_PLANE) {
        /* The plane is hidden WHOLE -- no batch index of it is ever consumed -- so unlike the car
         * and hovercraft there is no carve that a wrong geometry fingerprint could misapply, and no
         * per-LOD vertex/triangle/batch table is required for correctness.
         *
         * What still has to hold is identity: we must be hiding the plane we think we are hiding.
         * Fall through to the shared model-ID loop below, which asserts the donor really is
         * KREMPLANE_0..5 and fails to a complete donor otherwise. The geometry fingerprint is
         * skipped for the plane by wizpig_donor_model_schema_ready() returning TRUE for it. */
        firstModel = ASSET_OBJECTMODEL_KREMPLANE_0;
    } else {
        return FALSE;
    }
    ids = DKR_PTR(s32, obj->header->modelIds);
    for (i = 0; i < 6; i++) {
        ObjectModel *model;
        if (ids[i] != firstModel + i)
            return FALSE;
        if (obj->modelInstances[i] == NULL)
            continue;
        model = obj->modelInstances[i]->objModel;
        if (!wizpig_donor_model_schema_ready(obj, i, model)) {
            return FALSE;
        }
        loaded++;
    }
    return loaded > 0;
}

static s32 wizpig_select_schema_ready(const Object *obj) {
    ObjectModel *model = wizpig_model_find(obj, ASSET_OBJECTMODEL_WIZPIG);
    return model != NULL && model->numberOfTextures == 17 &&
           model->numberOfVertices == 740 && model->numberOfTriangles == 561 &&
           model->numberOfBatches == 58 && wizpig_animations_ready(model);
}

static s32 wizpig_sign_schema_ready(const Object *obj) {
    ObjectModel *model = wizpig_model_find(obj, ASSET_OBJECTMODEL_DIDDYSELECT);
    TriangleBatchInfo *batches;
    s32 i;
    if (model == NULL || model->numberOfTextures != 20 ||
        model->numberOfVertices != 343 || model->numberOfTriangles != 297 ||
        model->numberOfBatches != 28 || model->batches == 0)
        return FALSE;
    batches = DKR_PTR(TriangleBatchInfo, model->batches);
    if (batches[0].textureIndex >= 4 || batches[0].verticesOffset != 0 ||
        batches[0].facesOffset != 0 || batches[1].verticesOffset != 4 ||
        batches[1].facesOffset != 2)
        return FALSE;
    for (i = 1; i < model->numberOfBatches; i++) {
        if (batches[i].textureIndex < 4)
            return FALSE;
    }
    return TRUE;
}

static Object *wizpig_spawn(WizpigVisualLease lease, s32 objectID,
                            const Object *source) {
    LevelObjectEntry_Racer racerEntry;
    LevelObjectEntry_AnimatedObject animatedEntry;
    LevelObjectEntryCommon *entry;
    Object *obj;
    if (sLease != WIZPIG_LEASE_NONE || source == NULL)
        return NULL;
    if (lease == WIZPIG_LEASE_SIGN) {
        memset(&animatedEntry, 0, sizeof(animatedEntry));
        entry = &animatedEntry.common;
        entry->size = (u8)sizeof(animatedEntry);
    } else {
        /* Both Wizpig object headers are BHV_RACER. The native spawn contract
         * validates the entry before our lease can replace that behaviour, so
         * supplying the full racer entry is required even for presentation-only
         * actors. This is also important on N64: the racer initializer reads
         * the trailing angles and player index before the claim returns. */
        memset(&racerEntry, 0, sizeof(racerEntry));
        entry = &racerEntry.common;
        entry->size = (u8)sizeof(racerEntry);
        racerEntry.playerIndex =
            4; /* Never bind a camera if a claim regresses. */
    }
    entry->objectID = (u8)objectID;
    entry->size |= (u8)((objectID & 0x100) >> 1);
    entry->x = (s16)source->trans.x_position;
    entry->y = (s16)source->trans.y_position;
    entry->z = (s16)source->trans.z_position;
    sLease = lease;
    obj = spawn_object(entry, OBJECT_SPAWN_UNK01);
    /* The claim hook clears sLease as it consumes it. A still-set lease here means spawn_object()
     * returned without ever reaching run_object_init_func() for our entry -- so this object was
     * never neutered (BHV_NONE, no interactObj, no level_entry) and is a live, physics-owning
     * racer. Refuse it rather than tracking it as a presentation companion. */
    if (sLease != WIZPIG_LEASE_NONE) {
        sLease = WIZPIG_LEASE_NONE;
        if (obj != NULL) {
            free_object(obj);
        }
        return NULL;
    }
    return obj;
}

s32 wizpig_visual_claim_spawned_object(Object *obj) {
    if (sLease == WIZPIG_LEASE_NONE || obj == NULL)
        return FALSE;
    sLease = WIZPIG_LEASE_NONE;
    obj->behaviorId = BHV_NONE;
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

static WizpigVisualSlot *wizpig_slot_for_owner(const Object *owner) {
    s32 i;
    for (i = 0; i < WIZPIG_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == owner)
            return &sSlots[i];
    }
    return NULL;
}

static WizpigVisualSlot *wizpig_slot_for_object(const Object *obj) {
    s32 i;
    for (i = 0; i < WIZPIG_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == obj || sSlots[i].rider == obj)
            return &sSlots[i];
    }
    return NULL;
}

static WizpigVisualSlot *wizpig_slot_allocate(Object *owner) {
    WizpigVisualSlot *slot = wizpig_slot_for_owner(owner);
    s32 i;
    if (slot != NULL)
        return slot;
    for (i = 0; i < WIZPIG_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == NULL) {
            sSlots[i].owner = owner;
            return &sSlots[i];
        }
    }
    return NULL;
}

static s32 wizpig_owner_eligible(const Object *obj) {
    WizpigVisualSlot *slot;
    if (obj == NULL || obj->behaviorId != BHV_RACER || obj->racer == NULL ||
        obj->segmentID < -1 || obj->segmentID >= 128)
        return FALSE;
    if (obj->racer->playerIndex >= PLAYER_ONE &&
        obj->racer->playerIndex <= PLAYER_FOUR) {
        return sIdentityPredicate != NULL &&
               sIdentityPredicate(obj->racer->playerIndex) == MOD_RACER_WIZPIG;
    }
    slot = wizpig_slot_for_owner(obj);
    return obj->racer->raceFinished && slot != NULL && slot->composed;
}

static s32 wizpig_object_id_for_owner(const Object *owner) {
    (void)owner;
    /* Model 215 is the authored seated Wizpig/rocket composition. Ground
     * vehicles suppress its rocket-only batches, leaving the properly posed
     * rider in Krunch's kart or hovercraft; flying tracks retain the complete
     * composition and suppress the donor plane. */
    return ASSET_OBJECT_ID_WIZPIGROCKET;
}

static void wizpig_clear_slot(WizpigVisualSlot *slot, s32 freeRider) {
    Object *rider;
    if (slot == NULL)
        return;
    rider = slot->rider;
    memset(slot, 0, sizeof(*slot));
    if (freeRider && rider != NULL)
        free_object(rider);
}

static void wizpig_apply_owner_pose(Object *rider, const Object *owner) {
    Object_Racer *racer = owner->racer;
    f32 offsetY = 0.0f;
    f32 cosine;
    rider->trans = owner->trans;
    rider->trans.scale = bonus_visual_companion_scale(
        rider, owner,
        racer->vehicleIDPrev == VEHICLE_PLANE ? WIZPIG_ROCKET_SCALE
                                              : WIZPIG_VEHICLE_SCALE);
    rider->trans.x_position += racer->carBobX;
    rider->trans.y_position += racer->carBobY;
    rider->trans.z_position += racer->carBobZ;
    rider->trans.rotation.y_rotation += racer->y_rotation_offset;
    rider->trans.rotation.x_rotation += racer->x_rotation_offset;
    rider->trans.rotation.z_rotation += racer->z_rotation_offset;
    if (racer->vehicleIDPrev < VEHICLE_BOSSES) {
        cosine = coss_f(racer->z_rotation_offset);
        cosine = coss_f(racer->x_rotation_offset - racer->unk166) * cosine;
        cosine = cosine < 0.0f ? 0.0f : cosine * cosine;
        offsetY = (1.0f - cosine) * 24.0f + racer->unkD0;
    }
    bonus_visual_apply_local_offset(rider, 0.0f, offsetY, 0.0f);
    if (racer->vehicleIDPrev != VEHICLE_PLANE) {
        bonus_visual_apply_local_offset(
            rider, 0.0f,
            racer->vehicleIDPrev == VEHICLE_HOVERCRAFT ? WIZPIG_HOVER_SEAT_Y
                                                       : WIZPIG_VEHICLE_SEAT_Y,
            0.0f);
    }
    rider->segmentID = owner->segmentID;
    rider->opacity = owner->opacity;
    bonus_visual_scale_shadow(rider);
}

static void wizpig_sync(WizpigVisualSlot *slot, s32 updateRate) {
    /* Report the buffers obj_animate_tick published for the PREVIOUS tick, before this tick's
     * clock advance overwrites the inputs that produced them. */
    bonus_visual_trace_animation("WIZPIG", slot->rider, slot->animationWitness++);
    wizpig_apply_owner_pose(slot->rider, slot->owner);
    slot->rider->animationID = 0;
    slot->rider->animFrame += (s16)(updateRate * 2);
    obj_clamp_model_animation(slot->rider);
    /* Advance the clock only. obj_animate_tick() owns the deformation and the published
     * curVertData for every animated object, presentation actors included, and it runs after
     * obj_update() in both mode_game and update_menu_scene.
     *
     * This used to call obj_animate() and assign curVertData here as well. That ran the deformation
     * TWICE per tick against one animFrame: obj_animate() flips animationTaskNum and writes the
     * newly selected half (hasm_native/obj_animate.c:271-275), so both halves of the double buffer
     * ended up holding the same frame and retained animated-vertex interpolation had nothing to
     * blend -- the rider's deformation snapped at tick rate under an interpolated camera. It also
     * bypassed the animUpdateTimer cadence obj_animate_tick exists to own, and indexed
     * modelInstances[] without the NULL and range guards that loop carries. Terry never did this;
     * the two modules now agree. */
}

void wizpig_visual_set_identity_predicate(
    WizpigVisualIdentityPredicate predicate) {
    sIdentityPredicate = predicate;
    if (predicate == NULL)
        wizpig_visual_reset();
}

void wizpig_visual_tick(s32 updateRate) {
    Object **racers;
    s32 racerCount;
    s32 i;
    for (i = 0; i < WIZPIG_VISUAL_MAX_RACERS; i++) {
        WizpigVisualSlot *slot = &sSlots[i];
        if (slot->owner != NULL && !wizpig_owner_eligible(slot->owner)) {
            wizpig_clear_slot(slot, TRUE);
        }
    }
    racers = get_racer_objects(&racerCount);
    for (i = 0; racers != NULL && i < racerCount; i++) {
        Object *owner = racers[i];
        WizpigVisualSlot *slot;
        Object *rider;
        s32 objectID;
        if (!wizpig_owner_eligible(owner))
            continue;
        slot = wizpig_slot_allocate(owner);
        if (slot == NULL)
            continue;
        objectID = wizpig_object_id_for_owner(owner);
        if (slot->composed && slot->objectID != objectID) {
            wizpig_clear_slot(slot, TRUE);
            slot = wizpig_slot_allocate(owner);
            if (slot == NULL)
                continue;
        }
        if (slot->composed && slot->rider != NULL) {
            wizpig_sync(slot, updateRate);
            continue;
        }
        if (slot->attempts >= WIZPIG_VISUAL_RETRY_LIMIT)
            continue;
        if (slot->retryTimer > 0) {
            slot->retryTimer -= updateRate;
            continue;
        }
        rider = wizpig_spawn(WIZPIG_LEASE_RACE, objectID, owner);
        if (rider == NULL || !wizpig_race_schema_ready(rider, objectID) ||
            !wizpig_donor_schema_ready(owner) ||
            !isfinite(rider->trans.scale) || rider->trans.scale <= 0.0f) {
            if (rider != NULL)
                free_object(rider);
            slot->attempts++;
            slot->retryTimer = WIZPIG_SELECT_RETRY_TICKS;
            continue;
        }
        slot->rider = rider;
        slot->objectID = objectID;
        slot->composed = TRUE;
        MDKR_TRACE("wizpig_visual: composed player=%d vehicle=%d scale=%.3f "
                   "rotation=%d,%d,%d offsets=%d,%d,%d",
                   owner->racer->playerIndex, owner->racer->vehicleIDPrev,
                   bonus_visual_companion_scale(rider, owner,
                                                owner->racer->vehicleIDPrev ==
                                                        VEHICLE_PLANE
                                                    ? WIZPIG_ROCKET_SCALE
                                                    : WIZPIG_VEHICLE_SCALE),
                   owner->trans.rotation.x_rotation,
                   owner->trans.rotation.y_rotation,
                   owner->trans.rotation.z_rotation,
                   owner->racer->x_rotation_offset,
                   owner->racer->y_rotation_offset,
                   owner->racer->z_rotation_offset);
        wizpig_sync(slot, updateRate);
    }
    if (!sSelect.active || sSelect.source == NULL)
        return;
    if ((sSelect.actor == NULL || sSelect.sign == NULL) &&
        !sSelect.unavailable) {
        Object *actor;
        Object *sign;
        if (sSelect.retryTimer > 0) {
            sSelect.retryTimer -= updateRate;
            return;
        }
        actor = wizpig_spawn(WIZPIG_LEASE_SELECT, ASSET_OBJECT_ID_WIZPIG,
                             sSelect.source);
        sign = actor != NULL
                   ? wizpig_spawn(WIZPIG_LEASE_SIGN,
                                  ASSET_OBJECT_ID_DIDDYSELECT, sSelect.source)
                   : NULL;
        if (actor == NULL || sign == NULL ||
            !wizpig_select_schema_ready(actor) ||
            !wizpig_sign_schema_ready(sign)) {
            if (actor != NULL)
                free_object(actor);
            if (sign != NULL)
                free_object(sign);
            sSelect.attempts++;
            if (sSelect.attempts >= WIZPIG_VISUAL_RETRY_LIMIT) {
                sSelect.unavailable = TRUE;
            } else {
                sSelect.retryTimer = WIZPIG_SELECT_RETRY_TICKS;
            }
            return;
        }
        sSelect.actor = actor;
        sSelect.sign = sign;
        MDKR_TRACE("mod_select_visual: identity=WIZPIG actor=1 sign=1");
        if (sign->shadow != NULL)
            sign->shadow->scale = 0.0f;
    }
    if (sSelect.actor != NULL && sSelect.sign != NULL) {
        f32 x;
        Object *actor = sSelect.actor;
        Object *sign = sSelect.sign;
        if (!taj_select_layout_position(
                &sSelect.layout, sSelect.layout.wizpigIndex, NULL, NULL, &x))
            return;
        actor->trans = sSelect.source->trans;
        actor->trans.x_position = x;
        actor->trans.y_position += WIZPIG_SELECT_Y_OFFSET;
        actor->trans.rotation.x_rotation = 0;
        actor->trans.rotation.y_rotation = WIZPIG_SELECT_YAW;
        actor->trans.rotation.z_rotation = 0;
        actor->trans.scale = WIZPIG_SELECT_SCALE *
                             taj_select_layout_scale(
                                 &sSelect.layout, sSelect.layout.wizpigIndex);
        bonus_visual_scale_shadow(actor);
        actor->segmentID = sSelect.source->segmentID;
        actor->opacity = sSelect.source->opacity;
        actor->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
        {
            s32 animation;
            if (sSelect.confirmTimer > 0) {
                animation = WIZPIG_ANIM_JUMP;
                sSelect.confirmTimer -= updateRate;
                if (sSelect.confirmTimer < 0)
                    sSelect.confirmTimer = 0;
            } else if (sSelect.hoverMask != 0) {
                animation = WIZPIG_ANIM_IDLE;
            } else {
                animation = WIZPIG_ANIM_WALK;
            }
            if (actor->animationID != animation) {
                actor->animationID = animation;
                actor->animFrame = 0;
                MDKR_TRACE("mod_select_pose_state: identity=WIZPIG "
                           "animation=%d hover=0x%X confirmed=0x%X timer=%d",
                           animation, sSelect.hoverMask,
                           sSelect.confirmedMask, sSelect.confirmTimer);
            }
        }
        bonus_visual_trace_animation("WIZPIG_SELECT", actor,
                                     sSelect.animationWitness++);
        actor->animFrame += (s16)updateRate;
        obj_clamp_model_animation(actor);
        /* Clock only; obj_animate_tick() deforms and publishes. update_menu_scene() runs
         * obj_update() (which reaches here) before obj_animate_tick(), the same order mode_game
         * uses, so the picker actor is animated on exactly the same contract as the race actor.
         * See wizpig_sync() for why the explicit obj_animate() call was removed. */
        if (!sSelectPoseTraced) {
            ObjectModel *model =
                wizpig_model_find(actor, ASSET_OBJECTMODEL_WIZPIG);
            Vertex *vertices =
                model != NULL ? DKR_PTR(Vertex, model->vertices) : NULL;
            s32 minY = 0;
            s32 maxY = 0;
            s32 vertexIndex;
            if (vertices != NULL && model->numberOfVertices > 0) {
                minY = maxY = vertices[0].y;
                for (vertexIndex = 1; vertexIndex < model->numberOfVertices;
                     vertexIndex++) {
                    if (vertices[vertexIndex].y < minY)
                        minY = vertices[vertexIndex].y;
                    if (vertices[vertexIndex].y > maxY)
                        maxY = vertices[vertexIndex].y;
                }
            }
            MDKR_TRACE("mod_select_pose: identity=WIZPIG x=%.2f y=%.2f z=%.2f "
                       "scale=%.3f flags=0x%x model=%d minY=%d maxY=%d",
                       actor->trans.x_position, actor->trans.y_position,
                       actor->trans.z_position, actor->trans.scale,
                       actor->trans.flags, model != NULL, minY, maxY);
            sSelectPoseTraced = TRUE;
        }
        sign->trans = actor->trans;
        sign->segmentID = actor->segmentID;
        sign->opacity = actor->opacity;
        sign->animationID = 0;
        if (sSelect.hoverMask != 0) {
            sign->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
        } else {
            sign->trans.flags |= OBJ_FLAGS_INVISIBLE;
        }
        sign->animFrame += (s16)updateRate;
        obj_clamp_model_animation(sign);
        if (sSelect.hoverMask != 0) {
            s32 players = 0;
            s32 player;
            for (player = 0; player < TAJ_MOD_MAX_PLAYERS; player++) {
                if (sSelect.hoverMask & taj_mod_player_bit(player))
                    players++;
            }
            sSelect.signCycleTimer += updateRate;
            if (players > 0 && sSelect.signCycleTimer >= 16) {
                sSelect.signCycleTimer &= 0xF;
                sSelect.signCycleOffset =
                    (sSelect.signCycleOffset + 1) % players;
            }
        }
    }
}

void wizpig_visual_select_begin(const TajSelectLayout *layout) {
    wizpig_visual_select_end();
    if (layout == NULL || layout->wizpigIndex < 0)
        return;
    sSelect.layout = *layout;
    sSelect.active = TRUE;
}

void wizpig_visual_select_set_state(u32 hoverMask, u32 confirmedMask) {
    u32 newlyConfirmed;
    if (!sSelect.active)
        return;
    newlyConfirmed = confirmedMask & ~sSelect.previousConfirmedMask;
    if (hoverMask != sSelect.hoverMask) {
        sSelect.signCycleTimer = 0;
        sSelect.signCycleOffset = 0;
    }
    sSelect.hoverMask = hoverMask;
    sSelect.confirmedMask = confirmedMask;
    if (newlyConfirmed != 0) {
        sSelect.confirmTimer = WIZPIG_SELECT_CONFIRM_TICKS;
    }
    sSelect.previousConfirmedMask = confirmedMask;
}

void wizpig_visual_select_apply_authored_actor(Object *obj,
                                               s32 characterIndex) {
    TajSelectRow row;
    TajSelectRow wizpigRow;
    f32 x;
    if (!sSelect.active || obj == NULL ||
        !taj_select_layout_position(&sSelect.layout, characterIndex, &row, NULL,
                                    &x) ||
        !taj_select_layout_position(&sSelect.layout, sSelect.layout.wizpigIndex,
                                    &wizpigRow, NULL, NULL))
        return;
    /* Taj already owns the shared layout transform when present. */
    if (sSelect.layout.tajIndex < 0) {
        obj->trans.x_position = x;
        if (characterIndex >= 0 &&
            characterIndex < sSelect.layout.characterCount) {
            if (sSelect.authoredActors[characterIndex] != obj) {
                sSelect.authoredActors[characterIndex] = obj;
                sSelect.authoredScales[characterIndex] = obj->trans.scale;
            }
            obj->trans.scale =
                sSelect.authoredScales[characterIndex] *
                taj_select_layout_scale(&sSelect.layout, characterIndex);
        }
    }
    if (sSelect.source == NULL && row == wizpigRow)
        sSelect.source = obj;
}

void wizpig_visual_select_end(void) {
    Object *actor = sSelect.actor;
    Object *sign = sSelect.sign;
    memset(&sSelect, 0, sizeof(sSelect));
    sSelectPoseTraced = FALSE;
    sSelect.layout.tajIndex = -1;
    sSelect.layout.wizpigIndex = -1;
    sSelect.layout.terryIndex = -1;
    if (actor != NULL)
        free_object(actor);
    if (sign != NULL)
        free_object(sign);
}

TajSelectVisualStatus wizpig_visual_select_status(void) {
    if (!sSelect.active)
        return TAJ_SELECT_VISUAL_INACTIVE;
    if (sSelect.unavailable)
        return TAJ_SELECT_VISUAL_UNAVAILABLE;
    if (sSelect.actor != NULL && sSelect.sign != NULL) {
        return TAJ_SELECT_VISUAL_READY;
    }
    return TAJ_SELECT_VISUAL_LOADING;
}

s32 wizpig_visual_select_sign_object(const Object *obj) {
    return obj != NULL && obj == sSelect.sign;
}

s32 wizpig_visual_select_sign_player(const Object *obj) {
    s32 player;
    s32 selected = 0;
    if (!wizpig_visual_select_sign_object(obj) || sSelect.hoverMask == 0 ||
        wizpig_visual_select_status() != TAJ_SELECT_VISUAL_READY)
        return -1;
    for (player = 0; player < TAJ_MOD_MAX_PLAYERS; player++) {
        if (sSelect.hoverMask & taj_mod_player_bit(player)) {
            if (selected == sSelect.signCycleOffset)
                return player;
            selected++;
        }
    }
    return -1;
}

s32 wizpig_visual_select_sign_batch(const Object *obj, s32 batchIndex) {
    return wizpig_visual_select_sign_object(obj) && batchIndex == 0;
}

static s32 wizpig_owner_uses_rocket(const Object *owner);

s32 wizpig_visual_is_presentation_object(const Object *obj) {
    return obj != NULL && (obj == sSelect.actor || obj == sSelect.sign ||
                           (wizpig_slot_for_object(obj) != NULL &&
                            wizpig_slot_for_object(obj)->rider == obj));
}

s32 wizpig_visual_uses_boss_head_matrix(const Object *obj) {
    WizpigVisualSlot *slot = wizpig_slot_for_object(obj);
    return obj != NULL &&
           (obj == sSelect.actor ||
            (slot != NULL && slot->composed && slot->rider == obj));
}

s32 wizpig_visual_multiplayer_shadow_object(const Object *obj) {
    WizpigVisualSlot *slot = wizpig_slot_for_object(obj);
    return slot != NULL && slot->composed && slot->rider == obj &&
           wizpig_owner_uses_rocket(slot->owner);
}

static s32 wizpig_owner_uses_rocket(const Object *owner) {
    return owner != NULL && owner->racer != NULL &&
           owner->racer->vehicleIDPrev == VEHICLE_PLANE;
}

s32 wizpig_visual_suppress_donor_draw(const Object *obj) {
    WizpigVisualSlot *slot = wizpig_slot_for_owner(obj);
    return slot != NULL && slot->composed && slot->rider != NULL &&
           wizpig_owner_eligible(obj) && wizpig_owner_uses_rocket(obj);
}

s32 wizpig_visual_batch_visible(const ObjectModel *model, const Object *obj,
                                s32 batchIndex) {
    WizpigVisualSlot *slot;
    s32 i;
    if (model == NULL || obj == NULL || batchIndex < 0 ||
        batchIndex >= model->numberOfBatches)
        return TRUE;
    slot = wizpig_slot_for_object(obj);
    if (slot == NULL || !slot->composed)
        return TRUE;
    if (obj == slot->owner) {
        if (wizpig_owner_uses_rocket(obj))
            return FALSE;
        for (i = 0; i < obj->header->numberOfModelIds; i++) {
            if (obj->modelInstances[i] != NULL &&
                obj->modelInstances[i]->objModel == model) {
                /* LODs are lazy. Revalidate the exact model at draw time so a
                 * later-loaded, incompatible asset fails visible instead of
                 * having hard-coded driver ranges carve up its vehicle. */
                if (!wizpig_donor_model_schema_ready(obj, i, model)) {
                    return TRUE;
                }
                return !bonus_visual_krunch_driver_batch(
                    obj->racer->vehicleIDPrev, i, batchIndex);
            }
        }
    } else if (obj == slot->rider && !wizpig_owner_uses_rocket(slot->owner)) {
        /* These seven batch indices are the rocket-only materials (environment shell, red stripe,
         * lightning) in ASSET_OBJECTMODEL_WIZPIGROCKET. They are meaningless on any other model, so
         * revalidate the exact fingerprint at draw time for the same reason the donor branch above
         * does: this must fail VISIBLE (a complete rider) rather than carve seven arbitrary batches
         * out of an asset whose layout we have not proven. */
        if (model->numberOfTextures != 20 || model->numberOfVertices != 887 ||
            model->numberOfTriangles != 654 || model->numberOfBatches != 68) {
            return TRUE;
        }
        return !((batchIndex >= 9 && batchIndex <= 13) || batchIndex == 63 ||
                 batchIndex == 64);
    }
    return TRUE;
}

s32 wizpig_visual_cap_donor_lod(const Object *obj, s32 modelIndex) {
    WizpigVisualSlot *slot = wizpig_slot_for_owner(obj);
    if (slot != NULL && slot->composed && !wizpig_owner_uses_rocket(obj) &&
        modelIndex > 4)
        return 4;
    return modelIndex;
}

s32 wizpig_visual_suppress_shadow(const Object *obj) {
    WizpigVisualSlot *slot = wizpig_slot_for_object(obj);
    if (slot == NULL || !slot->composed)
        return FALSE;
    if (obj == slot->rider)
        return !wizpig_owner_uses_rocket(slot->owner);
    if (obj == slot->owner)
        return wizpig_owner_uses_rocket(slot->owner);
    return FALSE;
}

void wizpig_visual_on_object_free(Object *obj) {
    WizpigVisualSlot *slot = wizpig_slot_for_object(obj);
    s32 i;

    /* sSelect.source and sSelect.authoredActors[] are BORROWED pointers to retail character-select
     * actors this module never owns. They are dereferenced every tick while the picker is live
     * (wizpig_sync_select_actor), so they must be dropped the moment the pool reclaims them --
     * object slots are recycled aggressively, and a stale entry would silently re-target Wizpig
     * onto an unrelated live object's transform. Cleared before the actor/sign branch below, which
     * returns early. Mirrors taj_visual_on_object_free(). */
    if (sSelect.source == obj) {
        sSelect.source = NULL;
    }
    for (i = 0; i < (s32) ARRAY_COUNT(sSelect.authoredActors); i++) {
        if (sSelect.authoredActors[i] == obj) {
            sSelect.authoredActors[i] = NULL;
            sSelect.authoredScales[i] = 0.0f;
        }
    }
    if (obj == sSelect.actor || obj == sSelect.sign) {
        Object *other = obj == sSelect.actor ? sSelect.sign : sSelect.actor;
        sSelect.actor = NULL;
        sSelect.sign = NULL;
        if (other != NULL)
            free_object(other);
        return;
    }
    if (slot == NULL)
        return;
    if (obj == slot->owner) {
        wizpig_clear_slot(slot, TRUE);
    } else if (obj == slot->rider) {
        slot->rider = NULL;
        slot->composed = FALSE;
        slot->attempts++;
        slot->retryTimer = WIZPIG_SELECT_RETRY_TICKS;
    }
}

void wizpig_visual_on_object_destroy(Object *obj) {
    wizpig_visual_on_object_free(obj);
}

void wizpig_visual_reset(void) {
    s32 i;
    wizpig_visual_select_end();
    for (i = 0; i < WIZPIG_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner != NULL)
            wizpig_clear_slot(&sSlots[i], TRUE);
    }
    sLease = WIZPIG_LEASE_NONE;
}

#endif
