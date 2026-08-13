#include "match_preflight.h"

#include <string.h>

#include "sha256.h"

static bool any_nonzero(const uint8_t *bytes, unsigned count) {
    unsigned index;
    uint8_t combined = 0u;
    for (index = 0u; index < count; index++)
        combined |= bytes[index];
    return combined != 0u;
}

static bool all_zero(const void *value, size_t count) {
    return !any_nonzero((const uint8_t *)value, (unsigned)count);
}

static void put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void put64(uint8_t *output, uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; index++)
        output[index] = (uint8_t)(value >> (56u - index * 8u));
}

static uint32_t get32(const uint8_t *input) {
    return (uint32_t)input[0] << 24u | (uint32_t)input[1] << 16u |
           (uint32_t)input[2] << 8u | (uint32_t)input[3];
}

static uint64_t get64(const uint8_t *input) {
    uint64_t value = 0u;
    unsigned index;
    for (index = 0u; index < 8u; index++)
        value = value << 8u | input[index];
    return value;
}

static bool attestation_structural_valid(
    const MdkrMatchPreflightAttestationV1 *attestation) {
    return attestation != NULL &&
           attestation->protocol_version == MDKR_MATCH_PREFLIGHT_VERSION &&
           attestation->match_epoch != 0u &&
           attestation->connection_generation != 0u &&
           attestation->sequence != 0u && attestation->endpoint_id != 0u &&
           (attestation->flags & (uint8_t)~MDKR_MATCH_PREFLIGHT_ALL_FLAGS) ==
               0u &&
           !any_nonzero(attestation->reserved, sizeof(attestation->reserved));
}

static bool key_context_valid(const MdkrMatchPeerKeyContext *context) {
    return context != NULL && context->match_epoch != 0u &&
           context->source_endpoint_id != 0u &&
           context->source_generation != 0u &&
           context->destination_endpoint_id != 0u &&
           context->destination_generation != 0u &&
           context->source_endpoint_id != context->destination_endpoint_id;
}

static bool key_context_equal(const MdkrMatchPeerKeyContext *left,
                              const MdkrMatchPeerKeyContext *right) {
    return left->match_epoch == right->match_epoch &&
           left->source_endpoint_id == right->source_endpoint_id &&
           left->source_generation == right->source_generation &&
           left->destination_endpoint_id == right->destination_endpoint_id &&
           left->destination_generation == right->destination_generation;
}

static bool fragment_context_valid(
    const MdkrMatchPeerEnvelopeContext *context) {
    return context != NULL && key_context_valid(&context->key) &&
           context->sequence != 0u &&
           context->payload_type ==
               MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT &&
           (context->intermediate_endpoint_id == 0u ||
            (context->intermediate_endpoint_id !=
                 context->key.source_endpoint_id &&
             context->intermediate_endpoint_id !=
                 context->key.destination_endpoint_id));
}

static int endpoint_index(const MdkrMatchPeerGraph *graph,
                          uint64_t endpoint_id) {
    unsigned index;
    if (graph == NULL || endpoint_id == 0u)
        return -1;
    for (index = 0u; index < graph->endpoint_count; index++) {
        if (graph->endpoints[index].endpoint_id == endpoint_id)
            return (int)index;
    }
    return -1;
}

static bool graph_valid(const MdkrMatchPeerGraph *graph) {
    MdkrMatchPeerGraph copy;
    if (graph == NULL)
        return false;
    return mdkr_match_peer_graph_init(&copy, graph->match_epoch,
                                      graph->endpoints,
                                      graph->endpoint_count) &&
           memcmp(&copy, graph, sizeof(copy)) == 0;
}

