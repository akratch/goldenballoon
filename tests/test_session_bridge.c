#include "session/session_bridge.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (condition) printf("ok: %s\n", message);
    else {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static MdkrSessionLaunchV2 launch(uint32_t epoch, uint8_t local_mask) {
    MdkrSessionLaunchV2 value;
    memset(&value, 0, sizeof(value));
    value.version = MDKR_SESSION_LAUNCH_VERSION;
    value.size = sizeof(value);
    value.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    value.match.match_epoch = epoch;
    value.match.rng_seed = UINT64_C(0x9abcdef012345678);
    value.match.build_id[0] = 17u;
    value.match.gameplay_digest[0] = 23u;
    value.match.slot_owner[0] = UINT64_C(0x1111);
    value.match.slot_owner[1] = UINT64_C(0x2222);
    value.match.track_id = 5u;
    value.match.cadence_hz = 30u;
    value.match.slot_count = 2u;
    value.match.rom_revision = MDKR_ROM_US_11;
    value.match.rules = 1u;
    value.match.vehicle_mask = 7u;
    value.local_slot_mask = local_mask;
    value.viewport_slot_mask = local_mask;
    return value;
}

static MdkrSessionLaunchV3 launch_v3(uint32_t epoch, uint8_t local_mask) {
    MdkrSessionLaunchV2 legacy = launch(epoch, local_mask);
    MdkrSessionLaunchV3 value;
    unsigned slot;
    memset(&value, 0, sizeof(value));
    value.version = MDKR_SESSION_LAUNCH_V3_VERSION;
    value.size = sizeof(value);
    value.match.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    value.match.manifest = legacy.match;
    for (slot = 0u; slot < MDKR_MATCH_SLOTS; slot++) {
        value.match.selections[slot].character_id = MDKR_MATCH_NO_CHARACTER;
        value.match.selections[slot].vehicle_id = MDKR_MATCH_NO_VEHICLE;
    }
    value.match.selections[0].selection_revision = 3u;
    value.match.selections[0].character_id = 4u;
    value.match.selections[0].vehicle_id = 0u;
    value.match.selections[1].selection_revision = 7u;
    value.match.selections[1].character_id = 8u;
    value.match.selections[1].vehicle_id = 1u;
    value.local_slot_mask = local_mask;
    value.viewport_slot_mask = local_mask;
    return value;
}

static MdkrInputSet inputs(void) {
    MdkrInputSet value;
    memset(&value, 0, sizeof(value));
    value.present_mask = 3u;
    value.confirmed_mask = 1u;
    value.slots[0].present = 1u;
    value.slots[0].buttons = 0x8000u;
    value.slots[0].stick_x = 40;
    value.slots[1].present = 1u;
    value.slots[1].stick_y = -40;
    return value;
}

static void test_bridge_lifecycle(void) {
    MdkrSessionBridge bridge;
    MdkrSessionLaunchV2 match = launch(1u, 1u);
    MdkrInputSet frame = inputs();
    MdkrMatchResultV1 result;
    MdkrMatchResultV1 exported;
    MdkrSessionEvent event;
    uint32_t tick = 0u;

    mdkr_session_bridge_init(&bridge);
    expect(mdkr_session_bridge_apply_launch(&bridge, &match),
           "valid manifest applies");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING),
           "engine enters booting");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY),
           "engine enters ready");
    expect(mdkr_session_bridge_submit_inputs(&bridge, 1u, &frame),
           "ready engine accepts canonical input");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_RACING),
           "engine enters racing");
    frame.confirmed_mask = 3u;
    expect(mdkr_session_bridge_submit_inputs(&bridge, 2u, &frame),
           "racing engine accepts next input tick");
    expect(mdkr_session_bridge_inputs(&bridge, &tick) != NULL && tick == 2u,
           "bridge exposes the latest complete tick set");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_FINISHED),
           "engine enters finished");

    memset(&result, 0, sizeof(result));
    result.version = MDKR_MATCH_RESULT_VERSION;
    result.size = sizeof(result);
    result.match_epoch = 1u;
    result.terminal_tick = 2u;
    result.authority_hash = UINT64_C(0x12345678);
    result.finishing_order[0] = 1u;
    result.finishing_order[1] = 0u;
    result.player_count = 2u;
    result.synchronized = 1u;
    expect(mdkr_session_bridge_publish_result(&bridge, &result),
           "finished engine publishes validated result");
    expect(mdkr_session_bridge_export_result(&bridge, &exported) &&
           memcmp(&result, &exported, sizeof(result)) == 0,
           "result export is byte exact");

    unsigned events = 0u;
    while (mdkr_session_bridge_poll_event(&bridge, &event)) {
        expect(event.protocol_version == MDKR_SESSION_PROTOCOL_VERSION,
               "event carries protocol version");
        events++;
    }
    expect(events == 8u, "lifecycle emits bounded expected event count");
}

