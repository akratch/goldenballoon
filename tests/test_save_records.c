/*
 * Time Trial record saves: deterministic bytes, and the retail inventory.
 *
 * White-box by design, like test_void_pairs.c: this target links the
 * PRODUCTION game/src/save_data.c (dead-stripped to the record writers and
 * codecs) and stubs the engine around it: the memory pool, the EEPROM, the
 * Controller Pak and the level table.
 *
 * 1. Each 192-byte record block holds a checksum and one (time, initials)
 *    pair per retail (course, vehicle): 2 + 47 * 4 = 190 bytes. The other two
 *    bytes, and the Controller Pak times file's unused tail, are not the
 *    codec's. The writers encode into a fresh memory-pool block, so those
 *    bytes used to be whatever the pool held before (and were summed into
 *    the checksum). Two sessions with the same records must write the same
 *    bytes whatever the pool held: here the pool hands out blocks filled
 *    with 0xA5, 0x00 and 0x5A.
 * 2. A block written the old way (residue in the tail, checksum over it)
 *    still loads with every record intact, as a 1.7.0 save does
 *    (save_v170_fixture and save_v170_compat pin that save itself).
 * 3. Every record and progress codec sizes itself from the retail inventory
 *    (level_count_for_save), never from the live level table, which may
 *    include appended courses: the encoded bytes cannot change and decoding
 *    cannot touch an appended course. This replaces a source-text count of
 *    level_count_for_save calls in check_runtime_safety.py.
 */

#include "game.h"
#include "memory.h"
#include "menu.h"
#include "save_data.h"
#include "save_layout.h"
#include "thread3_main.h"
#include "joypad.h"
#include "PR/os_cont.h"
#include "PR/os_eeprom.h"
#include "PR/os_motor.h"
#include "PR/os_pfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OS_TV_TYPE_NTSC 1 /* PRinternal/viint.h */

extern OSMesgQueue *sControllerMesgQueue;
extern OSPfs pfs[MAXCONTROLLERS];

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
/* Level inventory: 24 retail entries, plus 3 appended courses that only  */
/* the live table (level_count) would report.                             */
/* ---------------------------------------------------------------------- */

enum { RETAIL_LEVELS = 24, RETAIL_WORLDS = 3, APPENDED = 3, ALL_LEVELS = RETAIL_LEVELS + APPENDED };
enum { RETAIL_PAIRS = 47, OWNED = 2 + RETAIL_PAIRS * 4, BLOCK = 192 };

static s32 sSaveInventoryLevels = RETAIL_LEVELS;

void level_count_for_save(s32 *outLevelCount, s32 *outWorldCount) {
    *outLevelCount = sSaveInventoryLevels;
    *outWorldCount = RETAIL_WORLDS;
}

/* Not referenced by any codec today. A codec that switched to the live table
 * would link against this and see the appended courses. */
void level_count(s32 *outLevelCount, s32 *outWorldCount) {
    *outLevelCount = ALL_LEVELS;
    *outWorldCount = RETAIL_WORLDS + 1;
}

s8 leveltable_type(s32 mapId) {
    if (mapId < 0 || mapId >= ALL_LEVELS) abort();
    if (mapId < RETAIL_WORLDS) return RACETYPE_HUBWORLD;
    if (mapId == 20) return RACETYPE_BOSS;
    if (mapId == 21) return RACETYPE_CHALLENGE | 1;
    return RACETYPE_DEFAULT;
}

s32 leveltable_vehicle_usable(s32 mapId) {
    if (mapId < 0 || mapId >= ALL_LEVELS) abort();
    /* 15 three-vehicle courses and 2 car-only courses: 47 retail pairs. */
    if (mapId >= 3 && mapId < 18) return 0x7;
    if (mapId == 18 || mapId == 19) return 0x1;
    if (mapId < RETAIL_LEVELS) return 0;
    return 0x7; /* appended courses offer every vehicle */
}

s32 level_world_id(s32 worldId) {
    return worldId;
}

/* ---------------------------------------------------------------------- */
/* Engine stubs                                                           */
/* ---------------------------------------------------------------------- */

static u8 sPoison;

void *mempool_alloc_safe(s32 size, u32 colourTag) {
    (void) colourTag;
    u8 *block = malloc((size_t) size);
    if (block == NULL) abort();
    memset(block, sPoison, (size_t) size);
    return block;
}

void mempool_free(void *data) {
    free(data);
}

static u8 sEeprom[512];
static OSMesgQueue sQueue;

OSMesgQueue *si_mesg(void) {
    return &sQueue;
}

