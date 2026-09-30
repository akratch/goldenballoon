/* Tests real codecs against retail ROM metadata plus synthetic appended courses.
 * Never publishes a package or reads/writes user save files. */
#ifdef NATIVE_PORT
#include "game.h"
#include "save_data.h"
#include "thread3_main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern LevelGlobalData *gGlobalLevelTable;
extern s32 gNumberOfLevelHeaders, gNumberOfWorlds;
extern u8 *D_801241EC;
extern u32 D_801241F0, D_801241F4;

enum { FIXTURE_LEVELS = 512, FIXTURE_WORLDS = 128, EXTRA_COURSES = 8 };
typedef struct SaveInventoryFixture {
    Settings settings;
    s32 progress[FIXTURE_LEVELS];
    s16 balloons[FIXTURE_WORLDS];
    u16 records[12][FIXTURE_LEVELS];
    LevelGlobalData table[FIXTURE_LEVELS];
} SaveInventoryFixture;
typedef struct GuardedProgress { u8 before[16], data[40], after[16]; } GuardedProgress;
typedef struct GuardedTimes { u8 before[16], data[384], after[16]; } GuardedTimes;

#define VERIFY(test) do { if (!(test)) { \
    fprintf(stderr, "[FATAL] save inventory fixture line %d\n", __LINE__); abort(); \
} } while (0)

static void verify_guard(const u8 *guard) {
    for (unsigned i = 0; i < 16; ++i) VERIFY(guard[i] == 0xA5);
}

void mdkr_save_inventory_selftest(void) {
    const char *enabled = getenv("MDKR_CUSTOM_SAVE_SELFTEST");
    if (!enabled || strcmp(enabled, "1")) return;
    s32 levels, worlds;
    level_count_for_save(&levels, &worlds);
    VERIFY(levels > 0 && levels <= FIXTURE_LEVELS - EXTRA_COURSES);
    VERIFY(worlds > 0 && worlds < FIXTURE_WORLDS);
    VERIFY(gNumberOfLevelHeaders == levels && gNumberOfWorlds == worlds);
    SaveInventoryFixture *fixture = calloc(1, sizeof(*fixture));
    VERIFY(fixture != NULL);
    Settings *settings = &fixture->settings;
    settings->courseFlagsPtr = fixture->progress;
    settings->balloonsPtr = fixture->balloons;
    for (unsigned v = 0; v < 3; ++v) {
        settings->flapTimesPtr[v] = fixture->records[v];
        settings->flapInitialsPtr[v] = fixture->records[v + 3];
        settings->courseTimesPtr[v] = fixture->records[v + 6];
        settings->courseInitialsPtr[v] = fixture->records[v + 9];
    }
    for (s32 i = 0; i < levels + EXTRA_COURSES; ++i) {
        fixture->progress[i] = (s32)(0x12340007u);
        for (unsigned row = 0; row < 12; ++row)
            fixture->records[row][i] = (u16)(100 + i + row * 512);
    }
    for (s32 i = 0; i <= worlds; ++i) fixture->balloons[i] = (s16)(i + 1);
    settings->tajFlags = 0x3F;
    settings->trophies = 0x2AA;
    settings->bosses = 0x321;
    settings->ttAmulet = 3;
    settings->wizpigAmulet = 4;
    settings->keys = 0x1E;
    settings->cutsceneFlags = 0x12345678;
    settings->filename = 0x4321;
    GuardedProgress baseline, expanded, roundtrip;
    GuardedTimes times, expandedTimes, roundtripTimes;
    memset(&baseline, 0xA5, sizeof baseline);
    memset(&expanded, 0xA5, sizeof expanded);
    memset(&roundtrip, 0xA5, sizeof roundtrip);
    memset(&times, 0xA5, sizeof times);
    memset(&expandedTimes, 0xA5, sizeof expandedTimes);
    memset(&roundtripTimes, 0xA5, sizeof roundtripTimes);
    u8 *savedCursor = D_801241EC;
    u32 savedByte = D_801241F0, savedBit = D_801241F4;
    func_800732E8(settings, baseline.data);
    func_800738A4(settings, times.data);
    LevelGlobalData *original = gGlobalLevelTable;
    memcpy(fixture->table, original, (size_t)levels * sizeof(*original));
    for (s32 i = levels; i < levels + EXTRA_COURSES; ++i) {
        fixture->table[i].raceType = RACETYPE_DEFAULT;
        fixture->table[i].world = (s8)worlds;
        fixture->table[i].vehicles = 0x70; /* all three playable vehicles */
    }
    gGlobalLevelTable = fixture->table;
    gNumberOfLevelHeaders = levels + EXTRA_COURSES;
    gNumberOfWorlds = worlds + 1;
    s32 visibleLevels, visibleWorlds;
    level_count(&visibleLevels, &visibleWorlds);
    VERIFY(visibleLevels == levels + EXTRA_COURSES && visibleWorlds == worlds + 1);
    func_800732E8(settings, expanded.data);
    func_800738A4(settings, expandedTimes.data);
    VERIFY(!memcmp(&baseline, &expanded, sizeof baseline));
    VERIFY(!memcmp(&times, &expandedTimes, sizeof times));
    populate_settings_from_save_data(settings, baseline.data);
    VERIFY(!settings->newGame);
    func_80073588(settings, times.data, 3);
    for (s32 i = levels; i < levels + EXTRA_COURSES; ++i) {
        VERIFY(fixture->progress[i] == 0);
        for (unsigned row = 0; row < 12; ++row) VERIFY(fixture->records[row][i] == 0);
    }
    func_800732E8(settings, roundtrip.data);
    func_800738A4(settings, roundtripTimes.data);
    VERIFY(!memcmp(&baseline, &roundtrip, sizeof baseline));
    VERIFY(!memcmp(&times, &roundtripTimes, sizeof times));
    verify_guard(baseline.before); verify_guard(baseline.after);
    verify_guard(times.before); verify_guard(times.after);
    gGlobalLevelTable = original;
    gNumberOfLevelHeaders = levels;
    gNumberOfWorlds = worlds;
    D_801241EC = savedCursor; D_801241F0 = savedByte; D_801241F4 = savedBit;
    free(fixture);
    fprintf(stderr, "[CUSTOM_SAVE] PASS appended=8 retail-bytes=identical guards=intact\n");
}
#endif