static void test_invalid_inputs_fail_atomically(void) {
    MdkrSessionBridge bridge;
    MdkrSessionBridge before;
    MdkrSessionLaunchV2 match = launch(1u, 1u);
    MdkrInputSet frame = inputs();

    mdkr_session_bridge_init(&bridge);
    match.version = 1u;
    before = bridge;
    expect(!mdkr_session_bridge_apply_launch(&bridge, &match),
           "legacy launch v1 is rejected after viewport semantics changed");
    expect(memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "legacy launch rejection cannot mutate bridge");

    match = launch(1u, 1u);
    match.size--;
    before = bridge;
    expect(!mdkr_session_bridge_apply_launch(&bridge, &match),
           "wrong manifest size is rejected");
    expect(memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "rejected manifest cannot mutate bridge");

    match = launch(1u, 1u);
    expect(mdkr_session_bridge_apply_launch(&bridge, &match),
           "valid manifest accepted after rejection");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY),
           "invalid-input case reaches ready");
    while (bridge.event_count != 0u) {
        MdkrSessionEvent ignored;
        (void)mdkr_session_bridge_poll_event(&bridge, &ignored);
    }
    before = bridge;
    frame.slots[0].stick_x = 81;
    expect(!mdkr_session_bridge_submit_inputs(&bridge, 1u, &frame),
           "out-of-range stick is rejected");
    expect(memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "rejected input cannot mutate bridge");

    frame = inputs();
    frame.present_mask = 1u;
    expect(!mdkr_session_bridge_submit_inputs(&bridge, 1u, &frame),
           "presence bitmap mismatch is rejected");
    expect(memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "presence mismatch is atomic");
}

static void test_input_events_coalesce_without_losing_progress(void) {
    MdkrSessionBridge bridge;
    MdkrSessionLaunchV2 match = launch(3u, 1u);
    MdkrInputSet frame = inputs();
    MdkrSessionBridge before;

    mdkr_session_bridge_init(&bridge);
    expect(mdkr_session_bridge_apply_launch(&bridge, &match),
           "overflow case applies manifest");
    expect(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY),
           "overflow case reaches ready");
    const uint8_t lifecycle_events = bridge.event_count;
    for (uint32_t tick = 1u; tick <= 1000u; ++tick) {
        expect(mdkr_session_bridge_submit_inputs(&bridge, tick, &frame),
               "high-rate input never backpressures a healthy match");
    }
    expect(bridge.event_count == (uint8_t)(lifecycle_events + 1u),
           "consecutive input telemetry coalesces to its newest tick");
    expect(bridge.events[(bridge.event_head + bridge.event_count - 1u) %
                         MDKR_SESSION_EVENT_CAPACITY].tick == 1000u,
           "coalesced input witness retains latest tick");
    before = bridge;
    frame.slots[0].stick_x = 81;
    expect(!mdkr_session_bridge_submit_inputs(&bridge, 1001u, &frame),
           "invalid input still fails after long coalesced run");
    expect(memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "invalid coalesced input rejection is atomic");
}

