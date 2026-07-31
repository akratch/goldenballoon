#include "gfx_level_lighting.h"

#include <math.h>
#include <string.h>

#include "gfx_uniforms.h"

#define GFX_LIGHT_PI 3.14159265358979323846f

static GfxLevelLightingRig s_current_rig;

static float clamp01(float value) {
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static float clampf(float value, float lo, float hi) {
    if (!(value > lo)) {
        return lo;
    }
    if (value >= hi) {
        return hi;
    }
    return value;
}

float gfx_level_light_srgb_to_linear(float encoded) {
    encoded = clamp01(encoded);
    if (encoded <= 0.04045f) {
        return encoded / 12.92f;
    }
    return powf((encoded + 0.055f) / 1.055f, 2.4f);
}

float gfx_level_light_linear_to_srgb(float linear) {
    linear = clamp01(linear);
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }
    return 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

static float linear_luma(const float rgb[3]) {
    return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}

static bool rgb_nonzero(const uint8_t rgb[3]) {
    return rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0;
}

static void add_encoded_colour(float sum[3], float *weight_sum,
                               const uint8_t rgb[3], float weight) {
    if (weight <= 0.0f || !rgb_nonzero(rgb)) {
        return;
    }
    for (int channel = 0; channel < 3; channel++) {
        sum[channel] +=
            gfx_level_light_srgb_to_linear(rgb[channel] / 255.0f) * weight;
    }
    *weight_sum += weight;
}

static bool normalize3(float vector[3]) {
    float length_sq =
        vector[0] * vector[0] +
        vector[1] * vector[1] +
        vector[2] * vector[2];
    if (!isfinite(length_sq) || length_sq < 1.0e-10f) {
        return false;
    }
    float inverse = 1.0f / sqrtf(length_sq);
    vector[0] *= inverse;
    vector[1] *= inverse;
    vector[2] *= inverse;
    return true;
}

bool gfx_level_lighting_derive(const GfxLevelLightingInput *input,
                               GfxLevelLightingRig *out) {
    float colour_sum[3] = {0.0f, 0.0f, 0.0f};
    float colour_weight = 0.0f;
    float top_linear[3];
    float bottom_linear[3];
    float top_luma;
    float bottom_luma;
    float horizontal_x = 0.0f;
    float horizontal_z = 0.0f;
    uint32_t source_mask = 0;
    bool grade_valid;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (input == NULL) {
        return false;
    }

    out->world_id = input->world_id;
    out->geometry_id = input->geometry_id;

    if (rgb_nonzero(input->background_rgb)) {
        add_encoded_colour(colour_sum, &colour_weight,
                           input->background_rgb, 1.0f);
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_BACKGROUND;
    }
    if (rgb_nonzero(input->sky_top_rgb) ||
        rgb_nonzero(input->sky_bottom_rgb)) {
        add_encoded_colour(colour_sum, &colour_weight,
                           input->sky_top_rgb, 1.75f);
        add_encoded_colour(colour_sum, &colour_weight,
                           input->sky_bottom_rgb, 0.50f);
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_SKY;
    }
    if (input->fog_far > input->fog_near &&
        rgb_nonzero(input->fog_rgb)) {
        add_encoded_colour(colour_sum, &colour_weight,
                           input->fog_rgb, 0.35f);
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_FOG;
    }

    for (int i = 0; i < GFX_LEVEL_LIGHT_MAX_CYCLES; i++) {
        const GfxLevelLightCycle *cycle = &input->cycles[i];
        if (!cycle->valid) {
            continue;
        }
        /*
         * The runtime colour is initialized from authored_rgba at level load.
         * Prefer it so the renderer observes the exact normalized
         * LevelHeader_70 record, but blend the authored endpoint too: a cycle
         * that begins at black should not manufacture a black sun.
         */
        float alpha =
            (float)(cycle->current_rgba[3] > cycle->authored_rgba[3]
                        ? cycle->current_rgba[3]
                        : cycle->authored_rgba[3]) /
            255.0f;
        float weight = 0.25f + 0.50f * alpha;
        add_encoded_colour(colour_sum, &colour_weight,
                           cycle->current_rgba, weight);
        add_encoded_colour(colour_sum, &colour_weight,
                           cycle->authored_rgba, weight * 0.5f);
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_CYCLE;
    }

    grade_valid =
        colour_weight > 0.0f && isfinite(colour_weight) &&
        input->world_id >= 0 && input->geometry_id >= 0;

    /*
     * Empty/cutscene headers are legal. A neutral D65-ish illuminant is a
     * controlled lighting fallback, still keyed by header identity for
     * direction. It must not manufacture a level grade.
     */
    if (colour_weight <= 0.0f) {
        colour_sum[0] = 1.0f;
        colour_sum[1] = 0.95f;
        colour_sum[2] = 0.90f;
        colour_weight = 1.0f;
    }
    for (int channel = 0; channel < 3; channel++) {
        out->colour_linear[channel] = colour_sum[channel] / colour_weight;
    }
    out->source_luma = linear_luma(out->colour_linear);
    if (out->source_luma < 0.02f) {
        out->colour_linear[0] = 1.0f;
        out->colour_linear[1] = 0.95f;
        out->colour_linear[2] = 0.90f;
        out->source_luma = linear_luma(out->colour_linear);
    }

    /*
     * Keep the level's hue but not its exposure. The baked vertex colour owns
     * low-frequency exposure/mood (RL-1); the sun contributes only restrained
     * chromatic directionality. Normalizing by luminance and clamping the tint
     * prevents a saturated colour-cycle entry from washing the model.
     */
    for (int channel = 0; channel < 3; channel++) {
        out->colour_linear[channel] =
            clampf(out->colour_linear[channel] / out->source_luma,
                   0.72f, 1.22f);
    }

    out->grade_saturation = 1.0f;
    out->grade_contrast = 1.0f;
    out->grade_tint[0] = 1.0f;
    out->grade_tint[1] = 1.0f;
    out->grade_tint[2] = 1.0f;
    out->grade_valid = grade_valid;
    if (grade_valid) {
        float minimum = out->colour_linear[0];
        float maximum = out->colour_linear[0];
        for (int channel = 1; channel < 3; channel++) {
            minimum = fminf(minimum, out->colour_linear[channel]);
            maximum = fmaxf(maximum, out->colour_linear[channel]);
        }
        /*
         * The baked colour remains the ambient base. The world grade only
         * carries a quiet echo of the normalized header mood: at most 2.5%
         * saturation, 2% contrast, and 2% per-channel tint. These values are
         * linear-light multipliers, not an encoded-RGB saturation trick.
         */
        out->grade_saturation =
            clampf(0.995f + (maximum - minimum) * 0.05f,
                   0.995f, 1.025f);
        out->grade_contrast =
            clampf(0.990f + out->source_luma * 0.03f,
                   0.990f, 1.020f);
        for (int channel = 0; channel < 3; channel++) {
            out->grade_tint[channel] =
                clampf(
                    1.0f +
                        (out->colour_linear[channel] - 1.0f) * 0.08f,
                    0.980f, 1.020f);
        }
    }

    for (int channel = 0; channel < 3; channel++) {
        top_linear[channel] =
            gfx_level_light_srgb_to_linear(
                input->sky_top_rgb[channel] / 255.0f);
        bottom_linear[channel] =
            gfx_level_light_srgb_to_linear(
                input->sky_bottom_rgb[channel] / 255.0f);
    }
    top_luma = linear_luma(top_linear);
    bottom_luma = linear_luma(bottom_linear);

    if (input->weather_velocity[0] != 0 ||
        input->weather_velocity[2] != 0) {
        horizontal_x -= (float)input->weather_velocity[0];
        horizontal_z -= (float)input->weather_velocity[2];
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_WEATHER;
    }
    if (input->sky_scroll[0] != 0 || input->sky_scroll[1] != 0) {
        horizontal_x -= (float)input->sky_scroll[0] * 32.0f;
        horizontal_z -= (float)input->sky_scroll[1] * 32.0f;
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_SCROLL;
    }

    if (fabsf(horizontal_x) + fabsf(horizontal_z) < 0.5f) {
        /*
         * No authored directional signal exists for this world. A hash of the
         * level identity used to invent an azimuth here, but once cascaded
         * shadows made the sun direction plainly visible, an arbitrary
         * per-world compass heading read as shadows pointing somewhere
         * meaningless. Use the same canonical key-light heading as the
         * degenerate-normalize fallback so every uncued world shares one
         * consistent, art-plausible sun; worlds with weather drift or sky
         * scroll keep their derived azimuth above. GFX_LEVEL_LIGHT_SOURCE_HASH
         * now simply means "heading was invented, not authored".
         */
        horizontal_x = 0.35f;
        horizontal_z = 0.45f;
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_HASH;
    }

    /*
     * A brighter upper gradient implies a higher sun. All elevations stay
     * comfortably above the horizon so kart undersides do not flash as normals
     * interpolate and low-poly silhouettes remain legible.
     */
    float elevation =
        clampf(0.58f + (top_luma - bottom_luma) * 0.28f,
               0.48f, 0.82f);
    float horizontal_length =
        sqrtf(horizontal_x * horizontal_x +
              horizontal_z * horizontal_z);
    if (!(horizontal_length > 0.0f) || !isfinite(horizontal_length)) {
        horizontal_x = 1.0f;
        horizontal_z = 0.0f;
        horizontal_length = 1.0f;
    }
    float horizontal_scale =
        sqrtf(fmaxf(0.0f, 1.0f - elevation * elevation)) /
        horizontal_length;
    out->direction_world[0] = horizontal_x * horizontal_scale;
    out->direction_world[1] = elevation;
    out->direction_world[2] = horizontal_z * horizontal_scale;
    if (!normalize3(out->direction_world)) {
        out->direction_world[0] = 0.35f;
        out->direction_world[1] = 0.82f;
        out->direction_world[2] = 0.45f;
        (void)normalize3(out->direction_world);
        source_mask |= GFX_LEVEL_LIGHT_SOURCE_HASH;
    }

    /*
     * 10–16% additive diffuse is intentionally quieter than RL-1's diagnostic
     * 42% replacement. Dark worlds stay dark; bright skies gain slightly more
     * readable shape. Weather only nudges strength, never animates it.
     */
    out->strength = clampf(
        0.10f + out->source_luma * 0.055f +
            (input->weather_intensity / 255.0f) * 0.01f,
        0.10f, 0.16f);
    out->source_mask = source_mask;
    out->valid = true;
    return true;
}

bool gfx_level_lighting_publish(const GfxLevelLightingInput *input) {
    GfxLevelLightingRig candidate;
    if (!gfx_level_lighting_derive(input, &candidate)) {
        gfx_level_lighting_reset();
        return false;
    }
    s_current_rig = candidate;
    for (int channel = 0; channel < 3; channel++) {
        g_pc_sun_dir_world[channel] =
            s_current_rig.direction_world[channel];
        g_pc_sun_color_linear[channel] =
            s_current_rig.colour_linear[channel];
    }
    g_pcSunStrength = s_current_rig.strength;
    g_pcGradeLevelSat = s_current_rig.grade_saturation;
    g_pcGradeLevelCon = s_current_rig.grade_contrast;
    g_pcGradeLevelTintR = s_current_rig.grade_tint[0];
    g_pcGradeLevelTintG = s_current_rig.grade_tint[1];
    g_pcGradeLevelTintB = s_current_rig.grade_tint[2];
    g_pcGradeLevelValid = s_current_rig.grade_valid ? 1 : 0;
    g_pcLevelLightingValid = 1;
    return true;
}

void gfx_level_lighting_reset(void) {
    memset(&s_current_rig, 0, sizeof(s_current_rig));
    g_pc_sun_dir_world[0] = 0.0f;
    g_pc_sun_dir_world[1] = 1.0f;
    g_pc_sun_dir_world[2] = 0.0f;
    g_pc_sun_color_linear[0] = 1.0f;
    g_pc_sun_color_linear[1] = 1.0f;
    g_pc_sun_color_linear[2] = 1.0f;
    g_pcSunStrength = 0.0f;
    g_pcGradeLevelSat = 1.0f;
    g_pcGradeLevelCon = 1.0f;
    g_pcGradeLevelTintR = 1.0f;
    g_pcGradeLevelTintG = 1.0f;
    g_pcGradeLevelTintB = 1.0f;
    g_pcGradeLevelValid = 0;
    g_pcLevelLightingValid = 0;
}

const GfxLevelLightingRig *gfx_level_lighting_current(void) {
    return &s_current_rig;
}

static uint32_t pack_signed_10(float value) {
    int packed = (int)lroundf(clampf(value, -1.0f, 1.0f) * 511.0f);
    return (uint32_t)packed & 0x3ffu;
}

uint32_t gfx_level_lighting_pack_direction(const float direction[3]) {
    float normalized[3];
    if (direction == NULL) {
        return 0;
    }
    memcpy(normalized, direction, sizeof(normalized));
    if (!normalize3(normalized)) {
        return 0;
    }
    return pack_signed_10(normalized[0]) |
           (pack_signed_10(normalized[1]) << 10) |
           (pack_signed_10(normalized[2]) << 20);
}

static int unpack_signed_10(uint32_t value) {
    int result = (int)(value & 0x3ffu);
    if ((result & 0x200) != 0) {
        result -= 0x400;
    }
    return result;
}

bool gfx_level_lighting_unpack_direction(uint32_t packed, float out[3]) {
    if (out == NULL || packed == 0) {
        return false;
    }
    out[0] = unpack_signed_10(packed) / 511.0f;
    out[1] = unpack_signed_10(packed >> 10) / 511.0f;
    out[2] = unpack_signed_10(packed >> 20) / 511.0f;
    return normalize3(out);
}
