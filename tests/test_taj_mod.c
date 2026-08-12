#include "taj_mod.h"

#include <stdio.h>
#include <string.h>

typedef struct MemoryStore {
    char text[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t length;
    int read_result;
    int write_result;
} MemoryStore;

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expression); \
        failures++; \
    } \
} while (0)

static int memory_read(void *context, char *text, size_t capacity, size_t *length) {
    MemoryStore *store = context;
    if (store->read_result != 1) return store->read_result;
    if (store->length >= capacity) return -1;
    memcpy(text, store->text, store->length);
    *length = store->length;
    return 1;
}

static int memory_write(void *context, const char *text, size_t length) {
    MemoryStore *store = context;
    if (store->write_result != 1 || length >= sizeof(store->text)) {
        return store->write_result;
    }
    memcpy(store->text, text, length);
    store->length = length;
    return 1;
}

static TajModStateStorage storage_for(MemoryStore *store) {
    TajModStateStorage storage = { store, memory_read, memory_write };
    return storage;
}

static int store_has_state(const MemoryStore *store, int unlocked,
                           int migration_complete) {
    TajModPersistentState state;

    taj_mod_state_defaults(&state);
    return store != NULL &&
           taj_mod_state_parse(&state, store->text, store->length) &&
           state.taj_unlocked == unlocked &&
           state.adventure_migration_complete == migration_complete;
}

