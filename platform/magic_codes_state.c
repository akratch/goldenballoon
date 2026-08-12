#include "magic_codes_state.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define MAGIC_CODES_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define MAGIC_CODES_KEEPALIVE
#endif

typedef struct MagicCodesRuntimeState {
    MagicCodesPersistentState persisted;
    MagicCodesPersistentState desired;
    MagicCodesPersistentState pending;
    const MagicCodesStateStorage *storage;
    unsigned int pending_generation;
    unsigned int next_generation;
    int pending_store;
    int failed;
    int booted;
#ifdef MAGIC_CODES_STATE_TESTING
    int test_async;
#endif
} MagicCodesRuntimeState;

static MagicCodesRuntimeState s_magic_codes;

static uint32_t magic_codes_checksum(const char *text, size_t length) {
    uint32_t hash = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < length; i++) {
        hash ^= (unsigned char)text[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int magic_codes_multiple_bits(uint32_t value) {
    return value != 0 && (value & (value - 1)) != 0;
}

void magic_codes_state_defaults(MagicCodesPersistentState *state) {
    if (state != NULL) {
        state->version = MAGIC_CODES_STATE_VERSION;
        state->unlocked = 0;
        state->active = 0;
    }
}

int magic_codes_state_is_valid(const MagicCodesPersistentState *state) {
    uint32_t active;
    const uint32_t sizes = UINT32_C(1) << 4 | UINT32_C(1) << 5;
    const uint32_t banana_modes = UINT32_C(1) << 7 | UINT32_C(1) << 13 |
                                    UINT32_C(1) << 14;
    const uint32_t balloon_colours = UINT32_C(0x1F) << 15;
    const uint32_t weapon_modes = balloon_colours | UINT32_C(1) << 20;

    if (state == NULL || state->version != MAGIC_CODES_STATE_VERSION ||
        (state->unlocked & ~MAGIC_CODES_PERSISTED_MASK) != 0 ||
        (state->active & ~MAGIC_CODES_PERSISTED_MASK) != 0 ||
        (state->active & ~state->unlocked) != 0) {
        return 0;
    }
    active = state->active;
    if (magic_codes_multiple_bits(active & sizes) ||
        ((active & (UINT32_C(1) << 12)) != 0 &&
         (active & banana_modes) != 0) ||
        ((active & (UINT32_C(1) << 11)) != 0 &&
         (active & weapon_modes) != 0) ||
        magic_codes_multiple_bits(active & balloon_colours)) {
        return 0;
    }
    return 1;
}

int magic_codes_state_serialize(const MagicCodesPersistentState *state,
                                char *text, size_t capacity, size_t *length) {
    int prefix_length;
    int written;
    uint32_t checksum;

    if (text == NULL || capacity == 0 || length == NULL ||
        !magic_codes_state_is_valid(state)) {
        return 0;
    }
    prefix_length = snprintf(text, capacity,
                             "magic_codes_version=1\n"
                             "unlocked=%08X\nactive=%08X\n",
                             state->unlocked, state->active);
    if (prefix_length < 0 || (size_t)prefix_length >= capacity) {
        return 0;
    }
    checksum = magic_codes_checksum(text, (size_t)prefix_length);
    written = snprintf(text + prefix_length, capacity - (size_t)prefix_length,
                       "checksum=%08X", checksum);
    if (written < 0 || (size_t)written >= capacity - (size_t)prefix_length) {
        return 0;
    }
    *length = (size_t)prefix_length + (size_t)written;
    return 1;
}

int magic_codes_state_parse(MagicCodesPersistentState *state, const char *text,
                            size_t length) {
    MagicCodesPersistentState candidate;
    char canonical[MAGIC_CODES_STATE_TEXT_CAPACITY];
    char input[MAGIC_CODES_STATE_TEXT_CAPACITY];
    size_t canonical_length;
    unsigned int unlocked;
    unsigned int active;
    unsigned int checksum;
    size_t parse_length = length;
    int consumed = 0;

    if (state == NULL || text == NULL || length == 0 || length >= sizeof(input)) {
        return 0;
    }
    if (text[parse_length - 1] == '\n') {
        parse_length--;
        if (parse_length > 0 && text[parse_length - 1] == '\r') parse_length--;
    }
    if (parse_length == 0) return 0;
    memcpy(input, text, parse_length);
    input[parse_length] = '\0';
    if (sscanf(input,
               "magic_codes_version=1\nunlocked=%8X\nactive=%8X\n"
               "checksum=%8X%n",
               &unlocked, &active, &checksum, &consumed) != 3 ||
        (size_t)consumed != parse_length) {
        return 0;
    }
    candidate.version = MAGIC_CODES_STATE_VERSION;
    candidate.unlocked = (uint32_t)unlocked;
    candidate.active = (uint32_t)active;
    if (!magic_codes_state_is_valid(&candidate) ||
        !magic_codes_state_serialize(&candidate, canonical, sizeof(canonical),
                                     &canonical_length) ||
        canonical_length != parse_length ||
        memcmp(canonical, input, parse_length) != 0) {
        return 0;
    }
    (void)checksum; /* Canonical comparison includes the computed checksum. */
    *state = candidate;
    return 1;
}

int magic_codes_state_load(MagicCodesPersistentState *state,
                           const MagicCodesStateStorage *storage) {
    char text[MAGIC_CODES_STATE_TEXT_CAPACITY];
    size_t length = 0;
    int result;
    if (state == NULL || storage == NULL || storage->read == NULL) return -1;
    magic_codes_state_defaults(state);
    result = storage->read(storage->context, text, sizeof(text), &length);
    if (result <= 0) return result;
    return magic_codes_state_parse(state, text, length) ? 1 : -1;
}

int magic_codes_state_store(const MagicCodesPersistentState *state,
                            const MagicCodesStateStorage *storage) {
    char text[MAGIC_CODES_STATE_TEXT_CAPACITY];
    size_t length;
    if (storage == NULL || storage->write == NULL ||
        !magic_codes_state_serialize(state, text, sizeof(text), &length)) {
        return -1;
    }
    return storage->write(storage->context, text, length);
}

static int magic_codes_uses_async(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#elif defined(MAGIC_CODES_STATE_TESTING)
    return s_magic_codes.test_async;
#else
    return 0;
#endif
}

static int magic_codes_same(const MagicCodesPersistentState *a,
                            const MagicCodesPersistentState *b) {
    return a->version == b->version && a->unlocked == b->unlocked &&
           a->active == b->active;
}

static unsigned int magic_codes_next_generation(void) {
    s_magic_codes.next_generation++;
    if (s_magic_codes.next_generation == 0) s_magic_codes.next_generation++;
    return s_magic_codes.next_generation;
}

static int magic_codes_start_store(const MagicCodesPersistentState *candidate) {
    if (s_magic_codes.storage == NULL) {
        s_magic_codes.persisted = *candidate;
        s_magic_codes.failed = 0;
        return 1;
    }
    if (magic_codes_uses_async()) {
        s_magic_codes.pending = *candidate;
        s_magic_codes.pending_generation = magic_codes_next_generation();
        s_magic_codes.pending_store = 1;
    }
    if (magic_codes_state_store(candidate, s_magic_codes.storage) != 1) {
        s_magic_codes.pending_store = 0;
        s_magic_codes.pending_generation = 0;
        s_magic_codes.failed = 1;
        fprintf(stderr, "[MAGIC CODES] state was not saved; current session remains active\n");
        return 0;
    }
    s_magic_codes.failed = 0;
    if (!magic_codes_uses_async()) s_magic_codes.persisted = *candidate;
    return 1;
}

void magic_codes_persistence_boot(const MagicCodesStateStorage *storage,
                                  uint32_t *unlocked, uint32_t *active) {
    int result;
    if (!s_magic_codes.booted) {
        memset(&s_magic_codes, 0, sizeof(s_magic_codes));
        magic_codes_state_defaults(&s_magic_codes.persisted);
        s_magic_codes.storage = storage;
        if (storage != NULL) {
            result = magic_codes_state_load(&s_magic_codes.persisted, storage);
            if (result < 0) {
                magic_codes_state_defaults(&s_magic_codes.persisted);
                s_magic_codes.failed = 1;
                fprintf(stderr, "[MAGIC CODES] saved state could not be loaded; using defaults\n");
            }
        }
        s_magic_codes.desired = s_magic_codes.persisted;
        s_magic_codes.booted = 1;
    }
    if (unlocked != NULL) *unlocked |= s_magic_codes.persisted.unlocked;
    if (active != NULL) *active |= s_magic_codes.persisted.active;
}

int magic_codes_persistence_update(uint32_t unlocked, uint32_t active) {
    MagicCodesPersistentState candidate;
    candidate.version = MAGIC_CODES_STATE_VERSION;
    candidate.unlocked = unlocked & MAGIC_CODES_PERSISTED_MASK;
    candidate.active = active & candidate.unlocked & MAGIC_CODES_PERSISTED_MASK;
    if (!magic_codes_state_is_valid(&candidate)) {
        /* Corrupt/conflicting runtime state must never be resurrected at boot. */
        candidate.active = 0;
    }
    s_magic_codes.desired = candidate;
    if (s_magic_codes.pending_store) return 1;
    if (!s_magic_codes.failed &&
        magic_codes_same(&candidate, &s_magic_codes.persisted)) return 1;
    return magic_codes_start_store(&candidate);
}

int magic_codes_persistence_failed(void) { return s_magic_codes.failed; }
int magic_codes_persistence_pending(void) { return s_magic_codes.pending_store; }
unsigned int magic_codes_persistence_pending_generation(void) {
    return s_magic_codes.pending_generation;
}

MAGIC_CODES_KEEPALIVE void magic_codes_report_persistence_success(
    unsigned int generation) {
    if (!s_magic_codes.pending_store ||
        generation != s_magic_codes.pending_generation) return;
    s_magic_codes.persisted = s_magic_codes.pending;
    s_magic_codes.pending_store = 0;
    s_magic_codes.pending_generation = 0;
    s_magic_codes.failed = 0;
    if (!magic_codes_same(&s_magic_codes.desired, &s_magic_codes.persisted)) {
        (void)magic_codes_start_store(&s_magic_codes.desired);
    }
}

MAGIC_CODES_KEEPALIVE void magic_codes_report_persistence_failure(
    unsigned int generation) {
    if (!s_magic_codes.pending_store ||
        generation != s_magic_codes.pending_generation) return;
    s_magic_codes.pending_store = 0;
    s_magic_codes.pending_generation = 0;
    s_magic_codes.failed = 1;
    fprintf(stderr, "[MAGIC CODES] browser storage rejected the saved state\n");
}

#ifdef MAGIC_CODES_STATE_TESTING
void magic_codes_persistence_reset_for_test(void) {
    memset(&s_magic_codes, 0, sizeof(s_magic_codes));
}
void magic_codes_persistence_set_async_for_test(int enabled) {
    s_magic_codes.test_async = enabled != 0;
}
#endif
