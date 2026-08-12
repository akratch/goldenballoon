/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_impairment.h"
#include "platform/rollback/rollback_driver.h"

#define ENDPOINTS 4u
#define TICKS 64u

typedef struct World {
    int64_t position[ENDPOINTS];
    uint32_t rng;
    uint32_t tick;
} World;

typedef struct WireInput {
    uint32_t tick;
    MdkrInputSample sample;
} WireInput;

typedef struct Endpoint {
    World world;
    MdkrNetInputHistory inputs;
    MdkrRollbackSnapshotRegistry registry;
    MdkrRollbackRing ring;
    MdkrRollbackDriver driver;
} Endpoint;

static MdkrInputSample sample_for(unsigned slot, uint32_t tick) {
    MdkrInputSample sample;
    sample.buttons = (uint16_t)(((tick + slot * 3u) % 11u) == 0u ? 1u : 0u);
    sample.stick_x = (int8_t)((int)((tick * (slot + 3u)) % 31u) - 15);
    sample.stick_y = (int8_t)((int)((tick + slot * 7u) % 17u) - 8);
    sample.present = true;
    return sample;
}

static bool simulate(void *context, uint32_t tick,
                     const MdkrNetInputSet *inputs, bool resimulating) {
    World *world = (World *)context;
    (void)resimulating;
    if (world->tick != tick) return false;
    for (unsigned slot = 0u; slot < ENDPOINTS; slot++) {
        const MdkrInputSample *sample = &inputs->samples[slot];
        world->position[slot] += sample->stick_x * 3 + sample->stick_y * 2;
        if ((sample->buttons & 1u) != 0u) world->position[slot] += 100;
    }
    world->rng = world->rng * 1664525u + 1013904223u;
    world->tick++;
    return true;
}

static void init_endpoint(Endpoint *endpoint, unsigned index) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->world.rng = 77u;
    mdkr_net_input_init(&endpoint->inputs, 0u);
    mdkr_rollback_snapshot_registry_init(&endpoint->registry, 1000u + index);
    assert(mdkr_rollback_snapshot_register(&endpoint->registry,
        &endpoint->world, sizeof(endpoint->world), 1u, 0u));
    assert(mdkr_rollback_snapshot_freeze(&endpoint->registry, 999u));
    assert(mdkr_rollback_ring_init(&endpoint->ring, &endpoint->registry, 12u));
    assert(mdkr_rollback_ring_capture(&endpoint->ring, UINT32_MAX));
    assert(mdkr_rollback_driver_init(&endpoint->driver, &endpoint->inputs,
        &endpoint->ring, NULL, simulate, &endpoint->world, 0u, 8u));
}

static void deliver(MdkrNetImpairment *network, Endpoint endpoints[ENDPOINTS],
                    uint32_t receiver_tick) {
    for (unsigned destination = 0u; destination < ENDPOINTS; destination++) {
        MdkrNetSimPacket packet;
        while (mdkr_net_impairment_receive(network, receiver_tick,
                                            destination, &packet)) {
            WireInput wire;
            MdkrNetInputSubmitResult result;
            assert(packet.length == sizeof(wire));
            memcpy(&wire, packet.bytes, sizeof(wire));
            result = mdkr_net_input_submit(&endpoints[destination].inputs,
                                           packet.source, wire.tick, &wire.sample);
            assert(result == MDKR_NET_SUBMIT_ACCEPTED ||
                   result == MDKR_NET_SUBMIT_CORRECTED ||
                   result == MDKR_NET_SUBMIT_DUPLICATE);
        }
    }
}

int main(void) {
    Endpoint endpoints[ENDPOINTS];
    MdkrNetImpairment network;
    const MdkrNetImpairmentProfile profile = {
        .latency_ticks = 3u,
        .jitter_ticks = 2u,
        .duplicate_per_thousand = 50u,
    };
    World reference = {{0}, 77u, 0u};
    bool predicted_differed = false;

    mdkr_net_impairment_init(&network, 0x12345678u, profile);
    for (unsigned endpoint = 0u; endpoint < ENDPOINTS; endpoint++) {
        init_endpoint(&endpoints[endpoint], endpoint);
    }
    for (uint32_t tick = 0u; tick < TICKS; tick++) {
        for (unsigned source = 0u; source < ENDPOINTS; source++) {
            const WireInput wire = {tick, sample_for(source, tick)};
            assert(mdkr_net_input_submit(&endpoints[source].inputs, source,
                                         tick, &wire.sample) == MDKR_NET_SUBMIT_ACCEPTED);
            for (unsigned destination = 0u; destination < ENDPOINTS; destination++) {
                if (destination == source) continue;
                assert(mdkr_net_impairment_send(&network, tick, source, destination,
                                                &wire, sizeof(wire)));
            }
        }
        deliver(&network, endpoints, tick);
        for (unsigned endpoint = 0u; endpoint < ENDPOINTS; endpoint++) {
            assert(mdkr_rollback_driver_advance(&endpoints[endpoint].driver));
        }
        MdkrNetInputSet exact;
        memset(&exact, 0, sizeof(exact)); exact.tick = tick;
        for (unsigned slot = 0u; slot < ENDPOINTS; slot++) {
            exact.samples[slot] = sample_for(slot, tick);
            exact.status[slot] = MDKR_NET_INPUT_RECEIVED;
        }
        exact.all_received = true;
        assert(simulate(&reference, tick, &exact, false));
    }
    for (unsigned endpoint = 0u; endpoint < ENDPOINTS; endpoint++) {
        if (memcmp(&endpoints[endpoint].world, &reference, sizeof(reference)) != 0) {
            predicted_differed = true;
        }
    }
    assert(predicted_differed); /* rollback-disabled positive control */
    deliver(&network, endpoints, TICKS + 16u);
    for (unsigned endpoint = 0u; endpoint < ENDPOINTS; endpoint++) {
        assert(mdkr_rollback_driver_reconcile(&endpoints[endpoint].driver));
        assert(memcmp(&endpoints[endpoint].world, &reference, sizeof(reference)) == 0);
        assert(endpoints[endpoint].driver.stats.rollback_count > 0u);
        assert(endpoints[endpoint].driver.stats.deepest_rollback <= 8u);
        mdkr_rollback_ring_destroy(&endpoints[endpoint].ring);
    }
    puts("test_rollback_four_endpoint: PASS");
    return 0;
}
