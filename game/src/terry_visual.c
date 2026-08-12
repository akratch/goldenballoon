#include "terry_visual.h"
#include "bonus_character_visual.h"
#include "taj_select_layout.h"

#ifdef NATIVE_PORT

#include "asset_enums.h"
#include "audio.h"
#include "audio_spatial.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERRY_VISUAL_MAX_RACERS 4
#define TERRY_VISUAL_RETRY_LIMIT 8
#define TERRY_VISUAL_RETRY_TICKS 15
#define TERRY_SELECT_CONFIRM_TICKS 60
#define TERRY_SELECT_SCALE 0.20f
#define TERRY_SELECT_Y_OFFSET 8.0f
#define TERRY_SELECT_SIGN_Y_OFFSET -8.0f
#define TERRY_VEHICLE_SCALE 0.30f
#define TERRY_FLIGHT_SCALE 0.40f
#define TERRY_VEHICLE_SEAT_Y 10.0f
#define TERRY_HOVER_SEAT_Y 18.0f
#define TERRY_SELECT_YAW 0x7F00

enum {
    TERRY_ANIM_IDLE = 0,
    TERRY_ANIM_WALK = 2,
    TERRY_ANIM_FLY = 4,
};

#define TERRY_FLAP_CUE_FRAME (2 << 4)

typedef enum TerryVisualLease {
    TERRY_LEASE_NONE,
    TERRY_LEASE_RACE,
    TERRY_LEASE_SELECT,
    TERRY_LEASE_SIGN,
} TerryVisualLease;

typedef struct TerryVisualSlot {
    Object *owner;
    Object *actor;
    u32 flightTicks;
    u32 lastFlapTick;
    u32 flapCount;
    s32 attempts;
    s32 retryTimer;
    s32 composed;
} TerryVisualSlot;

typedef struct TerrySelectVisual {
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
} TerrySelectVisual;

static TerryVisualIdentityPredicate sIdentityPredicate;
static TerryVisualLease sLease;
static TerryVisualSlot sSlots[TERRY_VISUAL_MAX_RACERS];
static TerrySelectVisual sSelect;
static s32 sSelectPoseTraced;

static s32 terry_audio_trace_enabled(void) {
    const char *value = getenv("MDKR_TERRY_AUDIO_TRACE");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static ObjectModel *terry_model_find(const Object *obj, s32 expectedModel) {
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

static s32 terry_actor_schema_ready(const Object *obj) {
    ObjectModel *model = terry_model_find(obj, ASSET_OBJECTMODEL_TERRYBOSS);
    return model != NULL && model->numberOfTextures == 6 &&
           model->numberOfVertices == 205 && model->numberOfTriangles == 166 &&
           model->numberOfBatches == 19;
}

static s32 terry_donor_model_schema_ready(const Object *obj, s32 lod,
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
    } else {
        return FALSE;
    }
    return model->numberOfVertices == vertices[lod] &&
           model->numberOfTriangles == triangles[lod] &&
           model->numberOfBatches == batches[lod];
}

static s32 terry_donor_schema_ready(const Object *obj) {
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
        return TRUE;
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
        if (!terry_donor_model_schema_ready(obj, i, model))
            return FALSE;
        loaded++;
    }
    return loaded > 0;
}

