#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_peer_forward.h"

int main(void) {
    const MdkrMatchPeerEndpoint endpoints[] = {
        {400u, 1u, 0x06u}, {300u, 2u, 0x09u},
        {100u, 3u, 0x09u}, {200u, 4u, 0x06u}};
    MdkrMatchPeerGraph graph;
    MdkrMatchPeerForwarder forwarder;
    MdkrMatchPeerForwarder before;
    MdkrMatchPeerEnvelopeContext context = {
        {9u, 400u, 1u, 200u, 4u}, 100u, 1u,
        MDKR_MATCH_PEER_PAYLOAD_INPUT};
    assert(mdkr_match_peer_graph_init(&graph, 9u, endpoints, 4u));
    assert(mdkr_match_peer_graph_admissible(&graph));
    assert(mdkr_match_peer_forwarder_init(&forwarder, 9u, 100u, 3u));
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 1u, &context) ==
        MDKR_MATCH_PEER_FORWARD_ACCEPTED);
    before = forwarder;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 1u, &context) ==
        MDKR_MATCH_PEER_FORWARD_DUPLICATE);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);

    context.sequence = 3u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 1u, &context) ==
        MDKR_MATCH_PEER_FORWARD_ACCEPTED);
    context.sequence = 2u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 1u, &context) ==
        MDKR_MATCH_PEER_FORWARD_ACCEPTED);
    before = forwarder;

    /* A new authenticated source generation owns a fresh key/sequence space
     * and reuses the existing direction slot instead of inheriting replay. */
    {
        MdkrMatchPeerEndpoint reconnected[4];
        MdkrMatchPeerGraph next_graph;
        memcpy(reconnected, endpoints, sizeof(reconnected));
        reconnected[0].generation = 5u;
        assert(mdkr_match_peer_graph_init(&next_graph, 9u, reconnected, 4u));
        context.key.source_generation = 5u;
        context.sequence = 1u;
        assert(mdkr_match_peer_forwarder_admit(
            &forwarder, &next_graph, 400u, 5u, &context) ==
            MDKR_MATCH_PEER_FORWARD_ACCEPTED);
        assert(forwarder.windows[0].source_generation == 5u &&
               forwarder.windows[0].greatest_sequence == 1u);
        before = forwarder;
        context.key.source_generation = 1u;
        assert(mdkr_match_peer_forwarder_admit(
            &forwarder, &next_graph, 400u, 5u, &context) ==
            MDKR_MATCH_PEER_FORWARD_STALE_GENERATION);
        assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
        context.key.source_generation = 5u;
        assert(mdkr_match_peer_forwarder_admit(
            &forwarder, &next_graph, 400u, 1u, &context) ==
            MDKR_MATCH_PEER_FORWARD_STALE_GENERATION);
        assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    }

    context.sequence = 4u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 300u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.intermediate_endpoint_id = 300u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.intermediate_endpoint_id = 0u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_INVALID);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.intermediate_endpoint_id = 100u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 0u, &context) ==
        MDKR_MATCH_PEER_FORWARD_INVALID);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.payload_type = MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX + 1u;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_INVALID);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.payload_type = MDKR_MATCH_PEER_PAYLOAD_INPUT;
    context.key.source_generation++;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_STALE_GENERATION);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    context.key.source_generation--;
    context.key.match_epoch--;
    assert(mdkr_match_peer_forwarder_admit(
        &forwarder, &graph, 400u, 5u, &context) ==
        MDKR_MATCH_PEER_FORWARD_STALE_EPOCH);
    assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);

    /* A direct edge removes forwarding authority immediately. */
    {
        MdkrMatchPeerEndpoint direct[4];
        memcpy(direct, endpoints, sizeof(direct));
        direct[0].reachable_mask |= 0x08u;
        direct[3].reachable_mask |= 0x01u;
        assert(mdkr_match_peer_graph_init(&graph, 9u, direct, 4u));
        context.key.match_epoch = 9u;
        context.key.source_generation = 1u;
        assert(mdkr_match_peer_forwarder_admit(
            &forwarder, &graph, 400u, 1u, &context) ==
            MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE);
        assert(memcmp(&forwarder, &before, sizeof(forwarder)) == 0);
    }

    puts("test_match_peer_forward: PASS");
    return 0;
}