static void test_endpoint_locality_is_not_match_identity(void) {
    MdkrSessionLaunchV2 left = launch(9u, 1u);
    MdkrSessionLaunchV2 right = launch(9u, 2u);
    MdkrSessionBridge left_bridge;
    MdkrSessionBridge right_bridge;
    MdkrSessionLaunchV2 no_render = launch(9u, 1u);
    MdkrSessionBridge no_render_bridge;
    no_render.viewport_slot_mask = 0u;

    expect(memcmp(&left.match, &right.match, sizeof(left.match)) == 0 &&
           mdkr_match_manifest_digest(&left.match) ==
               mdkr_match_manifest_digest(&right.match),
           "different endpoint seats retain one canonical match identity");
    mdkr_session_bridge_init(&left_bridge);
    mdkr_session_bridge_init(&right_bridge);
    mdkr_session_bridge_init(&no_render_bridge);
    expect(mdkr_session_bridge_apply_launch(&left_bridge, &left) &&
           mdkr_session_bridge_apply_launch(&right_bridge, &right),
           "peer-local launch envelopes both apply");
    expect(memcmp(mdkr_session_bridge_manifest(&left_bridge),
                  mdkr_session_bridge_manifest(&right_bridge),
                  sizeof(left.match)) == 0,
           "bridges expose byte-identical canonical manifests");
    expect(mdkr_session_bridge_local_slot_mask(&left_bridge) == 1u &&
           mdkr_session_bridge_local_slot_mask(&right_bridge) == 2u,
           "bridges preserve distinct endpoint-local seat maps");
    expect(mdkr_session_bridge_roster(&left_bridge)->canonical_player_count == 2u &&
           mdkr_session_bridge_roster(&left_bridge)->local_to_canonical[0] == 0u &&
           mdkr_session_bridge_roster(&right_bridge)->local_to_canonical[0] == 1u,
           "bridges materialize canonical-to-local roster mappings");
    expect(mdkr_session_bridge_apply_launch(&no_render_bridge, &no_render) &&
               mdkr_session_bridge_roster(&no_render_bridge)->local_seat_count == 1u &&
               mdkr_session_bridge_roster(&no_render_bridge)->viewport_count == 0u,
           "local seat may run with a genuine no-render viewport map");

    no_render.viewport_slot_mask = 2u;
    mdkr_session_bridge_init(&no_render_bridge);
    expect(!mdkr_session_bridge_apply_launch(&no_render_bridge, &no_render),
           "endpoint cannot render a slot it does not own");
}

static void test_v3_owns_frozen_selections_atomically(void) {
    MdkrSessionBridge bridge;
    MdkrSessionBridge before;
    MdkrSessionLaunchV3 match = launch_v3(12u, 1u);
    const MdkrMatchLaunchDescriptorV1 *owned;

    mdkr_session_bridge_init(&bridge);
    expect(mdkr_session_bridge_apply_launch_v3(&bridge, &match),
           "V3 applies a validated frozen launch descriptor");
    owned = mdkr_session_bridge_launch_descriptor(&bridge);
    expect(owned != NULL &&
               owned->manifest.match_epoch == 12u &&
               owned->selections[0].character_id == 4u &&
               mdkr_session_bridge_manifest(&bridge) == &bridge.manifest,
           "bridge exposes independent owned manifest and selections");
    match.match.selections[0].character_id = 6u;
    expect(owned != NULL && owned->selections[0].character_id == 4u,
           "bridge copy does not borrow launcher descriptor storage");

    mdkr_session_bridge_init(&bridge);
    match = launch_v3(12u, 1u);
    match.match.selections[1].character_id =
        match.match.selections[0].character_id;
    before = bridge;
    expect(!mdkr_session_bridge_apply_launch_v3(&bridge, &match) &&
               memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "duplicate-character V3 rejection is fail-atomic");
    match = launch_v3(12u, 1u);
    match.reserved[2] = 1u;
    expect(!mdkr_session_bridge_apply_launch_v3(&bridge, &match) &&
               memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "non-neutral V3 envelope rejection is fail-atomic");

    {
        MdkrSessionLaunchV2 legacy = launch(12u, 1u);
        expect(mdkr_session_bridge_apply_launch(&bridge, &legacy) &&
                   mdkr_session_bridge_launch_descriptor(&bridge) == NULL,
               "laboratory V2 cannot masquerade as a selection-complete launch");
    }
}

