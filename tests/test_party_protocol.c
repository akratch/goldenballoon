#include "party/party_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    failures++; } } while (0)

static MdkrPartyPadPacket fixture(void) {
    MdkrPartyPadPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.flags = MDKR_PARTY_PAD_FLAG_PRESENT |
                   MDKR_PARTY_PAD_FLAG_HAS_EDGES;
    packet.connection_sequence = 0x01020304u;
    packet.sample_sequence = 100u;
    packet.sender_time_ms = 123456u;
    packet.buttons = 0x8000u;
    packet.stick_x = 40;
    packet.stick_y = -20;
    packet.edge_count = 2u;
    packet.edges[0] = (MdkrPartyPadEdge){2u, 0x8000u, 20, 0};
    packet.edges[1] = (MdkrPartyPadEdge){1u, 0u, 0, 0};
    return packet;
}

int main(void) {
    static const uint8_t golden[] = {
        0x47, 0x42, 0x01, 0x01, 0x05, 0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0xe2, 0x40, 0x80,
        0x00, 0x28, 0xec, 0x02, 0x02, 0x80, 0x00, 0x14, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x3c
    };
    MdkrPartyPadPacket packet = fixture();
    MdkrPartyPadPacket decoded;
    MdkrPartyPadPacket sentinel;
    uint8_t encoded[MDKR_PARTY_PAD_MAX_BYTES];
    uint8_t mutated[MDKR_PARTY_PAD_MAX_BYTES];
    size_t length = 0u;
    size_t index;

    memset(&decoded, 0, sizeof(decoded));
    CHECK(mdkr_party_pad_encode(&packet, encoded, sizeof(encoded), &length));
    CHECK(length == MDKR_PARTY_PAD_FIXED_BYTES + 2u * MDKR_PARTY_PAD_EDGE_BYTES);
    CHECK(length == sizeof(golden));
    CHECK(memcmp(encoded, golden, sizeof(golden)) == 0);
    CHECK(mdkr_party_pad_decode(encoded, length, &decoded) == MDKR_PARTY_DECODE_OK);
    CHECK(memcmp(&packet, &decoded, sizeof(packet)) == 0);

    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (index = 0u; index < length; index++) {
        MdkrPartyDecodeResult result;
        memcpy(mutated, encoded, length);
        mutated[index] ^= 0x01u;
        decoded = sentinel;
        result = mdkr_party_pad_decode(mutated, length, &decoded);
        CHECK(result != MDKR_PARTY_DECODE_OK);
        CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
    }

    decoded = sentinel;
    CHECK(mdkr_party_pad_decode(encoded, length - 1u, &decoded) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
    decoded = sentinel;
    CHECK(mdkr_party_pad_decode(encoded, length + 1u, &decoded) !=
          MDKR_PARTY_DECODE_OK);
    CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);

    packet.flags |= MDKR_PARTY_PAD_FLAG_NEUTRAL;
    CHECK(!mdkr_party_pad_encode(&packet, encoded, sizeof(encoded), &length));
    packet = fixture();
    packet.edges[1].sequence_delta = packet.edges[0].sequence_delta;
    CHECK(!mdkr_party_pad_encode(&packet, encoded, sizeof(encoded), &length));
    packet = fixture();
    packet.buttons |= 0x0040u;
    CHECK(!mdkr_party_pad_encode(&packet, encoded, sizeof(encoded), &length));

    if (failures != 0) return 1;
    puts("party_protocol: PASS");
    return 0;
}
