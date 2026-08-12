#include "match_transport.h"

#include <string.h>

static bool sample_valid(const MdkrPadSample *sample) {
    return sample != NULL && sample->present <= 1u &&
        sample->stick_x >= -80 && sample->stick_x <= 80 &&
        sample->stick_y >= -80 && sample->stick_y <= 80 &&
        (sample->present != 0u ||
         (sample->buttons == 0u && sample->stick_x == 0 &&
          sample->stick_y == 0));
}

static MdkrInputSample net_sample(const MdkrPadSample *sample) {
    MdkrInputSample out;
    out.buttons = sample->buttons;
    out.stick_x = sample->stick_x;
    out.stick_y = sample->stick_y;
    out.present = sample->present != 0u;
    return out;
}

static bool tick_before(uint32_t candidate, uint32_t reference) {
    return mdkr_net_tick_after(reference, candidate);
}

static bool takeover_active(
    const MdkrMatchTransport *transport, unsigned slot, uint32_t tick) {
    const uint8_t bit = (uint8_t)(1u << slot);
    return (transport->ai_takeover_scheduled_mask & bit) != 0u &&
        (tick == transport->ai_takeover_tick[slot] ||
         mdkr_net_tick_after(tick, transport->ai_takeover_tick[slot]));
}

static void latch_recovery(
    MdkrMatchTransport *transport, MdkrMatchRecoveryReason reason,
    unsigned slot, uint32_t first_tick, uint32_t observed_tick) {
    if (transport->recovery.reason != MDKR_MATCH_RECOVERY_NONE) return;
    transport->recovery.reason = reason;
    transport->recovery.canonical_slot = (uint8_t)slot;
    transport->recovery.first_unrecoverable_tick = first_tick;
    transport->recovery.observed_at_tick = observed_tick;
}

static void advance_remote_confirmation(
    MdkrMatchTransport *transport, unsigned slot) {
    uint32_t candidate = transport->remote_have_confirmed[slot]
        ? transport->remote_confirmed_through[slot] + 1u
        : transport->history.first_tick;
    while (candidate == transport->history.current_tick ||
           tick_before(candidate, transport->history.current_tick)) {
        const MdkrNetInputCell *cell =
            &transport->history.cells[candidate % MDKR_NET_INPUT_CAPACITY];
        if (!cell->occupied || cell->tick != candidate ||
            cell->status[slot] != MDKR_NET_INPUT_RECEIVED) break;
        transport->remote_confirmed_through[slot] = candidate;
        transport->remote_have_confirmed[slot] = true;
        if (candidate == transport->history.current_tick) break;
        candidate++;
    }
}

static void detect_unrecoverable_gaps(MdkrMatchTransport *transport) {
    unsigned slot;
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        const uint8_t bit = (uint8_t)(1u << slot);
        uint32_t first_missing;
        if ((transport->remote_slot_mask & bit) == 0u) continue;
        advance_remote_confirmation(transport, slot);
        first_missing = transport->remote_have_confirmed[slot]
            ? transport->remote_confirmed_through[slot] + 1u
            : transport->history.first_tick;
        if ((transport->history.current_tick == first_missing ||
             mdkr_net_tick_after(
                 transport->history.current_tick, first_missing)) &&
            transport->history.current_tick - first_missing >=
                MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS + 1u) {
            latch_recovery(
                transport, MDKR_MATCH_RECOVERY_INPUT_GAP, slot,
                first_missing, transport->history.current_tick);
            return;
        }
    }
}