static s32 terry_sign_schema_ready(const Object *obj) {
    ObjectModel *model = terry_model_find(obj, ASSET_OBJECTMODEL_DIDDYSELECT);
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

static Object *terry_spawn(TerryVisualLease lease, s32 objectID,
                           const Object *source) {
    LevelObjectEntry_Racer racerEntry;
    LevelObjectEntry_AnimatedObject animatedEntry;
    LevelObjectEntryCommon *entry;
    Object *obj;
    if (sLease != TERRY_LEASE_NONE || source == NULL)
        return NULL;
    if (lease == TERRY_LEASE_SIGN) {
        memset(&animatedEntry, 0, sizeof(animatedEntry));
        entry = &animatedEntry.common;
        entry->size = (u8)sizeof(animatedEntry);
    } else {
        memset(&racerEntry, 0, sizeof(racerEntry));
        entry = &racerEntry.common;
        entry->size = (u8)sizeof(racerEntry);
        racerEntry.playerIndex = 4;
    }
    entry->objectID = (u8)objectID;
    entry->size |= (u8)((objectID & 0x100) >> 1);
    entry->x = (s16)source->trans.x_position;
    entry->y = (s16)source->trans.y_position;
    entry->z = (s16)source->trans.z_position;
    sLease = lease;
    obj = spawn_object(entry, OBJECT_SPAWN_UNK01);
    /* An unconsumed lease means the claim hook never ran for this entry, so the object was never
     * neutered; see wizpig_visual_spawn_companion(). Refuse it. */
    if (sLease != TERRY_LEASE_NONE) {
        sLease = TERRY_LEASE_NONE;
        if (obj != NULL) {
            free_object(obj);
        }
        return NULL;
    }
    return obj;
}

s32 terry_visual_claim_spawned_object(Object *obj) {
    if (sLease == TERRY_LEASE_NONE || obj == NULL)
        return FALSE;
    sLease = TERRY_LEASE_NONE;
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

static TerryVisualSlot *terry_slot_for_owner(const Object *owner) {
    s32 i;
    for (i = 0; i < TERRY_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == owner)
            return &sSlots[i];
    }
    return NULL;
}

static TerryVisualSlot *terry_slot_for_object(const Object *obj) {
    s32 i;
    for (i = 0; i < TERRY_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == obj || sSlots[i].actor == obj)
            return &sSlots[i];
    }
    return NULL;
}

static TerryVisualSlot *terry_slot_allocate(Object *owner) {
    TerryVisualSlot *slot = terry_slot_for_owner(owner);
    s32 i;
    if (slot != NULL)
        return slot;
    for (i = 0; i < TERRY_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner == NULL) {
            sSlots[i].owner = owner;
            return &sSlots[i];
        }
    }
    return NULL;
}

static s32 terry_owner_eligible(const Object *obj) {
    TerryVisualSlot *slot;
    if (obj == NULL || obj->behaviorId != BHV_RACER || obj->racer == NULL ||
        obj->segmentID < -1 || obj->segmentID >= 128)
        return FALSE;
    if (obj->racer->playerIndex >= PLAYER_ONE &&
        obj->racer->playerIndex <= PLAYER_FOUR) {
        return sIdentityPredicate != NULL &&
               sIdentityPredicate(obj->racer->playerIndex) == MOD_RACER_TERRY;
    }
    slot = terry_slot_for_owner(obj);
    return obj->racer->raceFinished && slot != NULL && slot->composed;
}

static s32 terry_is_flying(const Object *owner) {
    return owner != NULL && owner->racer != NULL &&
           owner->racer->vehicleIDPrev == VEHICLE_PLANE;
}

static void terry_clear_slot(TerryVisualSlot *slot, s32 freeActor) {
    Object *actor;
    if (slot == NULL)
        return;
    actor = slot->actor;
    memset(slot, 0, sizeof(*slot));
    if (freeActor && actor != NULL)
        free_object(actor);
}

