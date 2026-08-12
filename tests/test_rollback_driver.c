/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/rollback/rollback_driver.h"

typedef struct ToyState { int position[4]; uint32_t ticks; } ToyState;

static bool simulate(void *context, uint32_t tick,
                     const MdkrNetInputSet *inputs, bool resimulating) {
    ToyState *state = (ToyState *)context;
    (void)resimulating;
    assert(state->ticks == tick);
    for (unsigned slot = 0u; slot < 4u; slot++) {
        if ((inputs->samples[slot].buttons & 1u) != 0u) state->position[slot]++;
        state->position[slot] += inputs->samples[slot].stick_x;
    }
    state->ticks++;
    return true;
}

static MdkrInputSample input(unsigned buttons, int x) {
    const MdkrInputSample value = {(uint16_t)buttons, (int8_t)x, 0, true};
    return value;
}

int main(void) {
    ToyState predicted = {{0}, 0u};
    ToyState reference = {{0}, 0u};
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackRing ring;
    MdkrNetInputHistory history;
    MdkrRollbackDriver driver;
    MdkrNetInputSet set;
    const MdkrInputSample go = input(1u, 2);
    const MdkrInputSample neutral = input(0u, 0);
    const MdkrInputSample correction = input(1u, 5);

    mdkr_rollback_snapshot_registry_init(&registry, 123u);
    assert(mdkr_rollback_snapshot_register(
        &registry, &predicted, sizeof(predicted), 1u, 0u));
    assert(mdkr_rollback_snapshot_freeze(&registry, 456u));
    assert(mdkr_rollback_ring_init(&ring, &registry, 8u));
    assert(mdkr_rollback_ring_capture(&ring, UINT32_MAX));
    mdkr_net_input_init(&history, 0u);
    assert(mdkr_rollback_driver_init(&driver, &history, &ring, NULL,
                                     simulate, &predicted, 0u, 7u));

    for (uint32_t tick = 0u; tick < 3u; tick++) {
        assert(mdkr_net_input_submit(&history, 0u, tick, &go) ==
               MDKR_NET_SUBMIT_ACCEPTED);
        for (unsigned slot = 2u; slot < 4u; slot++) {
            assert(mdkr_net_input_submit(&history, slot, tick, &neutral) ==
                   MDKR_NET_SUBMIT_ACCEPTED);
        }
        assert(mdkr_rollback_driver_advance(&driver));
    }
    /* Slot 1 was predicted neutral for ticks 0..2. A late tick-0 press and a
     * known release at tick 1 force a three-tick correction before tick 3. */
    assert(mdkr_net_input_submit(&history, 1u, 0u, &correction) ==
           MDKR_NET_SUBMIT_CORRECTED);
    assert(mdkr_net_input_submit(&history, 1u, 1u, &neutral) ==
           MDKR_NET_SUBMIT_ACCEPTED);
    assert(mdkr_net_input_submit(&history, 1u, 2u, &neutral) ==
           MDKR_NET_SUBMIT_ACCEPTED);
    assert(mdkr_net_input_submit(&history, 0u, 3u, &go) ==
           MDKR_NET_SUBMIT_ACCEPTED);
    for (unsigned slot = 1u; slot < 4u; slot++) {
        assert(mdkr_net_input_submit(&history, slot, 3u, &neutral) ==
               MDKR_NET_SUBMIT_ACCEPTED);
    }
    assert(mdkr_rollback_driver_advance(&driver));
    assert(driver.stats.rollback_count == 1u &&
           driver.stats.resimulated_ticks == 3u && driver.stats.deepest_rollback == 3u);

    /* Clean delayed-input reference. */
    for (uint32_t tick = 0u; tick < 4u; tick++) {
        memset(&set, 0, sizeof(set)); set.tick = tick;
        for (unsigned slot = 0u; slot < 4u; slot++) {
            set.samples[slot] = neutral; set.status[slot] = MDKR_NET_INPUT_RECEIVED;
        }
        set.samples[0] = go;
        if (tick == 0u) set.samples[1] = correction;
        assert(simulate(&reference, tick, &set, false));
    }
    assert(memcmp(&predicted, &reference, sizeof(predicted)) == 0);
    assert(mdkr_rollback_driver_reconcile(&driver));
    mdkr_rollback_ring_destroy(&ring);
    puts("test_rollback_driver: PASS");
    return 0;
}