static void test_state_format(void) {
    TajModPersistentState state;
    char text[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t length;
    static const char terminated[] =
        "taj_mod_version=1\ntaj_unlocked=1\n"
        "adventure_migration_complete=1\n";
    static const char crlf_terminated[] =
        "taj_mod_version=1\ntaj_unlocked=1\n"
        "adventure_migration_complete=1\r\n";
    static const char extra_blank_line[] =
        "taj_mod_version=1\ntaj_unlocked=1\n"
        "adventure_migration_complete=1\n\n";
    static const char version_two[] =
        "mod_roster_version=2\ntaj_unlocked=1\n"
        "taj_migration_complete=1\nwizpig_unlocked=1\n"
        "wizpig_migration_complete=1";

    taj_mod_state_defaults(&state);
    state.taj_unlocked = 1;
    state.adventure_migration_complete = 1;
    CHECK(taj_mod_state_serialize(&state, text, sizeof(text), &length));
    taj_mod_state_defaults(&state);
    CHECK(taj_mod_state_parse(&state, text, length));
    CHECK(state.version == TAJ_MOD_STATE_VERSION && state.taj_unlocked == 1 &&
          state.adventure_migration_complete == 1);
    CHECK(strstr(text, "mod_roster_version=3") != NULL);
    CHECK(strstr(text, "wizpig_unlocked=0") != NULL);
    CHECK(strstr(text, "terry_unlocked=0") != NULL);
    CHECK(taj_mod_state_parse(&state, version_two,
                              sizeof(version_two) - 1));
    CHECK(state.version == TAJ_MOD_STATE_VERSION && state.taj_unlocked == 1 &&
          state.wizpig_unlocked == 1 && state.terry_unlocked == 0);
    CHECK(taj_mod_state_parse(&state, terminated,
                              sizeof(terminated) - 1));
    CHECK(taj_mod_state_parse(&state, crlf_terminated,
                              sizeof(crlf_terminated) - 1));
    CHECK(!taj_mod_state_parse(&state, extra_blank_line,
                               sizeof(extra_blank_line) - 1));
    CHECK(!taj_mod_state_parse(&state, "taj_mod_version=2\ntaj_unlocked=1", 33));
    CHECK(!taj_mod_state_parse(&state, "taj_mod_version=1\ntaj_unlocked=2", 33));
}

/* The Magic Codes list appends one row per unlocked bonus identity after the retail rows. Every
 * unlock combination must map its rows onto exactly the unlocked identities, with no identity
 * unreachable and no row resolving to MOD_RACER_RETAIL.
 *
 * Regression: with Taj and Wizpig unlocked, Terry's row used to resolve to MOD_RACER_RETAIL, which
 * rendered a "CONTROL TERRY" label that read OFF forever and whose toggle did nothing. */
static void test_cheat_row_identity_mapping(void) {
    static const char *const codes[3] = { "ABRACADABRA", "WIZPIGPOWER", "TERRYFLY" };
    static const ModRacerIdentity identities[3] = { MOD_RACER_TAJ, MOD_RACER_WIZPIG,
                                                    MOD_RACER_TERRY };
    int combo;

    for (combo = 0; combo < 8; combo++) {
        MemoryStore store = { {0}, 0, 0, 1 };
        TajModStateStorage storage = storage_for(&store);
        ModRacerIdentity seen[3];
        int expected = 0;
        int row;
        int i;

        taj_mod_reset_for_test();
        taj_mod_boot(&storage);
        for (i = 0; i < 3; i++) {
            if (combo & (1 << i)) {
                CHECK(mod_racer_submit_magic_code(codes[i]) == identities[i]);
                seen[expected] = identities[i];
                expected++;
            }
        }
        CHECK(mod_racer_unlocked_count() == expected);

        /* Every row resolves, in enum order, to a distinct unlocked identity. */
        for (row = 0; row < expected; row++) {
            CHECK(mod_racer_identity_for_cheat_row(row) == seen[row]);
            CHECK(mod_racer_is_unlocked(mod_racer_identity_for_cheat_row(row)));
        }
        /* Every unlocked identity is reachable from some row -- the property the old open-coded
         * ladder violated for Terry. */
        for (i = 0; i < 3; i++) {
            int reachable = 0;
            if (!(combo & (1 << i))) continue;
            for (row = 0; row < expected; row++) {
                if (mod_racer_identity_for_cheat_row(row) == identities[i]) reachable = 1;
            }
            CHECK(reachable);
        }
        /* Out-of-range rows are refused rather than aliasing onto a real identity. */
        CHECK(mod_racer_identity_for_cheat_row(-1) == MOD_RACER_RETAIL);
        CHECK(mod_racer_identity_for_cheat_row(expected) == MOD_RACER_RETAIL);
    }
}

static void test_terry_unlock_and_identity(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    TajModPersistentState parsed;

    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(mod_racer_submit_magic_code("TERRYFL") == MOD_RACER_RETAIL);
    CHECK(mod_racer_submit_magic_code("TERRYFLY") == MOD_RACER_TERRY);
    CHECK(mod_racer_is_unlocked(MOD_RACER_TERRY));
    CHECK(mod_racer_is_enabled(MOD_RACER_TERRY));
    CHECK(mod_racer_consume_unlock_announcement(MOD_RACER_TERRY));
    taj_mod_state_defaults(&parsed);
    CHECK(taj_mod_state_parse(&parsed, store.text, store.length));
    CHECK(parsed.terry_unlocked == 1 && parsed.terry_migration_complete == 1);
    mod_racer_set_player_identity(0, MOD_RACER_TERRY);
    CHECK(mod_racer_player_identity(0) == MOD_RACER_TERRY);
    CHECK(mod_racer_resolve_race_character(0, 5) ==
          TERRY_MOD_DONOR_CHARACTER);
    taj_mod_begin_racer_bindings();
    taj_mod_bind_racer_player(0, 3);
    CHECK(mod_racer_live_identity(3) == MOD_RACER_TERRY);

    taj_mod_reset_for_test();
    memset(&store, 0, sizeof(store));
    store.write_result = 1;
    taj_mod_boot(&storage);
    CHECK(!mod_racer_unlock_from_adventure_progress(MOD_RACER_TERRY, 0x40));
    CHECK(mod_racer_unlock_from_adventure_progress(MOD_RACER_TERRY, 0x80));
    CHECK(mod_racer_is_unlocked(MOD_RACER_TERRY));
}

static void test_wizpig_unlock_and_identity(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    TajModPersistentState parsed;

    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(mod_racer_submit_magic_code("WIZPIGPOWE") == MOD_RACER_RETAIL);
    CHECK(mod_racer_submit_magic_code("WIZPIGPOWER") == MOD_RACER_WIZPIG);
    CHECK(mod_racer_is_unlocked(MOD_RACER_WIZPIG));
    CHECK(mod_racer_is_enabled(MOD_RACER_WIZPIG));
    CHECK(mod_racer_consume_unlock_announcement(MOD_RACER_WIZPIG));
    CHECK(!mod_racer_consume_unlock_announcement(MOD_RACER_TAJ));
    taj_mod_state_defaults(&parsed);
    CHECK(taj_mod_state_parse(&parsed, store.text, store.length));
    CHECK(parsed.wizpig_unlocked == 1 &&
          parsed.wizpig_migration_complete == 1);

    mod_racer_set_player_identity(0, MOD_RACER_WIZPIG);
    CHECK(mod_racer_player_identity(0) == MOD_RACER_WIZPIG);
    CHECK(mod_racer_resolve_race_character(0, 7) ==
          WIZPIG_MOD_DONOR_CHARACTER);
    CHECK(!taj_mod_player_selected(0));
    mod_racer_set_player_identity(1, MOD_RACER_TAJ);
    CHECK(mod_racer_player_identity(1) == MOD_RACER_RETAIL);

    taj_mod_begin_racer_bindings();
    taj_mod_bind_racer_player(0, 2);
    CHECK(mod_racer_live_identity(2) == MOD_RACER_WIZPIG);

    taj_mod_reset_for_test();
    memset(&store, 0, sizeof(store));
    store.write_result = 1;
    taj_mod_boot(&storage);
    CHECK(!mod_racer_unlock_from_adventure_progress(MOD_RACER_WIZPIG,
                                                     0x10));
    CHECK(mod_racer_unlock_from_adventure_progress(MOD_RACER_WIZPIG,
                                                    0x20));
    CHECK(mod_racer_is_unlocked(MOD_RACER_WIZPIG));
    CHECK(taj_mod_erase_all_bonuses());
    CHECK(!mod_racer_is_unlocked(MOD_RACER_WIZPIG));
    CHECK(!mod_racer_reconcile_imported_progress(0, 0x20));
}

static void test_magic_code_and_lifecycle(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    char long_code[20];

    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(!taj_mod_is_unlocked());
    CHECK(!taj_mod_submit_magic_code("ABRACADABR"));
    CHECK(!taj_mod_submit_magic_code("ABRACADABRA!"));
    memset(long_code, 'A', sizeof(long_code) - 1);
    long_code[sizeof(long_code) - 1] = '\0';
    CHECK(!taj_mod_submit_magic_code(long_code));
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled());
    CHECK(taj_mod_consume_unlock_announcement());
    CHECK(!taj_mod_consume_unlock_announcement());
    CHECK(store.length != 0);
    taj_mod_clear_session_codes();
    CHECK(taj_mod_is_unlocked() && !taj_mod_is_enabled());
    taj_mod_on_title_return();
    CHECK(!taj_mod_is_enabled());
    taj_mod_set_enabled(1);
    CHECK(taj_mod_is_enabled());
    taj_mod_on_adventure_file_deleted();
    CHECK(taj_mod_is_unlocked());
    CHECK(taj_mod_erase_all_bonuses());
    CHECK(!taj_mod_is_unlocked() && !taj_mod_is_enabled());
}