static void terry_sync(TerryVisualSlot *slot, s32 updateRate) {
    Object *actor = slot->actor;
    Object *owner = slot->owner;
    Object_Racer *racer = owner->racer;
    s32 flying = terry_is_flying(owner);
    s16 previousAnimFrame;
    actor->trans = owner->trans;
    actor->trans.scale = bonus_visual_companion_scale(
        actor, owner, flying ? TERRY_FLIGHT_SCALE : TERRY_VEHICLE_SCALE);
    actor->trans.x_position += racer->carBobX;
    actor->trans.y_position += racer->carBobY;
    actor->trans.z_position += racer->carBobZ;
    actor->trans.rotation.y_rotation += racer->y_rotation_offset;
    actor->trans.rotation.x_rotation += racer->x_rotation_offset;
    actor->trans.rotation.z_rotation += racer->z_rotation_offset;
    if (!flying) {
        bonus_visual_apply_local_offset(
            actor, 0.0f,
            racer->vehicleIDPrev == VEHICLE_HOVERCRAFT ? TERRY_HOVER_SEAT_Y
                                                       : TERRY_VEHICLE_SEAT_Y,
            0.0f);
    }
    actor->segmentID = owner->segmentID;
    actor->opacity = owner->opacity;
    bonus_visual_scale_shadow(actor);
    /* The authored walk cycle draws the legs and wings inward around the body,
     * which reads as a compact perched rider from the chase camera. The idle
     * bind pose holds the full boss wingspan straight through the vehicle. */
    actor->animationID = flying ? TERRY_ANIM_FLY : TERRY_ANIM_WALK;
    previousAnimFrame = actor->animFrame;
    actor->animFrame += (s16)(updateRate * (flying ? 2 : 1));
    obj_clamp_model_animation(actor);
    if (flying) {
        slot->flightTicks += (u32)updateRate;
    } else {
        slot->flightTicks = 0;
        slot->lastFlapTick = 0;
        slot->flapCount = 0;
    }
    /* This is the exact one-shot used by the retail Terry/Smokey controller,
     * on the same fly-cycle threshold.  Crossing detection is intentionally
     * tolerant of multi-tick catch-up so a slow present cannot drop the flap. */
    /* obj_clamp_model_animation() resets animFrame to exactly 0 on wrap (object_models.c), never to
     * a partial remainder. The previous catch-up arm asked for `animFrame >= CUE` after a wrap,
     * which 0 can never satisfy -- so it was dead code and the miss it was written to prevent could
     * still happen: a tick large enough to step from below the cue straight past the end of the
     * cycle skipped the flap entirely. The live test is instead "did we wrap while still below the
     * cue", which is precisely the case where the cue frame was passed inside that step. */
    if (flying &&
        ((previousAnimFrame < TERRY_FLAP_CUE_FRAME &&
          actor->animFrame >= TERRY_FLAP_CUE_FRAME) ||
         (actor->animFrame < previousAnimFrame &&
          previousAnimFrame < TERRY_FLAP_CUE_FRAME))) {
        audspat_play_sound_at_position(
            SOUND_UNK_223, actor->trans.x_position, actor->trans.y_position,
            actor->trans.z_position, AUDIO_POINT_FLAG_ONE_TIME_TRIGGER, NULL);
        if (terry_audio_trace_enabled()) {
            fprintf(stderr,
                    "[TERRYAUDIO] event=flap racer=%d animation=%d "
                    "previous=%d frame=%d sound=%d tick=%u interval=%u "
                    "count=%u\n",
                    racer->racerIndex, actor->animationID, previousAnimFrame,
                    actor->animFrame, SOUND_UNK_223, slot->flightTicks,
                    slot->flapCount == 0
                        ? 0
                        : slot->flightTicks - slot->lastFlapTick,
                    slot->flapCount + 1);
        }
        slot->lastFlapTick = slot->flightTicks;
        slot->flapCount++;
    }
}

void terry_visual_set_identity_predicate(
    TerryVisualIdentityPredicate predicate) {
    sIdentityPredicate = predicate;
    if (predicate == NULL)
        terry_visual_reset();
}

