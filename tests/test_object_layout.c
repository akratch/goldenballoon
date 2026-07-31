#include "types.h"
#include "objects.h"
#include "object_layout.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "test_object_layout:%d: requirement failed: %s\n", \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct LegacyShadowData {
    float scale;
    void *texture;
    int16_t mesh_start;
    int16_t mesh_end;
    int16_t unknown_c;
    int16_t unknown_e;
} LegacyShadowData;

typedef struct LegacyLevelObjectEntryHud {
    uint8_t object_id;
    uint8_t size;
    int16_t x;
    int16_t y;
    int16_t z;
    int32_t offset_y;
} LegacyLevelObjectEntryHud;

#if defined(_MSC_VER)
#define MDKR_NOINLINE __declspec(noinline)
#else
#define MDKR_NOINLINE __attribute__((noinline))
#endif

MDKR_NOINLINE
static int16_t legacy_renderer_read_anim_frame(volatile Object *object) {
    return object->animFrame;
}

static int run_legacy_mem02(void) {
    _Alignas(16) uint8_t bytes[64] = { 0 };
    LegacyShadowData *shadow = (LegacyShadowData *) (bytes + 4);

    /*
     * Exact old object-tail shape: a four-byte cursor is cast to a struct whose
     * pointer member raises its required alignment to eight on LP64.
     */
    shadow->texture = (void *) (uintptr_t) 1;
    return shadow->texture == NULL;
}

static int run_legacy_mem03(void) {
    _Alignas(4) uint8_t bytes[32] = { 0 };
    LegacyLevelObjectEntryHud *entry =
        (LegacyLevelObjectEntryHud *) (bytes + 2);
    int32_t value = 0x12345678;

    /*
     * Exact old record-view shape: an even 10-byte predecessor places this
     * naturally four-byte-aligned union at address 2 mod 4.
     */
    memcpy(bytes + 2 + 8, &value, sizeof(value));
    return entry->offset_y == 0;
}

static int run_legacy_mem04(void) {
    volatile ObjectTransform transform = { 0 };

    /*
     * Exact old lens-flare renderer shape: a bare 0x18-byte transform is passed
     * as Object *, and render_sprite_billboard reads animFrame at offset 0x18.
     * AddressSanitizer must catch that first read beyond the real source.
     */
    return legacy_renderer_read_anim_frame((volatile Object *) &transform) == 0;
}

static int test_level_records(void) {
    _Alignas(4) uint8_t records[36] = { 0 };
    MdkrLevelObjectRecordView view;
    MdkrLevelObjectRecordError error;
    size_t offset = 0;
    size_t i;
    const uint8_t strides[] = { 10, 12, 14 };

    _Static_assert(sizeof(LevelObjectEntryCommon) == 8,
                   "serialized common header changed");
    _Static_assert(_Alignof(LevelObjectEntryCommon) == 2,
                   "serialized common header must be two-byte aligned");
    _Static_assert(sizeof(LevelObjectEntry_Boost2) == 10,
                   "dynamic boost record must retain its encoded size");
    _Static_assert(sizeof(LevelObjectEntry_Hud) == 12,
                   "HUD record must retain its encoded size");
    _Static_assert(_Alignof(LevelObjectEntry_Hud) == 2,
                   "HUD's s32 is an unaligned serialized field");
    _Static_assert(_Alignof(LevelObjectEntry) == 2,
                   "serialized record union must accept every legal record start");

    for (i = 0; i < sizeof(strides); i++) {
        records[offset] = (uint8_t) (17 + i);
        records[offset + 1] = strides[i];
        offset += strides[i];
    }
    REQUIRE(offset == sizeof(records));

    offset = 0;
    for (i = 0; i < sizeof(strides); i++) {
        error = mdkr_level_object_record_parse(
            records + offset, sizeof(records) - offset, 304, &view);
        REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_OK);
        REQUIRE(view.bytes == records + offset);
        REQUIRE(view.size == strides[i]);
        REQUIRE(view.object_id == (uint16_t) (17 + i));
        REQUIRE(((uintptr_t) view.bytes & 1u) == 0);
        if (i == 1) {
            REQUIRE(((uintptr_t) view.bytes & 3u) == 2);
        }
        offset += view.size;
    }
    REQUIRE(offset == sizeof(records));

    error = mdkr_level_object_record_parse(NULL, 8, 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_INVALID_ARGUMENT);
    error = mdkr_level_object_record_parse(records, 7, 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_HEADER);

    records[1] = 6;
    error = mdkr_level_object_record_parse(records, sizeof(records), 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_SHORT_STRIDE);
    records[1] = 9;
    error = mdkr_level_object_record_parse(records, sizeof(records), 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_ODD_STRIDE);
    records[1] = 40;
    error = mdkr_level_object_record_parse(records, sizeof(records), 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_TRUNCATED_BODY);
    records[0] = 0xFF;
    records[1] = 0x80 | 10;
    error = mdkr_level_object_record_parse(records, sizeof(records), 304, &view);
    REQUIRE(error == MDKR_LEVEL_OBJECT_RECORD_BAD_OBJECT_ID);

    return 0;
}

