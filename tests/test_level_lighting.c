#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx_level_lighting.h"

/*
 * The production definitions live in gfx_pc_dkr.c. Keep this unit independent
 * of SDL, a ROM, and a graphics backend by supplying only the four outputs the
 * pure lighting policy publishes.
 */
float g_pc_sun_dir_world[3];
float g_pc_sun_color_linear[3];
float g_pcSunStrength;
int g_pcLevelLightingValid;
float g_pcGradeLevelSat;
float g_pcGradeLevelCon;
float g_pcGradeLevelTintR;
float g_pcGradeLevelTintG;
float g_pcGradeLevelTintB;
int g_pcGradeLevelValid;

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int closef(float actual, float expected, float tolerance) {
    return fabsf(actual - expected) <= tolerance;
}

static float length3(const float vector[3]) {
    return sqrtf(
        vector[0] * vector[0] +
        vector[1] * vector[1] +
        vector[2] * vector[2]);
}

static GfxLevelLightingInput fixture(void) {
    GfxLevelLightingInput input = {0};
    input.world_id = 2;
    input.geometry_id = 11;
    input.skybox_id = 4;
    input.fog_near = 900;
    input.fog_far = 1500;
    input.fog_rgb[0] = 92;
    input.fog_rgb[1] = 70;
    input.fog_rgb[2] = 58;
    input.background_rgb[0] = 118;
    input.background_rgb[1] = 145;
    input.background_rgb[2] = 174;
    input.sky_top_rgb[0] = 182;
    input.sky_top_rgb[1] = 214;
    input.sky_top_rgb[2] = 245;
    input.sky_bottom_rgb[0] = 91;
    input.sky_bottom_rgb[1] = 117;
    input.sky_bottom_rgb[2] = 146;
    input.weather_velocity[0] = 7;
    input.weather_velocity[2] = -3;
    input.weather_intensity = 96;
    input.sky_scroll[0] = 1;
    input.sky_scroll[1] = -2;
    input.cycles[0].valid = true;
    input.cycles[0].current_rgba[0] = 228;
    input.cycles[0].current_rgba[1] = 196;
    input.cycles[0].current_rgba[2] = 154;
    input.cycles[0].current_rgba[3] = 180;
    memcpy(
        input.cycles[0].authored_rgba,
        input.cycles[0].current_rgba,
        sizeof(input.cycles[0].authored_rgba));
    return input;
}

