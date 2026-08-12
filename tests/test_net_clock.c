/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_clock.h"

int main(void) {
    MdkrNetClockProfile profile;
    MdkrNetClock left, right;
    MdkrNetClockStep a, b;
    unsigned offered = 0u;
    unsigned sleeping = 0u;
    unsigned woke = 0u;
    unsigned long_frames = 0u;

    assert(mdkr_net_clock_named_profile(
        MDKR_NET_PROFILE_ADVERSARIAL, 30u, 2u, &profile));
    assert(mdkr_net_clock_init(&left, &profile));
    assert(mdkr_net_clock_init(&right, &profile));
    for (unsigned index = 0u; index < 200u; index++) {
        a = mdkr_net_clock_step(&left);
        b = mdkr_net_clock_step(&right);
        assert(memcmp(&a, &b, sizeof(a)) == 0);
        assert(a.host_opportunity == index);
        if (index != 0u) assert(a.monotonic_us > 0);
        offered += a.tick_offered;
        sleeping += a.sleeping;
        woke += a.woke;
        long_frames += a.long_frame;
    }
    assert(memcmp(&left, &right, sizeof(left)) == 0);
    assert(offered == left.authored_offers && offered < 200u);
    assert(sleeping == 15u && left.sleep_skips == 15u);
    assert(woke == 1u);
    assert(long_frames == left.long_frames && long_frames > 0u);
    assert(left.skipped_offers > 0u);

    {
        MdkrNetClockProfile endpoint0, endpoint1;
        MdkrNetClock clock0, clock1;
        assert(mdkr_net_clock_named_profile(
            MDKR_NET_PROFILE_REGIONAL_VARIABLE, 25u, 0u, &endpoint0));
        assert(mdkr_net_clock_named_profile(
            MDKR_NET_PROFILE_REGIONAL_VARIABLE, 25u, 1u, &endpoint1));
        assert(endpoint0.initial_offset_us != endpoint1.initial_offset_us &&
               endpoint0.drift_ppm != endpoint1.drift_ppm);
        assert(mdkr_net_clock_init(&clock0, &endpoint0));
        assert(mdkr_net_clock_init(&clock1, &endpoint1));
        for (unsigned index = 0u; index < 1000u; index++) {
            (void)mdkr_net_clock_step(&clock0);
            (void)mdkr_net_clock_step(&clock1);
        }
        assert(clock0.authored_offers == clock1.authored_offers);
        assert(clock0.monotonic_us != clock1.monotonic_us);
    }

    {
        MdkrNetClock invalid;
        memset(&profile, 0, sizeof(profile));
        assert(!mdkr_net_clock_init(&invalid, &profile));
        profile.nominal_step_us = 33333u;
        profile.drift_ppm = 100001;
        assert(!mdkr_net_clock_init(&invalid, &profile));
        assert(!mdkr_net_clock_named_profile(
            MDKR_NET_PROFILE_LAN, 60u, 0u, &profile));
        assert(!mdkr_net_clock_named_profile(
            MDKR_NET_PROFILE_LAN, 30u, 4u, &profile));
    }

    puts("test_net_clock: PASS");
    return 0;
}