bool mdkr_match_transport_init(
    MdkrMatchTransport *transport, MdkrSessionBridge *bridge,
    uint32_t first_tick) {
    const MdkrMatchManifestV1 *manifest;
    const MdkrNetRoster *roster;
    MdkrMatchTransport next;
    uint8_t active;
    if (transport == NULL || bridge == NULL ||
        (bridge->engine_phase != MDKR_ENGINE_READY &&
         bridge->engine_phase != MDKR_ENGINE_RACING)) {
        return false;
    }
    manifest = mdkr_session_bridge_manifest(bridge);
    roster = mdkr_session_bridge_roster(bridge);
    if (manifest == NULL || roster == NULL || manifest->match_epoch == 0u ||
        manifest->slot_count == 0u ||
        manifest->slot_count > MDKR_NET_INPUT_SLOTS) {
        return false;
    }
    active = (uint8_t)((1u << manifest->slot_count) - 1u);
    memset(&next, 0, sizeof(next));
    mdkr_net_input_init(&next.history, first_tick);
    next.bridge = bridge;
    next.match_epoch = manifest->match_epoch;
    next.active_slot_mask = active;
    next.local_slot_mask = mdkr_session_bridge_local_slot_mask(bridge);
    next.remote_slot_mask = (uint8_t)(active & ~next.local_slot_mask);
    next.ready = true;
    *transport = next;
    return true;
}

MdkrMatchTransportIngressResult mdkr_match_transport_receive(
    MdkrMatchTransport *transport, uint32_t match_epoch,
    uint8_t authenticated_slot_mask, unsigned slot, uint32_t tick,
    const MdkrPadSample *sample) {
    MdkrNetInputSubmitResult result;
    MdkrInputSample input;
    uint8_t bit;
    if (transport == NULL || !transport->ready || transport->bridge == NULL ||
        (transport->bridge->engine_phase != MDKR_ENGINE_READY &&
         transport->bridge->engine_phase != MDKR_ENGINE_RACING) ||
        mdkr_session_bridge_manifest(transport->bridge) == NULL ||
        mdkr_session_bridge_manifest(transport->bridge)->match_epoch !=
            transport->match_epoch ||
        slot >= MDKR_NET_INPUT_SLOTS ||
        !sample_valid(sample) ||
        (authenticated_slot_mask &
         (uint8_t)~transport->active_slot_mask) != 0u) {
        if (transport != NULL) transport->stats.invalid++;
        return MDKR_MATCH_INGRESS_INVALID;
    }
    if (match_epoch != transport->match_epoch) {
        transport->stats.stale_epoch++;
        return MDKR_MATCH_INGRESS_STALE_EPOCH;
    }
    bit = (uint8_t)(1u << slot);
    if (authenticated_slot_mask == 0u ||
        (authenticated_slot_mask &
         (uint8_t)~transport->remote_slot_mask) != 0u ||
        (transport->remote_slot_mask & bit) == 0u ||
        (authenticated_slot_mask & bit) == 0u) {
        transport->stats.unauthorized++;
        return MDKR_MATCH_INGRESS_UNAUTHORIZED;
    }
    /* Once the room-authorized takeover tick has been authored, accepting any
     * more peer input for that seat would create two simultaneous owners. This
     * also rejects late pre-takeover corrections after the cutoff has become
     * final, so already-committed predicted effects cannot be reopened. */
    if ((transport->ai_takeover_scheduled_mask & bit) != 0u &&
        (takeover_active(transport, slot, tick) ||
         takeover_active(transport, slot, transport->history.current_tick))) {
        transport->stats.takeover_ignored_inputs++;
        return MDKR_MATCH_INGRESS_TAKEN_OVER;
    }
    if (mdkr_net_tick_after(transport->history.current_tick, tick) &&
        transport->history.current_tick - tick >
            MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS) {
        transport->stats.out_of_window++;
        latch_recovery(
            transport, MDKR_MATCH_RECOVERY_LATE_INPUT, slot, tick,
            transport->history.current_tick);
        return MDKR_MATCH_INGRESS_OUT_OF_WINDOW;
    }
    input = net_sample(sample);
    result = mdkr_net_input_submit(&transport->history, slot, tick, &input);
    switch (result) {
        case MDKR_NET_SUBMIT_ACCEPTED:
            transport->stats.accepted++;
            advance_remote_confirmation(transport, slot);
            return MDKR_MATCH_INGRESS_ACCEPTED;
        case MDKR_NET_SUBMIT_CORRECTED:
            transport->stats.corrected++;
            advance_remote_confirmation(transport, slot);
            return MDKR_MATCH_INGRESS_CORRECTED;
        case MDKR_NET_SUBMIT_DUPLICATE:
            transport->stats.duplicates++;
            advance_remote_confirmation(transport, slot);
            return MDKR_MATCH_INGRESS_DUPLICATE;
        case MDKR_NET_SUBMIT_CONFLICT:
            transport->stats.conflicts++;
            return MDKR_MATCH_INGRESS_CONFLICT;
        case MDKR_NET_SUBMIT_TOO_OLD:
        case MDKR_NET_SUBMIT_TOO_FAR_FUTURE:
            transport->stats.out_of_window++;
            return MDKR_MATCH_INGRESS_OUT_OF_WINDOW;
        default:
            transport->stats.invalid++;
            return MDKR_MATCH_INGRESS_INVALID;
    }
}

