#include "runtime_contracts.h"
#include "enums.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "test_runtime_contracts:%d: requirement failed: %s\n", \
                    __LINE__, #condition);                                        \
            return 1;                                                             \
        }                                                                         \
    } while (0)

static int test_audio_domains(void) {
    s32 character;
    s32 vehicle;
    s32 row;
    s32 mappedVehicle;

    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_CAR) == VEHICLE_CAR);
    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_HOVERCRAFT) == VEHICLE_HOVERCRAFT);
    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_PLANE) == VEHICLE_PLANE);
    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_FLYING_CAR) == VEHICLE_PLANE);
    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_LOOPDELOOP) == VEHICLE_CAR);
    REQUIRE(mdkr_vehicle_sound_model(VEHICLE_BOSSES) == -1);
    REQUIRE(mdkr_vehicle_sound_model(-1) == -1);

    for (vehicle = VEHICLE_CAR; vehicle < NUMBER_OF_PLAYER_VEHICLES; vehicle++) {
        for (character = 0; character < NUMBER_OF_CHARACTERS; character++) {
            REQUIRE(mdkr_vehicle_sound_row(character, vehicle, &row, &mappedVehicle));
            REQUIRE(mappedVehicle == vehicle);
            REQUIRE(row == vehicle * NUMBER_OF_CHARACTERS + character);
        }
    }
    REQUIRE(mdkr_vehicle_sound_row(CHARACTER_DIDDY, VEHICLE_FLYING_CAR, &row, &mappedVehicle));
    REQUIRE(mappedVehicle == VEHICLE_PLANE);
    REQUIRE(row == VEHICLE_PLANE * NUMBER_OF_CHARACTERS + CHARACTER_DIDDY);
    REQUIRE(mdkr_vehicle_sound_row(CHARACTER_DIDDY, VEHICLE_LOOPDELOOP, &row, &mappedVehicle));
    REQUIRE(mappedVehicle == VEHICLE_CAR);
    REQUIRE(row == CHARACTER_DIDDY);

    REQUIRE(!mdkr_vehicle_sound_row(-1, VEHICLE_CAR, &row, &mappedVehicle));
    REQUIRE(!mdkr_vehicle_sound_row(NUMBER_OF_CHARACTERS, VEHICLE_CAR, &row, &mappedVehicle));
    REQUIRE(!mdkr_vehicle_sound_row(0, VEHICLE_BOSSES, &row, &mappedVehicle));
    REQUIRE(!mdkr_vehicle_sound_row(0, -1, &row, &mappedVehicle));
    REQUIRE(!mdkr_vehicle_sound_row(0, VEHICLE_CAR, NULL, &mappedVehicle));

    for (vehicle = 0; vehicle < MDKR_SOUND_GROUP_COUNT; vehicle++) {
        REQUIRE(mdkr_audio_group_valid(vehicle));
    }
    REQUIRE(!mdkr_audio_group_valid(-1));
    REQUIRE(!mdkr_audio_group_valid(MDKR_SOUND_GROUP_COUNT));
    REQUIRE(!mdkr_audio_group_valid(63));

    REQUIRE(mdkr_sound_id_valid(0, 2));
    REQUIRE(mdkr_sound_id_valid(1, 2));
    REQUIRE(!mdkr_sound_id_valid(2, 2));
    REQUIRE(!mdkr_sound_id_valid(UINT16_MAX, 2));

    REQUIRE(mdkr_asset_rows_fit(100, 100 + 30 * 64, 30, 64));
    REQUIRE(!mdkr_asset_rows_fit(100, 100 + 30 * 64 - 1, 30, 64));
    REQUIRE(!mdkr_asset_rows_fit(100, 99, 30, 64));
    REQUIRE(!mdkr_asset_rows_fit(-1, 2000, 30, 64));
    REQUIRE(!mdkr_asset_rows_fit(0, INT32_MAX, INT32_MAX, INT32_MAX));
    for (s32 bytes = 0; bytes < 0xC4; ++bytes)
        REQUIRE(!mdkr_asset_rows_fit(100, 100 + bytes, 1, 0xC4));
    REQUIRE(mdkr_asset_rows_fit(100, 100 + 0xC4, 1, 0xC4));
    REQUIRE(mdkr_asset_rows_fit(100, 100 + 0xC8, 1, 0xC4));
    return 0;
}