void terry_visual_tick(s32 updateRate) {
    Object **racers;
    s32 racerCount;
    s32 i;
    for (i = 0; i < TERRY_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner != NULL && !terry_owner_eligible(sSlots[i].owner)) {
            terry_clear_slot(&sSlots[i], TRUE);
        }
    }
    racers = get_racer_objects(&racerCount);
    for (i = 0; racers != NULL && i < racerCount; i++) {
        Object *owner = racers[i];
        TerryVisualSlot *slot;
        Object *actor;
        if (!terry_owner_eligible(owner))
            continue;
        slot = terry_slot_allocate(owner);
        if (slot == NULL)
            continue;
        if (slot->composed && slot->actor != NULL) {
            terry_sync(slot, updateRate);
            continue;
        }
        if (slot->attempts >= TERRY_VISUAL_RETRY_LIMIT)
            continue;
        if (slot->retryTimer > 0) {
            slot->retryTimer -= updateRate;
            continue;
        }
        actor = terry_spawn(TERRY_LEASE_RACE, ASSET_OBJECT_ID_TERRYBOSS, owner);
        if (actor == NULL || !terry_actor_schema_ready(actor) ||
            !terry_donor_schema_ready(owner) || !isfinite(actor->trans.scale) ||
            actor->trans.scale <= 0.0f) {
            MDKR_TRACE("terry_visual: compose_retry player=%d vehicle=%d "
                       "actor=%d actor_schema=%d donor_schema=%d attempt=%d",
                       owner->racer->playerIndex, owner->racer->vehicleIDPrev,
                       actor != NULL, terry_actor_schema_ready(actor),
                       terry_donor_schema_ready(owner), slot->attempts + 1);
            if (slot->attempts == 0 && owner->header != NULL &&
                owner->modelInstances != NULL) {
                s32 *ids = DKR_PTR(s32, owner->header->modelIds);
                s32 modelIndex;
                for (modelIndex = 0;
                     modelIndex < owner->header->numberOfModelIds &&
                     modelIndex < 6;
                     modelIndex++) {
                    ObjectModel *model =
                        owner->modelInstances[modelIndex] != NULL
                            ? owner->modelInstances[modelIndex]->objModel
                            : NULL;
                    MDKR_TRACE("terry_visual: donor_model slot=%d id=%d v=%d "
                               "t=%d b=%d",
                               modelIndex, ids[modelIndex],
                               model != NULL ? model->numberOfVertices : -1,
                               model != NULL ? model->numberOfTriangles : -1,
                               model != NULL ? model->numberOfBatches : -1);
                }
            }
            if (actor != NULL)
                free_object(actor);
            slot->attempts++;
            slot->retryTimer = TERRY_VISUAL_RETRY_TICKS;
            continue;
        }
        slot->actor = actor;
        slot->composed = TRUE;
        MDKR_TRACE("terry_visual: composed player=%d vehicle=%d scale=%.3f",
                   owner->racer->playerIndex, owner->racer->vehicleIDPrev,
                   bonus_visual_companion_scale(actor, owner,
                                                terry_is_flying(owner)
                                                    ? TERRY_FLIGHT_SCALE
                                                    : TERRY_VEHICLE_SCALE));
        terry_sync(slot, updateRate);
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
        actor = terry_spawn(TERRY_LEASE_SELECT, ASSET_OBJECT_ID_TERRYBOSS,
                            sSelect.source);
        sign = actor != NULL
                   ? terry_spawn(TERRY_LEASE_SIGN, ASSET_OBJECT_ID_DIDDYSELECT,
                                 sSelect.source)
                   : NULL;
        if (actor == NULL || sign == NULL || !terry_actor_schema_ready(actor) ||
            !terry_sign_schema_ready(sign)) {
            if (actor != NULL)
                free_object(actor);
            if (sign != NULL)
                free_object(sign);
            sSelect.attempts++;
            if (sSelect.attempts >= TERRY_VISUAL_RETRY_LIMIT) {
                sSelect.unavailable = TRUE;
            } else {
                sSelect.retryTimer = TERRY_VISUAL_RETRY_TICKS;
            }
            return;
        }
        sSelect.actor = actor;
        sSelect.sign = sign;
        MDKR_TRACE("mod_select_visual: identity=TERRY actor=1 sign=1");
        if (sign->shadow != NULL)
            sign->shadow->scale = 0.0f;
    }
    if (sSelect.actor != NULL && sSelect.sign != NULL) {
        f32 x;
        Object *actor = sSelect.actor;
        Object *sign = sSelect.sign;
        if (!taj_select_layout_position(
                &sSelect.layout, sSelect.layout.terryIndex, NULL, NULL, &x))
            return;
        actor->trans = sSelect.source->trans;
        actor->trans.x_position = x;
        actor->trans.y_position += TERRY_SELECT_Y_OFFSET;
        actor->trans.rotation.x_rotation = 0;
        actor->trans.rotation.y_rotation = TERRY_SELECT_YAW;
        actor->trans.rotation.z_rotation = 0;
        actor->trans.scale =
            TERRY_SELECT_SCALE *
            taj_select_layout_scale(&sSelect.layout, sSelect.layout.terryIndex);
        bonus_visual_scale_shadow(actor);
        actor->segmentID = sSelect.source->segmentID;
        actor->opacity = sSelect.source->opacity;
        actor->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
        {
            s32 animation;
            if (sSelect.confirmTimer > 0) {
                animation = TERRY_ANIM_FLY;
                sSelect.confirmTimer -= updateRate;
                if (sSelect.confirmTimer < 0)
                    sSelect.confirmTimer = 0;
            } else if (sSelect.hoverMask != 0) {
                animation = TERRY_ANIM_IDLE;
            } else {
                animation = TERRY_ANIM_WALK;
            }
            if (actor->animationID != animation) {
                actor->animationID = animation;
                actor->animFrame = 0;
                MDKR_TRACE("mod_select_pose_state: identity=TERRY animation=%d "
                           "hover=0x%X confirmed=0x%X timer=%d",
                           animation, sSelect.hoverMask,
                           sSelect.confirmedMask, sSelect.confirmTimer);
            }
        }
        actor->animFrame += (s16)updateRate;
        obj_clamp_model_animation(actor);
        if (!sSelectPoseTraced) {
            ObjectModel *model =
                terry_model_find(actor, ASSET_OBJECTMODEL_TERRYBOSS);
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
            MDKR_TRACE("mod_select_pose: identity=TERRY x=%.2f y=%.2f z=%.2f "
                       "scale=%.3f flags=0x%x model=%d minY=%d maxY=%d",
                       actor->trans.x_position, actor->trans.y_position,
                       actor->trans.z_position, actor->trans.scale,
                       actor->trans.flags, model != NULL, minY, maxY);
            sSelectPoseTraced = TRUE;
        }
        sign->trans = actor->trans;
        /* Terry's head and chest occupy a much shorter screen silhouette than
         * the humanoid picker actors.  Lower only his presentation placard so
         * it reads as held in front of him instead of replacing his face. */
        sign->trans.y_position += TERRY_SELECT_SIGN_Y_OFFSET;
        sign->segmentID = actor->segmentID;
        sign->opacity = actor->opacity;
        sign->animationID = 0;
        if (sSelect.hoverMask != 0)
            sign->trans.flags &= ~OBJ_FLAGS_INVISIBLE;
        else
            sign->trans.flags |= OBJ_FLAGS_INVISIBLE;
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

void terry_visual_select_begin(const TajSelectLayout *layout) {
    terry_visual_select_end();
    if (layout == NULL || layout->terryIndex < 0)
        return;
    sSelect.layout = *layout;
    sSelect.active = TRUE;
}

void terry_visual_select_set_state(u32 hoverMask, u32 confirmedMask) {
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
        sSelect.confirmTimer = TERRY_SELECT_CONFIRM_TICKS;
    }
    sSelect.previousConfirmedMask = confirmedMask;
}

