/*
 * sim_hash.c — authoritative-state hash.
 *
 * One 64-bit FNV-1a hash per authoritative tick over a VERSIONED field
 * stream. This is the anchor instrument for every later fidelity gate:
 * render purity, the presentation-rate matrix and catch-up equivalence
 * are all "the [SIMHASH] stream is identical" assertions.
 *
 * SELECTING A VERSION. MDKR_STATE_HASH picks the field set:
 *
 *   unset / "0"  off (zero cost beyond one cached getenv)
 *   "1"          v1 — the original field set, byte-for-byte
 *   "2"          v2 — archived object/particle integrator field set
 *   "2x"         legacy v2 render-owned diagnostic
 *   "3"          v3 — the current authority and render-purity gate
 *
 * Any other non-empty value is v1, so every log and script written before
 * v2 existed keeps its exact meaning and a cross-version A/B against an
 * archived stream stays possible. The version number is folded into the
 * hash as its first input, so two versions can never produce a colliding
 * stream that would let an A/B silently compare unlike things.
 *
 * ---------------------------------------------------------------------
 * v1 (SIM_HASH_VERSION_V1) — retained verbatim
 * ---------------------------------------------------------------------
 *   - the gameplay RNG seed (the most divergence-sensitive scalar);
 *   - the live object count; and
 *   - per-object: behaviour id, position, Y ROTATION ONLY, and scale,
 *     read bitwise so float identity is exact.
 *
 * v1's y-rotation-only coverage is a measured blind spot, not a
 * theoretical one. On level 37 a line particle's x_rotation and
 * z_rotation diverged between two runs of the same binary at tick 3378 and
 * v1 could not see it until the drift had accumulated through
 * angularVelocity into y_rotation at tick 3381 — three ticks downstream of
 * the actual event. The [HASHOBJW] diagnostic row reads those fields by
 * hand; v2 is that row promoted into the hash.
 *
 * ---------------------------------------------------------------------
 * v2 (SIM_HASH_VERSION_V2) — a strict superset of v1
 * ---------------------------------------------------------------------
 * Every byte v1 hashes, v2 hashes too, so v2 detects everything v1
 * detects. Global inputs are unchanged: version, RNG seed, object count.
 *
 * The object list is MIXED: particles live in gObjPtrList alongside
 * objects and are flagged OBJ_FLAGS_PARTICLE (objects.c:2964). Object and
 * Particle share ObjectTransform and, on LP64 as on the N64, are both
 * pointer-free through offset 0x3C, so their first 0x3C bytes are the
 * same layout with different names. v2 hashes that shared prefix for
 * every entry and then branches, so each entry is read as what it
 * actually is:
 *
 *   shared prefix (Object name / Particle name)
 *     trans.rotation.x_rotation                        NEW in v2
 *     trans.rotation.y_rotation                        (v1)
 *     trans.rotation.z_rotation                        NEW in v2
 *     trans.flags                                      NEW in v2
 *     trans.scale                                      (v1)
 *     trans.x_position / y_position / z_position       (v1)
 *     animFrame        / textureFrame       (0x18)     NEW in v2
 *     numActiveEmitters/ textureFrameStep   (0x1A)     NEW in v2
 *     x_velocity       / velocity.x         (0x1C)     NEW in v2
 *     y_velocity       / velocity.y         (0x20)     NEW in v2
 *     z_velocity       / velocity.z         (0x24)     NEW in v2
 *     unk28            / scaleVelocity      (0x28)     NEW in v2
 *     headerType       / kind               (0x2C)     NEW in v2
 *     segmentID        / segmentID          (0x2E)     NEW in v2
 *
 *   non-particle Object only
 *     behaviorId                                       (v1)
 *     objectID                                         NEW in v2
 *     animationID                                      NEW in v2
 *
 *   Particle only
 *     movementType, destroyTimer, descFlags            NEW in v2
 *     localPos.x / .y / .z                             NEW in v2
 *     opacity, opacityVel, opacityTimer                NEW in v2
 *     angularVelocity.x / .y / .z                      NEW in v2
 *     the gravity / line-phase union word              NEW in v2
 *
 * v1 hashed `object->behaviorId` for particles too, which on LP64 lands
 * on Particle::unk_48 — a byte v2 no longer reads under that name. It is
 * still a superset in the sense that matters: v2 covers every particle
 * field that byte could have witnessed a change in, and many more, so no
 * divergence v1 can see is invisible to v2. Verified by running both
 * versions over the same sweep.
 *
 * ---------------------------------------------------------------------
 * Historical v2 exclusions
 * ---------------------------------------------------------------------
 * Excluded by design and unchanged from v1: pointers, allocation
 * addresses, renderer caches, display lists, GPU handles, audio queue
 * fill, wall-clock values, presentation snapshots. Particle::parentObj,
 * ::model/::sprite and the ::lineEmitter arm of the movementParam union
 * are all host pointers on LP64 and are excluded on exactly that ground —
 * hashing them would make the stream a function of the memory map. This
 * is not hypothetical: the level-37 divergence above was recycled pointer
 * bytes reaching an authoritative field.
 *
 * v2 excluded these because they were render-owned at the time:
 *
 *   Object::distanceToCamera  written by sort_objects_by_dist
 *                             (objects.c:6332/6347/6349/6352/6358)
 *   Object::opacity           written by render_3d_billboard
 *                             (objects.c:4726/4728) and
 *                             check_if_in_draw_range (tracks.c:2748…)
 *   Object::modelIndex        LOD, written by set_temp_model_transforms
 *                             (objects.c:5562)
 *
 * Excluded because they are PRESENTATION-only — stored in a simulation
 * struct, but read by nothing except the display-list builder:
 *
 *   Particle::colour      feeds gDPSetEnvColor / vertex colour
 *   Particle::brightness  feeds gDPSetPrimColor (particles.c:2598/2637/
 *                         2654); the ONLY writes are the three
 *                         constructors, the ONLY reads are those three
 *                         gDPSetPrimColor calls plus two `!= 255` guards
 *
 * brightness is worth its own paragraph, because it is not merely
 * unhashed — it is hashed-and-then-removed, and the reason is measured.
 * It is seeded at construction from `obj->shading->unk0 * 255.0f`, and
 * shading->unk0 is written on the RENDER path: shadow_update()
 * (tracks.c:5065 and the :4918 accumulator) is called from render_scene
 * (tracks.c:461). So a particle born on a tick whose render was skipped
 * captures a different brightness than one born on a rendered tick. With
 * brightness in the set, the render-purity skip-odd arm went red on level
 * 5 at tick 3410, four particles at once (list indices 123-126, kinds 3
 * and 128), br=199 against br=255 — a real render→state coupling,
 * correctly detected. It is left as a known gap rather than fixed here
 * because the field it corrupts is one nothing in the simulation reads:
 * the consequence is a particle drawn at the wrong brightness, which is a
 * presentation-fidelity bug in the same family as `colour`, not an
 * authoritative-state bug. Putting it in the authoritative hash would
 * assert that render lighting IS authoritative state, which is false.
 *
 * ---------------------------------------------------------------------
 * v3 (SIM_HASH_VERSION_V3) — current gate
 * ---------------------------------------------------------------------
 * v3 retains v2 and adds, field-by-field with no host pointer bytes:
 *
 *   - game mode, level/load/race timers, pause/countdown and save flags;
 *   - Settings progression, racer records, course/flap time records,
 *     time-trial state, course flags and world balloon counts;
 *   - stable object-list index/presence, distanceToCamera, unk34/unk38,
 *     opacity, modelIndex, particle-emitter enable state and interactions;
 *   - scalar members of the behavior-selected ObjProperties arm;
 *   - Object_Racer gameplay fields (including physics, route/lap, inventory,
 *     timers, AI/controller state and animation controls); and
 *   - scalar animation state for every ModelInstance of real 3D-model objects,
 *     including dormant LODs that can become authoritative later.
 *
 * The former render-owned trio is now valid authority: distance/order and
 * racer LOD are committed once by the fixed tick, while per-viewport distance,
 * opacity and LOD are draw-local overrides. Model animation cadence is likewise
 * committed in the fixed-step epilogue. Raw normal-vs-skip render schedules are
 * byte-identical under v3; no test-only state subtraction exists.
 *
 * Deliberate v3 exclusions are pointers/addresses, display lists and renderer
 * caches, GPU/audio queue/wall-clock state, presentation snapshots, particle
 * colour/brightness, shading, and Object_Racer::lightFlags. lightFlags is the
 * brake/headlight texture state machine; its NIGHT bit samples render-computed
 * shading and no physics, AI, progression or input path consumes it.
 *
 * `tests/check_state_hash.py` independently perturbs every v3 family for one
 * sample and proves exact restoration. `tests/check_render_purity.py` compares
 * raw schedules and uses an explicit injected leak as its positive control.
 */
#include <ultra64.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "structs.h"
#include "camera.h"
#include "game.h"
#include "object_behaviors.h"
#include "particles.h"
#include "textures_sprites.h"
#include "thread3_main.h"

#define SIM_HASH_VERSION_V1 1u
#define SIM_HASH_VERSION_V2 2u
#define SIM_HASH_VERSION_V3 3u
/* "v2 + render-owned", a legacy diagnostic. Keep it outside the public
 * version sequence so MDKR_STATE_HASH=3 can name the real v3 field set. */
#define SIM_HASH_VERSION_V2X 0x80000002u