bool mdkr_match_transport_drain_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    const MdkrPadSample *local_samples, unsigned local_sample_count) {
    MdkrMatchTransport next;
    MdkrSessionBridge next_bridge;
    MdkrNetInputSet predicted;
    MdkrInputSet base;
    MdkrPadSample effective_local[MDKR_SESSION_MAX_PLAYERS];
    const MdkrInputSet *accepted;
    uint32_t accepted_tick;
    unsigned slot;
    unsigned local;
    if (transport == NULL || !transport->ready || transport->bridge == NULL ||
        (transport->bridge->engine_phase != MDKR_ENGINE_READY &&
         transport->bridge->engine_phase != MDKR_ENGINE_RACING) ||
        mdkr_session_bridge_manifest(transport->bridge) == NULL ||
        mdkr_session_bridge_manifest(transport->bridge)->match_epoch !=
            transport->match_epoch ||
        match_epoch != transport->match_epoch ||
        local_sample_count != transport->bridge->roster.local_seat_count ||
        (local_sample_count != 0u && local_samples == NULL)) {
        if (transport != NULL) transport->stats.drain_rejected++;
        return false;
    }
    for (local = 0u; local < local_sample_count; local++) {
        if (!sample_valid(&local_samples[local])) {
            transport->stats.drain_rejected++;
            return false;
        }
        effective_local[local] = local_samples[local];
    }

    next = *transport;
    next_bridge = *transport->bridge;
    next.bridge = &next_bridge;
    /* The native engine's first authored input is tick one, while the pure
     * transport fixtures intentionally start at tick zero. Adopt the first
     * actual drain tick only when the configured origin was never authored. */
    if (next.stats.drained == 0u && tick != next.history.first_tick) {
        const MdkrNetInputCell *origin = &next.history.cells[
            next.history.first_tick % MDKR_NET_INPUT_CAPACITY];
        if (!origin->occupied || origin->tick != next.history.first_tick) {
            next.history.first_tick = tick;
        }
    }
    mdkr_net_input_set_current_tick(&next.history, tick);
    /* The first authored takeover boundary finalizes repeat-last input from
     * the disconnect grace interval. Thereafter this transport authors one
     * neutral, received sample per tick. The game consumes the independent AI
     * control mask, so this pad is only the canonical history placeholder. */
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        const uint8_t bit = (uint8_t)(1u << slot);
        if (!takeover_active(&next, slot, tick)) continue;
        if ((next.ai_takeover_started_mask & bit) == 0u) {
            next.ai_takeover_started_mask |= bit;
            next.stats.takeover_started++;
            if ((next.remote_slot_mask & bit) != 0u) {
                next.remote_confirmed_through[slot] =
                    next.ai_takeover_tick[slot] - 1u;
                next.remote_have_confirmed[slot] = true;
            }
        }
    }
    /* Publish owned seats before asking history to fill gaps. Otherwise the
     * generic repeat-last predictor would manufacture a local prediction and
     * the same drain would immediately look like a late correction. */
    for (local = 0u; local < local_sample_count; local++) {
        const unsigned canonical = next_bridge.roster.local_to_canonical[local];
        if (takeover_active(&next, canonical, tick)) {
            effective_local[local] = (MdkrPadSample){0u, 0, 0, 1u};
        }
        MdkrInputSample input = net_sample(&effective_local[local]);
        MdkrNetInputSubmitResult result = mdkr_net_input_submit(
            &next.history, canonical, tick, &input);
        if (result != MDKR_NET_SUBMIT_ACCEPTED &&
            result != MDKR_NET_SUBMIT_CORRECTED &&
            result != MDKR_NET_SUBMIT_DUPLICATE) {
            transport->stats.drain_rejected++;
            return false;
        }
    }
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        const uint8_t bit = (uint8_t)(1u << slot);
        MdkrInputSample input;
        MdkrNetInputSubmitResult result;
        if ((next.remote_slot_mask & bit) == 0u ||
            !takeover_active(&next, slot, tick)) continue;
        input = net_sample(&(MdkrPadSample){0u, 0, 0, 1u});
        result = mdkr_net_input_submit(&next.history, slot, tick, &input);
        if (result != MDKR_NET_SUBMIT_ACCEPTED &&
            result != MDKR_NET_SUBMIT_CORRECTED &&
            result != MDKR_NET_SUBMIT_DUPLICATE) {
            transport->stats.drain_rejected++;
            return false;
        }
        advance_remote_confirmation(&next, slot);
    }
    if (!mdkr_net_input_for_tick(&next.history, tick, &predicted)) {
        transport->stats.drain_rejected++;
        return false;
    }
    memset(&base, 0, sizeof(base));
    for (slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
        const uint8_t bit = (uint8_t)(1u << slot);
        if ((next.active_slot_mask & bit) == 0u ||
            (next.local_slot_mask & bit) != 0u) {
            continue;
        }
        base.slots[slot].buttons = predicted.samples[slot].buttons;
        base.slots[slot].stick_x = predicted.samples[slot].stick_x;
        base.slots[slot].stick_y = predicted.samples[slot].stick_y;
        base.slots[slot].present = predicted.samples[slot].present ? 1u : 0u;
        if (base.slots[slot].present) base.present_mask |= bit;
        if (predicted.status[slot] == MDKR_NET_INPUT_RECEIVED) {
            base.confirmed_mask |= bit;
        }
    }
    if (!mdkr_session_bridge_submit_local_inputs(
            &next_bridge, tick, effective_local, local_sample_count, &base)) {
        transport->stats.drain_rejected++;
        return false;
    }
    accepted = mdkr_session_bridge_inputs(&next_bridge, &accepted_tick);
    if (accepted == NULL || accepted_tick != tick) {
        transport->stats.drain_rejected++;
        return false;
    }
    next.stats.drained++;
    detect_unrecoverable_gaps(&next);
    next.bridge = transport->bridge;
    *transport->bridge = next_bridge;
    *transport = next;
    return true;
}