static void test_persisted_reload_and_failures(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);

    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    taj_mod_reset_for_test();
    store.read_result = 1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled());

    taj_mod_reset_for_test();
    memcpy(store.text, "broken", 6);
    store.length = 6;
    taj_mod_boot(&storage);
    CHECK(!taj_mod_is_unlocked() && taj_mod_persistence_failed());

    taj_mod_reset_for_test();
    store.length = 0;
    store.read_result = 0;
    store.write_result = -1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled() &&
          taj_mod_persistence_failed());
    CHECK(taj_mod_persistence_issue() == TAJ_MOD_PERSISTENCE_UNLOCK);
    CHECK(taj_mod_consume_unlock_announcement());

    /* Re-entering the accepted code is a real retry after a desktop store
     * failure, rather than a no-op caused by the useful in-session unlock. */
    store.write_result = 1;
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    CHECK(!taj_mod_persistence_failed() && store_has_state(&store, 1, 1));
    taj_mod_reset_for_test();
    store.read_result = 1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled());
}

static void test_async_persistence_state_machine(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    unsigned int first;
    unsigned int retry;
    unsigned int erase;
    unsigned int erase_retry;
    unsigned int roster_first;
    unsigned int roster_second;

    /* The web backend writes into MEMFS before IDBFS accepts it. Model the
     * asynchronous completion explicitly, including a rejected unlock. */
    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    taj_mod_set_async_persistence_for_test(1);
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    first = taj_mod_pending_generation_for_test();
    CHECK(first != 0 && taj_mod_persistence_pending());
    CHECK(store_has_state(&store, 1, 1));
    /* A second destructive action cannot replace the first transaction. */
    CHECK(!taj_mod_erase_all_bonuses());
    CHECK(taj_mod_pending_generation_for_test() == first &&
          taj_mod_is_unlocked() && taj_mod_is_enabled());
    taj_mod_report_persistence_failure(first);
    CHECK(!taj_mod_persistence_pending() && taj_mod_persistence_failed() &&
          taj_mod_persistence_issue() == TAJ_MOD_PERSISTENCE_UNLOCK &&
          taj_mod_is_unlocked() && taj_mod_is_enabled());
    CHECK(taj_mod_retry_persistence());
    retry = taj_mod_pending_generation_for_test();
    CHECK(retry != 0 && retry != first && taj_mod_persistence_pending());
    /* Delayed callbacks from the first request cannot settle the retry. */
    taj_mod_report_persistence_success(first);
    CHECK(taj_mod_persistence_pending() && !taj_mod_persistence_failed());
    taj_mod_report_persistence_success(retry);
    CHECK(!taj_mod_persistence_pending() && !taj_mod_persistence_failed() &&
          store_has_state(&store, 1, 1));

    /* A rejected erase must restore the prior bytes in MEMFS before the
     * shell's independent retry timer can ever flush the failed candidate. */
    CHECK(taj_mod_erase_all_bonuses());
    erase = taj_mod_pending_generation_for_test();
    CHECK(erase != 0 && !taj_mod_is_unlocked() &&
          store_has_state(&store, 0, 1));
    taj_mod_report_persistence_failure(erase);
    CHECK(!taj_mod_persistence_pending() && taj_mod_persistence_failed() &&
          taj_mod_persistence_issue() == TAJ_MOD_PERSISTENCE_ERASE &&
          taj_mod_is_unlocked() && taj_mod_is_enabled() &&
          store_has_state(&store, 1, 1));
    CHECK(taj_mod_retry_persistence());
    erase_retry = taj_mod_pending_generation_for_test();
    CHECK(erase_retry != 0 && erase_retry != erase &&
          !taj_mod_is_unlocked() && store_has_state(&store, 0, 1));
    taj_mod_report_persistence_success(erase);
    CHECK(taj_mod_persistence_pending() && !taj_mod_persistence_failed());
    taj_mod_report_persistence_success(erase_retry);
    CHECK(!taj_mod_persistence_pending() && !taj_mod_persistence_failed());
    taj_mod_reset_for_test();
    store.read_result = 1;
    taj_mod_boot(&storage);
    CHECK(!taj_mod_is_unlocked() && !taj_mod_is_enabled());

    /* Importing saves can discover every bonus racer in one listing pass.
     * Later unlocks must coalesce behind the first rather than disappear when
     * the first browser generation succeeds. */
    taj_mod_reset_for_test();
    memset(&store, 0, sizeof(store));
    store.write_result = 1;
    storage = storage_for(&store);
    taj_mod_boot(&storage);
    taj_mod_set_async_persistence_for_test(1);
    CHECK(mod_racer_submit_magic_code("ABRACADABRA") == MOD_RACER_TAJ);
    roster_first = taj_mod_pending_generation_for_test();
    CHECK(mod_racer_submit_magic_code("WIZPIGPOWER") == MOD_RACER_WIZPIG);
    CHECK(mod_racer_submit_magic_code("TERRYFLY") == MOD_RACER_TERRY);
    CHECK(roster_first != 0 && taj_mod_persistence_pending());
    taj_mod_report_persistence_success(roster_first);
    roster_second = taj_mod_pending_generation_for_test();
    CHECK(roster_second != 0 && roster_second != roster_first &&
          taj_mod_persistence_pending());
    CHECK(store_has_state(&store, 1, 1));
    taj_mod_report_persistence_success(roster_second);
    taj_mod_reset_for_test();
    store.read_result = 1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_is_unlocked());
    CHECK(mod_racer_is_unlocked(MOD_RACER_WIZPIG));
    CHECK(mod_racer_is_unlocked(MOD_RACER_TERRY));
}