extern s32 get_rng_seed(void);
extern s32 get_race_countdown(void);
extern Object **objGetObjList(s32 *first, s32 *count);
extern s16 gLevelLoadTimer;
extern s32 gRaceStartTimer;
extern s16 gRaceEndTimer;
extern Camera gCameras[8];
extern s32 gTTCamPlayerID;
extern s32 gTTCamID;
extern s32 gTTCamSmoothTimer;
extern s32 gTTCamSpectateIndex[10];

static uint64_t fnv1a64(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

#define SIM_HASH_FIELD(h, obj, member) \
    ((h) = fnv1a64((h), &(obj)->member, sizeof((obj)->member)))

/* 0 = off, otherwise SIM_HASH_VERSION_*. Parsed once. */
static uint32_t sim_hash_version(void) {
    static uint32_t version = 0xffffffffu;
    if (version == 0xffffffffu) {
        const char *value = getenv("MDKR_STATE_HASH");
        if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0) {
            version = 0u;
        } else if (strcmp(value, "2") == 0) {
            version = SIM_HASH_VERSION_V2;
        } else if (strcmp(value, "2x") == 0) {
            version = SIM_HASH_VERSION_V2X;
        } else if (strcmp(value, "3") == 0) {
            version = SIM_HASH_VERSION_V3;
        } else {
            /* Every value that meant "on" before v2 existed still means
             * v1, so archived streams stay comparable. */
            version = SIM_HASH_VERSION_V1;
        }
    }
    return version;
}

static int sim_hash_enabled(void) {
    return sim_hash_version() != 0u;
}

/* v1, unchanged. Kept as its own function rather than as branches inside
 * the v2 walk so that "v1 still means exactly what it meant" is verifiable
 * by reading, not by tracing conditionals. */
static uint64_t sim_hash_compute_v1(uint64_t hash, Object **objects,
                                    s32 count) {
    for (s32 index = 0; index < count; index++) {
        const Object *object = objects[index];
        if (object == NULL) {
            continue;
        }
        SIM_HASH_FIELD(hash, object, behaviorId);
        SIM_HASH_FIELD(hash, object, trans.x_position);
        SIM_HASH_FIELD(hash, object, trans.y_position);
        SIM_HASH_FIELD(hash, object, trans.z_position);
        SIM_HASH_FIELD(hash, object, trans.rotation.y_rotation);
        SIM_HASH_FIELD(hash, object, trans.scale);
    }
    return hash;
}

static uint64_t sim_hash_compute_v2(uint64_t hash, Object **objects,
                                    s32 count, int with_render_owned) {
    for (s32 index = 0; index < count; index++) {
        const Object *object = objects[index];
        if (object == NULL) {
            continue;
        }
        /* Shared prefix. Object and Particle are both pointer-free through
         * 0x3C, so these members name the same bytes in both. */
        SIM_HASH_FIELD(hash, object, trans.rotation.x_rotation);
        SIM_HASH_FIELD(hash, object, trans.rotation.y_rotation);
        SIM_HASH_FIELD(hash, object, trans.rotation.z_rotation);
        SIM_HASH_FIELD(hash, object, trans.flags);
        SIM_HASH_FIELD(hash, object, trans.scale);
        SIM_HASH_FIELD(hash, object, trans.x_position);
        SIM_HASH_FIELD(hash, object, trans.y_position);
        SIM_HASH_FIELD(hash, object, trans.z_position);
        SIM_HASH_FIELD(hash, object, animFrame);
        SIM_HASH_FIELD(hash, object, numActiveEmitters);
        SIM_HASH_FIELD(hash, object, x_velocity);
        SIM_HASH_FIELD(hash, object, y_velocity);
        SIM_HASH_FIELD(hash, object, z_velocity);
        SIM_HASH_FIELD(hash, object, unk28);
        SIM_HASH_FIELD(hash, object, headerType);
        SIM_HASH_FIELD(hash, object, segmentID);

        if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
            /* Past 0x3C the two layouts part company on LP64 (Object's
             * next member is a pointer, Particle's alignment differs), so
             * everything below MUST be read through the right type. */
            const Particle *particle = (const Particle *)object;
            SIM_HASH_FIELD(hash, particle, movementType);
            SIM_HASH_FIELD(hash, particle, destroyTimer);
            SIM_HASH_FIELD(hash, particle, descFlags);
            SIM_HASH_FIELD(hash, particle, localPos.x);
            SIM_HASH_FIELD(hash, particle, localPos.y);
            SIM_HASH_FIELD(hash, particle, localPos.z);
            SIM_HASH_FIELD(hash, particle, opacity);
            SIM_HASH_FIELD(hash, particle, opacityVel);
            SIM_HASH_FIELD(hash, particle, opacityTimer);
            /* The pair 2a4f281 found uninitialised. rotation above is the
             * accumulator; this is what drives it. */
            SIM_HASH_FIELD(hash, particle, angularVelocity.x_rotation);
            SIM_HASH_FIELD(hash, particle, angularVelocity.y_rotation);
            SIM_HASH_FIELD(hash, particle, angularVelocity.z_rotation);
            SIM_HASH_FIELD(hash, particle, gravity);
            if (with_render_owned) {
                SIM_HASH_FIELD(hash, particle, brightness);
                SIM_HASH_FIELD(hash, particle, colour.word);
            }
        } else {
            SIM_HASH_FIELD(hash, object, behaviorId);
            SIM_HASH_FIELD(hash, object, objectID);
            SIM_HASH_FIELD(hash, object, animationID);
            if (with_render_owned) {
                SIM_HASH_FIELD(hash, object, distanceToCamera);
                SIM_HASH_FIELD(hash, object, opacity);
                SIM_HASH_FIELD(hash, object, modelIndex);
            }
        }
    }
    return hash;
}

/* Hash the scalar part of the behavior-selected ObjProperties arm. Pointer
 * members are deliberately skipped; scalar siblings remain covered. Unknown
 * and pointer-only arms contribute their behavior id through the object walk
 * but no host-address bytes. */
