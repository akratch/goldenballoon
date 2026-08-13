/* Bounded launcher-owned direct/one-hop gameplay topology planner. */
#ifndef MDKR_MATCH_PEER_GRAPH_H
#define MDKR_MATCH_PEER_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PEER_GRAPH_VERSION 1u
#define MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS 4u
#define MDKR_MATCH_PEER_ROUTE_MAX_NODES 3u

typedef struct MdkrMatchPeerEndpoint {
    uint64_t endpoint_id;
    uint32_t generation;
    /* Directed observations. A usable edge exists only when both endpoints
     * report each other reachable. Self bits and out-of-roster bits are
     * forbidden. */
    uint8_t reachable_mask;
} MdkrMatchPeerEndpoint;

typedef struct MdkrMatchPeerGraph {
    uint32_t protocol_version;
    uint32_t match_epoch;
    uint8_t endpoint_count;
    uint8_t reserved[3];
    MdkrMatchPeerEndpoint endpoints[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
} MdkrMatchPeerGraph;

typedef enum MdkrMatchPeerRouteResult {
    MDKR_MATCH_PEER_ROUTE_OK = 0,
    MDKR_MATCH_PEER_ROUTE_INVALID,
    MDKR_MATCH_PEER_ROUTE_STALE_EPOCH,
    MDKR_MATCH_PEER_ROUTE_NOT_FOUND,
    MDKR_MATCH_PEER_ROUTE_STALE_GENERATION,
    MDKR_MATCH_PEER_ROUTE_UNREACHABLE
} MdkrMatchPeerRouteResult;

typedef struct MdkrMatchPeerRoute {
    uint32_t match_epoch;
    uint8_t node_count;
    /* Zero for a direct path, one for the only supported forwarded path. */
    uint8_t forward_count;
    uint8_t reserved[2];
    uint64_t endpoint_ids[MDKR_MATCH_PEER_ROUTE_MAX_NODES];
    uint32_t generations[MDKR_MATCH_PEER_ROUTE_MAX_NODES];
} MdkrMatchPeerRoute;

/* Copies and validates one topology snapshot. Asymmetric observations are
 * retained but never treated as a usable gameplay edge. */
bool mdkr_match_peer_graph_init(
    MdkrMatchPeerGraph *graph, uint32_t match_epoch,
    const MdkrMatchPeerEndpoint *endpoints, unsigned endpoint_count);

/* True only when every endpoint pair has a mutual direct or one-hop path. */
bool mdkr_match_peer_graph_admissible(const MdkrMatchPeerGraph *graph);

/* Selects a stable route. If several one-hop routes exist, the endpoint with
 * the numerically lowest authenticated endpoint id wins. The exact source and
 * destination generations prevent a stale connection from acquiring a route. */
MdkrMatchPeerRouteResult mdkr_match_peer_graph_route(
    const MdkrMatchPeerGraph *graph, uint32_t match_epoch,
    uint64_t source_endpoint_id, uint32_t source_generation,
    uint64_t destination_endpoint_id, uint32_t destination_generation,
    MdkrMatchPeerRoute *route);

#ifdef __cplusplus
}
#endif
#endif
