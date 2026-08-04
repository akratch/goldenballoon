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
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, 0, &value));
    REQUIRE(!mdkr_trophy_state(UINT32_MAX, 5, &value));

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

int main(void) {
    if (test_audio_domains() != 0 || test_path_normalization() != 0 ||
        test_safety_domains() != 0 || test_mips_audio_math() != 0) {
        return 1;
    }
    puts("runtime contract tests passed");
    return 0;
}