static uint64_t sim_hash_properties_v3(uint64_t hash, const Object *object) {
    switch (object->behaviorId) {
        case BHV_FOG_CHANGER:
        case BHV_WEATHER:
            SIM_HASH_FIELD(hash, object, properties.distance.radius);
            SIM_HASH_FIELD(hash, object, properties.distance.unk4);
            break;
        case BHV_TORCH_MIST:
            SIM_HASH_FIELD(hash, object, properties.torchMist.speed);
            break;
        case BHV_BANANA:
            SIM_HASH_FIELD(hash, object, properties.banana.status);
            SIM_HASH_FIELD(hash, object, properties.banana.intangibleTimer);
            SIM_HASH_FIELD(hash, object, properties.banana.destroyTimer);
            break;
        case BHV_LEVEL_NAME:
            SIM_HASH_FIELD(hash, object, properties.levelName.radius);
            SIM_HASH_FIELD(hash, object, properties.levelName.levelID);
            SIM_HASH_FIELD(hash, object, properties.levelName.opacity);
            break;
        case BHV_WEAPON_2:
            SIM_HASH_FIELD(hash, object, properties.projectile.timer);
            SIM_HASH_FIELD(hash, object, properties.projectile.unk4);
            break;
        case BHV_BUOY_PIRATE_SHIP:
        case BHV_LOG:
            SIM_HASH_FIELD(hash, object, properties.log.angleVel);
            SIM_HASH_FIELD(hash, object, properties.log.velocityY);
            break;
        case BHV_SCENERY:
            SIM_HASH_FIELD(hash, object, properties.scenery.hitTimer);
            SIM_HASH_FIELD(hash, object, properties.scenery.angleVel);
            break;
        case BHV_FIREBALL_OCTOWEAPON:
        case BHV_FIREBALL_OCTOWEAPON_2:
            SIM_HASH_FIELD(hash, object, properties.fireball.timer);
            break;
        case BHV_LASER_BOLT:
            SIM_HASH_FIELD(hash, object, properties.laserbolt.timer);
            break;
        case BHV_TROPHY_CABINET:
            SIM_HASH_FIELD(hash, object, properties.trophyCabinet.action);
            SIM_HASH_FIELD(hash, object, properties.trophyCabinet.trophy);
            break;
        case BHV_ZIPPER_GROUND:
            SIM_HASH_FIELD(hash, object, properties.zipper.radius);
            break;
        case BHV_CHARACTER_FLAG:
            SIM_HASH_FIELD(hash, object, properties.characterFlag.playerID);
            SIM_HASH_FIELD(hash, object, properties.characterFlag.characterID);
            break;
        case BHV_GOLDEN_BALLOON:
            SIM_HASH_FIELD(hash, object, properties.goldenBalloon.action);
            SIM_HASH_FIELD(hash, object, properties.goldenBalloon.timer);
            break;
        case BHV_SILVER_COIN:
        case BHV_SILVER_COIN_2:
            SIM_HASH_FIELD(hash, object, properties.silverCoin.action);
            SIM_HASH_FIELD(hash, object, properties.silverCoin.timer);
            break;
        case BHV_STOPWATCH_MAN:
            SIM_HASH_FIELD(hash, object, properties.tt.action);
            SIM_HASH_FIELD(hash, object, properties.tt.timer);
            break;
        case BHV_PARK_WARDEN:
        case BHV_PARK_WARDEN_2:
            SIM_HASH_FIELD(hash, object, properties.taj.action);
            SIM_HASH_FIELD(hash, object, properties.taj.timer);
            break;
        case BHV_LAVA_SPURT:
            SIM_HASH_FIELD(hash, object, properties.lavaSpurt.actionTimer);
            SIM_HASH_FIELD(hash, object, properties.lavaSpurt.delayTimer);
            break;
        case BHV_POS_ARROW:
            SIM_HASH_FIELD(hash, object, properties.posArrow.playerID);
            break;
        case BHV_ANIMATION:
            SIM_HASH_FIELD(hash, object, properties.animation.action);
            SIM_HASH_FIELD(hash, object, properties.animation.behaviourID);
            break;
        case BHV_WIZPIG_SHIP:
            SIM_HASH_FIELD(hash, object, properties.wizpigship.unk0);
            SIM_HASH_FIELD(hash, object, properties.wizpigship.timer);
            break;
        case BHV_DINO_WHALE:
        case BHV_ANIMATED_OBJECT:
        case BHV_CAMERA_ANIMATION:
        case BHV_CAR_ANIMATION:
        case BHV_CHARACTER_SELECT:
        case BHV_VEHICLE_ANIMATION:
        case BHV_HIT_TESTER:
        case BHV_HIT_TESTER_2:
        case BHV_ANIMATED_OBJECT_2:
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
            SIM_HASH_FIELD(hash, object, properties.animatedObj.unk0);
            SIM_HASH_FIELD(hash, object, properties.animatedObj.unk4);
            break;
        case BHV_INFO_POINT:
            SIM_HASH_FIELD(hash, object, properties.infoPoint.radius);
            SIM_HASH_FIELD(hash, object, properties.infoPoint.visible);
            break;
        case BHV_BOMB_EXPLOSION:
            SIM_HASH_FIELD(hash, object, properties.bombExplosion.timer);
            SIM_HASH_FIELD(hash, object, properties.bombExplosion.opacity);
            break;
        case BHV_TELEPORT:
            SIM_HASH_FIELD(hash, object, properties.lighthouse.active);
            break;
        case BHV_DOOR:
        case BHV_TT_DOOR:
            SIM_HASH_FIELD(hash, object, properties.door.closeAngle);
            SIM_HASH_FIELD(hash, object, properties.door.openAngle);
            break;
        case BHV_BRIDGE_WHALE_RAMP:
            SIM_HASH_FIELD(hash, object, properties.bridgeWhaleRamp.unk0);
            break;
        case BHV_RAMP_SWITCH:
            SIM_HASH_FIELD(hash, object, properties.rampSwitch.unk0);
            break;
        case BHV_SKY_CONTROL:
            SIM_HASH_FIELD(hash, object, properties.skyControl.setting);
            SIM_HASH_FIELD(hash, object, properties.skyControl.radius);
            break;
        case BHV_TREASURE_SUCKER:
            SIM_HASH_FIELD(hash, object, properties.treasureSucker.playerID);
            SIM_HASH_FIELD(hash, object, properties.treasureSucker.spawnTimer);
            break;
        case BHV_FLY_COIN:
            SIM_HASH_FIELD(hash, object, properties.flyCoin.diff);
            break;
        case BHV_BANANA_SPAWNER:
            SIM_HASH_FIELD(hash, object, properties.bananaSpawner.timer);
            SIM_HASH_FIELD(hash, object, properties.bananaSpawner.spawn);
            break;
        case BHV_WORLD_KEY:
            SIM_HASH_FIELD(hash, object, properties.worldKey.keyID);
            break;
        case BHV_WEAPON_BALLOON:
            SIM_HASH_FIELD(hash, object, properties.weaponBalloon.balloonID);
            SIM_HASH_FIELD(hash, object, properties.weaponBalloon.particleTimer);
            break;
        case BHV_SETUP_POINT:
            SIM_HASH_FIELD(hash, object, properties.setupPoint.racerIndex);
            SIM_HASH_FIELD(hash, object, properties.setupPoint.entranceID);
            break;
        case BHV_WEAPON:
            SIM_HASH_FIELD(hash, object, properties.weapon.decayTimer);
            SIM_HASH_FIELD(hash, object, properties.weapon.status);
            SIM_HASH_FIELD(hash, object, properties.weapon.submerged);
            SIM_HASH_FIELD(hash, object, properties.weapon.scale);
            break;
        case BHV_CAMERA_CONTROL:
            SIM_HASH_FIELD(hash, object, properties.camControl.cameraID);
            break;
        case BHV_TIMETRIAL_GHOST:
            SIM_HASH_FIELD(hash, object, properties.timeTrial.timestamp);
            break;
        case BHV_BUBBLER:
            SIM_HASH_FIELD(hash, object, properties.bubbler.unk0);
            break;
        case BHV_BOOST:
            SIM_HASH_FIELD(hash, object, properties.boost.indexes);
            break;
        default:
            break;
    }
    return hash;
}

