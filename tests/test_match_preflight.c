#include "platform/net/match_preflight.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int bytes_equal_hex(const uint8_t *bytes, size_t count,
                           const char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < count; index++) {
        if (hex[index * 2u] != digits[bytes[index] >> 4u] ||
            hex[index * 2u + 1u] != digits[bytes[index] & 15u])
            return 0;
    }
    return hex[count * 2u] == '\0';
}

static int bytes_all_zero(const uint8_t *bytes, size_t count) {
    size_t index;
    for (index = 0u; index < count; index++)
        if (bytes[index] != 0u) return 0;
    return 1;
}

static MdkrMatchLaunchDescriptorV1 descriptor(void) {
    MdkrMatchLaunchDescriptorV1 value;
    unsigned index;
    memset(&value, 0, sizeof(value));
    value.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    value.manifest.match_epoch = 7u;
    value.manifest.protocol_version = 1u;
    for (index = 0u; index < sizeof(value.manifest.build_id); index++)
        value.manifest.build_id[index] = (uint8_t)(index + 1u);
    for (index = 0u; index < sizeof(value.manifest.gameplay_digest); index++)
        value.manifest.gameplay_digest[index] = (uint8_t)(0x80u + index);
    value.manifest.slot_owner[0] = 10u;
    value.manifest.slot_owner[1] = 20u;
    value.manifest.slot_owner[2] = 30u;
    value.manifest.rng_seed = UINT64_C(0x1122334455667788);
    value.manifest.track_id = 7u;
    value.manifest.rom_revision = MDKR_ROM_US_11;
    value.manifest.cadence_hz = 30u;
    value.manifest.slot_count = 3u;
    value.manifest.rules = MDKR_MATCH_RULES_STANDARD_RACE;
    value.manifest.vehicle_mask = 7u;
    value.manifest.input_delay = 2u;
    value.selections[0].selection_revision = 1u;
    value.selections[0].character_id = 1u;
    value.selections[0].vehicle_id = 0u;
    value.selections[1].selection_revision = 2u;
    value.selections[1].character_id = 2u;
    value.selections[1].vehicle_id = 1u;
    value.selections[2].selection_revision = 3u;
    value.selections[2].character_id = 3u;
    value.selections[2].vehicle_id = 2u;
    value.selections[3].character_id = MDKR_MATCH_NO_CHARACTER;
    value.selections[3].vehicle_id = MDKR_MATCH_NO_VEHICLE;
    return value;
}

static MdkrMatchPeerGraph graph(unsigned connected) {
    MdkrMatchPeerEndpoint endpoints[3] = {{10u, 1u, connected ? 0x06u : 0u},
                                          {20u, 2u, connected ? 0x05u : 0u},
                                          {30u, 3u, connected ? 0x03u : 0u}};
    MdkrMatchPeerGraph value;
    memset(&value, 0, sizeof(value));
    expect(mdkr_match_peer_graph_init(&value, 7u, endpoints, 3u),
           "fixture graph initializes");
    return value;
}

static MdkrMatchPreflightAttestationV1
attestation(const MdkrMatchPreflightV1 *preflight, uint64_t endpoint_id,
            uint32_t generation, uint32_t sequence, uint8_t flags) {
    MdkrMatchPreflightAttestationV1 value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = MDKR_MATCH_PREFLIGHT_VERSION;
    value.match_epoch = preflight->match_epoch;
    value.connection_generation = generation;
    value.sequence = sequence;
    value.endpoint_id = endpoint_id;
    memcpy(value.descriptor_digest, preflight->descriptor_digest,
           sizeof(value.descriptor_digest));
    memcpy(value.transcript_digest, preflight->transcript_digest,
           sizeof(value.transcript_digest));
    memcpy(value.graph_digest, preflight->graph_digest,
           sizeof(value.graph_digest));
    value.flags = flags;
    return value;
}

static void expect_state(const MdkrMatchPreflightV1 *preflight,
                         MdkrMatchPreflightState state, uint64_t endpoint_id,
                         unsigned received) {
    MdkrMatchPreflightStatus result = mdkr_match_preflight_evaluate(preflight);
    expect(result.state == state && result.endpoint_id == endpoint_id &&
               result.received_count == received && result.required_count == 3u,
           "preflight status has exact state, endpoint and progress");
}