static void test_local_transport_drain_preserves_remote_ownership(void) {
    MdkrSessionBridge bridge;
    MdkrSessionLaunchV2 match = launch(4u, 5u);
    MdkrInputSet transport = inputs();
    MdkrPadSample local[2];
    const MdkrInputSet *accepted;
    uint32_t tick = 0u;

    match.match.slot_owner[2] = UINT64_C(0x3333);
    match.match.slot_owner[3] = UINT64_C(0x4444);
    match.match.slot_count = 4u;
    match.viewport_slot_mask = 5u;
    transport.present_mask = 15u;
    transport.confirmed_mask = 10u;
    transport.slots[2].present = 1u;
    transport.slots[2].buttons = 0x2222u;
    transport.slots[2].stick_x = -22;
    transport.slots[3].present = 1u;
    transport.slots[3].buttons = 0x3333u;
    transport.slots[3].stick_y = 33;
    memset(local, 0, sizeof(local));
    local[0].present = 1u;
    local[0].buttons = 0x0101u;
    local[0].stick_x = 11;
    local[1].present = 1u;
    local[1].buttons = 0x0202u;
    local[1].stick_y = -22;

    mdkr_session_bridge_init(&bridge);
    expect(mdkr_session_bridge_apply_launch(&bridge, &match) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY),
           "two-seat endpoint reaches transport-ready phase");
    expect(mdkr_session_bridge_submit_local_inputs(
               &bridge, 7u, local, 2u, &transport),
           "transport drain merges endpoint-local seat order");
    accepted = mdkr_session_bridge_inputs(&bridge, &tick);
    expect(accepted != NULL && tick == 7u &&
           accepted->slots[0].buttons == local[0].buttons &&
           accepted->slots[2].buttons == local[1].buttons,
           "local seats author canonical slots zero and two");
    expect(accepted != NULL &&
           accepted->slots[1].buttons == transport.slots[1].buttons &&
           accepted->slots[1].stick_y == transport.slots[1].stick_y &&
           accepted->slots[3].buttons == transport.slots[3].buttons &&
           accepted->slots[3].stick_y == transport.slots[3].stick_y,
           "transport drain preserves every remote slot byte-for-byte");
    expect(accepted != NULL && accepted->confirmed_mask == 15u &&
           accepted->present_mask == 15u,
           "local ownership is confirmed without dropping remote confirmation");
}

static void test_local_transport_drain_rejects_atomically(void) {
    MdkrSessionBridge bridge;
    MdkrSessionBridge before;
    MdkrSessionLaunchV2 match = launch(5u, 2u);
    MdkrInputSet transport = inputs();
    MdkrPadSample local;

    memset(&local, 0, sizeof(local));
    local.present = 1u;
    local.buttons = 0x1234u;
    mdkr_session_bridge_init(&bridge);
    expect(mdkr_session_bridge_apply_launch(&bridge, &match) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING) &&
           mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY),
           "single-seat endpoint reaches transport-ready phase");
    before = bridge;
    expect(!mdkr_session_bridge_submit_local_inputs(
               &bridge, 1u, &local, 2u, &transport) &&
           memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "seat-count mismatch rejects without partial bridge mutation");
    local.stick_x = 81;
    expect(!mdkr_session_bridge_submit_local_inputs(
               &bridge, 1u, &local, 1u, &transport) &&
           memcmp(&bridge, &before, sizeof(bridge)) == 0,
           "malformed local sample rejects without partial bridge mutation");
}

int main(void) {
    test_bridge_lifecycle();
    test_invalid_inputs_fail_atomically();
    test_input_events_coalesce_without_losing_progress();
    test_endpoint_locality_is_not_match_identity();
    test_v3_owns_frozen_selections_atomically();
    test_local_transport_drain_preserves_remote_ownership();
    test_local_transport_drain_rejects_atomically();
    if (failures != 0) return 1;
    printf("session bridge contract passed\n");
    return 0;
}