s32 osEepromProbe(OSMesgQueue *mq) {
    (void) mq;
    return 1;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int length) {
    (void) mq;
    if ((size_t) address * 8 + (size_t) length > sizeof sEeprom) abort();
    memcpy(sEeprom + (size_t) address * 8, buffer, (size_t) length);
    return 0;
}

s32 is_reset_pressed(void) {
    return FALSE;
}

s32 get_language(void) {
    return 0;
}

s32 osTvType = OS_TV_TYPE_NTSC;

s32 osContStartReadData(OSMesgQueue *mq) {
    (void) mq;
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flags) {
    (void) mq; (void) msg; (void) flags;
    return -1;
}

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pak, int channel) {
    (void) mq; (void) pak; (void) channel;
    return -1;
}

s32 osPfsInit(OSMesgQueue *mq, OSPfs *pak, int channel) {
    (void) mq; (void) pak; (void) channel;
    return 0;
}

s32 osPfsFreeBlocks(OSPfs *pak, s32 *bytes) {
    (void) pak;
    *bytes = 123 * 256;
    return 0;
}

s32 osPfsNumFiles(OSPfs *pak, s32 *maxFiles, s32 *usedFiles) {
    (void) pak;
    *maxFiles = 16;
    *usedFiles = 0;
    return 0;
}

s32 osPfsFileState(OSPfs *pak, s32 file, OSPfsState *state) {
    (void) pak; (void) file; (void) state;
    return PFS_ERR_INVALID;
}

s32 osPfsFindFile(OSPfs *pak, u16 company, u32 game, u8 *name, u8 *ext, s32 *file) {
    (void) pak; (void) company; (void) game; (void) name; (void) ext; (void) file;
    return PFS_ERR_INVALID;
}

s32 osPfsAllocateFile(OSPfs *pak, u16 company, u32 game, u8 *name, u8 *ext, int bytes, s32 *file) {
    (void) pak; (void) company; (void) game; (void) name; (void) ext; (void) bytes;
    *file = 3;
    return 0;
}

static u8 sPakFile[512];
static int sPakBytes;

s32 osPfsReadWriteFile(OSPfs *pak, s32 file, u8 flag, int offset, int bytes, u8 *data) {
    (void) pak; (void) file;
    if (flag != PFS_WRITE || offset != 0 || bytes < 0 || bytes > (int) sizeof sPakFile) abort();
    memcpy(sPakFile, data, (size_t) bytes);
    sPakBytes = bytes;
    return 0;
}

/* The progress and lap-record resets live in thread3_main.c; the decoders
 * call them first. */
static Settings *sResetSettings;
void clear_lap_records(Settings *settings, s32 flags) {
    (void) flags;
    sResetSettings = settings;
}

void clear_game_progress(Settings *settings) {
    (void) settings;
}

/* ---------------------------------------------------------------------- */
/* Settings fixture                                                       */
/* ---------------------------------------------------------------------- */

typedef struct Fixture {
    Settings settings;
    s32 progress[ALL_LEVELS];
    s16 balloons[RETAIL_WORLDS + 2];
    u16 records[12][ALL_LEVELS];
} Fixture;

