/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_input_runtime.h"

typedef struct Probe {
    uint32_t epoch;
    uint32_t tick;
    unsigned drains;
    unsigned reads;
    unsigned dirties;
} Probe;

static bool drain(void *context, uint32_t epoch, uint32_t tick,
                  const MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS],
                  unsigned count, MdkrInputSet *out) {
    Probe *probe = (Probe *)context;
    if (probe == NULL || epoch != probe->epoch || count != 4u) return false;
    memset(out, 0, sizeof(*out));
    out->slots[2] = physical[1];
    out->present_mask = physical[1].present ? 0x04u : 0u;
    out->confirmed_mask = 0x04u;
    probe->tick = tick;
    probe->drains++;
    return true;
}

static bool inputs_for_tick(void *context, uint32_t epoch, uint32_t tick,
                            MdkrInputSet *out) {
    Probe *probe = (Probe *)context;
    if (probe == NULL || epoch != probe->epoch || tick != probe->tick) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->slots[2].buttons = 0x1234u;
    out->slots[2].present = 1u;
    out->present_mask = 0x04u;
    probe->reads++;
    return true;
}

static bool take_dirty(void *context, uint32_t epoch, uint32_t *tick) {
    Probe *probe = (Probe *)context;
    if (probe == NULL || epoch != probe->epoch || probe->dirties != 0u) {
        return false;
    }
    *tick = probe->tick;
    probe->dirties++;
    return true;
}

static bool ai_mask_for_tick(void *context, uint32_t epoch, uint32_t tick,
                             uint8_t *slot_mask) {
    Probe *probe = (Probe *)context;
    if (probe == NULL || epoch != probe->epoch || slot_mask == NULL) {
        return false;
    }
    *slot_mask = tick >= 6u ? 0x04u : 0u;
    return true;
}

int main(void) {
    Probe probe = {9u, 0u, 0u, 0u, 0u};
    MdkrMatchInputSource source = {
        MDKR_MATCH_INPUT_SOURCE_VERSION, 9u, &probe,
        drain, inputs_for_tick, take_dirty, ai_mask_for_tick};
    MdkrPadSample physical[MDKR_SESSION_MAX_PLAYERS] = {{0}};
    MdkrInputSet out;
    uint32_t dirty = 0u;

    mdkr_match_input_runtime_clear();
    assert(!mdkr_match_input_runtime_active());
    assert(!mdkr_match_input_runtime_install(NULL));
    assert(mdkr_match_input_runtime_install(&source));
    assert(!mdkr_match_input_runtime_install(&source));
    assert(mdkr_match_input_runtime_active());
    assert(mdkr_match_input_runtime_epoch() == 9u);

    physical[1].buttons = 0x8000u;
    physical[1].present = 1u;
    assert(!mdkr_match_input_runtime_drain(4u, physical, 3u, &out));
    assert(mdkr_match_input_runtime_drain(4u, physical, 4u, &out));
    assert(probe.drains == 1u && out.slots[2].buttons == 0x8000u);
    assert(mdkr_match_input_runtime_inputs_for_tick(4u, &out));
    assert(probe.reads == 1u && out.slots[2].buttons == 0x1234u);
    assert(mdkr_match_input_runtime_take_dirty(&dirty) && dirty == 4u);
    assert(!mdkr_match_input_runtime_take_dirty(&dirty));
    assert(mdkr_match_input_runtime_begin_tick(5u));
    assert(!mdkr_match_input_runtime_slot_ai_controlled(2u));
    assert(mdkr_match_input_runtime_begin_tick(6u));
    assert(mdkr_match_input_runtime_slot_ai_controlled(2u));
    assert(!mdkr_match_input_runtime_slot_ai_controlled(4u));

    mdkr_match_input_runtime_clear();
    assert(!mdkr_match_input_runtime_active());
    assert(mdkr_match_input_runtime_epoch() == 0u);
    assert(!mdkr_match_input_runtime_drain(5u, physical, 4u, &out));
    assert(!mdkr_match_input_runtime_begin_tick(5u));
    puts("test_match_input_runtime: PASS");
    return 0;
}
