/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_impairment.h"

int main(void) {
    const MdkrNetImpairmentProfile profile = {
        .latency_ticks = 4u,
        .jitter_ticks = 3u,
        .loss_per_thousand = 100u,
        .duplicate_per_thousand = 200u,
    };
    MdkrNetImpairment first, second;
    MdkrNetSimPacket left, right;
    unsigned received = 0u;
    mdkr_net_impairment_init(&first, 12345u, profile);
    mdkr_net_impairment_init(&second, 12345u, profile);
    for (uint32_t tick = 0u; tick < 100u; tick++) {
        const uint32_t payload[2] = {tick, tick ^ 0xa5a5a5a5u};
        assert(mdkr_net_impairment_send(&first, tick, 0u, 1u,
                                        payload, sizeof(payload)));
        assert(mdkr_net_impairment_send(&second, tick, 0u, 1u,
                                        payload, sizeof(payload)));
    }
    assert(first.dropped > 0u && first.duplicated > 0u);
    assert(first.dropped == second.dropped && first.duplicated == second.duplicated);
    for (uint32_t tick = 0u; tick < 120u; tick++) {
        for (;;) {
            const bool have_left = mdkr_net_impairment_receive(&first, tick, 1u, &left);
            const bool have_right = mdkr_net_impairment_receive(&second, tick, 1u, &right);
            assert(have_left == have_right);
            if (!have_left) break;
            assert(left.length == right.length && left.source == right.source &&
                   left.destination == right.destination &&
                   memcmp(left.bytes, right.bytes, left.length) == 0);
            received++;
        }
    }
    assert(received == 100u - first.dropped + first.duplicated);
    assert(!mdkr_net_impairment_send(&first, 0u, 0u, 1u, &first,
                                     MDKR_NET_SIM_MAX_BYTES + 1u));
    assert(!mdkr_net_impairment_receive(&first, 0u, 4u, &left));

    {
        MdkrNetImpairmentProfile named;
        MdkrNetImpairmentProfile unchanged;
        memset(&unchanged, 0xA5, sizeof(unchanged));
        named = unchanged;
        assert(!mdkr_net_impairment_named_profile(
            MDKR_NET_PROFILE_COUNT, 30u, &named));
        assert(memcmp(&named, &unchanged, sizeof(named)) == 0);
        assert(!mdkr_net_impairment_named_profile(
            MDKR_NET_PROFILE_LAN, 60u, &named));
        assert(memcmp(&named, &unchanged, sizeof(named)) == 0);
        for (unsigned name = 0u; name < MDKR_NET_PROFILE_COUNT; name++) {
            assert(mdkr_net_impairment_named_profile(
                (MdkrNetImpairmentProfileName)name, 30u, &named));
            assert(named.max_deliveries_per_tick != 0u);
        }
    }

    {
        MdkrNetImpairmentProfile outage;
        MdkrNetImpairment simulator;
        assert(mdkr_net_impairment_named_profile(
            MDKR_NET_PROFILE_TWO_SECOND_OUTAGE, 30u, &outage));
        assert(outage.outage_start_tick == 30u &&
               outage.outage_end_tick == 90u);
        mdkr_net_impairment_init(&simulator, 7u, outage);
        for (uint32_t tick = 0u; tick < 120u; tick++) {
            assert(mdkr_net_impairment_send(
                &simulator, tick, 0u, 1u, &tick, sizeof(tick)));
        }
        assert(simulator.outage_dropped == 60u &&
               simulator.dropped == 60u);
    }

    {
        MdkrNetImpairmentProfile forced;
        MdkrNetImpairment simulator;
        uint32_t payload = UINT32_C(0x12345678);
        memset(&forced, 0, sizeof(forced));
        forced.reorder_per_thousand = 1000u;
        forced.reorder_extra_ticks = 5u;
        forced.malformed_per_thousand = 1000u;
        mdkr_net_impairment_init(&simulator, 9u, forced);
        assert(mdkr_net_impairment_send(
            &simulator, 0u, 0u, 1u, &payload, sizeof(payload)));
        assert(simulator.reordered == 1u && simulator.corrupted == 1u);
        assert(!mdkr_net_impairment_receive(&simulator, 4u, 1u, &left));
        assert(mdkr_net_impairment_receive(&simulator, 5u, 1u, &left));
        assert(memcmp(left.bytes, &payload, sizeof(payload)) != 0);
    }

    {
        MdkrNetImpairmentProfile capped;
        MdkrNetImpairment simulator;
        uint32_t payload = 1u;
        memset(&capped, 0, sizeof(capped));
        capped.max_deliveries_per_tick = 1u;
        mdkr_net_impairment_init(&simulator, 11u, capped);
        for (unsigned index = 0u; index < 3u; index++) {
            assert(mdkr_net_impairment_send(
                &simulator, 0u, 0u, 1u, &payload, sizeof(payload)));
        }
        assert(mdkr_net_impairment_receive(&simulator, 0u, 1u, &left));
        assert(!mdkr_net_impairment_receive(&simulator, 0u, 1u, &left));
        assert(simulator.throttled == 1u);
        assert(mdkr_net_impairment_receive(&simulator, 1u, 1u, &left));
    }

    {
        MdkrNetImpairmentProfile adversarial;
        MdkrNetImpairment a, b;
        assert(mdkr_net_impairment_named_profile(
            MDKR_NET_PROFILE_ADVERSARIAL, 25u, &adversarial));
        mdkr_net_impairment_init(&a, UINT64_C(0xfeedbeef), adversarial);
        mdkr_net_impairment_init(&b, UINT64_C(0xfeedbeef), adversarial);
        for (uint32_t tick = 0u; tick < 200u; tick++) {
            assert(mdkr_net_impairment_send(
                &a, tick, 2u, 3u, &tick, sizeof(tick)));
            assert(mdkr_net_impairment_send(
                &b, tick, 2u, 3u, &tick, sizeof(tick)));
        }
        assert(memcmp(&a, &b, sizeof(a)) == 0);
        assert(a.dropped > 0u && a.duplicated > 0u &&
               a.reordered > 0u && a.corrupted > 0u);
    }
    puts("test_net_impairment: PASS");
    return 0;
}
