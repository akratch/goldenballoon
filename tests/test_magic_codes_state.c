#include "magic_codes_state.h"

#include <stdio.h>
#include <string.h>

typedef struct MemoryStore {
    char text[MAGIC_CODES_STATE_TEXT_CAPACITY];
    size_t length;
    int read_result;
    int write_result;
    int writes;
} MemoryStore;

static int failures;
#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); failures++; } \
} while (0)

static int memory_read(void *context, char *text, size_t capacity,
                       size_t *length) {
    MemoryStore *store = context;
    if (store->read_result != 1) return store->read_result;
    if (store->length >= capacity) return -1;
    memcpy(text, store->text, store->length);
    *length = store->length;
    return 1;
}

static int memory_write(void *context, const char *text, size_t length) {
    MemoryStore *store = context;
    store->writes++;
    if (store->write_result != 1 || length >= sizeof(store->text)) {
        return store->write_result;
    }
    memcpy(store->text, text, length);
    store->length = length;
    return 1;
}

static MagicCodesStateStorage storage_for(MemoryStore *store) {
    MagicCodesStateStorage storage = { store, memory_read, memory_write };
    return storage;
}

static MagicCodesPersistentState parsed_store(const MemoryStore *store) {
    MagicCodesPersistentState state;
    magic_codes_state_defaults(&state);
    CHECK(magic_codes_state_parse(&state, store->text, store->length));
    return state;
}

static void test_format_and_policy(void) {
    MagicCodesPersistentState state;
    char text[MAGIC_CODES_STATE_TEXT_CAPACITY];
    size_t length;

    magic_codes_state_defaults(&state);
    /* The issue's exact TIMETOLOSE + TEENYWEENIES pair. */
    state.unlocked = (UINT32_C(1) << 25) | (UINT32_C(1) << 5);
    state.active = state.unlocked;
    CHECK(magic_codes_state_is_valid(&state));
    CHECK(magic_codes_state_serialize(&state, text, sizeof(text), &length));
    magic_codes_state_defaults(&state);
    CHECK(magic_codes_state_parse(&state, text, length));
    CHECK(state.unlocked == ((UINT32_C(1) << 25) | (UINT32_C(1) << 5)));
    CHECK(state.active == state.unlocked);

    text[length - 1] = text[length - 1] == '0' ? '1' : '0';
    CHECK(!magic_codes_state_parse(&state, text, length));
    CHECK(!magic_codes_state_parse(&state, "magic_codes_version=2", 21));

    magic_codes_state_defaults(&state);
    state.unlocked = UINT32_C(1) << 0; /* TT belongs to EEPROM. */
    CHECK(!magic_codes_state_is_valid(&state));
    magic_codes_state_defaults(&state);
    state.unlocked = state.active = (UINT32_C(1) << 4) | (UINT32_C(1) << 5);
    CHECK(!magic_codes_state_is_valid(&state));
    magic_codes_state_defaults(&state);
    state.unlocked = state.active = (UINT32_C(1) << 15) | (UINT32_C(1) << 19);
    CHECK(!magic_codes_state_is_valid(&state));
    magic_codes_state_defaults(&state);
    state.unlocked = UINT32_C(1) << 25;
    state.active = UINT32_C(1) << 5;
    CHECK(!magic_codes_state_is_valid(&state));
}

static void test_sync_lifecycle_and_filter(void) {
    MemoryStore store = { {0}, 0, 0, 1, 0 };
    MagicCodesStateStorage storage = storage_for(&store);
    MagicCodesPersistentState state;
    uint32_t unlocked = UINT32_C(1) << 0;
    uint32_t active = UINT32_C(1) << 0;

    magic_codes_persistence_reset_for_test();
    magic_codes_persistence_boot(&storage, &unlocked, &active);
    CHECK(unlocked == (UINT32_C(1) << 0) && active == (UINT32_C(1) << 0));
    CHECK(magic_codes_persistence_update(
        unlocked | (UINT32_C(1) << 5) | (UINT32_C(1) << 25) |
            (UINT32_C(1) << 26) | (UINT32_C(1) << 27),
        active | (UINT32_C(1) << 5) | (UINT32_C(1) << 25) |
            (UINT32_C(1) << 26) | (UINT32_C(1) << 27)));
    state = parsed_store(&store);
    CHECK(state.unlocked == ((UINT32_C(1) << 5) | (UINT32_C(1) << 25)));
    CHECK(state.active == state.unlocked);
    CHECK(!magic_codes_persistence_failed());

    magic_codes_persistence_reset_for_test();
    store.read_result = 1;
    unlocked = UINT32_C(1) << 1;
    active = UINT32_C(1) << 1;
    magic_codes_persistence_boot(&storage, &unlocked, &active);
    CHECK(unlocked == ((UINT32_C(1) << 1) | (UINT32_C(1) << 5) |
                       (UINT32_C(1) << 25)));
    CHECK(active == unlocked);

    /* A failed desktop save keeps gameplay state useful and retries the same
     * desired bytes on the next real update. */
    store.write_result = -1;
    CHECK(!magic_codes_persistence_update(unlocked, UINT32_C(1) << 5));
    CHECK(magic_codes_persistence_failed());
    store.write_result = 1;
    CHECK(magic_codes_persistence_update(unlocked, UINT32_C(1) << 5));
    CHECK(!magic_codes_persistence_failed());
    state = parsed_store(&store);
    CHECK(state.active == (UINT32_C(1) << 5));
}

static void test_async_coalescing_and_generations(void) {
    MemoryStore store = { {0}, 0, 0, 1, 0 };
    MagicCodesStateStorage storage = storage_for(&store);
    MagicCodesPersistentState state;
    uint32_t unlocked = 0;
    uint32_t active = 0;
    unsigned int first;
    unsigned int second;

    magic_codes_persistence_reset_for_test();
    magic_codes_persistence_boot(&storage, &unlocked, &active);
    magic_codes_persistence_set_async_for_test(1);
    CHECK(magic_codes_persistence_update(UINT32_C(1) << 5,
                                         UINT32_C(1) << 5));
    first = magic_codes_persistence_pending_generation();
    CHECK(first != 0 && magic_codes_persistence_pending() && store.writes == 1);
    CHECK(magic_codes_persistence_update(
        (UINT32_C(1) << 5) | (UINT32_C(1) << 25),
        (UINT32_C(1) << 5) | (UINT32_C(1) << 25)));
    CHECK(store.writes == 1); /* queued, never an overlapping IDBFS flush */
    magic_codes_report_persistence_success(first + 99);
    CHECK(magic_codes_persistence_pending() && store.writes == 1);
    magic_codes_report_persistence_success(first);
    second = magic_codes_persistence_pending_generation();
    CHECK(second != 0 && second != first && store.writes == 2);
    state = parsed_store(&store);
    CHECK(state.active == ((UINT32_C(1) << 5) | (UINT32_C(1) << 25)));
    magic_codes_report_persistence_failure(first);
    CHECK(magic_codes_persistence_pending());
    magic_codes_report_persistence_failure(second);
    CHECK(!magic_codes_persistence_pending() && magic_codes_persistence_failed());
    CHECK(magic_codes_persistence_update(state.unlocked, state.active));
    CHECK(magic_codes_persistence_pending());
}

int main(void) {
    test_format_and_policy();
    test_sync_lifecycle_and_filter();
    test_async_coalescing_and_generations();
    if (failures != 0) {
        fprintf(stderr, "magic-code state tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("magic-code state tests: PASS");
    return 0;
}
