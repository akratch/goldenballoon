#include "match_peer_graph.h"

#include <string.h>

static int endpoint_index(const MdkrMatchPeerGraph *graph, uint64_t endpoint_id) {
    unsigned index;
    for (index = 0u; index < graph->endpoint_count; index++) {
        if (graph->endpoints[index].endpoint_id == endpoint_id) return (int)index;
    }
    return -1;
}

static bool graph_valid(const MdkrMatchPeerGraph *graph) {
    unsigned index;
    if (graph == NULL ||
        graph->protocol_version != MDKR_MATCH_PEER_GRAPH_VERSION ||
        graph->match_epoch == 0u || graph->endpoint_count < 2u ||
        graph->endpoint_count > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS ||
        graph->reserved[0] != 0u || graph->reserved[1] != 0u ||
        graph->reserved[2] != 0u) return false;
    for (index = 0u; index < MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS; index++) {
        const MdkrMatchPeerEndpoint *item = &graph->endpoints[index];
        unsigned other;
        if (index >= graph->endpoint_count) {
            if (item->endpoint_id != 0u || item->generation != 0u ||
                item->reachable_mask != 0u) return false;
            continue;
        }
        if (item->endpoint_id == 0u || item->generation == 0u ||
            (item->reachable_mask & (uint8_t)(1u << index)) != 0u ||
            (item->reachable_mask &
             (uint8_t)~((1u << graph->endpoint_count) - 1u)) != 0u) return false;
        for (other = 0u; other < index; other++) {
            if (graph->endpoints[other].endpoint_id == item->endpoint_id)
                return false;
        }
    }
    return true;
}

static bool mutually_reachable(
    const MdkrMatchPeerGraph *graph, unsigned left, unsigned right) {
    return (graph->endpoints[left].reachable_mask & (uint8_t)(1u << right)) != 0u &&
        (graph->endpoints[right].reachable_mask & (uint8_t)(1u << left)) != 0u;
}

static int intermediate_index(
    const MdkrMatchPeerGraph *graph, unsigned source, unsigned destination) {
    int selected = -1;
    unsigned index;
    for (index = 0u; index < graph->endpoint_count; index++) {
        if (index == source || index == destination ||
            !mutually_reachable(graph, source, index) ||
            !mutually_reachable(graph, index, destination)) continue;
        if (selected < 0 || graph->endpoints[index].endpoint_id <
                              graph->endpoints[(unsigned)selected].endpoint_id)
            selected = (int)index;
    }
    return selected;
}

bool mdkr_match_peer_graph_init(
    MdkrMatchPeerGraph *graph, uint32_t match_epoch,
    const MdkrMatchPeerEndpoint *endpoints, unsigned endpoint_count) {
    MdkrMatchPeerGraph next;
    unsigned index;
    if (graph == NULL || endpoints == NULL || match_epoch == 0u ||
        endpoint_count < 2u ||
        endpoint_count > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS) return false;
    memset(&next, 0, sizeof(next));
    next.protocol_version = MDKR_MATCH_PEER_GRAPH_VERSION;
    next.match_epoch = match_epoch;
    next.endpoint_count = (uint8_t)endpoint_count;
    /* Copy fields, not object representations. MdkrMatchPeerEndpoint has
     * ABI-dependent tail padding after reachable_mask; copying caller padding
     * would make two equal authenticated graphs compare or hash differently. */
    for (index = 0u; index < endpoint_count; index++) {
        next.endpoints[index].endpoint_id = endpoints[index].endpoint_id;
        next.endpoints[index].generation = endpoints[index].generation;
        next.endpoints[index].reachable_mask = endpoints[index].reachable_mask;
    }
    if (!graph_valid(&next)) return false;
    *graph = next;
    return true;
}

bool mdkr_match_peer_graph_admissible(const MdkrMatchPeerGraph *graph) {
    unsigned source;
    unsigned destination;
    if (!graph_valid(graph)) return false;
    for (source = 0u; source < graph->endpoint_count; source++) {
        for (destination = source + 1u; destination < graph->endpoint_count;
             destination++) {
            if (!mutually_reachable(graph, source, destination) &&
                intermediate_index(graph, source, destination) < 0) return false;
        }
    }
    return true;
}

MdkrMatchPeerRouteResult mdkr_match_peer_graph_route(
    const MdkrMatchPeerGraph *graph, uint32_t match_epoch,
    uint64_t source_endpoint_id, uint32_t source_generation,
    uint64_t destination_endpoint_id, uint32_t destination_generation,
    MdkrMatchPeerRoute *route) {
    MdkrMatchPeerRoute next;
    int source;
    int destination;
    int intermediate;
    if (!graph_valid(graph) || route == NULL || source_endpoint_id == 0u ||
        destination_endpoint_id == 0u ||
        source_endpoint_id == destination_endpoint_id)
        return MDKR_MATCH_PEER_ROUTE_INVALID;
    if (match_epoch != graph->match_epoch)
        return MDKR_MATCH_PEER_ROUTE_STALE_EPOCH;
    source = endpoint_index(graph, source_endpoint_id);
    destination = endpoint_index(graph, destination_endpoint_id);
    if (source < 0 || destination < 0) return MDKR_MATCH_PEER_ROUTE_NOT_FOUND;
    if (graph->endpoints[(unsigned)source].generation != source_generation ||
        graph->endpoints[(unsigned)destination].generation != destination_generation)
        return MDKR_MATCH_PEER_ROUTE_STALE_GENERATION;
    memset(&next, 0, sizeof(next));
    next.match_epoch = graph->match_epoch;
    next.endpoint_ids[0] = source_endpoint_id;
    next.generations[0] = source_generation;
    if (mutually_reachable(graph, (unsigned)source, (unsigned)destination)) {
        next.node_count = 2u;
        next.endpoint_ids[1] = destination_endpoint_id;
        next.generations[1] = destination_generation;
    } else {
        intermediate = intermediate_index(
            graph, (unsigned)source, (unsigned)destination);
        if (intermediate < 0) return MDKR_MATCH_PEER_ROUTE_UNREACHABLE;
        next.node_count = 3u;
        next.forward_count = 1u;
        next.endpoint_ids[1] =
            graph->endpoints[(unsigned)intermediate].endpoint_id;
        next.generations[1] =
            graph->endpoints[(unsigned)intermediate].generation;
        next.endpoint_ids[2] = destination_endpoint_id;
        next.generations[2] = destination_generation;
    }
    *route = next;
    return MDKR_MATCH_PEER_ROUTE_OK;
}
