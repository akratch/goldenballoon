/*
 * gfx_level_lighting.h — deterministic, ROM-authored level lighting policy.
 *
 * The game adapts its LevelHeader/LevelHeader_70 records into the pointer-free
 * input below. Keeping the derivation independent of game structs makes the
 * colour and direction policy unit-testable without a ROM, allocator, or GPU.
 */
#ifndef GFX_LEVEL_LIGHTING_H
#define GFX_LEVEL_LIGHTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_LEVEL_LIGHT_MAX_CYCLES 7

typedef struct GfxLevelLightCycle {
    uint8_t current_rgba[4];
    uint8_t authored_rgba[4];
    bool valid;
} GfxLevelLightCycle;

typedef struct GfxLevelLightingInput {
    int world_id;
    int geometry_id;
    int skybox_id;
    int fog_near;
    int fog_far;
    uint8_t fog_rgb[3];
    uint8_t background_rgb[3];
    uint8_t sky_top_rgb[3];
    uint8_t sky_bottom_rgb[3];
    int weather_velocity[3];
    uint8_t weather_intensity;
    int8_t sky_scroll[2];
    GfxLevelLightCycle cycles[GFX_LEVEL_LIGHT_MAX_CYCLES];
} GfxLevelLightingInput;

typedef struct GfxLevelLightingRig {
    float direction_world[3]; /* normalized direction from surface TO sun */
    float colour_linear[3];   /* restrained chromatic illuminant, linear RGB */
    float strength;           /* additive diffuse fraction, [0, 0.20] */
    float source_luma;        /* diagnostic authored-source luma, linear */
    float grade_saturation;   /* bounded world finish, [0.995, 1.025] */
    float grade_contrast;     /* linear-light contrast, [0.990, 1.020] */
    float grade_tint[3];      /* restrained linear RGB tint, [0.980, 1.020] */
    uint32_t source_mask;
    int world_id;
    int geometry_id;
    bool grade_valid;         /* false means publish an exactly neutral grade */
    bool valid;
} GfxLevelLightingRig;

enum {
    GFX_LEVEL_LIGHT_SOURCE_BACKGROUND = 1u << 0,
    GFX_LEVEL_LIGHT_SOURCE_SKY        = 1u << 1,
    GFX_LEVEL_LIGHT_SOURCE_FOG        = 1u << 2,
    GFX_LEVEL_LIGHT_SOURCE_CYCLE      = 1u << 3,
    GFX_LEVEL_LIGHT_SOURCE_WEATHER    = 1u << 4,
    GFX_LEVEL_LIGHT_SOURCE_SCROLL     = 1u << 5,
    GFX_LEVEL_LIGHT_SOURCE_HASH       = 1u << 6,
};

float gfx_level_light_srgb_to_linear(float encoded);
float gfx_level_light_linear_to_srgb(float linear);

bool gfx_level_lighting_derive(const GfxLevelLightingInput *input,
                               GfxLevelLightingRig *out);
bool gfx_level_lighting_publish(const GfxLevelLightingInput *input);
void gfx_level_lighting_reset(void);
const GfxLevelLightingRig *gfx_level_lighting_current(void);

/*
 * Packs a normalized object-local direction into three signed 10-bit fields.
 * This value is carried in a native-only F3DDKR MoveWord, so a queued display
 * list retains the direction that belonged to that object even if another
 * object is built before the renderer consumes it.
 */
uint32_t gfx_level_lighting_pack_direction(const float direction[3]);
bool gfx_level_lighting_unpack_direction(uint32_t packed, float out[3]);

#ifdef __cplusplus
}
#endif

#endif /* GFX_LEVEL_LIGHTING_H */