static int test_path_normalization(void) {
    f32 x;
    f32 z;

    REQUIRE(mdkr_normalize_xz(3.0f, 4.0f, &x, &z));
    REQUIRE(fabsf(x - 0.6f) < 0.00001f);
    REQUIRE(fabsf(z - 0.8f) < 0.00001f);
    REQUIRE(!mdkr_normalize_xz(0.0f, 0.0f, &x, &z));
    REQUIRE(x == 0.0f && z == 0.0f);
    REQUIRE(!mdkr_normalize_xz(1.0e-8f, 1.0e-8f, &x, &z));
    REQUIRE(x == 0.0f && z == 0.0f);
    REQUIRE(!mdkr_normalize_xz(INFINITY, 1.0f, &x, &z));
    REQUIRE(x == 0.0f && z == 0.0f);
    REQUIRE(!mdkr_normalize_xz(1.0f, NAN, &x, &z));
    REQUIRE(x == 0.0f && z == 0.0f);
    REQUIRE(!mdkr_normalize_xz(1.0f, 1.0f, NULL, &z));
    return 0;
}

static int test_safety_domains(void) {
    s32 resolved;
    s32 loadCount;
    s32 offset;
    s32 size;
    u32 value;
    size_t bytes;
    u32 fxTable[10] = { 0 };

    REQUIRE(mdkr_model_load_selection(0, 1, &resolved, &loadCount));
    REQUIRE(resolved == 0 && loadCount == 1);
    REQUIRE(mdkr_model_load_selection(4, 5, &resolved, &loadCount));
    REQUIRE(resolved == 4 && loadCount == 5);
    REQUIRE(mdkr_model_load_selection(-1, 5, &resolved, &loadCount));
    REQUIRE(resolved == 0 && loadCount == 1);
    REQUIRE(mdkr_model_load_selection(5, 5, &resolved, &loadCount));
    REQUIRE(resolved == 0 && loadCount == 1);
    REQUIRE(!mdkr_model_load_selection(0, 0, &resolved, &loadCount));
    REQUIRE(!mdkr_model_load_selection(0, 1, NULL, &loadCount));

    REQUIRE(mdkr_course_flag(0, &value) && value == UINT32_C(0x00010000));
    REQUIRE(mdkr_course_flag(15, &value) && value == UINT32_C(0x80000000));
    REQUIRE(!mdkr_course_flag(-1, &value));
    REQUIRE(!mdkr_course_flag(16, &value));

    REQUIRE(mdkr_trophy_state(UINT32_C(0xE4), 1, &value) && value == 0);
    REQUIRE(mdkr_trophy_state(UINT32_C(0xE4), 2, &value) && value == 1);
    REQUIRE(mdkr_trophy_state(UINT32_C(0xE4), 3, &value) && value == 2);
    REQUIRE(mdkr_trophy_state(UINT32_C(0xE4), 4, &value) && value == 3);
    /* Exhaust every persisted trophy combination, not just the mainland
     * unlock mask. Issue #63 was incorrectly encoded as rejecting world 5. */
    for (u32 trophies = 0; trophies < 1024; trophies++) {
        u32 fields = trophies;
        for (s32 world = WORLD_DINO_DOMAIN; world <= WORLD_FUTURE_FUN_LAND; world++) {
            REQUIRE(mdkr_trophy_state(trophies, world, &value));
            REQUIRE(value == (fields & 3U));
            fields >>= 2;
        }
        REQUIRE(fields == 0);
    }
    REQUIRE(mdkr_trophy_state(UINT32_C(0x300), WORLD_FUTURE_FUN_LAND, &value) && value == 3);
    REQUIRE(mdkr_trophy_state(UINT32_C(0xFF), WORLD_FUTURE_FUN_LAND, &value) && value == 0);
    value = UINT32_C(0x12345678);
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, WORLD_NONE, &value));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, 0, &value));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, WORLD_FUTURE_FUN_LAND + 1, &value));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, INT32_MIN, &value));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, INT32_MAX, &value));
    REQUIRE(value == UINT32_C(0x12345678));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, WORLD_FUTURE_FUN_LAND, NULL));
    /* Bronze and silver from different files must not synthesize a gold.
     * Check every pair of ranks in every field, with neighboring golds kept. */
    for (s32 world = WORLD_DINO_DOMAIN; world <= WORLD_FUTURE_FUN_LAND; world++) {
        u32 shift = (u32) (world - WORLD_DINO_DOMAIN) * 2U;
        u32 neighbors = UINT32_C(0x3FF) & ~(3U << shift);
        for (u32 left = 0; left < 4; left++) {
            for (u32 right = 0; right < 4; right++) {
                u32 expected = (left > right ? left : right) << shift;
                REQUIRE(mdkr_trophy_records_merge(left << shift, right << shift) == expected);
                REQUIRE(mdkr_trophy_records_merge(right << shift, left << shift) == expected);
                REQUIRE(mdkr_trophy_records_merge(neighbors | (left << shift), right << shift) ==
                        (neighbors | expected));
            }
        }
    }
    for (u32 trophies = 0; trophies < 1024; trophies++) {
        REQUIRE(mdkr_trophy_records_merge(trophies, 0) == trophies);
        REQUIRE(mdkr_trophy_records_merge(trophies, trophies) == trophies);
        REQUIRE(mdkr_trophy_records_merge(trophies, UINT32_C(0x3FF)) == UINT32_C(0x3FF));
    }
    REQUIRE(mdkr_trophy_records_merge(UINT32_MAX, 0) == UINT32_C(0x3FF));

    REQUIRE(mdkr_extension_bit('A', &value) && value == 1);
    REQUIRE(mdkr_extension_bit('Z', &value) && value == (UINT32_C(1) << 25));
    REQUIRE(!mdkr_extension_bit('@', &value));
    REQUIRE(!mdkr_extension_bit('[', &value));

    REQUIRE(mdkr_texture_allocation_size(32, 1, 96, 0, &bytes));
    REQUIRE(bytes == 143);
    REQUIRE(mdkr_texture_allocation_size(32, 2, 96, 48, &bytes));
    REQUIRE(bytes == 335);
    REQUIRE(!mdkr_texture_allocation_size(SIZE_MAX, 1, 96, 0, &bytes));
    REQUIRE(!mdkr_texture_allocation_size(32, SIZE_MAX, 96, 0, &bytes));
    REQUIRE(!mdkr_texture_allocation_size(32, 0, 96, 0, &bytes));
    REQUIRE(mdkr_texture_frame_advance(2760, 0, 32, 2768, TRUE,
                                       &bytes) &&
            bytes == 2768);
    REQUIRE(!mdkr_texture_frame_advance(2760, 0, 32, 2784, TRUE,
                                        &bytes));
    REQUIRE(mdkr_texture_frame_advance(3872, 0, 32, 0, TRUE, &bytes) &&
            bytes == 0);
    REQUIRE(!mdkr_texture_frame_advance(3872, 0, 32, 0, FALSE, &bytes));
    REQUIRE(!mdkr_texture_frame_advance(31, 0, 32, 0, TRUE, &bytes));
    REQUIRE(!mdkr_texture_frame_advance(64, 48, 32, 32, TRUE, &bytes));

    REQUIRE(mdkr_palette_reservation(0, 32, 640, &bytes) && bytes == 0);
    REQUIRE(mdkr_palette_reservation(608, 32, 640, &bytes) && bytes == 608);
    REQUIRE(!mdkr_palette_reservation(609, 32, 640, &bytes));
    REQUIRE(mdkr_palette_reservation(512, 128, 640, &bytes) && bytes == 512);
    REQUIRE(!mdkr_palette_reservation(513, 128, 640, &bytes));

    fxTable[8] = 100;
    fxTable[9] = 300;
    REQUIRE(mdkr_audio_fx_span(fxTable, sizeof(fxTable), 300, &offset, &size));
    REQUIRE(offset == 100 && size == 200);
    fxTable[9] = 301;
    REQUIRE(!mdkr_audio_fx_span(fxTable, sizeof(fxTable), 300, &offset, &size));
    fxTable[9] = 99;
    REQUIRE(!mdkr_audio_fx_span(fxTable, sizeof(fxTable), 300, &offset, &size));
    REQUIRE(!mdkr_audio_fx_span(fxTable, sizeof(fxTable) - 1, 300,
                                &offset, &size));
    return 0;
}

