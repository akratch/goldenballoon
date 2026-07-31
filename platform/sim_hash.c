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
 *   "2"          v2 — the current gate field set (below)
 *   "2x"         v2 plus the three render-OWNED object fields, diagnostic
 *                only; never a gate. See "the deliberate v2 exclusions".
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
 * The deliberate v2 exclusions
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
 * Excluded because they are RENDER-OWNED today. Each is an open migration:
 * until it lands, hashing the field would make the stream a function of
 * whether and how often the scene was drawn.
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
 * None of these exclusions exist to make a gate pass, and none of them is
 * taken on trust. MDKR_STATE_HASH=2x hashes the render-owned AND the
 * presentation-only fields, so every claim above stays measurable and
 * re-measurable as each migration lands: 2x is what turns "render writes
 * these" into a number. Measured on level 5, 3600 ticks,
 * MDKR_TEST_PURE_RENDER=1 normal vs skip-odd:
 *
 *   MDKR_STATE_HASH=2    0 of 3600 ticks differ   (gate green)
 *   MDKR_STATE_HASH=2x   2020 of 3600 ticks differ, first at tick 89
 *
 * So the gap between this instrument's ceiling and true purity is exactly
 * the render-owned exclusion list above, and it is 2020 ticks wide. When
 * those three migrations land, fold the fields in and bump to v3.
 *
 * Also excluded, for cause rather than by omission: Object::unk34 and
 * Particle::unk34 (the latter is commented "set to zero but never used"),
 * Object::unk38, and ObjProperties (a union whose arms include pointers).
 * Object_Racer's interior is authoritative and NOT covered by any version
 * yet; it is the largest remaining hole and the obvious v3 candidate.
 */
#include <ultra64.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "structs.h"
#include "object_behaviors.h"
#include "particles.h"
#include "textures_sprites.h"

#define SIM_HASH_VERSION_V1 1u
#define SIM_HASH_VERSION_V2 2u
/* "v2 + render-owned", a diagnostic. Its own number so a 2x stream can
 * never be mistaken for a v2 one by an A/B that forgot which was which. */
#define SIM_HASH_VERSION_V2X 3u

extern s32 get_rng_seed(void);
extern Object **objGetObjList(s32 *first, s32 *count);

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
}

/*
 * MDKR_TEST_HASH_PERTURB=<tick> — the positive control that makes "v2 is
 * STRICTLY stronger than v1" an assertion instead of a claim.
 *
 * At exactly the named tick, flip the low bit of one live object's
 * x_rotation, hash, and put it straight back. x_rotation is the field v1
 * provably cannot see — it is the field that diverged on level 37 three
 * ticks before the v1 hash noticed. The flip is reverted before
 * this function returns and nothing runs in between, so the simulation is
 * bit-for-bit unaffected and the ONLY thing that can change is the hash
 * input. That gives a two-sided control the gate can check:
 *
 *   under v1  the perturbed stream is IDENTICAL to the unperturbed one
 *             (v1 does not read this field — the blind spot, demonstrated)
 *   under v2  it differs at the named tick and NOWHERE else
 *             (v2 reads it, and reverting it really did revert it)
 *
 * Test-only, and never used in production: it exists so the gate cannot
 * quietly degrade into hashing fewer fields than it advertises.
 */
static long long hash_perturb_tick(void) {
    static long long value = -2;
    if (value == -2) {
        const char *env = getenv("MDKR_TEST_HASH_PERTURB");
        value = env != NULL ? atoll(env) : -1;
    }
    return value;
}

static uint64_t sim_hash_compute_perturbed(s32 *out_count) {
    s32 first = 0;
    s32 count = 0;
    Object **objects = objGetObjList(&first, &count);
    uint64_t hash;
    Object *victim = NULL;

    for (s32 index = 0; objects != NULL && index < count; index++) {
        if (objects[index] != NULL) {
            victim = objects[index];
            break;
        }
    }
    if (victim == NULL) {
        return sim_hash_compute(out_count);
    }
    victim->trans.rotation.x_rotation ^= 1;
    hash = sim_hash_compute(out_count);
    victim->trans.rotation.x_rotation ^= 1;
    return hash;
}

void mdkr_sim_hash_frame(void) {
    static unsigned long long tick;
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
    if (hash_perturb_tick() >= 0 &&
        (long long)tick == hash_perturb_tick()) {
        hash = sim_hash_compute_perturbed(&count);
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
 * measures impurity visible to WHICHEVER version MDKR_STATE_HASH selects — so
 * the same probe under =1, =2 and =2x reads out three different ceilings, and
 * anything left above the highest of them is beyond this instrument. Enabled
 * by MDKR_RENDER_CENSUS=1; one summary row every 600 ticks and at each
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
static unsigned long long s_census_ticks;
static unsigned long long s_census_mutated;

/*
 * Render-purity gate seams. Env-gated, zero-cost when unset.
 *
 * MDKR_TEST_SKIP_RENDER=odd  — render_scene's body is skipped on odd
 *   authoritative ticks. With every migrated subsystem out of the render
 *   path, skipping half of all renders must not change one bit of the
 *   authoritative stream; the registered check asserts exactly that.
 * MDKR_TEST_PURE_RENDER=1    — test-only seed bracket around the single
 *   remaining render-path RNG site (obj_tex_animate). NEVER production:
 *   bracketing measurably reroutes racer AI. It exists so the purity
 *   assertion can subtract the one documented blocker, and so the gate's
 *   positive-control arm (bracket off => streams diverge) proves the
 *   instrument can see impurity at all.
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

/* Outer pure-render bracket with a PRIVATE slot (the game's
 * save_rng_seed/load_rng_seed pair shares one gPrevRNGSeed and the render
 * tree contains balanced inner pairs — nesting through the shared slot
 * would corrupt the outer restore). Wraps the entire render_scene call
 * under MDKR_TEST_PURE_RENDER=1: every render-side consumer is subsumed. */
extern s32 get_rng_seed(void);
extern void set_rng_seed(s32 seed);
static s32 s_pure_render_saved_seed;

void mdkr_pure_render_begin(void) {
    s_pure_render_saved_seed = get_rng_seed();
}

void mdkr_pure_render_end(void) {
    set_rng_seed(s_pure_render_saved_seed);
}

int mdkr_test_pure_render_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MDKR_TEST_PURE_RENDER");
        enabled = value != NULL && value[0] != '\0' &&
                  strcmp(value, "0") != 0;
    }
    return enabled;
}

void mdkr_render_census_pre(void) {
    if (!render_census_enabled()) {
        return;
    }
    s_census_pre_hash = sim_hash_compute(NULL);
}

void mdkr_render_census_post(void) {
    uint64_t post;

    if (!render_census_enabled()) {
        return;
    }
    post = sim_hash_compute(NULL);
    s_census_ticks++;
    if (post != s_census_pre_hash) {
        s_census_mutated++;
    }
    if ((s_census_ticks % 600ull) == 0) {
        printf("[RENDER-MUT] ticks=%llu mutated=%llu\n",
               s_census_ticks, s_census_mutated);
    }
}