int main(void) {
    GfxLevelLightingInput input = fixture();
    GfxLevelLightingRig first;
    GfxLevelLightingRig second;

    expect(
        closef(gfx_level_light_srgb_to_linear(0.0f), 0.0f, 1.0e-7f) &&
        closef(gfx_level_light_srgb_to_linear(1.0f), 1.0f, 1.0e-7f),
        "sRGB endpoints decode exactly");
    expect(
        closef(
            gfx_level_light_srgb_to_linear(0.04045f),
            0.0031308f,
            1.0e-6f),
        "sRGB transfer-function knee is correct");
    expect(
        closef(gfx_level_light_srgb_to_linear(0.5f), 0.214041f, 1.0e-5f),
        "sRGB midpoint decodes in linear space");
    for (int i = 0; i <= 100; i++) {
        float encoded = i / 100.0f;
        float roundtrip = gfx_level_light_linear_to_srgb(
            gfx_level_light_srgb_to_linear(encoded));
        expect(closef(roundtrip, encoded, 2.0e-6f),
               "sRGB round trip is stable");
    }

    expect(
        !gfx_level_lighting_derive(NULL, &first),
        "NULL input fails closed");
    expect(
        !gfx_level_lighting_derive(&input, NULL),
        "NULL output fails closed");
    expect(
        gfx_level_lighting_derive(&input, &first),
        "authored fixture derives a rig");
    expect(first.valid, "derived rig is valid");
    expect(
        closef(length3(first.direction_world), 1.0f, 1.0e-5f) &&
        first.direction_world[1] >= 0.48f,
        "sun direction is normalized and above the horizon");
    expect(
        first.strength >= 0.10f && first.strength <= 0.16f,
        "sun strength stays restrained");
    expect(
        first.grade_valid &&
        first.grade_saturation >= 0.995f &&
        first.grade_saturation <= 1.025f &&
        first.grade_contrast >= 0.990f &&
        first.grade_contrast <= 1.020f &&
        first.grade_tint[0] >= 0.980f &&
        first.grade_tint[0] <= 1.020f &&
        first.grade_tint[1] >= 0.980f &&
        first.grade_tint[1] <= 1.020f &&
        first.grade_tint[2] >= 0.980f &&
        first.grade_tint[2] <= 1.020f,
        "authored world grade stays inside its restrained bounds");
    expect(
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_BACKGROUND) != 0 &&
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_SKY) != 0 &&
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_FOG) != 0 &&
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_CYCLE) != 0 &&
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_WEATHER) != 0 &&
        (first.source_mask & GFX_LEVEL_LIGHT_SOURCE_SCROLL) != 0,
        "telemetry reports every authored source used");
    expect(
        gfx_level_lighting_derive(&input, &second) &&
        memcmp(&first, &second, sizeof(first)) == 0,
        "same normalized level header produces the same rig");

    {
        GfxLevelLightingInput changed = input;
        GfxLevelLightingRig different;
        changed.weather_velocity[0] = -19;
        changed.weather_velocity[2] = 23;
        expect(
            gfx_level_lighting_derive(&changed, &different) &&
            (fabsf(first.direction_world[0] - different.direction_world[0]) >
                 0.05f ||
             fabsf(first.direction_world[2] - different.direction_world[2]) >
                 0.05f),
            "weather-vector positive control changes sun direction");
    }
    {
        GfxLevelLightingInput changed = input;
        GfxLevelLightingRig different;
        changed.sky_top_rgb[0] = 45;
        changed.sky_top_rgb[1] = 240;
        changed.sky_top_rgb[2] = 70;
        expect(
            gfx_level_lighting_derive(&changed, &different) &&
            (fabsf(first.colour_linear[0] - different.colour_linear[0]) >
                 0.02f ||
             fabsf(first.colour_linear[1] - different.colour_linear[1]) >
                 0.02f),
            "authored-colour positive control changes sun colour");
    }
    {
        GfxLevelLightingInput empty = {0};
        GfxLevelLightingRig fallback;
        empty.world_id = 9;
        empty.geometry_id = 4;
        expect(
            gfx_level_lighting_derive(&empty, &fallback) &&
            fallback.valid &&
            !fallback.grade_valid &&
            fallback.grade_saturation == 1.0f &&
            fallback.grade_contrast == 1.0f &&
            fallback.grade_tint[0] == 1.0f &&
            fallback.grade_tint[1] == 1.0f &&
            fallback.grade_tint[2] == 1.0f &&
            (fallback.source_mask & GFX_LEVEL_LIGHT_SOURCE_HASH) != 0,
            "legal empty header gets lighting but an exactly neutral grade");
    }
    {
        uint32_t packed =
            gfx_level_lighting_pack_direction(first.direction_world);
        float unpacked[3];
        expect(packed != 0, "valid direction packs to a nonzero token");
        expect(
            gfx_level_lighting_unpack_direction(packed, unpacked) &&
            fabsf(unpacked[0] - first.direction_world[0]) < 0.003f &&
            fabsf(unpacked[1] - first.direction_world[1]) < 0.003f &&
            fabsf(unpacked[2] - first.direction_world[2]) < 0.003f,
            "signed 10-bit display-list direction round trips");
        expect(
            !gfx_level_lighting_unpack_direction(0, unpacked) &&
            gfx_level_lighting_pack_direction(NULL) == 0,
            "invalid direction tokens fail closed");
    }

    expect(
        gfx_level_lighting_publish(&input) &&
        g_pcLevelLightingValid == 1 &&
        closef(g_pcSunStrength, first.strength, 1.0e-7f) &&
        g_pcGradeLevelValid == 1 &&
        closef(g_pcGradeLevelSat, first.grade_saturation, 1.0e-7f) &&
        closef(g_pcGradeLevelTintB, first.grade_tint[2], 1.0e-7f),
        "publish exports the derived rig");
    expect(
        memcmp(
            gfx_level_lighting_current()->direction_world,
            first.direction_world,
            sizeof(first.direction_world)) == 0,
        "published rig remains inspectable");
    expect(
        !gfx_level_lighting_publish(NULL) &&
        g_pcLevelLightingValid == 0 &&
        g_pcSunStrength == 0.0f &&
        g_pcGradeLevelSat == 1.0f &&
        g_pcGradeLevelCon == 1.0f &&
        g_pcGradeLevelTintR == 1.0f &&
        g_pcGradeLevelTintG == 1.0f &&
        g_pcGradeLevelTintB == 1.0f &&
        g_pcGradeLevelValid == 0 &&
        g_pc_sun_dir_world[1] == 1.0f,
        "failed publish resets to safe unlit and neutral-grade state");

    if (failures != 0) {
        fprintf(stderr, "%d level-lighting test(s) failed\n", failures);
        return 1;
    }
    puts("PASS level lighting");
    return 0;
}
