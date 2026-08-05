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

    taj_mod_state_defaults(&state);
    state.taj_unlocked = 1;
    state.adventure_migration_complete = 1;
    CHECK(taj_mod_state_serialize(&state, text, sizeof(text), &length));
    taj_mod_state_defaults(&state);
    CHECK(taj_mod_state_parse(&state, text, length));
    CHECK(state.version == TAJ_MOD_STATE_VERSION && state.taj_unlocked == 1 &&
          state.adventure_migration_complete == 1);
    CHECK(taj_mod_state_parse(&state, terminated,
                              sizeof(terminated) - 1));
    CHECK(taj_mod_state_parse(&state, crlf_terminated,
                              sizeof(crlf_terminated) - 1));
    CHECK(!taj_mod_state_parse(&state, extra_blank_line,
                               sizeof(extra_blank_line) - 1));
    CHECK(!taj_mod_state_parse(&state, "taj_mod_version=2\ntaj_unlocked=1", 33));
    CHECK(!taj_mod_state_parse(&state, "taj_mod_version=1\ntaj_unlocked=2", 33));
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
    if (failures != 0) {
        fprintf(stderr, "taj-mod tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("taj-mod tests: PASS");
    return 0;
}
