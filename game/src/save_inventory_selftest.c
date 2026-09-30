/* Tests real codecs against retail ROM metadata plus synthetic appended courses,
 * and (MDKR_SAVE_COMPAT_FIXTURE) decodes a caller-named EEPROM image with them.
 * Never publishes a package or writes any file; never reads the user's saves. */
#ifdef NATIVE_PORT
#include "game.h"
#include "save_codec.h"
#include "save_data.h"
#include "thread3_main.h"
#include "endian_utils.h"
#include "fs_utf8.h"
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

static void bind_fixture_settings(SaveInventoryFixture *fixture) {
    Settings *settings = &fixture->settings;
    settings->courseFlagsPtr = fixture->progress;
    settings->balloonsPtr = fixture->balloons;
    for (unsigned v = 0; v < 3; ++v) {
        settings->flapTimesPtr[v] = fixture->records[v];
        settings->flapInitialsPtr[v] = fixture->records[v + 3];
        settings->courseTimesPtr[v] = fixture->records[v + 6];
        settings->courseInitialsPtr[v] = fixture->records[v + 9];
    }
}

static int block_is_blank(const u8 *bytes, size_t size) {
    size_t i;
    int zero = 1, erased = 1;
    for (i = 0; i < size; ++i) {
        zero &= bytes[i] == 0x00;
        erased &= bytes[i] == 0xFF;
    }
    return zero || erased;
}

static int report_reencode(const char *what, const u8 *expected, const u8 *actual, size_t size) {
    size_t i, differing = 0;
    for (i = 0; i < size; ++i) {
        if (expected[i] != actual[i]) {
            if (differing++ < 8) {
                fprintf(stderr, "[SAVE_COMPAT] %s differs byte=%u expected=0x%02x got=0x%02x\n", what,
                        (unsigned) i, expected[i], actual[i]);
            }
        }
    }
    if (differing != 0) {
        fprintf(stderr, "[SAVE_COMPAT] %s reencode=DIFFERENT bytes=%u differing=%u\n", what, (unsigned) size,
                (unsigned) differing);
        return 1;
    }
    fprintf(stderr, "[SAVE_COMPAT] %s reencode=identical bytes=%u\n", what, (unsigned) size);
    return 0;
}

/*
 * MDKR_SAVE_COMPAT_FIXTURE=<path to a 512-byte EEPROM image>
 *
 * Decode the image with the game's own retail codec -- the same
 * populate_settings_from_save_data()/func_80073588() calls read_save_file()
 * and read_eeprom_data() make -- print what was decoded, and re-encode it with
 * func_800732E8()/func_800738A4(). tests/check_save_v170_compat.py feeds it a
 * save the published 1.7.0 build wrote, so a changed layout (a different save
 * course count, a moved field) shows up as different progress or different
 * bytes instead of as a player's lost Adventure. Reads only the named file and
 * writes nothing; the bit cursor the codec shares is restored afterwards.
 */