static int test_mips_audio_math(void) {
    u32 denominator;
    u32 numerator;

    REQUIRE(mdkr_mips_trunc_w_d(0.0) == 0);
    REQUIRE(mdkr_mips_trunc_w_d(32767.99) == 32767);
    REQUIRE(mdkr_mips_trunc_w_d(-32768.99) == -32768);
    REQUIRE(mdkr_mips_trunc_w_d(2147483647.99) == INT32_MAX);
    REQUIRE(mdkr_mips_trunc_w_d(-2147483648.99) == INT32_MIN);
    REQUIRE(mdkr_mips_trunc_w_d(2147483648.0) == INT32_MIN);
    REQUIRE(mdkr_mips_trunc_w_d(INFINITY) == INT32_MIN);
    REQUIRE(mdkr_mips_trunc_w_d(NAN) == INT32_MIN);

    REQUIRE(mdkr_mips_round_w_s(0.49f) == 0);
    REQUIRE(mdkr_mips_round_w_s(0.5f) == 0);
    REQUIRE(mdkr_mips_round_w_s(0.5001f) == 1);
    REQUIRE(mdkr_mips_round_w_s(1.5f) == 2);
    REQUIRE(mdkr_mips_round_w_s(2.5f) == 2);
    REQUIRE(mdkr_mips_round_w_s(-0.5f) == 0);
    REQUIRE(mdkr_mips_round_w_s(-1.5f) == -2);
    REQUIRE(mdkr_mips_round_w_s(-2.5f) == -2);
    REQUIRE(mdkr_mips_round_w_s(INFINITY) == INT32_MIN);
    REQUIRE(mdkr_mips_round_w_s(NAN) == INT32_MIN);

    REQUIRE(mdkr_mips_atan_index(0, 1) == 0);
    REQUIRE(mdkr_mips_atan_index(1, 2) == 512);
    REQUIRE(mdkr_mips_atan_index(1, 3) == 341);
    REQUIRE(mdkr_mips_atan_index(1, 0) == 0);
    REQUIRE(mdkr_mips_atan_index(UINT32_MAX, 1) == 1024);
    for (denominator = 1; denominator <= 2048; denominator++) {
        for (numerator = 0; numerator <= denominator; numerator++) {
            u32 byteOffset =
                (u32) (((uint64_t) numerator << 11) / denominator) &
                UINT32_C(0xFFE);
            REQUIRE(mdkr_mips_atan_index(numerator, denominator) ==
                    byteOffset / 2u);
        }
    }

    REQUIRE(mdkr_mips_f64_to_s16(32767.99) == 32767);
    REQUIRE(mdkr_mips_f64_to_s16(32768.0) == -32768);
    REQUIRE(mdkr_mips_f64_to_s16(-49151.2) == 16385);
    REQUIRE(mdkr_mips_f64_to_u16(65535.9) == UINT16_MAX);
    REQUIRE(mdkr_mips_f64_to_u16(65536.0) == 0);

    REQUIRE(mdkr_audio_rate_value(0, 0) == 0.0f);
    REQUIRE(mdkr_audio_rate_value(1, 0) == 1.0f);
    REQUIRE(mdkr_audio_rate_value(-1, 0) == -1.0f);
    REQUIRE(fabsf(mdkr_audio_rate_value(0, UINT16_MAX) -
                  (65535.0f / 65536.0f)) < 0.00001f);
    REQUIRE(mdkr_audio_rate_value(INT16_MIN, 0) == -32768.0f);
    return 0;
}

