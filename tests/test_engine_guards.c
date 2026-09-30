/*
 * Native guards added in 1.8 to two engine functions, pinned by behaviour.
 *
 * White-box by design, like test_void_pairs.c: this target links the
 * PRODUCTION game/src/weather.c and game/src/thread3_main.c (dead-stripped to
 * weather_reset and clear_lap_records) and stubs the engine around them.
 * Each case replaces a source-text pin in check_runtime_safety.py and fails,
 * or trips ASan, when its guard is removed.
 *
 * weather_reset: a level header's weather type is signed. A negative type
 *   must return before the previous weather is torn down and before any
 *   preset is read (retail indexed gWeatherPresets[-n]); a type above rain
 *   keeps the retail fallback to rain.
 * clear_lap_records: ASSET_MISC_23 holds lap and course defaults for the
 *   retail courses only. Courses past the retail inventory (a custom course
 *   appended to the level table) are cleared to "no record" instead of
 *   reading defaults from past the end of the asset.
 */

#include "audio_spatial.h"
#include "math_util.h"
#include "memory.h"
#include "objects.h"
#include "textures_sprites.h"
#include "thread3_main.h"
#include "weather.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define EXPECT(cond, ...)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------------- */
/* Engine stubs                                                           */
/* ---------------------------------------------------------------------- */

static int sAllocs, sFrees, sTextureLoads, sTextureFrees;

void *mempool_alloc_safe(s32 size, u32 colourTag) {
    (void) colourTag;
    sAllocs++;
    void *block = calloc(1, (size_t) (size > 0 ? size : 1));
    if (block == NULL) abort();
    return block;
}

void mempool_free(void *data) {
    sFrees++;
    free(data);
}

static TextureHeader sTexture;
static Sprite sSprite;

TextureHeader *load_texture(s32 id) {
    (void) id;
    sTextureLoads++;
    sTexture.width = 16;
    sTexture.height = 16;
    return &sTexture;
}

Sprite *tex_load_sprite(s32 spriteID, s32 arg1) {
    (void) spriteID; (void) arg1;
    sTextureLoads++;
    return &sSprite;
}

void tex_free(TextureHeader *tex) {
    (void) tex;
    sTextureFrees++;
}

void sprite_free(Sprite *sprite) {
    (void) sprite;
    sTextureFrees++;
}

void audspat_point_stop(AudioPoint *point) {
    (void) point;
}

s32 coss_s16(s16 angle) {
    (void) angle;
    return 0;
}

s32 sins_s16(s16 angle) {
    (void) angle;
    return 0;
}

s32 rand_range(s32 min, s32 max) {
    (void) max;
    return min;
}

/* ---------------------------------------------------------------------- */
/* weather_reset                                                          */
/* ---------------------------------------------------------------------- */

extern WeatherData gWeather;
extern s32 *gWeatherAssetTable;
extern SnowPosData *gSnowPhysics;
extern s32 gWeatherType;

static void case_weather(void) {
    static s32 assets[4] = {1, 2, 3, 4};
    gWeatherAssetTable = assets;

    /* Control: a valid snow type tears down and rebuilds. */
    weather_reset(WEATHER_SNOW, 4, 11, 12, 13, 14, 15);
    EXPECT(gWeather.velX == 11 && gSnowPhysics != NULL, "snow weather was not set up");

    /* Control: a type above rain keeps the retail fallback to rain. */
    weather_reset(99, 4, 21, 22, 23, 24, 25);
    EXPECT(gWeather.velX == 21 && gWeatherType == WEATHER_RAIN, "high weather type did not fall back to rain");

    /* A negative type changes nothing: no teardown, no preset read. */
    weather_reset(WEATHER_SNOW, 4, 31, 32, 33, 34, 35);
    SnowPosData *physics = gSnowPhysics;
    const s32 negative[] = {-1, -2, -3, INT32_MIN};
    for (unsigned i = 0; i < sizeof negative / sizeof negative[0]; ++i) {
        int allocs = sAllocs, frees = sFrees, loads = sTextureLoads, texFrees = sTextureFrees;
        weather_reset(negative[i], 4, 41, 42, 43, 44, 45);
        EXPECT(sFrees == frees && sTextureFrees == texFrees && gSnowPhysics == physics,
               "weather type %d tore down the previous weather", (int) negative[i]);
        EXPECT(sAllocs == allocs && sTextureLoads == loads, "weather type %d built new weather",
               (int) negative[i]);
        EXPECT(gWeather.velX == 31 && gWeather.opacity == 35, "weather type %d changed the weather state",
               (int) negative[i]);
    }
}