static void test_failed_erase_is_transactional(void) {
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    char durable[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t durable_length;

    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(taj_mod_submit_magic_code("ABRACADABRA"));
    durable_length = store.length;
    memcpy(durable, store.text, durable_length);

    store.write_result = -1;
    CHECK(!taj_mod_erase_all_bonuses());
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled());
    CHECK(taj_mod_persistence_failed());
    CHECK(taj_mod_persistence_issue() == TAJ_MOD_PERSISTENCE_ERASE);
    CHECK(store.length == durable_length &&
          memcmp(store.text, durable, durable_length) == 0);

    taj_mod_reset_for_test();
    store.read_result = 1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_is_unlocked() && taj_mod_is_enabled());

    store.write_result = 1;
    CHECK(taj_mod_erase_all_bonuses());
    CHECK(!taj_mod_is_unlocked() && !taj_mod_is_enabled());
    CHECK(!taj_mod_persistence_failed());
    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(!taj_mod_is_unlocked() && !taj_mod_is_enabled());
}

static void test_challenge_mask_and_identity(void) {
    static const unsigned int challenge_orders[][3] = {
        { 0x08, 0x10, 0x20 }, { 0x08, 0x20, 0x10 },
        { 0x10, 0x08, 0x20 }, { 0x10, 0x20, 0x08 },
        { 0x20, 0x08, 0x10 }, { 0x20, 0x10, 0x08 }
    };
    MemoryStore store = { {0}, 0, 0, 1 };
    TajModStateStorage storage = storage_for(&store);
    unsigned int flags;
    size_t order;
    int step;

    CHECK(taj_mod_player_bit(-1) == 0u);
    CHECK(taj_mod_player_bit(0) == 0x1u);
    CHECK(taj_mod_player_bit(1) == 0x2u);
    CHECK(taj_mod_player_bit(2) == 0x4u);
    CHECK(taj_mod_player_bit(3) == 0x8u);
    CHECK(taj_mod_player_bit(TAJ_MOD_MAX_PLAYERS) == 0u);

    for (order = 0; order < sizeof(challenge_orders) / sizeof(challenge_orders[0]); order++) {
        taj_mod_reset_for_test();
        taj_mod_boot(&storage);
        flags = 0;
        for (step = 0; step < 3; step++) {
            flags |= challenge_orders[order][step];
            CHECK(taj_mod_unlock_from_taj_flags(flags) == (step == 2));
        }
        CHECK(taj_mod_consume_unlock_announcement());
        CHECK(!taj_mod_consume_unlock_announcement());
        CHECK(!taj_mod_unlock_from_taj_flags(flags));
    }
    taj_mod_reset_for_test();
    taj_mod_boot(&storage);
    CHECK(!taj_mod_unlock_from_taj_flags(0x18));
    CHECK(taj_mod_unlock_from_taj_flags(0x38));
    taj_mod_set_player_selected(0, 1);
    taj_mod_set_player_selected(3, 1);
    CHECK(taj_mod_racer_is_taj(0) && taj_mod_racer_is_taj(3));
    CHECK(taj_mod_resolve_race_character(0, 4) == TAJ_MOD_DONOR_CHARACTER);
    CHECK(taj_mod_resolve_race_character(1, 4) == 4);
    /* An out-of-range requested character is a corrupt/uninitialised settings
     * slot, not a Taj selection. It used to fall back to the DONOR, which made
     * garbage indistinguishable from a genuine pick and handed Taj's donor
     * character to a player who never chose it. It now falls back to the
     * neutral default, and player 1 -- who is NOT selected here -- must not
     * come out of the resolver as Taj under any input. */
    CHECK(taj_mod_resolve_race_character(1, 99) == TAJ_MOD_NEUTRAL_CHARACTER);
    CHECK(taj_mod_resolve_race_character(1, -1) == TAJ_MOD_NEUTRAL_CHARACTER);
    CHECK(taj_mod_resolve_race_character(1, 99) != TAJ_MOD_DONOR_CHARACTER);
    /* A player who IS selected still resolves to the donor regardless. */
    CHECK(taj_mod_resolve_race_character(0, 99) == TAJ_MOD_DONOR_CHARACTER);
    taj_mod_swap_player_selections(0, 1);
    CHECK(!taj_mod_player_selected(0));
    CHECK(taj_mod_player_selected(1));
    taj_mod_swap_player_selections(-1, 1);
    taj_mod_swap_player_selections(1, TAJ_MOD_MAX_PLAYERS);
    taj_mod_swap_player_selections(1, 1);
    CHECK(!taj_mod_player_selected(0));
    CHECK(taj_mod_player_selected(1));
    taj_mod_begin_racer_bindings();
    taj_mod_bind_racer_player(0, 1);
    taj_mod_bind_racer_player(1, 0);
    taj_mod_bind_racer_player(3, 3);
    /* P2-lead Adventure swaps settings rows and then remaps those settings
     * slots onto live ports. Results use the swapped settings mask; gameplay
     * uses the separately bound live mask. */
    CHECK(!taj_mod_player_selected(0));
    CHECK(taj_mod_player_selected(1));
    CHECK(taj_mod_player_selected(3));
    CHECK(taj_mod_racer_is_taj(0));
    CHECK(!taj_mod_racer_is_taj(1));
    CHECK(taj_mod_racer_is_taj(3));
    taj_mod_reset_player_selections();
    CHECK(!taj_mod_racer_is_taj(0));

    taj_mod_reset_for_test();
    memset(&store, 0, sizeof(store));
    store.write_result = 1;
    taj_mod_boot(&storage);
    CHECK(taj_mod_reconcile_imported_taj_flags(0x38));
    CHECK(taj_mod_is_unlocked());
    CHECK(taj_mod_erase_all_bonuses());
    CHECK(!taj_mod_reconcile_imported_taj_flags(0x38));
    CHECK(!taj_mod_is_unlocked());
    CHECK(taj_mod_unlock_from_taj_flags(0x38));
}

int main(void) {
    test_state_format();
    test_magic_code_and_lifecycle();
    test_persisted_reload_and_failures();
    test_async_persistence_state_machine();
    test_failed_erase_is_transactional();
    test_challenge_mask_and_identity();
    test_wizpig_unlock_and_identity();
    test_terry_unlock_and_identity();
    test_cheat_row_identity_mapping();
    if (failures != 0) {
        fprintf(stderr, "taj-mod tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("taj-mod tests: PASS");
    return 0;
}
