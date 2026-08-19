/**
 * ghost_bank.h -- per-(level, vehicle) Time Trial ghost bank (native port).
 *
 * The authored DKRACING-GHOSTS Controller Pak note holds exactly six ghost
 * slots (DKR_GHOST_SLOT_COUNT, game/src/save_data.c), each keyed by a
 * (levelId, vehicleId) pair. That six-pair budget is the original ROM's own
 * file format, and once six distinct pairs exist every further pair reports
 * CONTROLLER_PAK_NO_ROOM_FOR_GHOSTS -- rendered as the generic "CONTROLLER
 * PAK FULL" dialog (issue #46).
 *
 * This module keeps that authored format, size, and every byte of
 * game/src/save_data.c untouched, and instead maintains one small bank file
 * per pair OUTSIDE the pak, under <save>/ghost-bank/. Before the game loads
 * or saves a pair's ghost, mdkr_ghost_bank_select() makes sure that pair has
 * a slot in the live six-slot window: a pair already present is a strict
 * no-op; otherwise the least-recently-used slot is flushed to its bank file
 * and the requested pair's banked bytes (verbatim, so the record round-trips
 * byte-identically) are spliced into the freed slot -- or the slot is simply
 * left empty for a pair that has never recorded a ghost. The game's own
 * GHSS read/write/checksum code always sees a spec-valid six-slot file, so
 * exactly one best-time ghost is loaded per race, with bytes identical to
 * what the authored path would load; only the number of PAIRS that can
 * coexist across sessions changes. The authored PAK FULL dialog stays wired
 * and still fires for genuine device/IO failures.
 *
 * A per-controller index sidecar records which pairs the bank last saw in
 * the window plus their LRU ticks. Its "in window" flags are what make the
 * authored delete paths (the pak menu's per-slot ghost erase,
 * save_data.c func_800753D8, and whole-note deletion/reformat) stick: a pair
 * the index says was in the window but is no longer there was erased by the
 * game, so its bank file is deleted rather than resurrected. The sidecar is
 * bookkeeping, never the source of truth for what exists: sweeps read the
 * bank DIRECTORY itself, so losing the sidecar fails toward the same
 * deletion semantics rather than toward resurrection — a record file
 * neither the live window nor the index can vouch for is quarantined, and a
 * missing note (deletion, reformat, or a quarantined-and-recreated pak
 * image) quarantines every record the sweep finds instead of unlinking
 * them. Quarantined files are never loaded again but remain recoverable by
 * hand, so a pak-side I/O accident cannot silently destroy the library.
 *
 * On-disk formats follow platform/virtual_pak.c's discipline: magic/version/
 * size validation, a SHA-256 digest over the whole image, copy-on-write
 * temp-file writes that only become live via an atomic replace, and
 * quarantine (rename to .bad.N) of anything that fails validation.
 *
 * The pure window/record/index logic below performs no host I/O so it can be
 * unit-tested directly (tests/test_ghost_bank.c); mdkr_ghost_bank_select()
 * is the only entry point the game-side hooks call.
 */
#ifndef MDKR_GHOST_BANK_H
#define MDKR_GHOST_BANK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The authored GHSS geometry, restated from game/src/save_data.c (which this
 * module deliberately does not include: the pure core must stay linkable
 * without game sources). save_data.c is the authority; these mirror
 * DKR_GHOST_SLOT_COUNT, GHSS_SIZE, and get_ghost_data_file_size(). */
#define MDKR_GHOST_WINDOW_SLOTS 6
#define MDKR_GHOST_WINDOW_DIRECTORY_BYTES 0x100
#define MDKR_GHOST_WINDOW_SLOT_BYTES 0x1100
#define MDKR_GHOST_WINDOW_BYTES 0x6700
#define MDKR_GHOST_RECORD_MAX_BYTES MDKR_GHOST_WINDOW_SLOT_BYTES

/* Bank record image: 64-byte header + payload (one window slot extent). */
#define MDKR_GHOST_BANK_RECORD_HEADER_BYTES 64
#define MDKR_GHOST_BANK_RECORD_IMAGE_MAX \
    (MDKR_GHOST_BANK_RECORD_HEADER_BYTES + MDKR_GHOST_RECORD_MAX_BYTES)

/* Index sidecar image: 64-byte header + 16 bytes per entry. */
#define MDKR_GHOST_BANK_INDEX_HEADER_BYTES 64
#define MDKR_GHOST_BANK_INDEX_ENTRY_BYTES 16
#define MDKR_GHOST_BANK_INDEX_MAX_ENTRIES 128
#define MDKR_GHOST_BANK_INDEX_IMAGE_MAX             \
    (MDKR_GHOST_BANK_INDEX_HEADER_BYTES +           \
     MDKR_GHOST_BANK_INDEX_MAX_ENTRIES * MDKR_GHOST_BANK_INDEX_ENTRY_BYTES)

typedef enum {
    MDKR_GHOST_BANK_OK = 0,
    MDKR_GHOST_BANK_ERR_ARGUMENT,
    MDKR_GHOST_BANK_ERR_FORMAT,
    MDKR_GHOST_BANK_ERR_DIGEST,
    MDKR_GHOST_BANK_ERR_RANGE,
    MDKR_GHOST_BANK_ERR_FULL,
    MDKR_GHOST_BANK_ERR_NOT_FOUND
} MdkrGhostBankResult;