static MdkrMatchPreflightSubmitResult submit_report(
    MdkrMatchPreflightV1 *preflight,
    const MdkrMatchPreflightAttestationV1 *report) {
    return mdkr_match_preflight_submit(
        preflight, report->endpoint_id, report->connection_generation, report);
}

int main(void) {
    MdkrMatchLaunchDescriptorV1 launch = descriptor();
    MdkrMatchPeerGraph connected = graph(1u);
    MdkrMatchPeerGraph disconnected = graph(0u);
    MdkrMatchPeerEndpoint reordered_endpoints[3] = {
        {30u, 3u, 0x06u}, {10u, 1u, 0x05u}, {20u, 2u, 0x03u}};
    MdkrMatchPeerGraph reordered;
    MdkrMatchPreflightV1 preflight;
    MdkrMatchPreflightV1 untouched;
    MdkrMatchPreflightAttestationV1 report;
    uint8_t transcript[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t digest_untouched[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t reordered_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t wire[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    uint8_t mutated[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    MdkrMatchPreflightAttestationV1 decoded;
    MdkrMatchPreflightAttestationV1 decoded_untouched;
    MdkrMatchPreflightFragmentState fragments_state;
    MdkrMatchPreflightFragmentState fragments_untouched;
    MdkrMatchPeerEnvelopeContext fragment_context;
    MdkrMatchPeerEnvelopeContext wrong_fragment_context;
    uint8_t fragments[MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT]
                     [MDKR_MATCH_PEER_PAYLOAD_BYTES];
    unsigned index;

    memset(&fragment_context, 0, sizeof(fragment_context));
    fragment_context.key.match_epoch = 7u;
    fragment_context.key.source_endpoint_id = 20u;
    fragment_context.key.source_generation = 2u;
    fragment_context.key.destination_endpoint_id = 10u;
    fragment_context.key.destination_generation = 1u;
    fragment_context.sequence = 1u;
    fragment_context.payload_type =
        MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT;

    for (index = 0u; index < sizeof(transcript); index++)
        transcript[index] = (uint8_t)(0x40u + index);
    memset(digest, 0xa5, sizeof(digest));
    memcpy(digest_untouched, digest, sizeof(digest));
    launch.version = 99u;
    expect(!mdkr_match_preflight_descriptor_digest(&launch, digest) &&
               memcmp(digest, digest_untouched, sizeof(digest)) == 0,
           "invalid descriptor cannot mutate digest output");
    launch = descriptor();
    expect(mdkr_match_preflight_descriptor_digest(&launch, digest) &&
               digest[0] != 0u,
           "valid canonical descriptor produces SHA-256");
    expect(mdkr_match_preflight_graph_digest(&connected, digest) &&
               bytes_equal_hex(
                   digest, sizeof(digest),
                   "04868b4d34d94911193e5a68c1dab2fb1c97d544a04d6b45123d21c7d1639f71"),
           "canonical graph digest matches the native/browser vector");
    expect(mdkr_match_peer_graph_init(&reordered, 7u, reordered_endpoints, 3u) &&
               mdkr_match_preflight_graph_digest(&reordered,
                                                   reordered_digest) &&
               memcmp(digest, reordered_digest, sizeof(digest)) == 0,
           "equivalent reordered graph has the same canonical digest");
    reordered.endpoints[0].reachable_mask = 0x02u;
    expect(mdkr_match_peer_graph_init(&reordered, 7u, reordered.endpoints, 3u) &&
               mdkr_match_preflight_graph_digest(&reordered,
                                                   reordered_digest) &&
               memcmp(digest, reordered_digest, sizeof(digest)) != 0,
           "a directed reachability change alters the graph digest");
    memset(reordered_digest, 0xa5, sizeof(reordered_digest));
    memcpy(digest_untouched, reordered_digest, sizeof(reordered_digest));
    reordered.endpoint_count = 255u;
    expect(!mdkr_match_preflight_graph_digest(&reordered, reordered_digest) &&
               memcmp(reordered_digest, digest_untouched,
                      sizeof(reordered_digest)) == 0,
           "invalid graph cannot mutate digest output");

    memset(&preflight, 0xa5, sizeof(preflight));
    untouched = preflight;
    expect(!mdkr_match_preflight_init(&preflight, &launch, &connected,
                                      transcript, 10u, 2u) &&
               memcmp(&preflight, &untouched, sizeof(preflight)) == 0,
           "wrong local generation rejects without output mutation");
    launch.manifest.match_epoch = 8u;
    expect(!mdkr_match_preflight_init(&preflight, &launch, &connected,
                                      transcript, 10u, 1u) &&
               memcmp(&preflight, &untouched, sizeof(preflight)) == 0,
           "descriptor and graph epoch mismatch rejects atomically");
    launch = descriptor();
    expect(mdkr_match_preflight_init(&preflight, &launch, &connected,
                                     transcript, 10u, 1u),
           "valid immutable preflight expectations initialize");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS, 10u, 0u);

    report =
        attestation(&preflight, 20u, 2u, 9u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    for (index = 0u; index < sizeof(report.descriptor_digest); index++) {
        report.descriptor_digest[index] = (uint8_t)index;
        report.transcript_digest[index] = (uint8_t)(0xa0u + index);
        report.graph_digest[index] = (uint8_t)(0xc0u + index);
    }
    memset(wire, 0xa5, sizeof(wire));
    expect(
        mdkr_match_preflight_attestation_encode(&report, wire, sizeof(wire)) &&
            bytes_equal_hex(
                wire, sizeof(wire),
                "4d504631010700000000000700000002000000090000000000000014"
                "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e"
                "1f"
                "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbe"
                "bf"
                "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcddde"
                "df"),
        "MPF1 encoding matches the exact native/browser wire vector");
    memset(&decoded, 0xa5, sizeof(decoded));
    expect(
        mdkr_match_preflight_attestation_decode(wire, sizeof(wire), &decoded) &&
            memcmp(&decoded, &report, sizeof(decoded)) == 0,
        "canonical attestation round-trips exactly");
    memset(mutated, 0x5a, sizeof(mutated));
    expect(!mdkr_match_preflight_attestation_encode(&report, mutated,
                                                    sizeof(mutated) - 1u) &&
               mutated[0] == 0x5au && mutated[sizeof(mutated) - 1u] == 0x5au,
           "short encode capacity leaves output unchanged");

    for (index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT; index++)
        expect(mdkr_match_preflight_fragment_encode(&report, index,
                                                     fragments[index]),
               "each report fragment encodes into one fixed carrier payload");
    expect(fragments[0][0] == 0u && fragments[0][1] == 0u &&
               fragments[0][2] == 0u && fragments[0][3] == 9u &&
               fragments[0][4] == 0u && fragments[0][5] == 3u &&
               fragments[2][4] == 2u &&
               bytes_all_zero(fragments[2] + 14u,
                              MDKR_MATCH_PEER_PAYLOAD_BYTES - 14u),
           "fragment headers bind report sequence/index and tail padding is zero");
    memset(mutated, 0x5a, sizeof(mutated));
    expect(!mdkr_match_preflight_fragment_encode(
               &report, MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT, mutated) &&
               mutated[0] == 0x5au,
           "invalid fragment index leaves payload unchanged");
    memset(&fragments_state, 0xa5, sizeof(fragments_state));
    fragments_untouched = fragments_state;
    wrong_fragment_context = fragment_context;
    wrong_fragment_context.key.destination_endpoint_id =
        wrong_fragment_context.key.source_endpoint_id;
    expect(!mdkr_match_preflight_fragment_state_init(
               &fragments_state, &wrong_fragment_context.key) &&
               memcmp(&fragments_state, &fragments_untouched,
                      sizeof(fragments_state)) == 0,
           "invalid authenticated direction leaves reassembly state unchanged");
    expect(!mdkr_match_preflight_fragment_state_init(
               NULL, &fragment_context.key) &&
               mdkr_match_preflight_fragment_state_init(
                   &fragments_state, &fragment_context.key),
           "fragment reassembly state initializes canonically");
    wrong_fragment_context = fragment_context;
    wrong_fragment_context.payload_type = MDKR_MATCH_PEER_PAYLOAD_INPUT;
    fragments_untouched = fragments_state;
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &wrong_fragment_context, fragments[2],
               &decoded) == MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID &&
               memcmp(&fragments_state, &fragments_untouched,
                      sizeof(fragments_state)) == 0,
           "non-preflight authenticated payload cannot enter reassembly");
    wrong_fragment_context = fragment_context;
    wrong_fragment_context.key.source_endpoint_id = 30u;
    wrong_fragment_context.key.source_generation = 3u;
    fragments_untouched = fragments_state;
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &wrong_fragment_context, fragments[2],
               &decoded) == MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH &&
               memcmp(&fragments_state, &fragments_untouched,
                      sizeof(fragments_state)) == 0,
           "fragments from another authenticated direction cannot be spliced");
    memset(&decoded, 0xa5, sizeof(decoded));
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, fragments[2], &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED &&
               decoded.protocol_version == 0xa5a5a5a5u,
           "out-of-order first fragment is retained without output");
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, fragments[0], &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED,
           "out-of-order second fragment remains incomplete");
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, fragments[1], &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE &&
               memcmp(&decoded, &report, sizeof(decoded)) == 0,
           "three authenticated fragments reconstruct one exact report");
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, fragments[1], &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_DUPLICATE,
           "exact fragment retry is idempotent");
    fragments_untouched = fragments_state;
    memcpy(mutated, fragments[1], MDKR_MATCH_PEER_PAYLOAD_BYTES);
    mutated[10] ^= 1u;
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, mutated, &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT &&
               memcmp(&fragments_state, &fragments_untouched,
                      sizeof(fragments_state)) == 0,
           "same report/index changed fragment conflicts atomically");
    report.sequence = 10u;
    expect(mdkr_match_preflight_fragment_encode(&report, 0u, mutated) &&
               mdkr_match_preflight_fragment_submit(
                   &fragments_state, &fragment_context, mutated, &decoded) ==
                   MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED &&
               fragments_state.sequence == 10u &&
               fragments_state.present_mask == 1u,
           "newer report atomically replaces incomplete or complete fragments");
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, fragments[2], &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_STALE_SEQUENCE,
           "older report fragment cannot roll reassembly backward");
    fragments_untouched = fragments_state;
    report.sequence = 9u;
    memcpy(mutated, fragments[2], MDKR_MATCH_PEER_PAYLOAD_BYTES);
    mutated[63] = 1u;
    expect(mdkr_match_preflight_fragment_submit(
               &fragments_state, &fragment_context, mutated, &decoded) ==
               MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID &&
               memcmp(&fragments_state, &fragments_untouched,
                      sizeof(fragments_state)) == 0,
           "nonzero final-fragment padding rejects atomically");
    {
        MdkrMatchPreflightAttestationV1 forged = report;
        uint8_t forged_fragments[MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT]
                                [MDKR_MATCH_PEER_PAYLOAD_BYTES];
        forged.endpoint_id = 30u;
        forged.connection_generation = 3u;
        forged.sequence = 11u;
        for (index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT; index++)
            expect(mdkr_match_preflight_fragment_encode(
                       &forged, index, forged_fragments[index]),
                   "forged-attribution negative encodes structurally");
        expect(mdkr_match_preflight_fragment_state_init(
                   &fragments_state, &fragment_context.key) &&
                   mdkr_match_preflight_fragment_submit(
                       &fragments_state, &fragment_context,
                       forged_fragments[0], &decoded) ==
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED &&
                   mdkr_match_preflight_fragment_submit(
                       &fragments_state, &fragment_context,
                       forged_fragments[1], &decoded) ==
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED,
               "authenticated direction retains only its own incomplete report");
        fragments_untouched = fragments_state;
        memset(&decoded, 0xa5, sizeof(decoded));
        decoded_untouched = decoded;
        expect(mdkr_match_preflight_fragment_submit(
                   &fragments_state, &fragment_context, forged_fragments[2],
                   &decoded) ==
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH &&
                   memcmp(&fragments_state, &fragments_untouched,
                          sizeof(fragments_state)) == 0 &&
                   memcmp(&decoded, &decoded_untouched, sizeof(decoded)) == 0,
               "complete report cannot claim another authenticated endpoint");
    }

    memset(&decoded, 0xa5, sizeof(decoded));
    decoded_untouched = decoded;
    expect(!mdkr_match_preflight_attestation_decode(wire, sizeof(wire) - 1u,
                                                    &decoded) &&
               !mdkr_match_preflight_attestation_decode(wire, sizeof(wire) + 1u,
                                                        &decoded) &&
               memcmp(&decoded, &decoded_untouched, sizeof(decoded)) == 0,
           "noncanonical wire lengths reject without output mutation");
    {
        const unsigned invalid_offsets[] = {0u, 4u, 5u, 6u, 7u};
        unsigned invalid;
        for (invalid = 0u;
             invalid < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]);
             invalid++) {
            memcpy(mutated, wire, sizeof(mutated));
            mutated[invalid_offsets[invalid]] ^=
                invalid_offsets[invalid] == 5u ? 0x08u : 0x01u;
            expect(!mdkr_match_preflight_attestation_decode(
                       mutated, sizeof(mutated), &decoded),
                   "magic, version, flags and reserved mutations reject");
        }
    }
    {
        const unsigned zero_offsets[] = {8u, 12u, 16u, 20u};
        const unsigned zero_sizes[] = {4u, 4u, 4u, 8u};
        unsigned field;
        for (field = 0u; field < sizeof(zero_offsets) / sizeof(zero_offsets[0]);
             field++) {
            memcpy(mutated, wire, sizeof(mutated));
            memset(mutated + zero_offsets[field], 0, zero_sizes[field]);
            expect(!mdkr_match_preflight_attestation_decode(
                       mutated, sizeof(mutated), &decoded),
                   "zero epoch, generation, sequence and endpoint reject");
        }
    }
    for (index = 28u; index < sizeof(wire); index++) {
        memcpy(mutated, wire, sizeof(mutated));
        mutated[index] ^= 1u;
        expect(mdkr_match_preflight_attestation_decode(mutated, sizeof(mutated),
                                                       &decoded) &&
                   memcmp(&decoded, &report, sizeof(decoded)) != 0,
               "digest mutation remains a decodable disagreement");
    }

    report =
        attestation(&preflight, 99u, 1u, 1u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    untouched = preflight;
    expect(mdkr_match_preflight_submit(&preflight, 20u, 2u, &report) ==
                   MDKR_MATCH_PREFLIGHT_SUBMIT_AUTHENTICATED_SOURCE_MISMATCH &&
               memcmp(&preflight, &untouched, sizeof(preflight)) == 0,
           "authenticated peer cannot attribute a report to another endpoint");
    expect(submit_report(&preflight, &report) ==
                   MDKR_MATCH_PREFLIGHT_SUBMIT_UNKNOWN_ENDPOINT &&
               memcmp(&preflight, &untouched, sizeof(preflight)) == 0,
           "unknown authenticated endpoint rejects atomically");
    report.endpoint_id = 20u;
    report.connection_generation = 1u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_GENERATION,
           "stale connection generation cannot inherit preflight authority");
    report.connection_generation = 2u;
    report.match_epoch = 6u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_EPOCH,
           "stale match epoch rejects before mutation");
    report.match_epoch = 7u;
    report.sequence = 0u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_INVALID,
           "zero report sequence is invalid");

    report =
        attestation(&preflight, 30u, 3u, 1u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "first authenticated endpoint report is accepted");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS, 10u, 1u);

    report =
        attestation(&preflight, 20u, 2u, 1u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    report.descriptor_digest[0] ^= 1u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "authenticated disagreement is retained for honest diagnosis");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH, 20u, 2u);
    report =
        attestation(&preflight, 20u, 2u, 2u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    report.transcript_digest[31] ^= 1u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "higher sequence replaces prior endpoint report");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_TRANSCRIPT_MISMATCH, 20u, 2u);
    report =
        attestation(&preflight, 20u, 2u, 3u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    report.graph_digest[17] ^= 1u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "higher sequence can expose a topology equivocation");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH, 20u, 2u);
    report =
        attestation(&preflight, 20u, 2u, 4u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "endpoint can repair a mismatched report with a higher sequence");

    report = attestation(&preflight, 10u, 1u, 1u, 0u);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "local endpoint enters staged checks");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_ROM_UNVERIFIED, 10u, 3u);
    report =
        attestation(&preflight, 10u, 1u, 2u, MDKR_MATCH_PREFLIGHT_ROM_VERIFIED);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "local ROM verification advances preflight");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE, 10u, 3u);
    report = attestation(&preflight, 10u, 1u, 3u,
                         MDKR_MATCH_PREFLIGHT_ROM_VERIFIED |
                             MDKR_MATCH_PREFLIGHT_PHRASE_CONFIRMED);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "phrase confirmation advances preflight");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_CHANNELS_NOT_READY, 10u, 3u);
    report =
        attestation(&preflight, 10u, 1u, 4u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "ready channels complete the local report");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_READY, 0u, 3u);

    report = attestation(&preflight, 20u, 2u, 5u,
                         MDKR_MATCH_PREFLIGHT_ROM_VERIFIED |
                             MDKR_MATCH_PREFLIGHT_CHANNELS_READY);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "higher sequence may withdraw a transient readiness check");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE, 20u, 3u);
    report.flags = MDKR_MATCH_PREFLIGHT_ALL_FLAGS;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_CONFLICT,
           "same-sequence changed payload conflicts atomically");
    report.flags =
        MDKR_MATCH_PREFLIGHT_ROM_VERIFIED | MDKR_MATCH_PREFLIGHT_CHANNELS_READY;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_DUPLICATE,
           "exact report retry is idempotent");
    report.sequence = 4u;
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_SEQUENCE,
           "older report cannot roll readiness backward");
    report =
        attestation(&preflight, 20u, 2u, 6u, MDKR_MATCH_PREFLIGHT_ALL_FLAGS);
    expect(submit_report(&preflight, &report) ==
               MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED,
           "fresh report can restore consensus");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_READY, 0u, 3u);

    preflight.attestations[0].reserved[0] = 1u;
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_INVALID, 0u, 3u);

    expect(mdkr_match_preflight_init(&preflight, &launch, &connected,
                                     transcript, 10u, 1u),
           "fresh preflight reinitializes for corruption negatives");
    preflight.attestations[3].sequence = 1u;
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_INVALID, 0u, 0u);
    expect(mdkr_match_preflight_init(&preflight, &launch, &connected,
                                     transcript, 10u, 1u),
           "fresh preflight reinitializes after unused-slot corruption");
    preflight.graph.endpoints[0].reachable_mask = 0x02u;
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_INVALID, 0u, 0u);
    expect(mdkr_match_preflight_init(&preflight, &launch, &connected,
                                     transcript, 10u, 1u),
           "fresh preflight reinitializes after graph/digest corruption");
    preflight.graph.endpoint_count = 255u;
    {
        MdkrMatchPreflightStatus invalid = mdkr_match_preflight_evaluate(&preflight);
        expect(invalid.state == MDKR_MATCH_PREFLIGHT_INVALID &&
                   invalid.received_count == 0u && invalid.required_count == 0u,
               "corrupt endpoint count fails without out-of-bounds status walk");
    }

    expect(mdkr_match_preflight_init(&preflight, &launch, &disconnected,
                                     transcript, 10u, 1u),
           "structurally valid disconnected graph remains diagnosable");
    expect_state(&preflight, MDKR_MATCH_PREFLIGHT_ROUTE_UNAVAILABLE, 0u, 0u);

    if (failures != 0)
        return 1;
    puts("match preflight: PASS (consensus, staged UX and atomic negatives)");
    return 0;
}