static uint64_t sim_hash_racer_v3(uint64_t hash, const Object_Racer *racer) {
    /* Pointer and SoundHandle members are excluded field-by-field. Everything
     * below is a scalar/array that can influence a later racer tick. */
    SIM_HASH_FIELD(hash, racer, playerIndex);
    SIM_HASH_FIELD(hash, racer, racerIndex);
    SIM_HASH_FIELD(hash, racer, characterId);
    SIM_HASH_FIELD(hash, racer, unk4);
    SIM_HASH_FIELD(hash, racer, forwardVel);
    SIM_HASH_FIELD(hash, racer, animationSpeed);
    SIM_HASH_FIELD(hash, racer, lastSoundID);
    SIM_HASH_FIELD(hash, racer, unk2A);
    SIM_HASH_FIELD(hash, racer, velocity);
    SIM_HASH_FIELD(hash, racer, lateral_velocity);
    SIM_HASH_FIELD(hash, racer, unk34);
    SIM_HASH_FIELD(hash, racer, ox1);
    SIM_HASH_FIELD(hash, racer, oy1);
    SIM_HASH_FIELD(hash, racer, oz1);
    SIM_HASH_FIELD(hash, racer, ox2);
    SIM_HASH_FIELD(hash, racer, oy2);
    SIM_HASH_FIELD(hash, racer, oz2);
    SIM_HASH_FIELD(hash, racer, ox3);
    SIM_HASH_FIELD(hash, racer, oy3);
    SIM_HASH_FIELD(hash, racer, oz3);
    SIM_HASH_FIELD(hash, racer, prev_x_position);
    SIM_HASH_FIELD(hash, racer, prev_y_position);
    SIM_HASH_FIELD(hash, racer, prev_z_position);
    SIM_HASH_FIELD(hash, racer, unk68);
    SIM_HASH_FIELD(hash, racer, unk6C);
    SIM_HASH_FIELD(hash, racer, unk70);
    SIM_HASH_FIELD(hash, racer, unk74);
    SIM_HASH_FIELD(hash, racer, carBobX);
    SIM_HASH_FIELD(hash, racer, carBobY);
    SIM_HASH_FIELD(hash, racer, carBobZ);
    SIM_HASH_FIELD(hash, racer, unk84);
    SIM_HASH_FIELD(hash, racer, unk88);
    SIM_HASH_FIELD(hash, racer, stretch_height);
    SIM_HASH_FIELD(hash, racer, stretch_height_cap);
    SIM_HASH_FIELD(hash, racer, camera_zoom);
    SIM_HASH_FIELD(hash, racer, unk98);
    SIM_HASH_FIELD(hash, racer, pitch);
    SIM_HASH_FIELD(hash, racer, roll);
    SIM_HASH_FIELD(hash, racer, yaw);
    SIM_HASH_FIELD(hash, racer, checkpoint_distance);
    SIM_HASH_FIELD(hash, racer, unkAC);
    SIM_HASH_FIELD(hash, racer, unkB0);
    SIM_HASH_FIELD(hash, racer, throttle);
    SIM_HASH_FIELD(hash, racer, brake);
    SIM_HASH_FIELD(hash, racer, unkBC);
    SIM_HASH_FIELD(hash, racer, buoyancy);
    SIM_HASH_FIELD(hash, racer, unkC4);
    SIM_HASH_FIELD(hash, racer, unkC8);
    SIM_HASH_FIELD(hash, racer, unkCC);
    SIM_HASH_FIELD(hash, racer, unkD0);
    SIM_HASH_FIELD(hash, racer, unkD4);
    SIM_HASH_FIELD(hash, racer, unkD8);
    SIM_HASH_FIELD(hash, racer, unk10C);
    SIM_HASH_FIELD(hash, racer, unk110);
    SIM_HASH_FIELD(hash, racer, unk114);
    SIM_HASH_FIELD(hash, racer, unk11C);
    SIM_HASH_FIELD(hash, racer, unk120);
    SIM_HASH_FIELD(hash, racer, unk124);
    SIM_HASH_FIELD(hash, racer, lap_times);
    SIM_HASH_FIELD(hash, racer, unk13C);
    SIM_HASH_FIELD(hash, racer, y_rotation_offset);
    SIM_HASH_FIELD(hash, racer, x_rotation_offset);
    SIM_HASH_FIELD(hash, racer, z_rotation_offset);
    SIM_HASH_FIELD(hash, racer, unk166);
    SIM_HASH_FIELD(hash, racer, unk168);
    SIM_HASH_FIELD(hash, racer, headAngle);
    SIM_HASH_FIELD(hash, racer, headAngleTarget);
    SIM_HASH_FIELD(hash, racer, unk16E);
    SIM_HASH_FIELD(hash, racer, unk170);
    SIM_HASH_FIELD(hash, racer, balloon_type);
    SIM_HASH_FIELD(hash, racer, balloon_quantity);
    SIM_HASH_FIELD(hash, racer, balloon_level);
    SIM_HASH_FIELD(hash, racer, magnetTimer);
    SIM_HASH_FIELD(hash, racer, unk176);
    SIM_HASH_FIELD(hash, racer, magnetModelID);
    SIM_HASH_FIELD(hash, racer, bananas);
    SIM_HASH_FIELD(hash, racer, unk186);
    SIM_HASH_FIELD(hash, racer, attackType);
    SIM_HASH_FIELD(hash, racer, unk188);
    SIM_HASH_FIELD(hash, racer, shieldType);
    SIM_HASH_FIELD(hash, racer, unk18A);
    SIM_HASH_FIELD(hash, racer, unk18C);
    SIM_HASH_FIELD(hash, racer, shieldTimer);
    SIM_HASH_FIELD(hash, racer, courseCheckpoint);
    SIM_HASH_FIELD(hash, racer, nextCheckpoint);
    SIM_HASH_FIELD(hash, racer, lap);
    SIM_HASH_FIELD(hash, racer, countLap);
    SIM_HASH_FIELD(hash, racer, magnetLevel3);
    SIM_HASH_FIELD(hash, racer, cameraYaw);
    SIM_HASH_FIELD(hash, racer, unk198);
    SIM_HASH_FIELD(hash, racer, unk19A);
    SIM_HASH_FIELD(hash, racer, unk19C);
    SIM_HASH_FIELD(hash, racer, unk19E);
    SIM_HASH_FIELD(hash, racer, steerVisualRotation);
    SIM_HASH_FIELD(hash, racer, y_rotation_vel);
    SIM_HASH_FIELD(hash, racer, x_rotation_vel);
    SIM_HASH_FIELD(hash, racer, z_rotation_vel);
    SIM_HASH_FIELD(hash, racer, unk1A8);
    SIM_HASH_FIELD(hash, racer, racerOrder);
    SIM_HASH_FIELD(hash, racer, finishPosition);
    SIM_HASH_FIELD(hash, racer, racePosition);
    SIM_HASH_FIELD(hash, racer, unk1B0);
    SIM_HASH_FIELD(hash, racer, unk1B2);
    SIM_HASH_FIELD(hash, racer, unk1B4);
    SIM_HASH_FIELD(hash, racer, unk1B8);
    SIM_HASH_FIELD(hash, racer, unk1BA);
    SIM_HASH_FIELD(hash, racer, unk1BC);
    SIM_HASH_FIELD(hash, racer, unk1BE);
    SIM_HASH_FIELD(hash, racer, unk1C0);
    SIM_HASH_FIELD(hash, racer, unk1C2);
    SIM_HASH_FIELD(hash, racer, unk1C4);
    SIM_HASH_FIELD(hash, racer, unk1C6);
    SIM_HASH_FIELD(hash, racer, isOnAlternateRoute);
    SIM_HASH_FIELD(hash, racer, unk1C9);
    SIM_HASH_FIELD(hash, racer, unk1CA);
    SIM_HASH_FIELD(hash, racer, unk1CB);
    SIM_HASH_FIELD(hash, racer, aiSkill);
    SIM_HASH_FIELD(hash, racer, unk1CD);
    SIM_HASH_FIELD(hash, racer, unk1CE);
    SIM_HASH_FIELD(hash, racer, eggHudCounter);
    SIM_HASH_FIELD(hash, racer, spectateCamID);
    SIM_HASH_FIELD(hash, racer, unk1D1);
    SIM_HASH_FIELD(hash, racer, unk1D2);
    SIM_HASH_FIELD(hash, racer, boostTimer);
    SIM_HASH_FIELD(hash, racer, unk1D4);
    SIM_HASH_FIELD(hash, racer, unk1D5);
    SIM_HASH_FIELD(hash, racer, vehicleID);
    SIM_HASH_FIELD(hash, racer, vehicleIDPrev);
    SIM_HASH_FIELD(hash, racer, raceFinished);
    SIM_HASH_FIELD(hash, racer, unk1D9);
    SIM_HASH_FIELD(hash, racer, unk1DA);
    SIM_HASH_FIELD(hash, racer, spinout_timer);
    SIM_HASH_FIELD(hash, racer, wheel_surfaces);
    SIM_HASH_FIELD(hash, racer, trickType);
    SIM_HASH_FIELD(hash, racer, steerAngle);
    SIM_HASH_FIELD(hash, racer, groundedWheels);
    SIM_HASH_FIELD(hash, racer, unk1E3);
    SIM_HASH_FIELD(hash, racer, unk1E4);
    SIM_HASH_FIELD(hash, racer, waterTimer);
    SIM_HASH_FIELD(hash, racer, drift_direction);
    SIM_HASH_FIELD(hash, racer, miscAnimCounter);
    SIM_HASH_FIELD(hash, racer, unk1E8);
    SIM_HASH_FIELD(hash, racer, unk1E9);
    SIM_HASH_FIELD(hash, racer, unk1EA);
    SIM_HASH_FIELD(hash, racer, tapTimerR);
    SIM_HASH_FIELD(hash, racer, tappedR);
    SIM_HASH_FIELD(hash, racer, squish_timer);
    SIM_HASH_FIELD(hash, racer, unk1EE);
    SIM_HASH_FIELD(hash, racer, boost_sound);
    SIM_HASH_FIELD(hash, racer, unk1F0);
    SIM_HASH_FIELD(hash, racer, unk1F1);
    SIM_HASH_FIELD(hash, racer, unk1F2);
    SIM_HASH_FIELD(hash, racer, unk1F3);
    SIM_HASH_FIELD(hash, racer, startInput);
    SIM_HASH_FIELD(hash, racer, zipperDirCorrection);
    SIM_HASH_FIELD(hash, racer, unk1F6);
    SIM_HASH_FIELD(hash, racer, transparency);
    SIM_HASH_FIELD(hash, racer, indicator_type);
    SIM_HASH_FIELD(hash, racer, indicator_timer);
    SIM_HASH_FIELD(hash, racer, drifting);
    SIM_HASH_FIELD(hash, racer, unk1FB);
    SIM_HASH_FIELD(hash, racer, wrongWayCounter);
    SIM_HASH_FIELD(hash, racer, cameraIndex);
    SIM_HASH_FIELD(hash, racer, unk1FE);
    SIM_HASH_FIELD(hash, racer, unk1FF);
    SIM_HASH_FIELD(hash, racer, transitionTimer);
    SIM_HASH_FIELD(hash, racer, unk201);
    SIM_HASH_FIELD(hash, racer, silverCoinCount);
    SIM_HASH_FIELD(hash, racer, boostType);
    SIM_HASH_FIELD(hash, racer, bubbleTrapTimer);
    SIM_HASH_FIELD(hash, racer, unk206);
    SIM_HASH_FIELD(hash, racer, unk208);
    SIM_HASH_FIELD(hash, racer, unk209);
    /* lightFlags is the brake/headlight texture state machine. Its only
     * consumers are racer.c's light timer and objects.c's texture-offset
     * selection; the NIGHT bit samples render-computed shading. It is
     * presentation state stored in Object_Racer, not gameplay authority. */
    SIM_HASH_FIELD(hash, racer, unk20B);
    SIM_HASH_FIELD(hash, racer, throttleReleased);
    SIM_HASH_FIELD(hash, racer, unk20D);
    SIM_HASH_FIELD(hash, racer, delaySoundID);
    SIM_HASH_FIELD(hash, racer, delaySoundTimer);
    SIM_HASH_FIELD(hash, racer, unk211);
    SIM_HASH_FIELD(hash, racer, elevation);
    SIM_HASH_FIELD(hash, racer, unk213);
    SIM_HASH_FIELD(hash, racer, unk214);
    SIM_HASH_FIELD(hash, racer, unk215);
    SIM_HASH_FIELD(hash, racer, unk216);
    SIM_HASH_FIELD(hash, racer, unk217);
    return hash;
}

