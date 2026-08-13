#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_peer_graph.h"

static MdkrMatchPeerEndpoint endpoint(
    uint64_t id, uint32_t generation, uint8_t reachable) {
    MdkrMatchPeerEndpoint value = {id, generation, reachable};
    return value;
}

static void untouched(const MdkrMatchPeerRoute *route) {
    const uint8_t *bytes = (const uint8_t *)route;
    unsigned index;
    for (index = 0u; index < sizeof(*route); index++) assert(bytes[index] == 0xa5u);
}

int main(void) {
    MdkrMatchPeerGraph graph;
    MdkrMatchPeerGraph before;
    MdkrMatchPeerRoute route;
    MdkrMatchPeerEndpoint direct[] = {
        endpoint(100u, 1u, 0x02u), endpoint(200u, 3u, 0x01u)};
    assert(mdkr_match_peer_graph_init(&graph, 7u, direct, 2u));
    assert(mdkr_match_peer_graph_admissible(&graph));
    memset(&route, 0xa5, sizeof(route));
    assert(mdkr_match_peer_graph_route(
        &graph, 7u, 100u, 1u, 200u, 3u, &route) == MDKR_MATCH_PEER_ROUTE_OK);
    assert(route.node_count == 2u && route.forward_count == 0u &&
           route.endpoint_ids[0] == 100u && route.endpoint_ids[1] == 200u);

    /* Equivalent field values produce one canonical graph even when caller
     * object padding contains unrelated bytes. */
    {
        MdkrMatchPeerEndpoint poisoned[2];
        MdkrMatchPeerGraph canonical = graph;
        memset(poisoned, 0xa5, sizeof(poisoned));
        poisoned[0].endpoint_id = 100u;
        poisoned[0].generation = 1u;
        poisoned[0].reachable_mask = 0x02u;
        poisoned[1].endpoint_id = 200u;
        poisoned[1].generation = 3u;
        poisoned[1].reachable_mask = 0x01u;
        assert(mdkr_match_peer_graph_init(&graph, 7u, poisoned, 2u));
        assert(memcmp(&graph, &canonical, sizeof(graph)) == 0);
    }

    /* A partial three-peer graph is admitted at diameter two and chooses the
     * middle endpoint for both directions. */
    {
        MdkrMatchPeerEndpoint line[] = {
            endpoint(300u, 4u, 0x02u), endpoint(100u, 5u, 0x05u),
            endpoint(200u, 6u, 0x02u)};
        assert(mdkr_match_peer_graph_init(&graph, 8u, line, 3u));
        assert(mdkr_match_peer_graph_admissible(&graph));
        assert(mdkr_match_peer_graph_route(
            &graph, 8u, 300u, 4u, 200u, 6u, &route) == MDKR_MATCH_PEER_ROUTE_OK);
        assert(route.node_count == 3u && route.forward_count == 1u &&
               route.endpoint_ids[1] == 100u && route.generations[1] == 5u);
        assert(mdkr_match_peer_graph_route(
            &graph, 8u, 200u, 6u, 300u, 4u, &route) == MDKR_MATCH_PEER_ROUTE_OK &&
               route.endpoint_ids[1] == 100u);
    }

    /* Multiple valid intermediates choose stable authenticated endpoint id,
     * never array position. */
    {
        MdkrMatchPeerEndpoint diamond[] = {
            endpoint(400u, 1u, 0x06u), endpoint(300u, 2u, 0x09u),
            endpoint(100u, 3u, 0x09u), endpoint(200u, 4u, 0x06u)};
        assert(mdkr_match_peer_graph_init(&graph, 9u, diamond, 4u));
        assert(mdkr_match_peer_graph_admissible(&graph));
        assert(mdkr_match_peer_graph_route(
            &graph, 9u, 400u, 1u, 200u, 4u, &route) == MDKR_MATCH_PEER_ROUTE_OK);
        assert(route.endpoint_ids[1] == 100u);
    }

    /* A length-three chain is honest refusal: some endpoints can communicate,
     * but the full match graph is not admissible and the end pair has no route. */
    {
        MdkrMatchPeerEndpoint chain[] = {
            endpoint(10u, 1u, 0x02u), endpoint(20u, 1u, 0x05u),
            endpoint(30u, 1u, 0x0au), endpoint(40u, 1u, 0x04u)};
        assert(mdkr_match_peer_graph_init(&graph, 10u, chain, 4u));
        assert(!mdkr_match_peer_graph_admissible(&graph));
        memset(&route, 0xa5, sizeof(route));
        assert(mdkr_match_peer_graph_route(
            &graph, 10u, 10u, 1u, 40u, 1u, &route) ==
            MDKR_MATCH_PEER_ROUTE_UNREACHABLE);
        untouched(&route);
    }

    /* Asymmetric claims never create an edge. */
    {
        MdkrMatchPeerEndpoint asymmetric[] = {
            endpoint(10u, 1u, 0x02u), endpoint(20u, 1u, 0x00u)};
        assert(mdkr_match_peer_graph_init(&graph, 11u, asymmetric, 2u));
        assert(!mdkr_match_peer_graph_admissible(&graph));
        memset(&route, 0xa5, sizeof(route));
        assert(mdkr_match_peer_graph_route(
            &graph, 11u, 10u, 1u, 20u, 1u, &route) ==
            MDKR_MATCH_PEER_ROUTE_UNREACHABLE);
        untouched(&route);
    }

    /* Epoch and both endpoint generations are exact and failure is atomic. */
    assert(mdkr_match_peer_graph_init(&graph, 7u, direct, 2u));
    memset(&route, 0xa5, sizeof(route));
    assert(mdkr_match_peer_graph_route(
        &graph, 6u, 100u, 1u, 200u, 3u, &route) ==
        MDKR_MATCH_PEER_ROUTE_STALE_EPOCH);
    untouched(&route);
    assert(mdkr_match_peer_graph_route(
        &graph, 7u, 100u, 2u, 200u, 3u, &route) ==
        MDKR_MATCH_PEER_ROUTE_STALE_GENERATION);
    untouched(&route);
    assert(mdkr_match_peer_graph_route(
        &graph, 7u, 100u, 1u, 200u, 2u, &route) ==
        MDKR_MATCH_PEER_ROUTE_STALE_GENERATION);
    untouched(&route);

    /* Invalid snapshots never partially replace the live topology. */
    before = graph;
    direct[1].endpoint_id = 100u;
    assert(!mdkr_match_peer_graph_init(&graph, 7u, direct, 2u));
    assert(memcmp(&graph, &before, sizeof(graph)) == 0);
    direct[1].endpoint_id = 200u;
    direct[0].reachable_mask = 0x01u;
    assert(!mdkr_match_peer_graph_init(&graph, 7u, direct, 2u));
    assert(memcmp(&graph, &before, sizeof(graph)) == 0);

    puts("test_match_peer_graph: PASS");
    return 0;
}