bool mdkr_match_transport_take_dirty(
    MdkrMatchTransport *transport, uint32_t *tick) {
    uint32_t first;
    if (transport == NULL || !transport->ready || tick == NULL ||
        !transport->history.have_dirty) return false;
    first = transport->history.earliest_dirty_tick;
    if (!mdkr_net_input_rebuild_predictions(
            &transport->history, first,
            transport->history.current_tick)) return false;
    return mdkr_net_input_take_dirty(&transport->history, tick);
}

MdkrMatchTakeoverResult mdkr_match_transport_schedule_ai_takeover(
    MdkrMatchTransport *transport, uint32_t match_epoch, unsigned slot,
    uint32_t activation_tick) {
    uint8_t bit;
    if (transport == NULL || !transport->ready || transport->bridge == NULL ||
        slot >= MDKR_NET_INPUT_SLOTS ||
        (transport->bridge->engine_phase != MDKR_ENGINE_READY &&
         transport->bridge->engine_phase != MDKR_ENGINE_RACING)) {
        return MDKR_MATCH_TAKEOVER_INVALID;
    }
    if (match_epoch != transport->match_epoch ||
        mdkr_session_bridge_manifest(transport->bridge) == NULL ||
        mdkr_session_bridge_manifest(transport->bridge)->match_epoch !=
            transport->match_epoch) {
        return MDKR_MATCH_TAKEOVER_STALE_EPOCH;
    }
    bit = (uint8_t)(1u << slot);
    if ((transport->active_slot_mask & bit) == 0u) {
        return MDKR_MATCH_TAKEOVER_INVALID;
    }
    if ((transport->ai_takeover_scheduled_mask & bit) != 0u) {
        return transport->ai_takeover_tick[slot] == activation_tick
            ? MDKR_MATCH_TAKEOVER_DUPLICATE
            : MDKR_MATCH_TAKEOVER_CONFLICT;
    }
    if (!mdkr_net_tick_after(activation_tick,
                             transport->history.current_tick)) {
        return MDKR_MATCH_TAKEOVER_TOO_LATE;
    }
    for (unsigned index = 0u; index < MDKR_NET_INPUT_CAPACITY; index++) {
        const MdkrNetInputCell *cell = &transport->history.cells[index];
        if (cell->occupied &&
            (cell->tick == activation_tick ||
             mdkr_net_tick_after(cell->tick, activation_tick)) &&
            cell->status[slot] == MDKR_NET_INPUT_RECEIVED) {
            return MDKR_MATCH_TAKEOVER_CONFLICT;
        }
    }
    transport->ai_takeover_tick[slot] = activation_tick;
    transport->ai_takeover_scheduled_mask |= bit;
    return MDKR_MATCH_TAKEOVER_ACCEPTED;
}