static uint64_t sim_hash_globals_v3(uint64_t hash) {
    Settings *settings = get_settings();
    s32 levelCount = 0;
    s32 worldCount = 0;
    s32 gameMode = get_game_mode();
    s8 paused = is_game_paused();
    s32 level = level_id();
    s32 countdown = get_race_countdown();

    hash = fnv1a64(hash, &gameMode, sizeof(gameMode));
    hash = fnv1a64(hash, &paused, sizeof(paused));
    hash = fnv1a64(hash, &level, sizeof(level));
    hash = fnv1a64(hash, &gLevelLoadTimer, sizeof(gLevelLoadTimer));
    hash = fnv1a64(hash, &gRaceStartTimer, sizeof(gRaceStartTimer));
    hash = fnv1a64(hash, &gRaceEndTimer, sizeof(gRaceEndTimer));
    hash = fnv1a64(hash, &countdown, sizeof(countdown));
    hash = fnv1a64(hash, &gSaveDataFlags, sizeof(gSaveDataFlags));
    /* Three-player TT camera state is authoritative on native: its pose and
     * target feed next-tick object sort/LOD/visibility. Hash the exact camera
     * slot rendered as PLAYER_FOUR, plus its cross-tick target/smoothing state,
     * so a render-elision leak cannot hide outside the object graph. */
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], trans);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], cam_unk_18);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], boomLength);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], cam_unk_20);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], x_velocity);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], y_velocity);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], z_velocity);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], shakeMagnitude);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], cameraSegmentID);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], mode);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], pitch);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], shakeTimer);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], zoom);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], unk3C);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], unk3D);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], unk3E);
    SIM_HASH_FIELD(hash, &gCameras[PLAYER_FOUR], unk3F);
    hash = fnv1a64(hash, &gTTCamPlayerID, sizeof(gTTCamPlayerID));
    hash = fnv1a64(hash, &gTTCamID, sizeof(gTTCamID));
    hash = fnv1a64(hash, &gTTCamSmoothTimer, sizeof(gTTCamSmoothTimer));
    hash = fnv1a64(hash, gTTCamSpectateIndex,
                   sizeof(gTTCamSpectateIndex));
    if (settings == NULL) {
        return hash;
    }

    SIM_HASH_FIELD(hash, settings, keys);
    SIM_HASH_FIELD(hash, settings, unkA);
    SIM_HASH_FIELD(hash, settings, bosses);
    SIM_HASH_FIELD(hash, settings, trophies);
    SIM_HASH_FIELD(hash, settings, cutsceneFlags);
    SIM_HASH_FIELD(hash, settings, tajFlags);
    SIM_HASH_FIELD(hash, settings, ttAmulet);
    SIM_HASH_FIELD(hash, settings, wizpigAmulet);
    SIM_HASH_FIELD(hash, settings, worldId);
    SIM_HASH_FIELD(hash, settings, courseId);
    SIM_HASH_FIELD(hash, settings, gNumRacers);
    SIM_HASH_FIELD(hash, settings, newGame);
    SIM_HASH_FIELD(hash, settings, filename);
    SIM_HASH_FIELD(hash, settings, racers);
    SIM_HASH_FIELD(hash, settings, timeTrialRacer);
    SIM_HASH_FIELD(hash, settings, unk115);
    SIM_HASH_FIELD(hash, settings, display_times);

    level_count(&levelCount, &worldCount);
    hash = fnv1a64(hash, &levelCount, sizeof(levelCount));
    hash = fnv1a64(hash, &worldCount, sizeof(worldCount));
    if (settings->courseFlagsPtr != NULL && levelCount > 0) {
        hash = fnv1a64(hash, settings->courseFlagsPtr,
                       (size_t)levelCount * sizeof(*settings->courseFlagsPtr));
    }
    if (settings->balloonsPtr != NULL && worldCount > 0) {
        hash = fnv1a64(hash, settings->balloonsPtr,
                       (size_t)worldCount * sizeof(*settings->balloonsPtr));
    }
    if (levelCount > 0) {
        for (s32 vehicle = 0; vehicle < 3; vehicle++) {
            if (settings->flapInitialsPtr[vehicle] != NULL) {
                hash = fnv1a64(
                    hash, settings->flapInitialsPtr[vehicle],
                    (size_t)levelCount *
                        sizeof(*settings->flapInitialsPtr[vehicle]));
            }
            if (settings->flapTimesPtr[vehicle] != NULL) {
                hash = fnv1a64(
                    hash, settings->flapTimesPtr[vehicle],
                    (size_t)levelCount *
                        sizeof(*settings->flapTimesPtr[vehicle]));
            }
            if (settings->courseInitialsPtr[vehicle] != NULL) {
                hash = fnv1a64(
                    hash, settings->courseInitialsPtr[vehicle],
                    (size_t)levelCount *
                        sizeof(*settings->courseInitialsPtr[vehicle]));
            }
            if (settings->courseTimesPtr[vehicle] != NULL) {
                hash = fnv1a64(
                    hash, settings->courseTimesPtr[vehicle],
                    (size_t)levelCount *
                        sizeof(*settings->courseTimesPtr[vehicle]));
            }
        }
    }
    return hash;
}

static int sim_hash_object_has_models_v3(const Object *object) {
    return object->header != NULL &&
           object->header->modelType == OBJECT_MODEL_TYPE_3D_MODEL &&
           object->modelInstances != NULL &&
           object->header->numberOfModelIds > 0;
}

static uint64_t sim_hash_models_v3(uint64_t hash, const Object *object) {
    if (!sim_hash_object_has_models_v3(object)) {
        return hash;
    }
    for (s32 index = 0; index < object->header->numberOfModelIds; index++) {
        const ModelInstance *instance = object->modelInstances[index];
        uint8_t present = instance != NULL;
        hash = fnv1a64(hash, &index, sizeof(index));
        hash = fnv1a64(hash, &present, sizeof(present));
        if (instance == NULL) {
            continue;
        }
        SIM_HASH_FIELD(hash, instance, animationID);
        SIM_HASH_FIELD(hash, instance, animationFrame);
        SIM_HASH_FIELD(hash, instance, animationFrameCount);
        SIM_HASH_FIELD(hash, instance, offsetX);
        SIM_HASH_FIELD(hash, instance, offsetY);
        SIM_HASH_FIELD(hash, instance, offsetZ);
        SIM_HASH_FIELD(hash, instance, headTilt);
        SIM_HASH_FIELD(hash, instance, modelType);
        SIM_HASH_FIELD(hash, instance, animationTaskNum);
        SIM_HASH_FIELD(hash, instance, animUpdateTimer);
    }
    return hash;
}

static uint64_t sim_hash_compute_v3(uint64_t hash, Object **objects,
                                    s32 count) {
    hash = sim_hash_globals_v3(hash);
    for (s32 index = 0; index < count; index++) {
        const Object *object = objects[index];
        uint8_t present = object != NULL;
        hash = fnv1a64(hash, &index, sizeof(index));
        hash = fnv1a64(hash, &present, sizeof(present));
        if (object == NULL) {
            continue;
        }

        /* v3 is a strict field superset of v2. */
        hash = sim_hash_compute_v2(hash, (Object **)&objects[index], 1, 0);
        if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
            continue;
        }
        SIM_HASH_FIELD(hash, object, distanceToCamera);
        SIM_HASH_FIELD(hash, object, unk34);
        SIM_HASH_FIELD(hash, object, unk38);
        SIM_HASH_FIELD(hash, object, opacity);
        SIM_HASH_FIELD(hash, object, modelIndex);
        SIM_HASH_FIELD(hash, object, particleEmittersEnabled);
        if (object->interactObj != NULL) {
            SIM_HASH_FIELD(hash, object->interactObj, x_position);
            SIM_HASH_FIELD(hash, object->interactObj, y_position);
            SIM_HASH_FIELD(hash, object->interactObj, z_position);
            SIM_HASH_FIELD(hash, object->interactObj, hitboxRadius);
            SIM_HASH_FIELD(hash, object->interactObj, unk11);
            SIM_HASH_FIELD(hash, object->interactObj, pushForce);
            SIM_HASH_FIELD(hash, object->interactObj, distance);
            SIM_HASH_FIELD(hash, object->interactObj, flags);
            SIM_HASH_FIELD(hash, object->interactObj, unk16);
            SIM_HASH_FIELD(hash, object->interactObj, unk17);
        }
        hash = sim_hash_properties_v3(hash, object);
        if (object->behaviorId == BHV_RACER && object->racer != NULL) {
            hash = sim_hash_racer_v3(hash, object->racer);
        }
        hash = sim_hash_models_v3(hash, object);
    }
    return hash;
}

/* Independent v3 family digests for diagnostics. These are not the published
 * [SIMHASH] byte stream; they let the render census name which family moved
 * without weakening or reordering that stream. */
typedef struct SimHashV3Parts {
    uint64_t globals;
    uint64_t core;
    uint64_t object_extra;
    uint64_t interaction;
    uint64_t property;
    uint64_t racer;
    uint64_t model;
    uint64_t object_distance;
    uint64_t object_opacity;
    uint64_t object_model_index;
    uint64_t racer_light_flags;
    uint64_t model_anim_timer;
    uint64_t racer_head_angle;
} SimHashV3Parts;

static SimHashV3Parts sim_hash_v3_parts(void) {
    const uint64_t basis = 14695981039346656037ull;
    SimHashV3Parts parts = {
        basis, basis, basis, basis, basis, basis, basis,
        basis, basis, basis, basis, basis, basis
    };
    s32 first = 0;
    s32 count = 0;
    Object **objects = objGetObjList(&first, &count);

    parts.globals = sim_hash_globals_v3(parts.globals);
    for (s32 index = 0; objects != NULL && index < count; index++) {
        const Object *object = objects[index];
        if (object == NULL) {
            continue;
        }
        parts.core = sim_hash_compute_v2(
            parts.core, &objects[index], 1, 0);
        if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
            continue;
        }
        SIM_HASH_FIELD(parts.object_extra, object, distanceToCamera);
        SIM_HASH_FIELD(parts.object_extra, object, unk34);
        SIM_HASH_FIELD(parts.object_extra, object, unk38);
        SIM_HASH_FIELD(parts.object_extra, object, opacity);
        SIM_HASH_FIELD(parts.object_extra, object, modelIndex);
        SIM_HASH_FIELD(parts.object_extra, object, particleEmittersEnabled);
        SIM_HASH_FIELD(parts.object_distance, object, distanceToCamera);
        SIM_HASH_FIELD(parts.object_opacity, object, opacity);
        SIM_HASH_FIELD(parts.object_model_index, object, modelIndex);
        if (object->interactObj != NULL) {
            SIM_HASH_FIELD(parts.interaction, object->interactObj, x_position);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, y_position);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, z_position);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, hitboxRadius);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, unk11);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, pushForce);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, distance);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, flags);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, unk16);
            SIM_HASH_FIELD(parts.interaction, object->interactObj, unk17);
        }
        parts.property = sim_hash_properties_v3(parts.property, object);
        if (object->behaviorId == BHV_RACER && object->racer != NULL) {
            parts.racer = sim_hash_racer_v3(parts.racer, object->racer);
            SIM_HASH_FIELD(parts.racer_light_flags, object->racer, lightFlags);
            SIM_HASH_FIELD(parts.racer_head_angle, object->racer, headAngle);
        }
        parts.model = sim_hash_models_v3(parts.model, object);
        if (sim_hash_object_has_models_v3(object)) {
            for (s32 model = 0;
                 model < object->header->numberOfModelIds; model++) {
                const ModelInstance *instance = object->modelInstances[model];
                if (instance != NULL) {
                    SIM_HASH_FIELD(parts.model_anim_timer, instance,
                                   animUpdateTimer);
                }
            }
        }
    }
    return parts;
}