static int test_object_map_indices(void) {
    s32 resolved = 77;
    REQUIRE(!mdkr_object_map_index_resolve(-1, 136, &resolved));
    REQUIRE(resolved == 77);
    REQUIRE(!mdkr_object_map_index_resolve(INT32_MIN, 136, &resolved));
    REQUIRE(!mdkr_object_map_index_resolve(0, 0, &resolved));
    REQUIRE(!mdkr_object_map_index_resolve(0, -1, &resolved));
    REQUIRE(resolved == 77);
    REQUIRE(!mdkr_object_map_index_resolve(0, 136, NULL));
    for (s32 i = 0; i < 136; ++i) {
        REQUIRE(mdkr_object_map_index_resolve(i, 136, &resolved));
        REQUIRE(resolved == i);
    }
    REQUIRE(mdkr_object_map_index_resolve(136, 136, &resolved));
    REQUIRE(resolved == 0);
    REQUIRE(mdkr_object_map_index_resolve(INT32_MAX, 136, &resolved));
    REQUIRE(resolved == 0);
    resolved = 135;
    REQUIRE(mdkr_object_map_index_resolve(resolved, 136, &resolved));
    REQUIRE(resolved == 135);
    return 0;
}

static int test_postrace_background_indices(void) {
    /* Exhaust the signed header byte domain, plus wider caller extremes.
     * Every three-column background access must remain inside 15 entries. */
    for (s32 world = INT8_MIN; world <= INT8_MAX; ++world) {
        s32 row = mdkr_postrace_background_index(world);
        REQUIRE(row >= 0 && row * 3 + 2 < 15);
        REQUIRE(row == (world >= 1 && world <= 5 ? world - 1 : 0));
    }
    REQUIRE(mdkr_postrace_background_index(INT32_MIN) == 0);
    REQUIRE(mdkr_postrace_background_index(INT32_MAX) == 0);
    return 0;
}