static void save_compat_fixture(void) {
    const char *path = getenv("MDKR_SAVE_COMPAT_FIXTURE");
    u8 image[MDKR_SAVE_IMAGE_SIZE + 1];
    u8 slot[40], slotOut[40], times[384], timesOut[384];
    s32 levels, worlds, i, s, v;
    s32 slotsDecoded = 0, recordsDecoded = 0, configDecoded = 0, mismatches = 0;
    SaveInventoryFixture *fixture;
    Settings *settings;
    FILE *file;
    size_t got;
    u8 *savedCursor = D_801241EC;
    u32 savedByte = D_801241F0, savedBit = D_801241F4;

    if (path == NULL || path[0] == '\0') return;
    file = mdkr_fopen_utf8(path, "rb");
    got = file != NULL ? fread(image, 1, sizeof image, file) : 0;
    if (file != NULL) fclose(file);
    if (got != MDKR_SAVE_IMAGE_SIZE) {
        fprintf(stderr, "[SAVE_COMPAT] FAIL cannot read a %u-byte EEPROM image from %s (read %u)\n",
                MDKR_SAVE_IMAGE_SIZE, path, (unsigned) got);
        return;
    }
    level_count_for_save(&levels, &worlds);
    VERIFY(levels > 0 && levels <= FIXTURE_LEVELS && worlds > 0 && worlds < FIXTURE_WORLDS);
    fixture = calloc(1, sizeof(*fixture));
    VERIFY(fixture != NULL);
    settings = &fixture->settings;
    bind_fixture_settings(fixture);
    fprintf(stderr, "[SAVE_COMPAT] image levels=%d worlds=%d\n", levels, worlds);

    for (s = 0; s < 3; ++s) {
        char label[16];
        memcpy(slot, image + s * 40, sizeof slot);
        if (block_is_blank(slot, sizeof slot)) {
            fprintf(stderr, "[SAVE_COMPAT] slot=%d status=empty\n", s);
            continue;
        }
        populate_settings_from_save_data(settings, slot);
        if (settings->newGame) {
            fprintf(stderr, "[SAVE_COMPAT] slot=%d status=rejected\n", s);
            mismatches++;
            continue;
        }
        slotsDecoded++;
        fprintf(stderr,
                "[SAVE_COMPAT] slot=%d status=valid taj=%u trophies=%u bosses=%u ttAmulet=%u wizpigAmulet=%u "
                "keys=%u cutscenes=%u filename=%u\n",
                s, (unsigned) settings->tajFlags, (unsigned) settings->trophies, (unsigned) settings->bosses,
                (unsigned) settings->ttAmulet, (unsigned) settings->wizpigAmulet, (unsigned) settings->keys,
                (unsigned) settings->cutsceneFlags, (unsigned) settings->filename);
        fprintf(stderr, "[SAVE_COMPAT] slot=%d courses=", s);
        for (i = 0, v = 0; i < levels; ++i) {
            s32 raceType = leveltable_type(i);
            if (raceType == RACETYPE_DEFAULT || (raceType & RACETYPE_CHALLENGE) || raceType == RACETYPE_BOSS) {
                fprintf(stderr, "%s%d:%u", v++ ? "," : "", i,
                        (unsigned) mdkr_save_course_flags_to_status((u32) settings->courseFlagsPtr[i]));
            }
        }
        fprintf(stderr, "\n[SAVE_COMPAT] slot=%d balloons=", s);
        for (i = 0; i < worlds; ++i) fprintf(stderr, "%s%d", i ? "," : "", settings->balloonsPtr[i]);
        fprintf(stderr, "\n[SAVE_COMPAT] slot=%d worldFlags=", s);
        for (i = 0; i < worlds; ++i) {
            fprintf(stderr, "%s%u", i ? "," : "",
                    (unsigned) (((u32) settings->courseFlagsPtr[level_world_id(i)] >> 16) & 0xFFFF));
        }
        fprintf(stderr, "\n");
        memset(slotOut, 0xA5, sizeof slotOut);
        func_800732E8(settings, slotOut);
        snprintf(label, sizeof label, "slot=%d", s);
        mismatches += report_reencode(label, slot, slotOut, sizeof slot);
    }

    memcpy(times, image + 128, sizeof times);
    if (block_is_blank(times, 192) && block_is_blank(times + 192, 192)) {
        fprintf(stderr, "[SAVE_COMPAT] records status=empty\n");
    } else {
        s32 pairs = 0;
        size_t owned, b;
        recordsDecoded = 1;
        func_80073588(settings, times, 3);
        for (i = 0; i < levels; ++i) {
            if (leveltable_type(i) != RACETYPE_DEFAULT) continue;
            for (v = 0; v < 3; ++v) {
                if (!(leveltable_vehicle_usable(i) & (1 << v))) continue;
                pairs++;
                fprintf(stderr, "[SAVE_COMPAT] record level=%d vehicle=%d lap=%u lapInitials=%u course=%u "
                                "courseInitials=%u\n",
                        i, v, (unsigned) settings->flapTimesPtr[v][i], (unsigned) settings->flapInitialsPtr[v][i],
                        (unsigned) settings->courseTimesPtr[v][i], (unsigned) settings->courseInitialsPtr[v][i]);
            }
        }
        /* Each 192-byte block holds a checksum and one (time, initials) pair per
         * (course, vehicle); the bytes after the last pair are not the codec's.
         * write_eeprom_data() encodes into an uninitialised mempool buffer, so
         * retail persists whatever was there and sums it into the checksum.
         * Re-encode over the original bytes, as that carries the tail, and
         * separately prove over a poisoned buffer that every byte the codec
         * owns was written by it rather than carried. */
        owned = 2 + (size_t) pairs * 4;
        VERIFY(owned <= 192);
        memset(timesOut, 0xA5, sizeof timesOut);
        func_800738A4(settings, timesOut);
        for (b = 0; b < 2; ++b) {
            char label[24];
            snprintf(label, sizeof label, "records-owned=%u", (unsigned) b);
            mismatches += report_reencode(label, times + b * 192 + 2, timesOut + b * 192 + 2, owned - 2);
        }
        memcpy(timesOut, times, sizeof timesOut);
        func_800738A4(settings, timesOut);
        fprintf(stderr, "[SAVE_COMPAT] records pairs=%d owned=%u unowned-tail=%u\n", pairs, (unsigned) owned,
                (unsigned) (192 - owned));
        mismatches += report_reencode("records", times, timesOut, sizeof times);
    }

    if (block_is_blank(image + 120, 8)) {
        fprintf(stderr, "[SAVE_COMPAT] config status=empty\n");
    } else {
        u64 value = mdkr_read_be_u64(image + 120);
        u64 canonical = (value << 8) >> 8;
        u8 configOut[8];
        configDecoded = 1;
        canonical |= (u64) calculate_eeprom_settings_checksum(canonical) << 56;
        fprintf(stderr, "[SAVE_COMPAT] config value=0x%016llx checksum=%s\n", (unsigned long long) value,
                canonical == value ? "valid" : "INVALID");
        mdkr_write_be_u64(configOut, canonical);
        mismatches += report_reencode("config", image + 120, configOut, sizeof configOut);
    }

    D_801241EC = savedCursor;
    D_801241F0 = savedByte;
    D_801241F4 = savedBit;
    free(fixture);
    fprintf(stderr, "[SAVE_COMPAT] done slots=%d records=%d config=%d mismatches=%d\n", slotsDecoded,
            recordsDecoded, configDecoded, mismatches);
}

void mdkr_save_inventory_selftest(void) {
    const char *enabled;
    save_compat_fixture();
    enabled = getenv("MDKR_CUSTOM_SAVE_SELFTEST");
    if (!enabled || strcmp(enabled, "1")) return;
    s32 levels, worlds;
    level_count_for_save(&levels, &worlds);
    VERIFY(levels > 0 && levels <= FIXTURE_LEVELS - EXTRA_COURSES);
    VERIFY(worlds > 0 && worlds < FIXTURE_WORLDS);
    VERIFY(gNumberOfLevelHeaders == levels && gNumberOfWorlds == worlds);
    SaveInventoryFixture *fixture = calloc(1, sizeof(*fixture));
    VERIFY(fixture != NULL);
    Settings *settings = &fixture->settings;
    bind_fixture_settings(fixture);
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