static uint64_t sim_hash_compute(s32 *out_count) {
    uint64_t hash = 14695981039346656037ull;
    uint32_t version = sim_hash_version();
    s32 first = 0;
    s32 count = 0;
    s32 rng;
    Object **objects;

    hash = fnv1a64(hash, &version, sizeof(version));
    rng = get_rng_seed();
    hash = fnv1a64(hash, &rng, sizeof(rng));
    objects = objGetObjList(&first, &count);
    hash = fnv1a64(hash, &count, sizeof(count));
    if (objects != NULL) {
        if (version == SIM_HASH_VERSION_V1) {
            hash = sim_hash_compute_v1(hash, objects, count);
        } else if (version == SIM_HASH_VERSION_V3) {
            hash = sim_hash_compute_v3(hash, objects, count);
        } else {
            hash = sim_hash_compute_v2(
                hash, objects, count,
                version == SIM_HASH_VERSION_V2X);
        }
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    return hash;
}

/* Diagnostic: MDKR_HASH_DUMP_TICK=N dumps one row per object at tick N so a
 * skip-render A/B can name the exact leaking object. Test-only. */
static long long hash_dump_tick(void) {
    static long long value = -2;
    if (value == -2) {
        const char *env = getenv("MDKR_HASH_DUMP_TICK");
        value = env != NULL ? atoll(env) : -1;
    }
    return value;
}

/* MDKR_HASH_DUMP_UNTIL=M extends MDKR_HASH_DUMP_TICK=N to the closed range
 * [N, M], so a field's history across the divergence can be read directly
 * instead of one tick at a time. Unset == the single tick N. */
static long long hash_dump_until(void) {
    static long long value = -2;
    if (value == -2) {
        const char *env = getenv("MDKR_HASH_DUMP_UNTIL");
        value = env != NULL ? atoll(env) : -1;
    }
    return value;
}

static int hash_dump_selected(unsigned long long tick) {
    long long first = hash_dump_tick();
    long long last = hash_dump_until();

    if (first < 0) {
        return 0;
    }
    if (last < first) {
        last = first;
    }
    return (long long)tick >= first && (long long)tick <= last;
}

/* Diagnostic companion to MDKR_HASH_DUMP_TICK: MDKR_HASH_DUMP_IDS=1 adds one
 * [HASHOBJID] row per object naming it (objectID, behaviour, host address, and
 * the racer slot when it has one), so a divergent [HASHOBJ] row can be tied to
 * a concrete actor. Kept separate from [HASHOBJ] because the host address moves
 * with the mapping and would otherwise make every row differ. Test-only. */
static int hash_dump_ids(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_HASH_DUMP_IDS");
        enabled = value != NULL && value[0] != '\0' &&
                  strcmp(value, "0") != 0;
    }
    return enabled;
}

static void sim_hash_dump_object_ids(unsigned long long tick) {
    s32 first = 0;
    s32 count = 0;
    Object **objects = objGetObjList(&first, &count);

    if (objects == NULL) {
        return;
    }
    for (s32 index = 0; index < count; index++) {
        const Object *object = objects[index];
        const Object_Racer *racer;
        if (object == NULL) {
            continue;
        }
        /* A Particle overlays an Object and shares only ObjectTransform, so the
         * Object fields below are other members reinterpreted. Print the real
         * ones instead when the transform says this slot is a particle. */
        if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
            const Particle *particle = (const Particle *)object;
            printf("[HASHOBJID] tick=%llu i=%d obj=%p PARTICLE kind=%d "
                   "move=%d seg=%d destroyTimer=%d descFlags=0x%08x "
                   "parent=%p opacity=%d\n",
                   tick, (int)index, (const void *)object,
                   (int)particle->kind, (int)particle->movementType,
                   (int)particle->segmentID, (int)particle->destroyTimer,
                   (unsigned)particle->descFlags,
                   (const void *)particle->parentObj,
                   (int)particle->opacity);
            continue;
        }
        racer = object->behaviorId == BHV_RACER ? object->racer : NULL;
        printf("[HASHOBJID] tick=%llu i=%d obj=%p bhv=%d objectID=0x%04x "
               "hdrType=%d seg=%d anim=%d model=%d racer=%p player=%d "
               "vehicle=%d\n",
               tick, (int)index, (const void *)object,
               (int)object->behaviorId, (unsigned)object->objectID,
               (int)object->headerType, (int)object->segmentID,
               (int)object->animationID, (int)object->modelIndex,
               (const void *)racer,
               racer != NULL ? (int)racer->playerIndex : -1,
               racer != NULL ? (int)racer->vehicleID : -1);
    }
}

