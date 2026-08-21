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

    /* A duplicate is still proof of life: it refreshes liveness (now_ms=1010)
     * without publishing a sample, so the silence timeout now counts from the
     * keepalive rather than the last state change. */
    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1010u) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(pad.stats.duplicates == 1u);
    /* A wrong-connection frame is NOT this phone and must not refresh liveness. */
    length = make_packet(bytes, 8u, 101u, 0x8000u, NULL, 0u);
    CHECK(mdkr_remote_pad_accept(&pad, bytes, length, 1020u) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(pad.stats.stale_connections == 1u);

    CHECK(!mdkr_remote_pad_expire(&pad, 1259u));
    CHECK(mdkr_remote_pad_expire(&pad, 1260u));
    CHECK(!mdkr_remote_pad_expire(&pad, 1500u));
    CHECK(mdkr_remote_pad_pop(&pad, &transition));
    CHECK(!transition.sample.present && transition.sample.buttons == 0u);
    CHECK(pad.stats.timeouts == 1u);

    /* A held button emits no state change, so the phone's 50 ms heartbeat
     * resends the same sample_sequence. Those keepalives must refresh liveness
     * or the 250 ms silence timeout neutralizes a legitimate hold (the reported
     * "a hold registers as a tap"). */
    {
        uint8_t hold[MDKR_PARTY_PAD_MAX_BYTES];
        size_t hlen;
        MdkrRemotePad hpad;
        MdkrRemotePadTransition htrans;
        mdkr_remote_pad_init(&hpad, 5u);
        hlen = make_packet(hold, 5u, 10u, 0x8000u, NULL, 0u); /* press + hold A */
        CHECK(mdkr_remote_pad_accept(&hpad, hold, hlen, 1000u) ==
              MDKR_PARTY_DECODE_OK);
        CHECK(mdkr_remote_pad_pop(&hpad, &htrans));
        CHECK(htrans.sample.buttons == 0x8000u && htrans.sample.present);
        /* Keepalives (duplicates) at +100 ms and +300 ms keep the pad alive
         * well past the old 250 ms deadline. */
        CHECK(mdkr_remote_pad_accept(&hpad, hold, hlen, 1100u) !=
              MDKR_PARTY_DECODE_OK);
        CHECK(!mdkr_remote_pad_expire(&hpad, 1200u));
        CHECK(mdkr_remote_pad_accept(&hpad, hold, hlen, 1300u) !=
              MDKR_PARTY_DECODE_OK);
        CHECK(!mdkr_remote_pad_expire(&hpad, 1400u));
        CHECK(!mdkr_remote_pad_pop(&hpad, &htrans)); /* no spurious neutral */
        /* Genuine silence 250 ms after the last keepalive finally fails safe. */
        CHECK(mdkr_remote_pad_expire(
            &hpad, 1300u + MDKR_REMOTE_PAD_TIMEOUT_MS));
        CHECK(mdkr_remote_pad_pop(&hpad, &htrans));
        CHECK(!htrans.sample.present && htrans.sample.buttons == 0u);

        /* Recovery: if silence already neutralized a hold, the next keepalive
         * re-asserts the phone's still-held state rather than leaving controls
         * stuck neutral. */
        mdkr_remote_pad_init(&hpad, 6u);
        hlen = make_packet(hold, 6u, 20u, 0x8000u, NULL, 0u);
        CHECK(mdkr_remote_pad_accept(&hpad, hold, hlen, 2000u) ==
              MDKR_PARTY_DECODE_OK);
        CHECK(mdkr_remote_pad_pop(&hpad, &htrans));
        CHECK(htrans.sample.buttons == 0x8000u);
        CHECK(mdkr_remote_pad_expire(
            &hpad, 2000u + MDKR_REMOTE_PAD_TIMEOUT_MS));
        CHECK(mdkr_remote_pad_pop(&hpad, &htrans));
        CHECK(!htrans.sample.present && htrans.sample.buttons == 0u);
        CHECK(mdkr_remote_pad_accept(&hpad, hold, hlen, 2300u) !=
              MDKR_PARTY_DECODE_OK);
        CHECK(mdkr_remote_pad_pop(&hpad, &htrans));
        CHECK(htrans.sample.present && htrans.sample.buttons == 0x8000u);
    }

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