static int test_object_tail(void) {
    _Alignas(16) uint8_t storage[4096] = { 0 };
    MdkrObjectLayout layout;
    Object *object;
    ModelInstance **models;
    Object_Racer *behavior;
    ShadeProperties *shading;
    ShadowData *shadow;
    WaterEffect *water;
    ObjectInteraction *interaction;
    ObjectCollision *collision;
    AttachPoint *attach;
    ParticleEmitter *emitters;
    struct ObjectLight **lights;
    size_t final_size;

    mdkr_object_layout_init(&layout, storage, sizeof(storage));
    object = mdkr_object_layout_append(
        &layout, sizeof(*object), _Alignof(Object));
    models = mdkr_object_layout_append_array(
        &layout, 3, sizeof(*models), _Alignof(ModelInstance *));
    behavior = mdkr_object_layout_append(
        &layout, sizeof(*behavior), 16);
    shading = mdkr_object_layout_append(
        &layout, sizeof(*shading), _Alignof(ShadeProperties));
    shadow = mdkr_object_layout_append(
        &layout, sizeof(*shadow), _Alignof(ShadowData));
    water = mdkr_object_layout_append(
        &layout, sizeof(*water), _Alignof(WaterEffect));
    interaction = mdkr_object_layout_append(
        &layout, sizeof(*interaction), _Alignof(ObjectInteraction));
    collision = mdkr_object_layout_append(
        &layout, sizeof(*collision), _Alignof(ObjectCollision));
    attach = mdkr_object_layout_append(
        &layout, sizeof(*attach), _Alignof(AttachPoint));
    emitters = mdkr_object_layout_append_array(
        &layout, 3, sizeof(*emitters), _Alignof(ParticleEmitter));
    lights = mdkr_object_layout_append_array(
        &layout, 2, sizeof(*lights), _Alignof(struct ObjectLight *));

    REQUIRE(object == (Object *) storage);
    REQUIRE(models == (ModelInstance **) (object + 1));
    REQUIRE(((uintptr_t) behavior & 15u) == 0);
    REQUIRE(((uintptr_t) shading % _Alignof(ShadeProperties)) == 0);
    REQUIRE(((uintptr_t) shadow % _Alignof(ShadowData)) == 0);
    REQUIRE(((uintptr_t) water % _Alignof(WaterEffect)) == 0);
    REQUIRE(((uintptr_t) interaction % _Alignof(ObjectInteraction)) == 0);
    REQUIRE(((uintptr_t) collision % _Alignof(ObjectCollision)) == 0);
    REQUIRE(((uintptr_t) attach % _Alignof(AttachPoint)) == 0);
    REQUIRE(((uintptr_t) emitters % _Alignof(ParticleEmitter)) == 0);
    REQUIRE(((uintptr_t) lights % _Alignof(struct ObjectLight *)) == 0);
    REQUIRE((uint8_t *) lights + 2 * sizeof(*lights) <=
            storage + sizeof(storage));
    REQUIRE(mdkr_object_layout_finish(&layout, 16, &final_size));
    REQUIRE((final_size & 15u) == 0);
    REQUIRE(final_size <= sizeof(storage));

    mdkr_object_layout_init(&layout, storage, 32);
    REQUIRE(mdkr_object_layout_append(&layout, 8, 3) == NULL);
    REQUIRE(mdkr_object_layout_error(&layout) ==
            MDKR_OBJECT_LAYOUT_INVALID_ALIGNMENT);
    REQUIRE(mdkr_object_layout_append(&layout, 1, 1) == NULL);

    mdkr_object_layout_init(&layout, storage, 32);
    REQUIRE(mdkr_object_layout_append_array(
                &layout, SIZE_MAX, 2, 8) == NULL);
    REQUIRE(mdkr_object_layout_error(&layout) ==
            MDKR_OBJECT_LAYOUT_ARITHMETIC_OVERFLOW);

    mdkr_object_layout_init(&layout, storage, 32);
    REQUIRE(mdkr_object_layout_append(&layout, 24, 8) != NULL);
    REQUIRE(mdkr_object_layout_append(&layout, 16, 8) == NULL);
    REQUIRE(mdkr_object_layout_error(&layout) ==
            MDKR_OBJECT_LAYOUT_CAPACITY_EXCEEDED);

    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--legacy-mem02") == 0) {
        return run_legacy_mem02();
    }
    if (argc == 2 && strcmp(argv[1], "--legacy-mem03") == 0) {
        return run_legacy_mem03();
    }
    if (argc == 2 && strcmp(argv[1], "--legacy-mem04") == 0) {
        return run_legacy_mem04();
    }
    if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--legacy-mem02|--legacy-mem03|--legacy-mem04]\n",
                argv[0]);
        return 2;
    }
    if (test_level_records() != 0 || test_object_tail() != 0) {
        return 1;
    }
    puts("object_layout: all checks passed");
    return 0;
}