static void sim_hash_dump_objects(unsigned long long tick) {
    s32 first = 0;
    s32 count = 0;
    Object **objects = objGetObjList(&first, &count);
    if (objects == NULL) {
        return;
    }
    printf("[HASHOBJ] tick=%llu RNG=%08x\n", tick,
           (unsigned)get_rng_seed());
    for (s32 index = 0; index < count; index++) {
        const Object *object = objects[index];
        unsigned px, py, pz, sc;
        if (object == NULL) continue;
        memcpy(&px, &object->trans.x_position, 4);
        memcpy(&py, &object->trans.y_position, 4);
        memcpy(&pz, &object->trans.z_position, 4);
        memcpy(&sc, &object->trans.scale, 4);
        printf("[HASHOBJ] tick=%llu i=%d bhv=%d p=%08x,%08x,%08x r=%04x s=%08x\n",
               tick, (int)index, (int)object->behaviorId, px, py, pz,
               (unsigned)(object->trans.rotation.y_rotation & 0xffff), sc);
        /* Wider companion row (MDKR_HASH_DUMP_IDS=1): the fields v1 does NOT
         * cover. Under v1 the hash sees y_rotation only, so an x/z rotation or
         * a velocity that diverges first is invisible to it and the first
         * [SIMHASH] disagreement is already downstream of the real event.
         * Under v2 every field on this row IS hashed, so the row is no longer
         * the only way to see these values: it is how you read them once the
         * hash has told you which tick and which object to look at. */
        if (hash_dump_ids()) {
            unsigned vx, vy, vz;
            memcpy(&vx, &object->x_velocity, 4);
            memcpy(&vy, &object->y_velocity, 4);
            memcpy(&vz, &object->z_velocity, 4);
            printf("[HASHOBJW] tick=%llu i=%d rx=%04x rz=%04x flags=%04x "
                   "v=%08x,%08x,%08x af=%d seg=%d op=%u\n",
                   tick, (int)index,
                   (unsigned)(object->trans.rotation.x_rotation & 0xffff),
                   (unsigned)(object->trans.rotation.z_rotation & 0xffff),
                   (unsigned)(object->trans.flags & 0xffff), vx, vy, vz,
                   (int)object->animFrame, (int)object->segmentID,
                   (unsigned)object->opacity);
            /* [HASHOBJV2]: the rest of the v2 field set, so the dump covers
             * EXACTLY what the gate hashes. A field that is hashed but not
             * dumped can only be bisected for: on level 11, [SIMHASH] once
             * disagreed at tick 2729 while every printed row matched, because
             * the differing field (a line particle's localPos) was hashed and
             * not dumped. Hash set and dump set are kept in step deliberately. */
            {
                unsigned u28;
                memcpy(&u28, &object->unk28, 4);
                if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
                    const Particle *p = (const Particle *)object;
                    unsigned lx, ly, lz, gr;
                    memcpy(&lx, &p->localPos.x, 4);
                    memcpy(&ly, &p->localPos.y, 4);
                    memcpy(&lz, &p->localPos.z, 4);
                    memcpy(&gr, &p->gravity, 4);
                    printf("[HASHOBJV2] tick=%llu i=%d PARTICLE kind=%d "
                           "sv=%08x mv=%d dt=%d df=%08x lp=%08x,%08x,%08x "
                           "op=%d ov=%d ot=%d br=%d av=%04x,%04x,%04x "
                           "g=%08x tf=%d tfs=%d\n",
                           tick, (int)index, (int)p->kind, u28,
                           (int)p->movementType, (int)p->destroyTimer,
                           (unsigned)p->descFlags, lx, ly, lz,
                           (int)p->opacity, (int)p->opacityVel,
                           (int)p->opacityTimer, (int)p->brightness,
                           (unsigned)(p->angularVelocity.x_rotation & 0xffff),
                           (unsigned)(p->angularVelocity.y_rotation & 0xffff),
                           (unsigned)(p->angularVelocity.z_rotation & 0xffff),
                           gr, (int)p->textureFrame,
                           (int)p->textureFrameStep);
                } else {
                    printf("[HASHOBJV2] tick=%llu i=%d OBJECT unk28=%08x "
                           "hdr=%d objectID=0x%04x animID=%d nae=%d\n",
                           tick, (int)index, u28, (int)object->headerType,
                           (unsigned)object->objectID,
                           (int)object->animationID,
                           (int)object->numActiveEmitters);
                }
            }
        }
    }
    if (sim_hash_version() == SIM_HASH_VERSION_V3) {
        const uint64_t basis = 14695981039346656037ull;
        uint64_t globals = sim_hash_globals_v3(basis);
        printf("[HASHV3] tick=%llu globals=%016llx\n", tick,
               (unsigned long long)globals);
        for (s32 index = 0; index < count; index++) {
            const Object *object = objects[index];
            uint64_t core = basis;
            uint64_t extra = basis;
            uint64_t property = basis;
            uint64_t interaction = basis;
            uint64_t racer = basis;
            uint64_t model = basis;
            if (object == NULL) {
                continue;
            }
            core = sim_hash_compute_v2(core, &objects[index], 1, 0);
            if (!(object->trans.flags & OBJ_FLAGS_PARTICLE)) {
                SIM_HASH_FIELD(extra, object, distanceToCamera);
                SIM_HASH_FIELD(extra, object, unk34);
                SIM_HASH_FIELD(extra, object, unk38);
                SIM_HASH_FIELD(extra, object, opacity);
                SIM_HASH_FIELD(extra, object, modelIndex);
                SIM_HASH_FIELD(extra, object, particleEmittersEnabled);
                property = sim_hash_properties_v3(property, object);
                if (object->interactObj != NULL) {
                    SIM_HASH_FIELD(interaction, object->interactObj, x_position);
                    SIM_HASH_FIELD(interaction, object->interactObj, y_position);
                    SIM_HASH_FIELD(interaction, object->interactObj, z_position);
                    SIM_HASH_FIELD(interaction, object->interactObj, hitboxRadius);
                    SIM_HASH_FIELD(interaction, object->interactObj, unk11);
                    SIM_HASH_FIELD(interaction, object->interactObj, pushForce);
                    SIM_HASH_FIELD(interaction, object->interactObj, distance);
                    SIM_HASH_FIELD(interaction, object->interactObj, flags);
                    SIM_HASH_FIELD(interaction, object->interactObj, unk16);
                    SIM_HASH_FIELD(interaction, object->interactObj, unk17);
                }
                if (object->behaviorId == BHV_RACER &&
                    object->racer != NULL) {
                    racer = sim_hash_racer_v3(racer, object->racer);
                }
                model = sim_hash_models_v3(model, object);
                if (sim_hash_object_has_models_v3(object)) {
                    for (s32 modelIndex = 0;
                         modelIndex < object->header->numberOfModelIds;
                         modelIndex++) {
                        const ModelInstance *instance =
                            object->modelInstances[modelIndex];
                        if (instance == NULL) {
                            continue;
                        }
                        printf("[HASHMODEL] tick=%llu i=%d mi=%d aid=%d "
                               "af=%d afc=%d off=%d,%d,%d tilt=%d type=%d "
                               "task=%d timer=%d\n",
                               tick, (int)index, (int)modelIndex,
                               (int)instance->animationID,
                               (int)instance->animationFrame,
                               (int)instance->animationFrameCount,
                               (int)instance->offsetX, (int)instance->offsetY,
                               (int)instance->offsetZ,
                               (int)instance->headTilt,
                               (int)instance->modelType,
                               (int)instance->animationTaskNum,
                               (int)instance->animUpdateTimer);
                    }
                }
            }
            printf("[HASHV3] tick=%llu i=%d core=%016llx extra=%016llx "
                   "prop=%016llx interact=%016llx racer=%016llx "
                   "model=%016llx\n", tick, (int)index,
                   (unsigned long long)core, (unsigned long long)extra,
                   (unsigned long long)property,
                   (unsigned long long)interaction,
                   (unsigned long long)racer, (unsigned long long)model);
            if (object->behaviorId == BHV_RACER && object->racer != NULL) {
                const Object_Racer *r = object->racer;
                printf("[HASHRACER] tick=%llu i=%d head=%d target=%d "
                       "light=%u indicator=%d/%u visible=%d anim=%d "
                       "timer=%d transition=%d transparency=%u "
                       "misc=%d,%d,%d,%d\n", tick, (int)index,
                       (int)r->headAngle, (int)r->headAngleTarget,
                       (unsigned)r->lightFlags, (int)r->indicator_timer,
                       (unsigned)r->indicator_type, (int)r->unk201,
                       (int)object->animationID, (int)r->miscAnimCounter,
                       (int)r->transitionTimer, (unsigned)r->transparency,
                       (int)r->unk1F0, (int)r->unk1F1, (int)r->unk1F2,
                       (int)r->unk1F3);
            }
        }
    }
}

/* Test-only field-set controls. The historical numeric spelling still means
 * object:<tick>; v3 adds one independently selected target per field family:
 *
 *   MDKR_TEST_HASH_PERTURB=object:2000
 *   MDKR_TEST_HASH_PERTURB=racer:2000
 *   MDKR_TEST_HASH_PERTURB=global:2000
 *
 * One byte is flipped only while the hash is computed and is restored before
 * any game code can observe it. A control must therefore move exactly one
 * [SIMHASH] row. No movement means the advertised family is not covered;
 * later movement means restoration leaked into simulation state. */
typedef enum HashPerturbClass {
    HASH_PERTURB_NONE,
    HASH_PERTURB_OBJECT,
    HASH_PERTURB_PARTICLE,
    HASH_PERTURB_RACER,
    HASH_PERTURB_GLOBAL,
    HASH_PERTURB_SETTINGS,
    HASH_PERTURB_PROPERTY,
    HASH_PERTURB_INTERACTION,
    HASH_PERTURB_MODEL,
    HASH_PERTURB_RENDER_OWNED,
    HASH_PERTURB_CAMERA
} HashPerturbClass;

typedef struct HashPerturbSpec {
    HashPerturbClass class_id;
    long long tick;
} HashPerturbSpec;

static int hash_perturb_class_is(const char *name, size_t length,
                                 const char *expected) {
    return strlen(expected) == length &&
           strncmp(name, expected, length) == 0;
}

static HashPerturbSpec hash_perturb_spec(void) {
    static HashPerturbSpec spec = { HASH_PERTURB_NONE, -1 };
    static int parsed;
    const char *env;
    const char *colon;
    const char *tick_text;
    size_t class_length;
    char *end;

    if (parsed) {
        return spec;
    }
    parsed = 1;
    env = getenv("MDKR_TEST_HASH_PERTURB");
    if (env == NULL || env[0] == '\0') {
        return spec;
    }
    colon = strchr(env, ':');
    if (colon == NULL) {
        spec.class_id = HASH_PERTURB_OBJECT;
        tick_text = env;
    } else {
        class_length = (size_t)(colon - env);
        tick_text = colon + 1;
        if (hash_perturb_class_is(env, class_length, "object")) {
            spec.class_id = HASH_PERTURB_OBJECT;
        } else if (hash_perturb_class_is(env, class_length, "particle")) {
            spec.class_id = HASH_PERTURB_PARTICLE;
        } else if (hash_perturb_class_is(env, class_length, "racer")) {
            spec.class_id = HASH_PERTURB_RACER;
        } else if (hash_perturb_class_is(env, class_length, "global")) {
            spec.class_id = HASH_PERTURB_GLOBAL;
        } else if (hash_perturb_class_is(env, class_length, "settings")) {
            spec.class_id = HASH_PERTURB_SETTINGS;
        } else if (hash_perturb_class_is(env, class_length, "property")) {
            spec.class_id = HASH_PERTURB_PROPERTY;
        } else if (hash_perturb_class_is(env, class_length, "interaction")) {
            spec.class_id = HASH_PERTURB_INTERACTION;
        } else if (hash_perturb_class_is(env, class_length, "model")) {
            spec.class_id = HASH_PERTURB_MODEL;
        } else if (hash_perturb_class_is(env, class_length,
                                         "render-owned")) {
            spec.class_id = HASH_PERTURB_RENDER_OWNED;
        } else if (hash_perturb_class_is(env, class_length, "camera")) {
            spec.class_id = HASH_PERTURB_CAMERA;
        } else {
            return spec;
        }
    }
    spec.tick = strtoll(tick_text, &end, 10);
    if (end == tick_text || *end != '\0' || spec.tick < 0) {
        spec.class_id = HASH_PERTURB_NONE;
        spec.tick = -1;
    }
    return spec;
}

static const char *hash_perturb_class_name(HashPerturbClass class_id) {
    switch (class_id) {
        case HASH_PERTURB_OBJECT: return "object";
        case HASH_PERTURB_PARTICLE: return "particle";
        case HASH_PERTURB_RACER: return "racer";
        case HASH_PERTURB_GLOBAL: return "global";
        case HASH_PERTURB_SETTINGS: return "settings";
        case HASH_PERTURB_PROPERTY: return "property";
        case HASH_PERTURB_INTERACTION: return "interaction";
        case HASH_PERTURB_MODEL: return "model";
        case HASH_PERTURB_RENDER_OWNED: return "render-owned";
        case HASH_PERTURB_CAMERA: return "camera";
        default: return "none";
    }
}