static int test_level_name_bounds(void) {
    const char names[] = {'E', 0, 'D', 0, 'F', 0, 'J', 0};
    for (unsigned language = 0; language < 4; ++language) {
        REQUIRE(mdkr_level_name_bounded(names, sizeof names, language) == names + language * 2);
        for (size_t size = 0; size < (language + 1) * 2; ++size)
            REQUIRE(mdkr_level_name_bounded(names, size, language) == NULL);
    }
    const char empty[] = {0, 0, 0, 0};
    REQUIRE(mdkr_level_name_bounded(empty, sizeof empty, 3) == empty + 3);
    REQUIRE(mdkr_level_name_bounded(names, sizeof names, 4) == NULL);
    REQUIRE(mdkr_level_name_bounded(NULL, 1, 0) == NULL);
    REQUIRE(mdkr_level_name_bounded("unterminated", 12, 0) == NULL);
    return 0;
}

static int test_weather_preset_indices(void) {
    for (s32 type = INT16_MIN; type <= INT16_MAX; ++type) {
        s32 index = mdkr_weather_preset_index(type, 1);
        REQUIRE(index == (type < 0 ? -1 : type > 1 ? 1 : type));
    }
    REQUIRE(mdkr_weather_preset_index(INT32_MIN, 1) == -1);
    REQUIRE(mdkr_weather_preset_index(INT32_MAX, 1) == 1);
    REQUIRE(mdkr_weather_preset_index(0, -1) == -1);
    REQUIRE(mdkr_weather_preset_index(1, 0) == 0);
    return 0;
}

static int test_light_fraction(void) {
    REQUIRE(mdkr_light_cycle_time(65000, 1000, 65535) == 465);
    REQUIRE(mdkr_light_cycle_time(INT32_MAX - 1, INT32_MAX, INT32_MAX) == INT32_MAX - 1);
    REQUIRE(mdkr_light_cycle_time(0, 0, 1) == 0);
    REQUIRE(mdkr_light_cycle_time(-1, 1, 10) == -1);
    REQUIRE(mdkr_light_cycle_time(1, -1, 10) == -1);
    REQUIRE(mdkr_light_cycle_time(1, 1, 0) == -1);
    REQUIRE(mdkr_light_cycle_time(1, 1, -1) == -1);
    REQUIRE(mdkr_light_fraction(65536, 32768, 65536) == 32768);
    REQUIRE(mdkr_light_fraction(65535, 65534, 65535) == 65534);
    REQUIRE(mdkr_light_fraction(UINT32_MAX, INT32_MAX - 1, INT32_MAX) == UINT32_MAX - 3u);
    REQUIRE(mdkr_light_fraction(65536, -1, 10) == 0);
    REQUIRE(mdkr_light_fraction(65536, 1, 0) == 0);
    REQUIRE(mdkr_light_fraction(65536, 1, -1) == 0);
    REQUIRE(mdkr_light_fraction(65536, 10, 10) == 65536);
    REQUIRE(mdkr_light_fraction(65536, 11, 10) == 65536);
    for (s32 duration = 1; duration <= 256; ++duration)
        for (s32 time = 0; time < duration; ++time) {
            for (s32 delta = 0; delta <= 8; ++delta) {
                s32 previous = time + delta;
                while (previous >= duration) previous -= duration;
                REQUIRE(mdkr_light_cycle_time(time, delta, duration) == previous);
            }
            REQUIRE(mdkr_light_fraction(65536, time, duration) == (u32)((time << 16) / duration));
            REQUIRE(mdkr_light_fraction(255, time, duration) == (u32)((255 * time) / duration));
        }
    return 0;
}