/* ---- Pure GHSS window operations (no I/O) --------------------------------
 * `window`/`size` is the full DKRACING-GHOSTS note as the native game laid it
 * out in the virtual pak: host-endian directory of seven 4-byte entries after
 * the 32-bit 'GHSS' signature, then the slot payloads. All mutating
 * operations rebuild the window in the canonical shape the authored writers
 * produce (occupied slots as a compact prefix, uniform empty-entry tail,
 * zeroed slack). */

/* Structural directory validation, same bounds ghost_directory_is_valid()
 * enforces in save_data.c. Returns MDKR_GHOST_BANK_OK for a usable window. */
MdkrGhostBankResult mdkr_ghost_window_validate(
    const uint8_t *window, size_t size);

/* Slot index holding (level, vehicle), or -1. */
int mdkr_ghost_window_find(
    const uint8_t *window, size_t size, int level, int vehicle);

/* Number of occupied slots (0..6), or -1 on an invalid window. */
int mdkr_ghost_window_occupied(const uint8_t *window, size_t size);

/* Pair stored in `slot`. Fails on an empty or out-of-range slot. */
MdkrGhostBankResult mdkr_ghost_window_pair_at(
    const uint8_t *window, size_t size, int slot,
    int *level, int *vehicle);

/* Copy `slot`'s whole extent (record plus its authored padding) out
 * verbatim, so a later insert restores the identical bytes. */
MdkrGhostBankResult mdkr_ghost_window_extract(
    const uint8_t *window, size_t size, int slot,
    uint8_t *payload, size_t capacity, size_t *length);

/* Remove `slot` and rebuild canonically. */
MdkrGhostBankResult mdkr_ghost_window_remove(
    uint8_t *window, size_t size, int slot);

/* Splice a pair into the first free slot; `payload`/`length` are the verbatim
 * extent bytes a prior extract produced. Fails when the directory or byte
 * budget is exhausted or the pair is already present. */
MdkrGhostBankResult mdkr_ghost_window_insert(
    uint8_t *window, size_t size, int level, int vehicle,
    const uint8_t *payload, size_t length);

/* ---- Pure bank record codec (no I/O) ------------------------------------ */

MdkrGhostBankResult mdkr_ghost_bank_record_encode(
    int level, int vehicle, const uint8_t *payload, size_t length,
    uint8_t *image, size_t capacity, size_t *image_size);

MdkrGhostBankResult mdkr_ghost_bank_record_decode(
    const uint8_t *image, size_t image_size, int *level, int *vehicle,
    uint8_t *payload, size_t capacity, size_t *length);

/* ---- Pure index (LRU + window membership) codec and bookkeeping --------- */

typedef struct {
    uint8_t level;
    uint8_t vehicle;
    uint8_t in_window;
    uint64_t tick;
} MdkrGhostBankIndexEntry;

typedef struct {
    uint64_t tick_counter;
    uint32_t count;
    MdkrGhostBankIndexEntry entries[MDKR_GHOST_BANK_INDEX_MAX_ENTRIES];
} MdkrGhostBankIndex;

void mdkr_ghost_bank_index_init(MdkrGhostBankIndex *index);

MdkrGhostBankResult mdkr_ghost_bank_index_encode(
    const MdkrGhostBankIndex *index, uint8_t *image, size_t capacity,
    size_t *image_size);

MdkrGhostBankResult mdkr_ghost_bank_index_decode(
    const uint8_t *image, size_t image_size, MdkrGhostBankIndex *index);

/* Entry position for a pair, or -1. */
int mdkr_ghost_bank_index_find(
    const MdkrGhostBankIndex *index, int level, int vehicle);

/* Upsert a pair, stamp it with the next LRU tick, set its window flag. */
MdkrGhostBankResult mdkr_ghost_bank_index_touch(
    MdkrGhostBankIndex *index, int level, int vehicle, int in_window);

/* Set the window flag without consuming an LRU tick. */
MdkrGhostBankResult mdkr_ghost_bank_index_set_in_window(
    MdkrGhostBankIndex *index, int level, int vehicle, int in_window);

MdkrGhostBankResult mdkr_ghost_bank_index_remove(
    MdkrGhostBankIndex *index, int level, int vehicle);

/* The occupied window slot to evict: the smallest LRU tick over the window's
 * occupied pairs (a pair the index has never seen counts as oldest of all;
 * ties break toward the lowest slot). Returns -1 when the window has no
 * occupied slot or is invalid. */
int mdkr_ghost_bank_pick_victim(
    const uint8_t *window, size_t size, const MdkrGhostBankIndex *index);

/* ---- Host entry points --------------------------------------------------- */

/* Make sure (levelId, vehicleId) can be loaded/saved through the authored
 * six-slot path on this controller, banking the LRU pair first if the window
 * is full. Best-effort: every failure leaves the pak, the bank, and the
 * authored behavior exactly as they were, so callers ignore the result in
 * shipping paths. Returns 0 when the pair now has (or already had) a live
 * slot or an empty slot to claim, non-zero otherwise. */
int mdkr_ghost_bank_select(int controllerIndex, int levelId, int vehicleId);

/* Test seams. `mdkr_ghost_bank_set_root(NULL)` restores the default
 * <save>/ghost-bank resolution; reset drops per-process caches. */
void mdkr_ghost_bank_set_root(const char *directory);
void mdkr_ghost_bank_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_GHOST_BANK_H */