static bool attestation_equal(const MdkrMatchPreflightAttestationV1 *left,
                              const MdkrMatchPreflightAttestationV1 *right) {
    return left->protocol_version == right->protocol_version &&
           left->match_epoch == right->match_epoch &&
           left->connection_generation == right->connection_generation &&
           left->sequence == right->sequence &&
           left->endpoint_id == right->endpoint_id &&
           left->flags == right->flags &&
           memcmp(left->reserved, right->reserved, sizeof(left->reserved)) ==
               0 &&
           memcmp(left->descriptor_digest, right->descriptor_digest,
                  sizeof(left->descriptor_digest)) == 0 &&
           memcmp(left->transcript_digest, right->transcript_digest,
                  sizeof(left->transcript_digest)) == 0 &&
           memcmp(left->graph_digest, right->graph_digest,
                  sizeof(left->graph_digest)) == 0;
}

static bool preflight_valid(const MdkrMatchPreflightV1 *preflight) {
    uint8_t canonical_graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    int local;
    unsigned index;
    if (preflight == NULL ||
        preflight->protocol_version != MDKR_MATCH_PREFLIGHT_VERSION ||
        preflight->match_epoch == 0u ||
        preflight->match_epoch != preflight->graph.match_epoch ||
        !graph_valid(&preflight->graph) ||
        !any_nonzero(preflight->descriptor_digest,
                     sizeof(preflight->descriptor_digest)) ||
        !any_nonzero(preflight->transcript_digest,
                     sizeof(preflight->transcript_digest)) ||
        !any_nonzero(preflight->graph_digest,
                     sizeof(preflight->graph_digest)) ||
        (preflight->present_mask &
         (uint8_t)~((1u << preflight->graph.endpoint_count) - 1u)) != 0u)
        return false;
    if (!mdkr_match_preflight_graph_digest(&preflight->graph,
                                            canonical_graph_digest) ||
        memcmp(canonical_graph_digest, preflight->graph_digest,
               sizeof(canonical_graph_digest)) != 0)
        return false;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        const MdkrMatchPreflightAttestationV1 *attestation =
            &preflight->attestations[index];
        if ((preflight->present_mask & (uint8_t)(1u << index)) == 0u) {
            if (!all_zero(attestation, sizeof(*attestation)))
                return false;
            continue;
        }
        if (attestation->protocol_version != MDKR_MATCH_PREFLIGHT_VERSION ||
            attestation->match_epoch != preflight->match_epoch ||
            attestation->endpoint_id !=
                preflight->graph.endpoints[index].endpoint_id ||
            attestation->connection_generation !=
                preflight->graph.endpoints[index].generation ||
            attestation->sequence == 0u ||
            (attestation->flags & (uint8_t)~MDKR_MATCH_PREFLIGHT_ALL_FLAGS) !=
                0u ||
            any_nonzero(attestation->reserved, sizeof(attestation->reserved)))
            return false;
    }
    for (; index < MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS; index++)
        if (!all_zero(&preflight->attestations[index],
                      sizeof(preflight->attestations[index])))
            return false;
    local = endpoint_index(&preflight->graph, preflight->local_endpoint_id);
    return local >= 0 && preflight->local_connection_generation != 0u &&
           preflight->graph.endpoints[local].generation ==
               preflight->local_connection_generation;
}

bool mdkr_match_preflight_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    uint8_t output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]) {
    uint8_t encoded[MDKR_MATCH_LAUNCH_DESCRIPTOR_BYTES];
    uint8_t next[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrSha256 hash;
    if (output == NULL || !mdkr_match_launch_descriptor_encode(
                              descriptor, encoded, sizeof(encoded)))
        return false;
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, encoded, sizeof(encoded));
    mdkr_sha256_final(&hash, next);
    memcpy(output, next, sizeof(next));
    return true;
}

