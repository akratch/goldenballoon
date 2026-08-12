/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_local_input.h"

static MdkrInputSample sample(unsigned buttons, int x, int y, bool present) {
    const MdkrInputSample value = {
        (uint16_t)buttons, (int8_t)x, (int8_t)y, present};
    return value;
}

static MdkrNetRoster roster(const uint8_t *slots, unsigned count) {
    MdkrNetRoster value;
    memset(&value, 0, sizeof(value));
    memset(value.local_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(value.local_to_canonical));
    memset(value.viewport_to_canonical, MDKR_NET_ROSTER_UNMAPPED,
           sizeof(value.viewport_to_canonical));
    value.canonical_player_count = 4u;
    value.local_seat_count = (uint8_t)count;
    for (unsigned index = 0u; index < count; index++) {
        value.local_to_canonical[index] = slots[index];
    }
    return value;
}

int main(void) {
    const MdkrInputSample neutral = sample(0u, 0, 0, false);
    MdkrInputSample base[MDKR_MATCH_SLOTS] = {
        sample(1u, 1, 2, true), sample(2u, 3, 4, true),
        sample(4u, 5, 6, true), sample(8u, 7, 8, true)};
    MdkrCanonicalInputFrame output;
    MdkrCanonicalInputFrame sentinel;
    const uint8_t slot1[] = {1u};
    const uint8_t couch[] = {0u, 2u};
    MdkrNetRoster one = roster(slot1, 1u);
    MdkrNetRoster two = roster(couch, 2u);
    MdkrNetRoster verifier = roster(NULL, 0u);
    MdkrInputSample local[2] = {
        sample(0x8000u, -30, 40, true),
        sample(0x4000u, 50, -60, true)};

    assert(mdkr_net_local_input_merge(&one, local, 1u, base, &output));
    assert(output.locally_authored_mask == 0x02u);
    assert(output.slots[1].buttons == 0x8000u &&
           output.slots[1].stick_x == -30);
    assert(memcmp(&output.slots[0], &base[0], sizeof(base[0])) == 0);
    assert(memcmp(&output.slots[2], &base[2], sizeof(base[2])) == 0);
    assert(memcmp(&output.slots[3], &base[3], sizeof(base[3])) == 0);

    assert(mdkr_net_local_input_merge(&two, local, 2u, base, &output));
    assert(output.locally_authored_mask == 0x05u);
    assert(output.slots[0].buttons == 0x8000u);
    assert(output.slots[2].buttons == 0x4000u);
    assert(output.slots[1].buttons == base[1].buttons);
    assert(output.slots[3].buttons == base[3].buttons);

    /* A verifier authors nothing and cannot erase transport-owned slots. */
    assert(mdkr_net_local_input_merge(
        &verifier, NULL, 0u, base, &output));
    assert(output.locally_authored_mask == 0u);
    assert(memcmp(output.slots, base, sizeof(base)) == 0);

    memset(&sentinel, 0xa5, sizeof(sentinel));
    output = sentinel;
    local[0].stick_x = 81;
    assert(!mdkr_net_local_input_merge(&one, local, 1u, base, &output));
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    local[0] = sample(1u, 0, 0, false);
    assert(!mdkr_net_local_input_merge(&one, local, 1u, base, &output));
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    local[0] = sample(1u, 0, 0, true);
    assert(!mdkr_net_local_input_merge(&one, local, 0u, base, &output));
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);

    base[3] = sample(1u, 0, 0, false);
    assert(!mdkr_net_local_input_merge(&one, local, 1u, base, &output));
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);
    base[3] = neutral;
    one.local_to_canonical[1] = 0u; /* non-unmapped tail */
    assert(!mdkr_net_local_input_merge(&one, local, 1u, base, &output));
    assert(memcmp(&output, &sentinel, sizeof(output)) == 0);

    puts("test_net_local_input: PASS");
    return 0;
}