static int test_ainode_rebuild_bounds(void) {
    REQUIRE(mdkr_ainode_distance_cost(3, 4, 0) == 5);
    REQUIRE(mdkr_ainode_distance_cost(0, 0, 0) == 0);
    REQUIRE(mdkr_ainode_distance_cost(32768, 0, 0) == 32768);
    REQUIRE(mdkr_ainode_distance_cost(-65535, 0, 0) == 65535);
    REQUIRE(mdkr_ainode_distance_cost(65535, 65535, 65535) == 113509);
    REQUIRE(mdkr_ainode_distance_cost(NAN, 0, 0) == -1);
    REQUIRE(mdkr_ainode_distance_cost(INFINITY, 0, 0) == -1);
    REQUIRE(mdkr_ainode_distance_cost(0, -INFINITY, 0) == -1);
    REQUIRE(mdkr_ainode_distance_cost(0, 0, 1e30f) == -1);
    const s32 edge_limit = INT32_MAX / MDKR_AI_NODE_COUNT;
    REQUIRE(mdkr_ainode_distance_cost(edge_limit, 0, 0) == edge_limit);
    REQUIRE(mdkr_ainode_distance_cost(edge_limit + 1, 0, 0) == -1);
    for (s32 id = -1; id <= 255; ++id)
        for (s32 count = -1; count <= MDKR_AI_NODE_COUNT; ++count)
            for (s32 occupied = 0; occupied <= 1; ++occupied)
                REQUIRE(mdkr_ainode_can_append(id, count, occupied) ==
                    (id >= 0 && id < MDKR_AI_NODE_COUNT && count >= 0 &&
                     count < MDKR_AI_NODE_COUNT && !occupied));
    REQUIRE(!mdkr_ainode_can_append(INT32_MIN, 0, 0));
    REQUIRE(!mdkr_ainode_can_append(INT32_MAX, 0, 0));
    REQUIRE(!mdkr_ainode_can_append(0, INT32_MAX, 0));
    struct { u8 before; u8 ids[MDKR_AI_NODE_COUNT]; u8 after; } bounded = {.before = 0xA5, .after = 0x5A};
    u8 occupied[MDKR_AI_NODE_COUNT] = {0};
    s32 count = 0;
    /* More objects than capacity, despite every ID being individually valid. */
    for (s32 id = 0; id < MDKR_AI_NODE_COUNT; ++id)
        for (unsigned duplicate = 0; duplicate < 3; ++duplicate)
            if (mdkr_ainode_can_append(id, count, occupied[id])) {
                occupied[id] = 1;
                bounded.ids[count++] = id;
            }
    REQUIRE(count == MDKR_AI_NODE_COUNT && bounded.before == 0xA5 && bounded.after == 0x5A);
    for (s32 i = 0; i < count; ++i) REQUIRE(bounded.ids[i] == i);
    return 0;
}

int main(void) {
    if (test_audio_domains() != 0 || test_path_normalization() != 0 ||
        test_safety_domains() != 0 || test_mips_audio_math() != 0 ||
        test_object_map_indices() != 0 || test_postrace_background_indices() != 0 ||
        test_level_name_bounds() != 0 || test_weather_preset_indices() != 0 ||
        test_light_fraction() != 0 || test_ainode_rebuild_bounds() != 0) {
        return 1;
    }
    puts("runtime contract tests passed");
    return 0;
}