static uint64_t sim_hash_compute_perturbed(HashPerturbClass class_id,
                                           s32 *out_count) {
    s32 first = 0;
    s32 count = 0;
    Object **objects = objGetObjList(&first, &count);
    uint64_t hash;
    void *target = NULL;

    if (class_id == HASH_PERTURB_GLOBAL) {
        target = &gLevelLoadTimer;
    } else if (class_id == HASH_PERTURB_CAMERA) {
        target = &gCameras[PLAYER_FOUR].trans.x_position;
    } else if (class_id == HASH_PERTURB_SETTINGS) {
        Settings *settings = get_settings();
        if (settings != NULL) {
            target = &settings->bosses;
        }
    } else {
        for (s32 index = 0; objects != NULL && index < count; index++) {
            Object *object = objects[index];
            if (object == NULL) {
                continue;
            }
            if (class_id == HASH_PERTURB_PARTICLE) {
                if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
                    Particle *particle = (Particle *)object;
                    target = &particle->angularVelocity.x_rotation;
                    break;
                }
                continue;
            }
            if (object->trans.flags & OBJ_FLAGS_PARTICLE) {
                continue;
            }
            switch (class_id) {
                case HASH_PERTURB_OBJECT:
                    target = &object->trans.rotation.x_rotation;
                    break;
                case HASH_PERTURB_RACER:
                    if (object->behaviorId == BHV_RACER &&
                        object->racer != NULL) {
                        target = &object->racer->velocity;
                    }
                    break;
                case HASH_PERTURB_PROPERTY:
                    if (object->behaviorId == BHV_BANANA) {
                        target = &object->properties.banana.status;
                    }
                    break;
                case HASH_PERTURB_INTERACTION:
                    if (object->interactObj != NULL) {
                        target = &object->interactObj->x_position;
                    }
                    break;
                case HASH_PERTURB_MODEL:
                    if (object->header != NULL &&
                        object->header->modelType ==
                            OBJECT_MODEL_TYPE_3D_MODEL &&
                        object->modelInstances != NULL &&
                        object->modelIndex >= 0 &&
                        object->modelIndex < object->header->numberOfModelIds &&
                        object->modelInstances[object->modelIndex] != NULL) {
                        target = &object->modelInstances[object->modelIndex]
                                      ->animationFrame;
                    }
                    break;
                case HASH_PERTURB_RENDER_OWNED:
                    target = &object->opacity;
                    break;
                default:
                    break;
            }
            if (target != NULL) {
                break;
            }
        }
    }
    if (target == NULL) {
        printf("[HASHCONTROL] class=%s applied=0\n",
               hash_perturb_class_name(class_id));
        return sim_hash_compute(out_count);
    }
    *(unsigned char *)target ^= 1u;
    hash = sim_hash_compute(out_count);
    *(unsigned char *)target ^= 1u;
    printf("[HASHCONTROL] class=%s applied=1\n",
           hash_perturb_class_name(class_id));
    return hash;
}

void mdkr_sim_hash_frame(void) {
    static unsigned long long tick;
    HashPerturbSpec perturb;
    s32 count = 0;
    uint64_t hash;

    if (!sim_hash_enabled()) {
        return;
    }
    if (hash_dump_selected(tick)) {
        sim_hash_dump_objects(tick);
        if (hash_dump_ids()) {
            sim_hash_dump_object_ids(tick);
        }
    }
    perturb = hash_perturb_spec();
    if (perturb.tick >= 0 && (long long)tick == perturb.tick) {
        hash = sim_hash_compute_perturbed(perturb.class_id, &count);
    } else {
        hash = sim_hash_compute(&count);
    }
    printf("[SIMHASH] tick=%llu objs=%d h=%016llx\n",
           tick, (int)count, (unsigned long long)hash);
    tick++;
}

/*
 * Render-mutation probe: hash the authoritative state immediately before and
 * after render_scene() and count ticks where the RENDER path changed it. It
 * measures impurity visible to whichever version MDKR_STATE_HASH selects; v3
 * is the release gate and its component hashes identify the mutated family.
 * Enabled by MDKR_RENDER_CENSUS=1; one summary row every 600 ticks and at each
 * change.
 */
static int render_census_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_RENDER_CENSUS");
        enabled = value != NULL && value[0] != '\0' &&
                  strcmp(value, "0") != 0;
    }
    return enabled;
}

static uint64_t s_census_pre_hash;
static SimHashV3Parts s_census_pre_parts;
static unsigned long long s_census_ticks;
static unsigned long long s_census_mutated;
static unsigned long long s_census_v3_globals;
static unsigned long long s_census_v3_core;
static unsigned long long s_census_v3_object_extra;
static unsigned long long s_census_v3_interaction;
static unsigned long long s_census_v3_property;
static unsigned long long s_census_v3_racer;
static unsigned long long s_census_v3_model;
static unsigned long long s_census_v3_distance;
static unsigned long long s_census_v3_opacity;
static unsigned long long s_census_v3_model_index;
static unsigned long long s_census_v3_racer_light;
static unsigned long long s_census_v3_model_timer;
static unsigned long long s_census_v3_racer_head;

/*
 * Render-purity gate seams. Env-gated, zero-cost when unset.
 *
 * MDKR_TEST_SKIP_RENDER=odd  — render_scene's body is skipped on odd
 *   authoritative ticks. With every migrated subsystem out of the render
 *   path, skipping half of all renders must not change one bit of the
 *   authoritative stream; the registered check asserts exactly that.
 * MDKR_TEST_RENDER_IMPURITY=1 — explicit positive control: inject one
 *   authoritative RNG write inside each non-skipped render. The raw purity
 *   arms must detect this; production has no subtraction/bracketing mode.
 */
static int skip_render_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *value = getenv("MDKR_TEST_SKIP_RENDER");
        mode = value != NULL && strcmp(value, "odd") == 0;
    }
    return mode;
}

static unsigned long long s_render_tick_parity;

int mdkr_test_render_skip_this_tick(void) {
    if (!skip_render_mode()) {
        return 0;
    }
    return (s_render_tick_parity & 1ull) != 0;
}

void mdkr_test_render_tick_advance(void) {
    s_render_tick_parity++;
}

extern void set_rng_seed(s32 seed);

void mdkr_test_render_impurity_inject(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_TEST_RENDER_IMPURITY");
        enabled = value != NULL && value[0] != '\0' &&
                  strcmp(value, "0") != 0;
    }
    if (enabled) {
        uint32_t seed = (uint32_t)get_rng_seed();
        set_rng_seed((s32)(seed * 1664525u + 1013904223u));
    }
}

void mdkr_render_census_pre(void) {
    if (!render_census_enabled()) {
        return;
    }
    s_census_pre_hash = sim_hash_compute(NULL);
    if (sim_hash_version() == SIM_HASH_VERSION_V3) {
        s_census_pre_parts = sim_hash_v3_parts();
    }
}

void mdkr_render_census_post(void) {
    uint64_t post;
    SimHashV3Parts post_parts;

    if (!render_census_enabled()) {
        return;
    }
    post = sim_hash_compute(NULL);
    s_census_ticks++;
    if (post != s_census_pre_hash) {
        s_census_mutated++;
    }
    if (sim_hash_version() == SIM_HASH_VERSION_V3) {
        post_parts = sim_hash_v3_parts();
        s_census_v3_globals +=
            post_parts.globals != s_census_pre_parts.globals;
        s_census_v3_core += post_parts.core != s_census_pre_parts.core;
        s_census_v3_object_extra +=
            post_parts.object_extra != s_census_pre_parts.object_extra;
        s_census_v3_interaction +=
            post_parts.interaction != s_census_pre_parts.interaction;
        s_census_v3_property +=
            post_parts.property != s_census_pre_parts.property;
        s_census_v3_racer += post_parts.racer != s_census_pre_parts.racer;
        s_census_v3_model += post_parts.model != s_census_pre_parts.model;
        s_census_v3_distance +=
            post_parts.object_distance != s_census_pre_parts.object_distance;
        s_census_v3_opacity +=
            post_parts.object_opacity != s_census_pre_parts.object_opacity;
        s_census_v3_model_index +=
            post_parts.object_model_index !=
                s_census_pre_parts.object_model_index;
        s_census_v3_racer_light +=
            post_parts.racer_light_flags !=
                s_census_pre_parts.racer_light_flags;
        s_census_v3_model_timer +=
            post_parts.model_anim_timer !=
                s_census_pre_parts.model_anim_timer;
        s_census_v3_racer_head +=
            post_parts.racer_head_angle !=
                s_census_pre_parts.racer_head_angle;
    }
    if ((s_census_ticks % 600ull) == 0) {
        printf("[RENDER-MUT] ticks=%llu mutated=%llu\n",
               s_census_ticks, s_census_mutated);
        if (sim_hash_version() == SIM_HASH_VERSION_V3) {
            printf("[RENDER-MUT-V3] globals=%llu core=%llu object=%llu "
                   "interaction=%llu property=%llu racer=%llu model=%llu\n",
                   s_census_v3_globals, s_census_v3_core,
                   s_census_v3_object_extra, s_census_v3_interaction,
                   s_census_v3_property, s_census_v3_racer,
                   s_census_v3_model);
            printf("[RENDER-MUT-FIELDS] distance=%llu opacity=%llu "
                   "modelIndex=%llu racerLight=%llu modelTimer=%llu "
                   "racerHead=%llu\n",
                   s_census_v3_distance, s_census_v3_opacity,
                   s_census_v3_model_index, s_census_v3_racer_light,
                   s_census_v3_model_timer, s_census_v3_racer_head);
        }
    }
}