static void fixture_bind(Fixture *fixture) {
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

static void fixture_fill(Fixture *fixture) {
    memset(fixture, 0, sizeof *fixture);
    fixture_bind(fixture);
    for (int i = 0; i < ALL_LEVELS; ++i) {
        fixture->progress[i] = (i % 3) | ((i < RETAIL_WORLDS) ? 0x00050000 : 0);
        for (unsigned row = 0; row < 12; ++row) fixture->records[row][i] = (u16) (1000 + i * 17 + row * 211);
    }
    for (int w = 0; w < RETAIL_WORLDS + 2; ++w) fixture->balloons[w] = (s16) (w + 2);
    fixture->settings.tajFlags = 0x0B; /* car beaten, so car offered */
    fixture->settings.trophies = 0x155;
    fixture->settings.bosses = 0x3C3;
    fixture->settings.ttAmulet = 2;
    fixture->settings.wizpigAmulet = 3;
    fixture->settings.keys = 0x0E;
    fixture->settings.cutsceneFlags = 0x00040000;
    fixture->settings.filename = 0x1234;
}

static int block_checksum_valid(const u8 *block) {
    s16 sum = 5;
    for (int i = 2; i < BLOCK; ++i) sum += block[i];
    return (u16) sum == (u16) ((block[0] << 8) | block[1]);
}

/* ---------------------------------------------------------------------- */
/* 1. Deterministic record bytes                                          */
/* ---------------------------------------------------------------------- */

static void eeprom_session(u8 poison, u8 out[2 * BLOCK]) {
    Fixture fixture;
    fixture_fill(&fixture);
    memset(sEeprom, 0xEE, sizeof sEeprom);
    sPoison = poison;
    EXPECT(write_eeprom_data(&fixture.settings, SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES) == 0,
           "write_eeprom_data failed");
    memcpy(out, sEeprom + FASTEST_LAPS_START, 2 * BLOCK);
}

static void pak_session(u8 poison, u8 out[512]) {
    Fixture fixture;
    static OSMesgQueue queue;
    fixture_fill(&fixture);
    memset(&queue, 0, sizeof queue);
    sControllerMesgQueue = &queue;
    pfs[0].status |= PFS_INITIALIZED;
    memset(sPakFile, 0xEE, sizeof sPakFile);
    sPakBytes = 0;
    sPoison = poison;
    s32 status = write_time_data_to_controller_pak(0, &fixture.settings);
    EXPECT(status == CONTROLLER_PAK_GOOD, "Controller Pak times write failed: 0x%x", (unsigned) status);
    EXPECT(sPakBytes == get_time_data_file_size(), "Controller Pak times file wrote %d bytes", sPakBytes);
    memcpy(out, sPakFile, 512);
}

static void case_deterministic_bytes(void) {
    static const u8 poisons[] = {0xA5, 0x00, 0x5A};
    u8 first[2 * BLOCK], again[2 * BLOCK];
    eeprom_session(poisons[0], first);
    for (unsigned b = 0; b < 2; ++b) {
        const u8 *block = first + b * BLOCK;
        EXPECT(block_checksum_valid(block), "EEPROM record block %u checksum", b);
        EXPECT(block[OWNED] == 0 && block[OWNED + 1] == 0,
               "EEPROM record block %u tail is %02x %02x, not zero", b, block[OWNED], block[OWNED + 1]);
    }
    for (unsigned p = 1; p < sizeof poisons; ++p) {
        eeprom_session(poisons[p], again);
        for (unsigned b = 0; b < 2; ++b) {
            const u8 *block = again + b * BLOCK;
            EXPECT(!memcmp(first + b * BLOCK, block, BLOCK),
                   "EEPROM record block %u differs after a pool that held 0x%02x (tail %02x %02x)", b,
                   poisons[p], block[OWNED], block[OWNED + 1]);
        }
    }

    u8 pakFirst[512], pakAgain[512];
    pak_session(poisons[0], pakFirst);
    EXPECT(!memcmp(pakFirst + 4, first, 2 * BLOCK), "Controller Pak records differ from the EEPROM records");
    for (unsigned i = 4 + 2 * BLOCK; i < 512; ++i)
        if (pakFirst[i] != 0) {
            EXPECT(0, "Controller Pak times file byte %u is %02x, not zero", i, pakFirst[i]);
            break;
        }
    for (unsigned p = 1; p < sizeof poisons; ++p) {
        pak_session(poisons[p], pakAgain);
        EXPECT(!memcmp(pakFirst, pakAgain, 512),
               "Controller Pak times file differs after a pool that held 0x%02x", poisons[p]);
    }
}

/* ---------------------------------------------------------------------- */
/* 2. Old residue still loads                                             */
/* ---------------------------------------------------------------------- */

static void case_residue_block_loads(void) {
    Fixture written, loaded;
    u8 blocks[2 * BLOCK];
    fixture_fill(&written);
    /* Encode over residue, as 1.7.0 did: the tail keeps 0x0a 0x00 / 0xc3 0x00
     * and the checksum covers it. */
    memset(blocks, 0, sizeof blocks);
    blocks[OWNED] = 0x0a;
    blocks[BLOCK + OWNED] = 0xc3;
    func_800738A4(&written.settings, blocks);
    EXPECT(blocks[OWNED] == 0x0a && blocks[BLOCK + OWNED] == 0xc3, "codec overwrote bytes it does not own");
    EXPECT(block_checksum_valid(blocks) && block_checksum_valid(blocks + BLOCK), "residue checksum");

    fixture_fill(&loaded);
    for (unsigned row = 0; row < 12; ++row)
        for (int i = 0; i < ALL_LEVELS; ++i) loaded.records[row][i] = 0;
    func_80073588(&loaded.settings, blocks, SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES);
    for (int i = 0; i < RETAIL_LEVELS; ++i) {
        if (leveltable_type(i) != RACETYPE_DEFAULT) continue;
        for (unsigned v = 0; v < 3; ++v) {
            if (!(leveltable_vehicle_usable(i) & (1 << v))) continue;
            for (unsigned kind = 0; kind < 4; ++kind) {
                unsigned row = v + kind * 3;
                EXPECT(loaded.records[row][i] == written.records[row][i],
                       "residue block: level %d vehicle %u row %u read %u, wrote %u", i, v, row,
                       loaded.records[row][i], written.records[row][i]);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* 3. Codecs use the retail inventory                                     */
/* ---------------------------------------------------------------------- */

static void case_retail_inventory(void) {
    Fixture fixture;
    u8 slot[40], slotAgain[40], times[2 * BLOCK], timesAgain[2 * BLOCK];

    fixture_fill(&fixture);
    memset(slot, 0, sizeof slot);
    memset(times, 0, sizeof times);
    func_800732E8(&fixture.settings, slot);
    func_800738A4(&fixture.settings, times);

    /* The appended courses carry progress and records of their own; the
     * retail encoders must not see them. */
    for (int i = RETAIL_LEVELS; i < ALL_LEVELS; ++i) {
        fixture.progress[i] = 3;
        for (unsigned row = 0; row < 12; ++row) fixture.records[row][i] = 0xBEEF;
    }
    memset(slotAgain, 0, sizeof slotAgain);
    memset(timesAgain, 0, sizeof timesAgain);
    func_800732E8(&fixture.settings, slotAgain);
    func_800738A4(&fixture.settings, timesAgain);
    EXPECT(!memcmp(slot, slotAgain, sizeof slot), "progress encoding reads appended courses");
    EXPECT(!memcmp(times, timesAgain, sizeof times), "record encoding reads appended courses");

    /* The same bytes must be the ones a retail-only table produces: an
     * encoder sized from anything larger would place more pairs. */
    EXPECT(times[OWNED] == 0 && times[OWNED + 1] == 0 && times[BLOCK + OWNED] == 0,
           "record encoding wrote past the %d retail pairs", RETAIL_PAIRS);

    /* Decoding restores retail entries and leaves appended ones alone. */
    Fixture decoded;
    fixture_fill(&decoded);
    for (int i = 0; i < ALL_LEVELS; ++i) {
        decoded.progress[i] = 0x7777;
        for (unsigned row = 0; row < 12; ++row) decoded.records[row][i] = 0x7777;
    }
    populate_settings_from_save_data(&decoded.settings, slot);
    func_80073588(&decoded.settings, times, SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES);
    for (int i = RETAIL_LEVELS; i < ALL_LEVELS; ++i) {
        EXPECT(decoded.progress[i] == 0x7777, "progress decoding wrote appended course %d", i);
        for (unsigned row = 0; row < 12; ++row)
            EXPECT(decoded.records[row][i] == 0x7777, "record decoding wrote appended course %d row %u", i, row);
    }
    EXPECT(decoded.records[0][3] == fixture.records[0][3], "record decoding lost a retail record");
    /* Every field after the course area must come back as written: a decoder
     * sized from a different inventory reads them from shifted bits. */
    const Settings *in = &fixture.settings, *out = &decoded.settings;
    EXPECT(out->tajFlags == in->tajFlags && out->trophies == in->trophies && out->bosses == in->bosses,
           "progress decoding misread taj/trophies/bosses: %x/%x/%x", out->tajFlags, out->trophies, out->bosses);
    for (int w = 0; w < RETAIL_WORLDS; ++w)
        EXPECT(out->balloonsPtr[w] == in->balloonsPtr[w], "progress decoding misread world %d balloons", w);
    EXPECT(out->ttAmulet == in->ttAmulet && out->wizpigAmulet == in->wizpigAmulet,
           "progress decoding misread the amulets");
    EXPECT(out->keys == in->keys && out->cutsceneFlags == in->cutsceneFlags && out->filename == in->filename,
           "progress decoding misread keys/cutscenes/name: %x/%x/%x", out->keys, (unsigned) out->cutsceneFlags,
           out->filename);

    /* Positive control: an inventory that includes the appended courses
     * does encode them, so the comparisons above can fail. */
    static u8 larger[1024]; /* 56 pairs no longer fit two 192-byte blocks */
    sSaveInventoryLevels = ALL_LEVELS;
    func_800738A4(&fixture.settings, larger);
    sSaveInventoryLevels = RETAIL_LEVELS;
    EXPECT(memcmp(times, larger, sizeof times) != 0, "control: a larger inventory changed nothing");
}

int main(void) {
    case_deterministic_bytes();
    case_residue_block_loads();
    case_retail_inventory();
    if (failures) {
        fprintf(stderr, "save records: FAIL (%d)\n", failures);
        return 1;
    }
    puts("save records: PASS");
    return 0;
}