/* ---------------------------------------------------------------------- */
/* clear_lap_records                                                      */
/* ---------------------------------------------------------------------- */

enum { RETAIL_LEVELS = 5, APPENDED = 2, ALL_LEVELS = RETAIL_LEVELS + APPENDED, VEHICLES = 3 };

void level_count(s32 *outLevelCount, s32 *outWorldCount) {
    *outLevelCount = ALL_LEVELS;
    *outWorldCount = 2;
}

void level_count_for_save(s32 *outLevelCount, s32 *outWorldCount) {
    *outLevelCount = RETAIL_LEVELS;
    *outWorldCount = 1;
}

/* Retail defaults, then words that stand for whatever follows the asset. */
static u16 sLapDefaults[ALL_LEVELS * 12];

s32 *get_misc_asset(s32 index) {
    if (index != ASSET_MISC_23) abort();
    return (s32 *) sLapDefaults;
}

typedef struct LapFixture {
    Settings settings;
    u16 rows[12][ALL_LEVELS];
} LapFixture;

static void lap_fixture(LapFixture *fixture) {
    memset(fixture, 0, sizeof *fixture);
    for (unsigned v = 0; v < VEHICLES; ++v) {
        fixture->settings.flapTimesPtr[v] = fixture->rows[v];
        fixture->settings.flapInitialsPtr[v] = fixture->rows[v + 3];
        fixture->settings.courseTimesPtr[v] = fixture->rows[v + 6];
        fixture->settings.courseInitialsPtr[v] = fixture->rows[v + 9];
    }
    for (unsigned row = 0; row < 12; ++row)
        for (int j = 0; j < ALL_LEVELS; ++j) fixture->rows[row][j] = 0x7777;
}

static void case_lap_records(void) {
    for (int j = 0; j < ALL_LEVELS; ++j)
        for (int k = 0; k < 12; ++k)
            sLapDefaults[j * 12 + k] = j < RETAIL_LEVELS ? (u16) (100 * j + k) : 0xABCD;

    for (s32 flags = 1; flags <= 3; ++flags) {
        LapFixture fixture;
        lap_fixture(&fixture);
        clear_lap_records(&fixture.settings, flags);
        for (unsigned v = 0; v < VEHICLES; ++v) {
            for (int j = 0; j < ALL_LEVELS; ++j) {
                const u16 *t = &sLapDefaults[j * 12 + v * 4];
                u16 flapTime = fixture.settings.flapTimesPtr[v][j];
                u16 flapInitials = fixture.settings.flapInitialsPtr[v][j];
                u16 courseTime = fixture.settings.courseTimesPtr[v][j];
                u16 courseInitials = fixture.settings.courseInitialsPtr[v][j];
                if (j < RETAIL_LEVELS) {
                    if (flags & 1)
                        EXPECT(flapTime == t[2] && flapInitials == t[3], "retail lap default %d/%u", j, v);
                    if (flags & 2)
                        EXPECT(courseTime == t[0] && courseInitials == t[1], "retail course default %d/%u", j, v);
                } else {
                    if (flags & 1)
                        EXPECT(flapTime == 0 && flapInitials == 0,
                               "appended course %d vehicle %u lap record read past the retail defaults (%04x)",
                               j, v, flapTime);
                    if (flags & 2)
                        EXPECT(courseTime == 0 && courseInitials == 0,
                               "appended course %d vehicle %u course record read past the retail defaults (%04x)",
                               j, v, courseTime);
                }
                if (!(flags & 1)) EXPECT(flapTime == 0x7777, "flags %d cleared lap records", (int) flags);
                if (!(flags & 2)) EXPECT(courseTime == 0x7777, "flags %d cleared course records", (int) flags);
            }
        }
    }
}

int main(void) {
    case_weather();
    case_lap_records();
    if (failures) {
        fprintf(stderr, "engine guards: FAIL (%d)\n", failures);
        return 1;
    }
    puts("engine guards: PASS");
    return 0;
}
