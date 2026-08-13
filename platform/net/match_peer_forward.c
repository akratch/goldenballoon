#include "match_peer_forward.h"

#include <string.h>

static bool window_accept(MdkrMatchPeerForwardWindow *window, uint64_t sequence) {
    if (sequence > window->greatest_sequence) {
        const uint64_t distance = sequence - window->greatest_sequence;
        window->seen_bitmap = distance >= 64u ? 1u :
            (window->seen_bitmap << (unsigned)distance) | 1u;
        window->greatest_sequence = sequence;
        return true;
    }
    {
        const uint64_t distance = window->greatest_sequence - sequence;
        if (distance >= 64u ||
            (window->seen_bitmap & (UINT64_C(1) << (unsigned)distance)) != 0u)
            return false;
        window->seen_bitmap |= UINT64_C(1) << (unsigned)distance;
    }
    return true;
}

bool mdkr_match_peer_forwarder_init(
    MdkrMatchPeerForwarder *forwarder, uint32_t match_epoch,
    uint64_t local_endpoint_id, uint32_t local_generation) {
    MdkrMatchPeerForwarder next;
    if (forwarder == NULL || match_epoch == 0u || local_endpoint_id == 0u ||
        local_generation == 0u) return false;
    memset(&next, 0, sizeof(next));
    next.match_epoch = match_epoch;
    next.local_endpoint_id = local_endpoint_id;
    next.local_generation = local_generation;
    next.ready = true;
    *forwarder = next;
    return true;
}

MdkrMatchPeerForwardResult mdkr_match_peer_forwarder_admit(
    MdkrMatchPeerForwarder *forwarder, const MdkrMatchPeerGraph *graph,
    uint64_t authenticated_source_endpoint_id,
    uint32_t authenticated_source_generation,
    const MdkrMatchPeerEnvelopeContext *context) {
    MdkrMatchPeerRoute route;
    MdkrMatchPeerRouteResult route_result;
    MdkrMatchPeerForwardWindow *window = NULL;
    MdkrMatchPeerForwarder next;
    unsigned index;
    if (forwarder == NULL || !forwarder->ready || graph == NULL ||
        context == NULL || authenticated_source_endpoint_id == 0u ||
        authenticated_source_generation == 0u ||
        context->sequence == 0u || context->key.source_endpoint_id == 0u ||
        context->key.destination_endpoint_id == 0u ||
        context->payload_type > MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX ||
        context->intermediate_endpoint_id == 0u)
        return MDKR_MATCH_PEER_FORWARD_INVALID;
    if (context->key.match_epoch != forwarder->match_epoch ||
        graph->match_epoch != forwarder->match_epoch)
        return MDKR_MATCH_PEER_FORWARD_STALE_EPOCH;
    if (authenticated_source_endpoint_id != context->key.source_endpoint_id ||
        context->intermediate_endpoint_id != forwarder->local_endpoint_id)
        return MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE;
    if (authenticated_source_generation != context->key.source_generation)
        return MDKR_MATCH_PEER_FORWARD_STALE_GENERATION;
    route_result = mdkr_match_peer_graph_route(
        graph, forwarder->match_epoch, context->key.source_endpoint_id,
        context->key.source_generation, context->key.destination_endpoint_id,
        context->key.destination_generation, &route);
    if (route_result == MDKR_MATCH_PEER_ROUTE_STALE_GENERATION)
        return MDKR_MATCH_PEER_FORWARD_STALE_GENERATION;
    if (route_result == MDKR_MATCH_PEER_ROUTE_STALE_EPOCH)
        return MDKR_MATCH_PEER_FORWARD_STALE_EPOCH;
    if (route_result != MDKR_MATCH_PEER_ROUTE_OK || route.node_count != 3u ||
        route.forward_count != 1u ||
        route.endpoint_ids[1] != forwarder->local_endpoint_id ||
        route.generations[1] != forwarder->local_generation)
        return MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE;
    next = *forwarder;
    for (index = 0u; index < MDKR_MATCH_PEER_FORWARD_DIRECTIONS; index++) {
        MdkrMatchPeerForwardWindow *candidate = &next.windows[index];
        if (candidate->occupied &&
            candidate->source_endpoint_id == context->key.source_endpoint_id &&
            candidate->destination_endpoint_id ==
                context->key.destination_endpoint_id) {
            window = candidate;
            /* A reconnect derives a new generation-bound key and restarts its
             * sequence space. Reuse the direction slot, but never carry the
             * prior key's replay bitmap into the new authenticated channel. */
            if (candidate->source_generation !=
                    context->key.source_generation ||
                candidate->destination_generation !=
                    context->key.destination_generation)
                memset(candidate, 0, sizeof(*candidate));
            break;
        }
        if (!candidate->occupied && window == NULL) window = candidate;
    }
    if (window == NULL) return MDKR_MATCH_PEER_FORWARD_CAPACITY;
    if (!window->occupied) {
        window->occupied = true;
        window->source_endpoint_id = context->key.source_endpoint_id;
        window->destination_endpoint_id = context->key.destination_endpoint_id;
        window->source_generation = context->key.source_generation;
        window->destination_generation = context->key.destination_generation;
        window->greatest_sequence = context->sequence;
        window->seen_bitmap = 1u;
    } else if (!window_accept(window, context->sequence)) {
        return MDKR_MATCH_PEER_FORWARD_DUPLICATE;
    }
    *forwarder = next;
    return MDKR_MATCH_PEER_FORWARD_ACCEPTED;
}