bool mdkr_match_transport_ai_takeover_mask_for_tick(
    const MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    uint8_t *slot_mask) {
    uint8_t mask = 0u;
    unsigned slot;
    if (transport == NULL || !transport->ready || slot_mask == NULL ||
        match_epoch != transport->match_epoch) return false;
    for (slot = 0u; slot < MDKR_NET_INPUT_SLOTS; slot++) {
        if (takeover_active(transport, slot, tick)) {
            mask |= (uint8_t)(1u << slot);
        }
    }
    *slot_mask = mask;
    return true;
}

bool mdkr_match_transport_inputs_for_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    MdkrInputSet *out) {
    MdkrNetInputSet set;
    unsigned slot;
    if (transport == NULL || !transport->ready || out == NULL ||
        match_epoch != transport->match_epoch ||
        !mdkr_net_input_copy_existing(&transport->history, tick, &set)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (slot = 0u; slot < MDKR_SESSION_MAX_PLAYERS; slot++) {
        const uint8_t bit = (uint8_t)(1u << slot);
        if ((transport->active_slot_mask & bit) == 0u) continue;
        out->slots[slot].buttons = set.samples[slot].buttons;
        out->slots[slot].stick_x = set.samples[slot].stick_x;
        out->slots[slot].stick_y = set.samples[slot].stick_y;
        out->slots[slot].present = set.samples[slot].present ? 1u : 0u;
        if (out->slots[slot].present) out->present_mask |= bit;
        if (set.status[slot] == MDKR_NET_INPUT_RECEIVED) {
            out->confirmed_mask |= bit;
        }
    }
    return true;
}

const MdkrMatchTransportStats *mdkr_match_transport_stats(
    const MdkrMatchTransport *transport) {
    return transport != NULL && transport->ready ? &transport->stats : NULL;
}

bool mdkr_match_transport_recovery(
    const MdkrMatchTransport *transport, MdkrMatchRecovery *recovery) {
    if (transport == NULL || !transport->ready || recovery == NULL ||
        transport->recovery.reason == MDKR_MATCH_RECOVERY_NONE) return false;
    *recovery = transport->recovery;
    return true;
}
