/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_transport.h"

static MdkrSessionLaunchV2 launch(void) {
    MdkrSessionLaunchV2 value;
    memset(&value, 0, sizeof(value));
    value.version = MDKR_SESSION_LAUNCH_VERSION;
    value.size = sizeof(value);
    value.match.match_epoch = 7u;
    value.match.protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    value.match.build_id[0] = 0x42u;
    value.match.gameplay_digest[0] = 0x91u;
    value.match.slot_owner[0] = 11u;
    value.match.slot_owner[1] = 22u;
    value.match.slot_owner[2] = 33u;
    value.match.slot_owner[3] = 44u;
    value.match.rng_seed = UINT64_C(0x0123456789abcdef);
    value.match.track_id = 5u;
    value.match.rom_revision = MDKR_ROM_US_11;
    value.match.cadence_hz = 30u;
    value.match.slot_count = 4u;
    value.match.rules = 1u;
    value.match.vehicle_mask = 7u;
    value.local_slot_mask = 0x5u;
    value.viewport_slot_mask = 0x5u;
    return value;
}

static MdkrPadSample sample(uint16_t buttons, int x) {
    const MdkrPadSample value = {buttons, (int8_t)x, 0, 1u};
    return value;
}

int main(void) {
    MdkrSessionLaunchV2 admitted = launch();
    MdkrSessionBridge bridge;
    MdkrMatchTransport transport;
    MdkrMatchTransport before;
    MdkrSessionBridge bridge_before;
    MdkrPadSample remote1 = sample(0x1000u, -20);
    MdkrPadSample remote3 = sample(0x2000u, 30);
    MdkrPadSample local[2] = {sample(0x8000u, 10), sample(0x4000u, 40)};
    const MdkrInputSet *frame;
    const MdkrMatchTransportStats *stats;
    MdkrMatchRecovery recovery;
    uint32_t tick = 0u;

    mdkr_session_bridge_init(&bridge);
    assert(mdkr_session_bridge_apply_launch(&bridge, &admitted));
    assert(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_BOOTING));
    assert(mdkr_session_bridge_set_engine_phase(&bridge, MDKR_ENGINE_READY));
    assert(mdkr_match_transport_init(&transport, &bridge, 100u));

    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 100u, &remote1) ==
        MDKR_MATCH_INGRESS_ACCEPTED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x8u, 3u, 100u, &remote3) ==
        MDKR_MATCH_INGRESS_ACCEPTED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 100u, &remote1) ==
        MDKR_MATCH_INGRESS_DUPLICATE);
    assert(mdkr_match_transport_drain_tick(
        &transport, 7u, 100u, local, 2u));
    frame = mdkr_session_bridge_inputs(&bridge, &tick);
    assert(frame != NULL && tick == 100u);
    assert(frame->present_mask == 0x0fu && frame->confirmed_mask == 0x0fu);
    assert(frame->slots[0].buttons == local[0].buttons);
    assert(frame->slots[1].buttons == remote1.buttons);
    assert(frame->slots[2].buttons == local[1].buttons);
    assert(frame->slots[3].buttons == remote3.buttons);

    /* Missing remote input repeats the prior sample but is not confirmed. */
    assert(mdkr_match_transport_drain_tick(
        &transport, 7u, 101u, local, 2u));
    frame = mdkr_session_bridge_inputs(&bridge, &tick);
    assert(frame != NULL && tick == 101u && frame->present_mask == 0x0fu);
    assert(frame->confirmed_mask == 0x05u);
    assert(frame->slots[1].buttons == remote1.buttons &&
           frame->slots[3].buttons == remote3.buttons);

    /* Authenticated identity, not packet claims, owns ingress authorization. */
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 3u, 101u, &remote3) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x1u, 0u, 101u, &local[0]) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x3u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_UNAUTHORIZED);
    assert(mdkr_match_transport_receive(
        &transport, 6u, 0x2u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_STALE_EPOCH);

    /* A late correction marks the rollback boundary. */
    remote1.buttons = 0x0040u;
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 101u, &remote1) ==
        MDKR_MATCH_INGRESS_CORRECTED);
    assert(mdkr_match_transport_take_dirty(&transport, &tick) && tick == 101u);
    assert(mdkr_match_transport_inputs_for_tick(
        &transport, 7u, 101u, &bridge_before.latest_inputs));
    assert(bridge_before.latest_inputs.slots[1].buttons == remote1.buttons);
    assert((bridge_before.latest_inputs.confirmed_mask & 0x02u) != 0u);
    assert(!mdkr_match_transport_inputs_for_tick(
        &transport, 6u, 101u, &bridge_before.latest_inputs));
    before = transport;
    assert(!mdkr_match_transport_inputs_for_tick(
        &transport, 7u, 102u, &bridge_before.latest_inputs));
    assert(memcmp(&transport.history, &before.history,
                  sizeof(transport.history)) == 0);

    /* Drain rejection is atomic across history and bridge. */
    before = transport;
    bridge_before = bridge;
    local[1].stick_x = 81;
    assert(!mdkr_match_transport_drain_tick(
        &transport, 7u, 102u, local, 2u));
    assert(memcmp(&transport.history, &before.history,
                  sizeof(transport.history)) == 0);
    assert(memcmp(&bridge, &bridge_before, sizeof(bridge)) == 0);

    stats = mdkr_match_transport_stats(&transport);
    assert(stats != NULL && stats->accepted == 2u &&
           stats->corrected == 1u && stats->duplicates == 1u &&
           stats->unauthorized == 3u && stats->stale_epoch == 1u &&
           stats->out_of_window == 0u &&
           stats->drained == 2u && stats->drain_rejected == 1u);

    /* An authenticated timeline gap is allowed only while its predecessor is
     * still retained. Once exact replay is impossible, the request is sticky
     * and typed so launcher UX can recover instead of crashing the engine. */
    assert(!mdkr_match_transport_recovery(&transport, &recovery));
    local[1].stick_x = 40;
    for (uint32_t future = 102u;
         future <= 101u + MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS + 1u;
         future++) {
        assert(mdkr_match_transport_drain_tick(
            &transport, 7u, future, local, 2u));
    }
    assert(mdkr_match_transport_recovery(&transport, &recovery));
    assert(recovery.reason == MDKR_MATCH_RECOVERY_INPUT_GAP &&
           recovery.canonical_slot == 3u &&
           recovery.first_unrecoverable_tick == 101u &&
           recovery.observed_at_tick ==
               101u + MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS + 1u);
    assert(mdkr_match_transport_receive(
        &transport, 7u, 0x2u, 1u, 70u, &remote1) ==
        MDKR_MATCH_INGRESS_OUT_OF_WINDOW);
    assert(mdkr_match_transport_recovery(&transport, &recovery) &&
           recovery.reason == MDKR_MATCH_RECOVERY_INPUT_GAP);

    /* A room-authorized future tick is the one-way ownership boundary for AI
     * takeover. Grace input remains legal, activation is neutral/confirmed in
     * history, and neither old packets nor a second owner can reopen the seat. */
    {
        MdkrSessionBridge takeover_bridge;
        MdkrMatchTransport takeover;
        MdkrPadSample takeover_local[2] = {
            sample(0x8000u, 10), sample(0x4000u, 40)};
        MdkrPadSample peer1 = sample(0x1000u, -20);
        MdkrPadSample peer3 = sample(0x2000u, 30);
        uint8_t ai_mask = 0xffu;
        mdkr_session_bridge_init(&takeover_bridge);
        assert(mdkr_session_bridge_apply_launch(&takeover_bridge, &admitted));
        assert(mdkr_session_bridge_set_engine_phase(
            &takeover_bridge, MDKR_ENGINE_BOOTING));
        assert(mdkr_session_bridge_set_engine_phase(
            &takeover_bridge, MDKR_ENGINE_READY));
        assert(mdkr_match_transport_init(&takeover, &takeover_bridge, 10u));
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 10u, &peer1) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x8u, 3u, 10u, &peer3) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_drain_tick(
            &takeover, 7u, 10u, takeover_local, 2u));
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 6u, 1u, 13u) == MDKR_MATCH_TAKEOVER_STALE_EPOCH);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 4u, 13u) == MDKR_MATCH_TAKEOVER_INVALID);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 10u) == MDKR_MATCH_TAKEOVER_TOO_LATE);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 13u) == MDKR_MATCH_TAKEOVER_ACCEPTED);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 13u) == MDKR_MATCH_TAKEOVER_DUPLICATE);
        assert(mdkr_match_transport_schedule_ai_takeover(
            &takeover, 7u, 1u, 14u) == MDKR_MATCH_TAKEOVER_CONFLICT);
        assert(mdkr_match_transport_ai_takeover_mask_for_tick(
            &takeover, 7u, 12u, &ai_mask) && ai_mask == 0u);
        assert(mdkr_match_transport_ai_takeover_mask_for_tick(
            &takeover, 7u, 13u, &ai_mask) && ai_mask == 0x02u);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 12u, &peer1) ==
            MDKR_MATCH_INGRESS_ACCEPTED);
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 13u, &peer1) ==
            MDKR_MATCH_INGRESS_TAKEN_OVER);
        for (uint32_t authored = 11u; authored <= 45u; authored++) {
            assert(mdkr_match_transport_receive(
                &takeover, 7u, 0x8u, 3u, authored, &peer3) ==
                MDKR_MATCH_INGRESS_ACCEPTED);
            if (authored == 15u) {
                assert(mdkr_match_transport_schedule_ai_takeover(
                    &takeover, 7u, 0u, authored) ==
                    MDKR_MATCH_TAKEOVER_ACCEPTED);
            }
            assert(mdkr_match_transport_drain_tick(
                &takeover, 7u, authored, takeover_local, 2u));
            if (authored >= 13u) {
                MdkrInputSet retained;
                assert(mdkr_match_transport_inputs_for_tick(
                    &takeover, 7u, authored, &retained));
                assert(retained.slots[1].buttons == 0u &&
                       (retained.confirmed_mask & 0x02u) != 0u);
                if (authored >= 15u) {
                    assert(retained.slots[0].buttons == 0u &&
                           (retained.confirmed_mask & 0x01u) != 0u);
                }
            }
        }
        assert(mdkr_match_transport_receive(
            &takeover, 7u, 0x2u, 1u, 12u, &peer1) ==
            MDKR_MATCH_INGRESS_TAKEN_OVER);
        assert(!mdkr_match_transport_recovery(&takeover, &recovery));
    }
    puts("test_match_transport: PASS");
    return 0;
}
