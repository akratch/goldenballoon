/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_input.h"

static MdkrInputSample sample(unsigned buttons, int x) {
    const MdkrInputSample value = {(uint16_t)buttons, (int8_t)x, 0, true};
    return value;
}

int main(void) {
    MdkrNetInputHistory history;
    MdkrNetInputHistory before;
    MdkrNetInputSet set;
    MdkrInputSample go = sample(0x8000u, 20);
    MdkrInputSample release = sample(0u, 0);
    uint32_t dirty = 0u;
    unsigned slot;

    mdkr_net_input_init(&history, 100u);
    assert(mdkr_net_input_submit(&history, 0u, 100u, &go) == MDKR_NET_SUBMIT_ACCEPTED);
    assert(mdkr_net_input_for_tick(&history, 100u, &set));
    assert(set.status[0] == MDKR_NET_INPUT_RECEIVED);
    assert(set.status[1] == MDKR_NET_INPUT_PREDICTED);
    mdkr_net_input_set_current_tick(&history, 101u);
    assert(mdkr_net_input_for_tick(&history, 101u, &set));
    assert(mdkr_net_input_copy_existing(&history, 101u, &set));
    before = history;
    assert(!mdkr_net_input_copy_existing(&history, 102u, &set));
    assert(memcmp(&history, &before, sizeof(history)) == 0);
    assert(set.samples[0].buttons == 0x8000u); /* repeat-last prediction */
    assert(mdkr_net_input_submit(&history, 0u, 101u, &release) ==
           MDKR_NET_SUBMIT_CORRECTED);
    assert(mdkr_net_input_take_dirty(&history, &dirty) && dirty == 101u);
    assert(!mdkr_net_input_take_dirty(&history, &dirty));
    assert(mdkr_net_input_submit(&history, 0u, 101u, &release) ==
           MDKR_NET_SUBMIT_DUPLICATE);
    assert(mdkr_net_input_submit(&history, 0u, 101u, &go) ==
           MDKR_NET_SUBMIT_CONFLICT);
    assert(mdkr_net_input_submit(&history, 4u, 101u, &go) ==
           MDKR_NET_SUBMIT_INVALID_SLOT);
    assert(mdkr_net_input_submit(&history, 1u, 134u, &go) ==
           MDKR_NET_SUBMIT_TOO_FAR_FUTURE);

    for (slot = 1u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        assert(mdkr_net_input_submit(&history, slot, 100u, &release) ==
               MDKR_NET_SUBMIT_ACCEPTED);
        assert(mdkr_net_input_submit(&history, slot, 101u, &release) ==
               MDKR_NET_SUBMIT_ACCEPTED);
    }
    assert(mdkr_net_input_confirm_through(&history, 101u));

    mdkr_net_input_init(&history, 0xfffffffdu);
    mdkr_net_input_set_current_tick(&history, 1u);
    assert(mdkr_net_tick_after(1u, 0xfffffffdu));
    assert(mdkr_net_input_submit(&history, 0u, 0xffffffffu, &go) ==
           MDKR_NET_SUBMIT_ACCEPTED);
    assert(mdkr_net_input_submit(&history, 0u, 40u, &go) ==
           MDKR_NET_SUBMIT_TOO_FAR_FUTURE);

    /* A reordered correction invalidates a dependent authored suffix. Replay
     * reads stay strict; the explicit atomic rebuild rematerializes it in tick
     * order from corrected received samples. */
    mdkr_net_input_init(&history, 9u);
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        assert(mdkr_net_input_submit(&history, slot, 9u, &go) ==
               MDKR_NET_SUBMIT_ACCEPTED);
    }
    assert(mdkr_net_input_for_tick(&history, 9u, &set));
    for (uint32_t tick = 10u; tick <= 12u; tick++) {
        mdkr_net_input_set_current_tick(&history, tick);
        assert(mdkr_net_input_for_tick(&history, tick, &set));
        assert(set.samples[0].buttons == go.buttons);
    }
    assert(mdkr_net_input_submit(&history, 0u, 10u, &release) ==
           MDKR_NET_SUBMIT_CORRECTED);
    assert(!mdkr_net_input_copy_existing(&history, 11u, &set));
    before = history;
    assert(!mdkr_net_input_rebuild_predictions(&history, 8u, 12u));
    assert(memcmp(&history, &before, sizeof(history)) == 0);
    assert(mdkr_net_input_rebuild_predictions(&history, 10u, 12u));
    assert(mdkr_net_input_copy_existing(&history, 11u, &set));
    assert(set.samples[0].buttons == release.buttons &&
           set.status[0] == MDKR_NET_INPUT_PREDICTED);

    puts("test_net_input: PASS");
    return 0;
}
