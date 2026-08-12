#include "party/remote_pad.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    failures++; } } while (0)

static size_t make_packet(
    uint8_t *bytes, uint32_t connection, uint32_t sequence,
    uint16_t buttons, const MdkrPartyPadEdge *edges, uint8_t edge_count) {
    MdkrPartyPadPacket packet;
    size_t length = 0u;
    memset(&packet, 0, sizeof(packet));
    packet.flags = MDKR_PARTY_PAD_FLAG_PRESENT;
    if (buttons == 0u) packet.flags |= MDKR_PARTY_PAD_FLAG_NEUTRAL;
    if (edge_count != 0u) packet.flags |= MDKR_PARTY_PAD_FLAG_HAS_EDGES;
    packet.connection_sequence = connection;
    packet.sample_sequence = sequence;
    packet.sender_time_ms = 500u;
    packet.buttons = buttons;
    packet.edge_count = edge_count;
    if (edge_count != 0u) {
        memcpy(packet.edges, edges, edge_count * sizeof(*edges));
    }
    CHECK(mdkr_party_pad_encode(
        &packet, bytes, MDKR_PARTY_PAD_MAX_BYTES, &length));
    return length;
}

int main(void) {
    MdkrRemotePad pad;
    MdkrRemotePadTransition transition;
    const MdkrPartyPadEdge tap[] = {
        {2u, 0x8000u, 0, 0},
        {1u, 0u, 0, 0},
    };
    uint8_t bytes[MDKR_PARTY_PAD_MAX_BYTES];
    size_t length;

    mdkr_remote_pad_init(&pad, 7u);
    length = make_packet(bytes, 7u, 100u, 0u, tap, 2u);
    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1000u) ==
          MDKR_PARTY_DECODE_OK);
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(transition.sequence == 98u && transition.sample.buttons == 0x8000u);
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(transition.sequence == 99u && transition.sample.buttons == 0u &&
          transition.sample.present);
    CHECK(!mdkr_remote_pad_pop(&pad, &transition));

    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1010u) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(pad.stats.duplicates == 1u);
    length = make_packet(bytes, 8u, 101u, 0x8000u, NULL, 0u);
    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1020u) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(pad.stats.stale_connections == 1u);

    CHECK(!mdkr_remote_pad_expire(&pad, 1249u));
    CHECK(mdkr_remote_pad_expire(&pad, 1250u));
    CHECK(!mdkr_remote_pad_expire(&pad, 1500u));
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(!transition.sample.present && transition.sample.buttons == 0u);
    CHECK(pad.stats.timeouts == 1u);

    mdkr_remote_pad_rebind(&pad, 9u, 1600u);
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(!transition.sample.present);
    length = make_packet(bytes, 9u, 1u, 0x8000u, NULL, 0u);
    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1601u) ==
          MDKR_PARTY_DECODE_OK);
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(transition.sample.present && transition.sample.buttons == 0x8000u);

    /* Eight alternating recovered edges fit; current state would be the ninth
     * mutation and must collapse fail-safe to one neutral event. */
    {
        MdkrPartyPadEdge edges[MDKR_PARTY_PAD_MAX_EDGES];
        unsigned index;
        for (index = 0u; index < MDKR_PARTY_PAD_MAX_EDGES; index++) {
            edges[index].sequence_delta = (uint8_t)(8u - index);
            edges[index].buttons = (index & 1u) == 0u ? 0u : 0x8000u;
            edges[index].stick_x = 0;
            edges[index].stick_y = 0;
        }
        mdkr_remote_pad_init(&pad, 11u);
        length = make_packet(bytes, 11u, 20u, 0x4000u, edges, 8u);
        CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 2000u) ==
              MDKR_PARTY_DECODE_OK);
        CHECK(pad.stats.overflow_neutralizations == 1u);
        CHECK(pad.count == 1u);
        CHECK(mdkr_remote_pad_pop(&pad, &transition));
        CHECK(!transition.sample.present);
    }

    if (failures != 0) return 1;
    puts("remote_pad: PASS");
    return 0;
}