void terry_visual_select_apply_authored_actor(Object *obj, s32 characterIndex) {
    TajSelectRow row;
    TajSelectRow terryRow;
    f32 x;
    if (!sSelect.active || obj == NULL ||
        !taj_select_layout_position(&sSelect.layout, characterIndex, &row, NULL,
                                    &x) ||
        !taj_select_layout_position(&sSelect.layout, sSelect.layout.terryIndex,
                                    &terryRow, NULL, NULL))
        return;
    /* Taj owns the shared retail actor transform whenever it is present; if it
     * is absent, Wizpig owns it when present. */
    if (sSelect.layout.tajIndex < 0 && sSelect.layout.wizpigIndex < 0) {
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
    if (sSelect.source == NULL && row == terryRow)
        sSelect.source = obj;
}

void terry_visual_select_end(void) {
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

TajSelectVisualStatus terry_visual_select_status(void) {
    if (!sSelect.active)
        return TAJ_SELECT_VISUAL_INACTIVE;
    if (sSelect.unavailable)
        return TAJ_SELECT_VISUAL_UNAVAILABLE;
    if (sSelect.actor != NULL && sSelect.sign != NULL) {
        return TAJ_SELECT_VISUAL_READY;
    }
    return TAJ_SELECT_VISUAL_LOADING;
}

s32 terry_visual_select_sign_object(const Object *obj) {
    return obj != NULL && obj == sSelect.sign;
}

s32 terry_visual_select_sign_player(const Object *obj) {
    s32 player;
    s32 selected = 0;
    if (!terry_visual_select_sign_object(obj) || sSelect.hoverMask == 0 ||
        terry_visual_select_status() != TAJ_SELECT_VISUAL_READY)
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

s32 terry_visual_select_sign_batch(const Object *obj, s32 batchIndex) {
    return terry_visual_select_sign_object(obj) && batchIndex == 0;
}

s32 terry_visual_is_presentation_object(const Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_object(obj);
    return obj != NULL && (obj == sSelect.actor || obj == sSelect.sign ||
                           (slot != NULL && slot->actor == obj));
}

s32 terry_visual_uses_boss_head_matrix(const Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_object(obj);
    return obj != NULL &&
           (obj == sSelect.actor ||
            (slot != NULL && slot->composed && slot->actor == obj));
}

s32 terry_visual_multiplayer_shadow_object(const Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_object(obj);
    return slot != NULL && slot->composed && slot->actor == obj &&
           terry_is_flying(slot->owner);
}

s32 terry_visual_suppress_donor_draw(const Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_owner(obj);
    return slot != NULL && slot->composed && slot->actor != NULL &&
           terry_owner_eligible(obj) && terry_is_flying(obj);
}

s32 terry_visual_batch_visible(const ObjectModel *model, const Object *obj,
                               s32 batchIndex) {
    TerryVisualSlot *slot;
    s32 i;
    if (model == NULL || obj == NULL || batchIndex < 0 ||
        batchIndex >= model->numberOfBatches)
        return TRUE;
    slot = terry_slot_for_object(obj);
    if (slot == NULL || !slot->composed)
        return TRUE;
    if (obj == slot->owner) {
        if (terry_is_flying(obj))
            return FALSE;
        for (i = 0; i < obj->header->numberOfModelIds; i++) {
            if (obj->modelInstances[i] != NULL &&
                obj->modelInstances[i]->objModel == model) {
                if (!terry_donor_model_schema_ready(obj, i, model)) {
                    return TRUE;
                }
                return !bonus_visual_krunch_driver_batch(
                    obj->racer->vehicleIDPrev, i, batchIndex);
            }
        }
    }
    return TRUE;
}

s32 terry_visual_cap_donor_lod(const Object *obj, s32 modelIndex) {
    TerryVisualSlot *slot = terry_slot_for_owner(obj);
    if (slot != NULL && slot->composed && !terry_is_flying(obj) &&
        modelIndex > 4)
        return 4;
    return modelIndex;
}

s32 terry_visual_suppress_shadow(const Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_object(obj);
    if (slot == NULL || !slot->composed)
        return FALSE;
    if (obj == slot->actor)
        return !terry_is_flying(slot->owner);
    if (obj == slot->owner)
        return terry_is_flying(slot->owner);
    return FALSE;
}

void terry_visual_on_object_free(Object *obj) {
    TerryVisualSlot *slot = terry_slot_for_object(obj);
    s32 i;

    /* Borrowed retail character-select actors; see wizpig_visual_on_object_free() for why these
     * must be dropped here rather than left to select_end(). Mirrors taj_visual_on_object_free(). */
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
        terry_clear_slot(slot, TRUE);
    } else if (obj == slot->actor) {
        slot->actor = NULL;
        slot->composed = FALSE;
        slot->attempts++;
        slot->retryTimer = TERRY_VISUAL_RETRY_TICKS;
    }
}

void terry_visual_on_object_destroy(Object *obj) {
    terry_visual_on_object_free(obj);
}

void terry_visual_reset(void) {
    s32 i;
    terry_visual_select_end();
    for (i = 0; i < TERRY_VISUAL_MAX_RACERS; i++) {
        if (sSlots[i].owner != NULL)
            terry_clear_slot(&sSlots[i], TRUE);
    }
    sLease = TERRY_LEASE_NONE;
}

#endif