bool mdkr_match_preflight_graph_digest(
    const MdkrMatchPeerGraph *graph,
    uint8_t output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]) {
    static const char domain[] = "golden-balloon-match-peer-graph-v1";
    unsigned order[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t encoded[8];
    uint8_t next[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrSha256 hash;
    unsigned source;
    if (!graph_valid(graph) || output == NULL)
        return false;
    for (source = 0u; source < graph->endpoint_count; source++)
        order[source] = source;
    for (source = 0u; source < graph->endpoint_count; source++) {
        unsigned other;
        for (other = source + 1u; other < graph->endpoint_count; other++) {
            if (graph->endpoints[order[source]].endpoint_id >
                graph->endpoints[order[other]].endpoint_id) {
                unsigned swap = order[source];
                order[source] = order[other];
                order[other] = swap;
            }
        }
    }
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, domain, sizeof(domain) - 1u);
    put32(encoded, graph->protocol_version);
    mdkr_sha256_update(&hash, encoded, 4u);
    put32(encoded, graph->match_epoch);
    mdkr_sha256_update(&hash, encoded, 4u);
    encoded[0] = graph->endpoint_count;
    mdkr_sha256_update(&hash, encoded, 1u);
    for (source = 0u; source < graph->endpoint_count; source++) {
        const unsigned original_source = order[source];
        const MdkrMatchPeerEndpoint *endpoint =
            &graph->endpoints[original_source];
        uint8_t canonical_mask = 0u;
        unsigned destination;
        put64(encoded, endpoint->endpoint_id);
        mdkr_sha256_update(&hash, encoded, 8u);
        put32(encoded, endpoint->generation);
        mdkr_sha256_update(&hash, encoded, 4u);
        for (destination = 0u; destination < graph->endpoint_count;
             destination++) {
            if ((endpoint->reachable_mask &
                 (uint8_t)(1u << order[destination])) != 0u)
                canonical_mask |= (uint8_t)(1u << destination);
        }
        mdkr_sha256_update(&hash, &canonical_mask, 1u);
    }
    mdkr_sha256_final(&hash, next);
    memcpy(output, next, sizeof(next));
    memset(next, 0, sizeof(next));
    return true;
}

bool mdkr_match_preflight_attestation_encode(
    const MdkrMatchPreflightAttestationV1 *attestation, uint8_t *output,
    size_t capacity) {
    uint8_t next[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    if (!attestation_structural_valid(attestation) || output == NULL ||
        capacity < sizeof(next))
        return false;
    memset(next, 0, sizeof(next));
    next[0] = 'M';
    next[1] = 'P';
    next[2] = 'F';
    next[3] = '1';
    next[4] = MDKR_MATCH_PREFLIGHT_VERSION;
    next[5] = attestation->flags;
    put32(next + 8u, attestation->match_epoch);
    put32(next + 12u, attestation->connection_generation);
    put32(next + 16u, attestation->sequence);
    put64(next + 20u, attestation->endpoint_id);
    memcpy(next + 28u, attestation->descriptor_digest,
           sizeof(attestation->descriptor_digest));
    memcpy(next + 60u, attestation->transcript_digest,
           sizeof(attestation->transcript_digest));
    memcpy(next + 92u, attestation->graph_digest,
           sizeof(attestation->graph_digest));
    memcpy(output, next, sizeof(next));
    return true;
}

bool mdkr_match_preflight_attestation_decode(
    const uint8_t *bytes, size_t length,
    MdkrMatchPreflightAttestationV1 *output) {
    MdkrMatchPreflightAttestationV1 next;
    if (bytes == NULL || output == NULL ||
        length != MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES || bytes[0] != 'M' ||
        bytes[1] != 'P' || bytes[2] != 'F' || bytes[3] != '1' ||
        bytes[4] != MDKR_MATCH_PREFLIGHT_VERSION || bytes[6] != 0u ||
        bytes[7] != 0u)
        return false;
    memset(&next, 0, sizeof(next));
    next.protocol_version = bytes[4];
    next.flags = bytes[5];
    next.match_epoch = get32(bytes + 8u);
    next.connection_generation = get32(bytes + 12u);
    next.sequence = get32(bytes + 16u);
    next.endpoint_id = get64(bytes + 20u);
    memcpy(next.descriptor_digest, bytes + 28u, sizeof(next.descriptor_digest));
    memcpy(next.transcript_digest, bytes + 60u, sizeof(next.transcript_digest));
    memcpy(next.graph_digest, bytes + 92u, sizeof(next.graph_digest));
    if (!attestation_structural_valid(&next))
        return false;
    *output = next;
    return true;
}

static size_t fragment_data_size(unsigned fragment_index) {
    const size_t offset =
        (size_t)fragment_index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    const size_t remaining = MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES - offset;
    return remaining < MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES
               ? remaining
               : MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
}

static bool fragment_state_valid(
    const MdkrMatchPreflightFragmentState *state) {
    unsigned index;
    if (state == NULL || !key_context_valid(&state->direction) ||
        (state->present_mask &
         (uint8_t)~((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) != 0u)
        return false;
    if (state->sequence == 0u)
        return state->present_mask == 0u &&
               all_zero(state->encoded, sizeof(state->encoded));
    if (state->present_mask == 0u)
        return false;
    for (index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT; index++) {
        const size_t offset =
            (size_t)index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
        if ((state->present_mask & (uint8_t)(1u << index)) == 0u &&
            !all_zero(state->encoded + offset, fragment_data_size(index)))
            return false;
    }
    if (state->present_mask ==
        (uint8_t)((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) {
        MdkrMatchPreflightAttestationV1 decoded;
        if (!mdkr_match_preflight_attestation_decode(
                state->encoded, sizeof(state->encoded), &decoded) ||
            decoded.sequence != state->sequence)
            return false;
    }
    return true;
}

bool mdkr_match_preflight_fragment_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    unsigned fragment_index,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    uint8_t encoded[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
    uint8_t next[MDKR_MATCH_PEER_PAYLOAD_BYTES];
    const size_t offset =
        (size_t)fragment_index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    size_t count;
    if (fragment_index >= MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ||
        payload == NULL ||
        !mdkr_match_preflight_attestation_encode(attestation, encoded,
                                                  sizeof(encoded)))
        return false;
    count = fragment_data_size(fragment_index);
    memset(next, 0, sizeof(next));
    put32(next, attestation->sequence);
    next[4] = (uint8_t)fragment_index;
    next[5] = MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT;
    memcpy(next + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES,
           encoded + offset, count);
    memcpy(payload, next, sizeof(next));
    return true;
}

bool mdkr_match_preflight_fragment_state_init(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerKeyContext *authenticated_direction) {
    MdkrMatchPreflightFragmentState next;
    if (state == NULL || !key_context_valid(authenticated_direction))
        return false;
    memset(&next, 0, sizeof(next));
    next.direction.match_epoch = authenticated_direction->match_epoch;
    next.direction.source_endpoint_id =
        authenticated_direction->source_endpoint_id;
    next.direction.source_generation =
        authenticated_direction->source_generation;
    next.direction.destination_endpoint_id =
        authenticated_direction->destination_endpoint_id;
    next.direction.destination_generation =
        authenticated_direction->destination_generation;
    *state = next;
    return true;
}

MdkrMatchPreflightFragmentResult mdkr_match_preflight_fragment_submit(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerEnvelopeContext *authenticated_context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchPreflightAttestationV1 *output) {
    MdkrMatchPreflightFragmentState next;
    MdkrMatchPreflightAttestationV1 decoded;
    uint32_t sequence;
    unsigned index;
    size_t offset;
    size_t count;
    if (!fragment_state_valid(state) ||
        !fragment_context_valid(authenticated_context) || payload == NULL ||
        output == NULL)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (!key_context_equal(&state->direction, &authenticated_context->key))
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH;
    sequence = get32(payload);
    index = payload[4];
    if (sequence == 0u || index >= MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ||
        payload[5] != MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    offset = (size_t)index * MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES;
    count = fragment_data_size(index);
    if (!all_zero(payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES + count,
                  MDKR_MATCH_PEER_PAYLOAD_BYTES -
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES - count))
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (state->sequence != 0u && sequence < state->sequence)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_STALE_SEQUENCE;
    next = *state;
    if (sequence > next.sequence) {
        memset(&next, 0, sizeof(next));
        next.direction.match_epoch = state->direction.match_epoch;
        next.direction.source_endpoint_id =
            state->direction.source_endpoint_id;
        next.direction.source_generation = state->direction.source_generation;
        next.direction.destination_endpoint_id =
            state->direction.destination_endpoint_id;
        next.direction.destination_generation =
            state->direction.destination_generation;
        next.sequence = sequence;
    }
    if ((next.present_mask & (uint8_t)(1u << index)) != 0u) {
        return memcmp(next.encoded + offset,
                      payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES,
                      count) == 0
                   ? MDKR_MATCH_PREFLIGHT_FRAGMENT_DUPLICATE
                   : MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT;
    }
    memcpy(next.encoded + offset,
           payload + MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES, count);
    next.present_mask |= (uint8_t)(1u << index);
    if (next.present_mask !=
        (uint8_t)((1u << MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT) - 1u)) {
        *state = next;
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED;
    }
    if (!mdkr_match_preflight_attestation_decode(
            next.encoded, sizeof(next.encoded), &decoded) ||
        decoded.sequence != sequence)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID;
    if (decoded.match_epoch != state->direction.match_epoch ||
        decoded.endpoint_id != state->direction.source_endpoint_id ||
        decoded.connection_generation != state->direction.source_generation)
        return MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH;
    *state = next;
    *output = decoded;
    return MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE;
}

bool mdkr_match_preflight_init(
    MdkrMatchPreflightV1 *preflight,
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    const MdkrMatchPeerGraph *graph,
    const uint8_t transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES],
    uint64_t local_endpoint_id, uint32_t local_connection_generation) {
    MdkrMatchPreflightV1 next;
    int local;
    if (preflight == NULL || descriptor == NULL || !graph_valid(graph) ||
        transcript_digest == NULL ||
        !any_nonzero(transcript_digest, MDKR_MATCH_PREFLIGHT_DIGEST_BYTES) ||
        descriptor->manifest.match_epoch != graph->match_epoch)
        return false;
    local = endpoint_index(graph, local_endpoint_id);
    if (local < 0 || local_connection_generation == 0u ||
        graph->endpoints[local].generation != local_connection_generation)
        return false;
    memset(&next, 0, sizeof(next));
    next.protocol_version = MDKR_MATCH_PREFLIGHT_VERSION;
    next.match_epoch = graph->match_epoch;
    next.local_endpoint_id = local_endpoint_id;
    next.local_connection_generation = local_connection_generation;
    next.graph = *graph;
    memcpy(next.transcript_digest, transcript_digest,
           sizeof(next.transcript_digest));
    if (!mdkr_match_preflight_graph_digest(graph, next.graph_digest))
        return false;
    if (!mdkr_match_preflight_descriptor_digest(descriptor,
                                                next.descriptor_digest))
        return false;
    *preflight = next;
    return true;
}

MdkrMatchPreflightSubmitResult mdkr_match_preflight_submit(
    MdkrMatchPreflightV1 *preflight,
    uint64_t authenticated_endpoint_id,
    uint32_t authenticated_connection_generation,
    const MdkrMatchPreflightAttestationV1 *attestation) {
    int index;
    MdkrMatchPreflightAttestationV1 *current;
    if (!preflight_valid(preflight) || authenticated_endpoint_id == 0u ||
        authenticated_connection_generation == 0u ||
        !attestation_structural_valid(attestation))
        return MDKR_MATCH_PREFLIGHT_SUBMIT_INVALID;
    if (attestation->endpoint_id != authenticated_endpoint_id ||
        attestation->connection_generation !=
            authenticated_connection_generation)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_AUTHENTICATED_SOURCE_MISMATCH;
    if (attestation->match_epoch != preflight->match_epoch)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_EPOCH;
    index = endpoint_index(&preflight->graph, attestation->endpoint_id);
    if (index < 0)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_UNKNOWN_ENDPOINT;
    if (attestation->connection_generation !=
        preflight->graph.endpoints[index].generation)
        return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_GENERATION;
    current = &preflight->attestations[index];
    if ((preflight->present_mask & (uint8_t)(1u << index)) != 0u) {
        if (attestation->sequence < current->sequence)
            return MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_SEQUENCE;
        if (attestation->sequence == current->sequence)
            return attestation_equal(current, attestation)
                       ? MDKR_MATCH_PREFLIGHT_SUBMIT_DUPLICATE
                       : MDKR_MATCH_PREFLIGHT_SUBMIT_CONFLICT;
    }
    *current = *attestation;
    preflight->present_mask |= (uint8_t)(1u << index);
    return MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED;
}

static MdkrMatchPreflightStatus status(const MdkrMatchPreflightV1 *preflight,
                                       MdkrMatchPreflightState state,
                                       uint64_t endpoint_id) {
    MdkrMatchPreflightStatus result = {state, endpoint_id, 0u, 0u};
    unsigned index;
    if (preflight == NULL ||
        preflight->graph.endpoint_count < 2u ||
        preflight->graph.endpoint_count > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS)
        return result;
    result.required_count = preflight->graph.endpoint_count;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        if ((preflight->present_mask & (uint8_t)(1u << index)) != 0u)
            result.received_count++;
    }
    return result;
}

static uint64_t lowest_endpoint_for(const MdkrMatchPreflightV1 *preflight,
                                    int condition) {
    uint64_t lowest = 0u;
    unsigned index;
    for (index = 0u; index < preflight->graph.endpoint_count; index++) {
        const MdkrMatchPreflightAttestationV1 *attestation =
            &preflight->attestations[index];
        uint64_t candidate;
        bool present = (preflight->present_mask & (uint8_t)(1u << index)) != 0u;
        bool matches = false;
        if (condition == 0) {
            matches = !present;
        } else if (present && condition == 1) {
            matches = memcmp(attestation->descriptor_digest,
                             preflight->descriptor_digest,
                             sizeof(preflight->descriptor_digest)) != 0;
        } else if (present && condition == 2) {
            matches = memcmp(attestation->transcript_digest,
                             preflight->transcript_digest,
                             sizeof(preflight->transcript_digest)) != 0;
        } else if (present && condition == 3) {
            matches = memcmp(attestation->graph_digest,
                             preflight->graph_digest,
                             sizeof(preflight->graph_digest)) != 0;
        } else if (present && condition >= 4) {
            matches =
                (attestation->flags & (uint8_t)(1u << (condition - 4))) == 0u;
        }
        candidate = present ? attestation->endpoint_id
                            : preflight->graph.endpoints[index].endpoint_id;
        if (matches && (lowest == 0u || candidate < lowest))
            lowest = candidate;
    }
    return lowest;
}

MdkrMatchPreflightStatus
mdkr_match_preflight_evaluate(const MdkrMatchPreflightV1 *preflight) {
    uint64_t endpoint;
    if (!preflight_valid(preflight))
        return status(preflight, MDKR_MATCH_PREFLIGHT_INVALID, 0u);
    endpoint = lowest_endpoint_for(preflight, 1);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 2);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_TRANSCRIPT_MISMATCH,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 3);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH, endpoint);
    if (!mdkr_match_peer_graph_admissible(&preflight->graph))
        return status(preflight, MDKR_MATCH_PREFLIGHT_ROUTE_UNAVAILABLE, 0u);
    endpoint = lowest_endpoint_for(preflight, 0);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS,
                      endpoint);
    endpoint = lowest_endpoint_for(preflight, 4);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_ROM_UNVERIFIED, endpoint);
    endpoint = lowest_endpoint_for(preflight, 5);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE, endpoint);
    endpoint = lowest_endpoint_for(preflight, 6);
    if (endpoint != 0u)
        return status(preflight, MDKR_MATCH_PREFLIGHT_CHANNELS_NOT_READY,
                      endpoint);
    return status(preflight, MDKR_MATCH_PREFLIGHT_READY, 0u);
}
