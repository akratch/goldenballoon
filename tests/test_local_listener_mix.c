/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/local_listener_mix.h"

static MdkrNetRoster roster_with_views(const uint8_t *views, uint8_t count) {
    MdkrNetRoster roster;
    unsigned index;
    memset(&roster, 0, sizeof(roster));
    roster.canonical_player_count = 4u;
    roster.viewport_count = count;
    memset(roster.viewport_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(roster.viewport_to_canonical));
    for (index = 0u; index < count; index++) {
        roster.viewport_to_canonical[index] = views[index];
    }
    return roster;
}

int main(void) {
    const int32_t volumes[MDKR_MATCH_SLOTS] = {90, 45, 110, 20};
    const uint8_t pans[MDKR_MATCH_SLOTS] = {10, 30, 100, 120};
    const uint8_t slot_one[] = {1u};
    const uint8_t couch[] = {0u, 2u};
    MdkrNetRoster one = roster_with_views(slot_one, 1u);
    MdkrNetRoster two = roster_with_views(couch, 2u);
    MdkrNetRoster headless = roster_with_views(NULL, 0u);
    MdkrLocalListenerMix mix;

    assert(mdkr_local_listener_mix_select(
        NULL, volumes, pans, 4u, 77, 23u, &mix));
    assert(!mix.endpoint_local && mix.volume == 77 && mix.pan == 23u);

    assert(mdkr_local_listener_mix_select(
        &one, volumes, pans, 4u, 77, 23u, &mix));
    assert(mix.endpoint_local && mix.listener_count == 1u);
    assert(mix.volume == 45 && mix.pan == 30u);

    assert(mdkr_local_listener_mix_select(
        &two, volumes, pans, 4u, 77, 23u, &mix));
    assert(mix.listener_count == 2u && mix.volume == 110);
    assert(mix.pan == 64u);

    assert(mdkr_local_listener_mix_select(
        &headless, volumes, pans, 4u, 77, 23u, &mix));
    assert(mix.listener_count == 0u && mix.volume == 0 && mix.pan == 64u);

    one.viewport_to_canonical[0] = 4u;
    assert(!mdkr_local_listener_mix_select(
        &one, volumes, pans, 4u, 77, 23u, &mix));
    two.viewport_to_canonical[1] = two.viewport_to_canonical[0];
    assert(!mdkr_local_listener_mix_select(
        &two, volumes, pans, 4u, 77, 23u, &mix));
    assert(!mdkr_local_listener_mix_select(
        &headless, volumes, pans, 3u, 77, 23u, &mix));
    assert(!mdkr_local_listener_mix_select(
        NULL, volumes, pans, 4u, -1, 23u, &mix));

    puts("test_local_listener_mix: PASS");
    return 0;
}
