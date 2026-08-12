#include "session_bridge.h"

#include "net/net_local_input.h"

#include <string.h>

static bool launch_valid(const MdkrSessionLaunchV2 *launch) {
    const uint8_t allowed_mask = (uint8_t)((1u << MDKR_SESSION_MAX_PLAYERS) - 1u);
    unsigned index;
    if (launch == NULL || launch->version != MDKR_SESSION_LAUNCH_VERSION ||
        launch->size != sizeof(*launch) ||
        !mdkr_match_manifest_validate(&launch->match) ||
        launch->match.protocol_version != MDKR_SESSION_PROTOCOL_VERSION ||
        launch->match.slot_count > MDKR_SESSION_MAX_PLAYERS ||
        (launch->local_slot_mask & (uint8_t)~allowed_mask) != 0u ||
        (launch->viewport_slot_mask & (uint8_t)~allowed_mask) != 0u ||
        (launch->viewport_slot_mask &
         (uint8_t)~launch->local_slot_mask) != 0u ||
        launch->local_slot_mask == 0u) {
        return false;
    }
    if ((launch->local_slot_mask &
         (uint8_t)~((1u << launch->match.slot_count) - 1u)) != 0u) {
        return false;
    }
    if ((launch->viewport_slot_mask &
         (uint8_t)~((1u << launch->match.slot_count) - 1u)) != 0u) {
        return false;
    }
    for (index = 0u; index < sizeof(launch->reserved); index++) {
        if (launch->reserved[index] != 0u) return false;
    }
    return true;
}

static bool phase_transition_allowed(MdkrEnginePhase from,
                                     MdkrEnginePhase to) {
    if (to == MDKR_ENGINE_FAILED) return from != MDKR_ENGINE_STOPPED;
    switch (from) {
        case MDKR_ENGINE_STOPPED: return to == MDKR_ENGINE_BOOTING;
        case MDKR_ENGINE_BOOTING: return to == MDKR_ENGINE_READY;
        case MDKR_ENGINE_READY: return to == MDKR_ENGINE_RACING;
        case MDKR_ENGINE_RACING: return to == MDKR_ENGINE_FINISHED;
        case MDKR_ENGINE_FINISHED:
        case MDKR_ENGINE_FAILED:
            return to == MDKR_ENGINE_STOPPED;
        default: return false;
    }
}

static bool push_event(MdkrSessionBridge *bridge, MdkrSessionEventType type,
                       uint32_t tick, uint32_t value) {
    MdkrSessionEvent *event;
    unsigned tail;
    /* Input acceptance is a level-triggered telemetry witness, not a command
     * log. While the blocking engine owns the host window the launcher cannot
     * poll every fixed tick, so retain the newest consecutive sample instead
     * of turning a healthy race into queue backpressure. Lifecycle/result/
     * rejection events remain lossless and ordered. */
    if (type == MDKR_SESSION_EVENT_INPUT_ACCEPTED &&
        bridge->event_count != 0u) {
        const unsigned latest =
            ((unsigned)bridge->event_head + bridge->event_count - 1u) %
            MDKR_SESSION_EVENT_CAPACITY;
        event = &bridge->events[latest];
        if (event->type == MDKR_SESSION_EVENT_INPUT_ACCEPTED) {
            event->tick = tick;
            event->value = value;
            return true;
        }
    }
    if (bridge->event_count >= MDKR_SESSION_EVENT_CAPACITY) return false;
    tail = ((unsigned)bridge->event_head + bridge->event_count) %
           MDKR_SESSION_EVENT_CAPACITY;
    event = &bridge->events[tail];
    event->protocol_version = MDKR_SESSION_PROTOCOL_VERSION;
    event->type = type;
    event->match_epoch = bridge->have_manifest
        ? bridge->manifest.match_epoch : 0u;
    event->tick = tick;
    event->value = value;
    bridge->event_count++;
    return true;
}

void mdkr_session_bridge_init(MdkrSessionBridge *bridge) {
    if (bridge == NULL) return;
    memset(bridge, 0, sizeof(*bridge));
    bridge->engine_phase = MDKR_ENGINE_STOPPED;
}

bool mdkr_session_bridge_apply_launch(
    MdkrSessionBridge *bridge, const MdkrSessionLaunchV2 *launch) {
    MdkrNetRoster roster;
    uint8_t local_slots[MDKR_MATCH_SLOTS];
    uint8_t viewport_slots[MDKR_MATCH_SLOTS];
    unsigned local_count = 0u;
    unsigned viewport_count = 0u;
    unsigned slot;
    if (bridge == NULL || !launch_valid(launch) ||
        (bridge->have_manifest && bridge->engine_phase != MDKR_ENGINE_STOPPED)) {
        return false;
    }
    if (!mdkr_net_roster_init(&roster, &launch->match)) return false;
    for (slot = 0u; slot < launch->match.slot_count; slot++) {
        if ((launch->local_slot_mask & (uint8_t)(1u << slot)) != 0u) {
            local_slots[local_count++] = (uint8_t)slot;
        }
        if ((launch->viewport_slot_mask & (uint8_t)(1u << slot)) != 0u) {
            viewport_slots[viewport_count++] = (uint8_t)slot;
        }
    }
    if (!mdkr_net_roster_configure_local(
            &roster, local_slots, local_count) ||
        !mdkr_net_roster_set_viewports(
            &roster, viewport_slots, viewport_count)) {
        return false;
    }
    bridge->event_head = 0u;
    bridge->event_count = 0u;
    bridge->manifest = launch->match;
    memset(&bridge->launch_descriptor, 0, sizeof(bridge->launch_descriptor));
    bridge->roster = roster;
    bridge->local_slot_mask = launch->local_slot_mask;
    bridge->viewport_slot_mask = launch->viewport_slot_mask;
    memset(&bridge->latest_inputs, 0, sizeof(bridge->latest_inputs));
    memset(&bridge->result, 0, sizeof(bridge->result));
    bridge->latest_tick = 0u;
    bridge->have_manifest = true;
    bridge->have_inputs = false;
    bridge->have_result = false;
    bridge->have_launch_descriptor = false;
    return push_event(bridge, MDKR_SESSION_EVENT_MANIFEST_APPLIED, 0u, 0u);
}

bool mdkr_session_bridge_apply_launch_v3(
    MdkrSessionBridge *bridge, const MdkrSessionLaunchV3 *launch) {
    MdkrSessionLaunchV2 legacy;
    unsigned index;
    if (bridge == NULL || launch == NULL ||
        launch->version != MDKR_SESSION_LAUNCH_V3_VERSION ||
        launch->size != sizeof(*launch) ||
        !mdkr_match_launch_descriptor_validate(&launch->match)) return false;
    for (index = 0u; index < sizeof(launch->reserved); index++) {
        if (launch->reserved[index] != 0u) return false;
    }
    memset(&legacy, 0, sizeof(legacy));
    legacy.version = MDKR_SESSION_LAUNCH_VERSION;
    legacy.size = sizeof(legacy);
    legacy.match = launch->match.manifest;
    legacy.local_slot_mask = launch->local_slot_mask;
    legacy.viewport_slot_mask = launch->viewport_slot_mask;
    if (!mdkr_session_bridge_apply_launch(bridge, &legacy)) return false;
    bridge->launch_descriptor = launch->match;
    bridge->have_launch_descriptor = true;
    return true;
}

static bool input_set_valid(const MdkrSessionBridge *bridge,
                            const MdkrInputSet *inputs) {
    uint8_t active_mask;
    if (inputs == NULL || !bridge->have_manifest) return false;
    active_mask = (uint8_t)((1u << bridge->manifest.slot_count) - 1u);
    if ((inputs->present_mask & (uint8_t)~active_mask) != 0u ||
        (inputs->confirmed_mask & (uint8_t)~active_mask) != 0u ||
        inputs->reserved[0] != 0u || inputs->reserved[1] != 0u) {
        return false;
    }
    for (unsigned i = 0; i < MDKR_SESSION_MAX_PLAYERS; ++i) {
        const MdkrPadSample *sample = &inputs->slots[i];
        const bool active = (active_mask & (uint8_t)(1u << i)) != 0u;
        if (sample->present > 1u || sample->stick_x < -80 ||
            sample->stick_x > 80 || sample->stick_y < -80 ||
            sample->stick_y > 80) {
            return false;
        }
        if (active && sample->present !=
            ((inputs->present_mask & (uint8_t)(1u << i)) != 0u)) {
            return false;
        }
        if ((!active || sample->present == 0u) &&
            (sample->buttons != 0u || sample->stick_x != 0 ||
             sample->stick_y != 0)) {
            return false;
        }
    }
    return true;
}

bool mdkr_session_bridge_submit_inputs(
    MdkrSessionBridge *bridge, uint32_t tick, const MdkrInputSet *inputs) {
    if (bridge == NULL || !input_set_valid(bridge, inputs) ||
        (bridge->have_inputs && tick <= bridge->latest_tick) ||
        (bridge->engine_phase != MDKR_ENGINE_READY &&
         bridge->engine_phase != MDKR_ENGINE_RACING)) {
        return false;
    }
    bridge->latest_inputs = *inputs;
    bridge->latest_tick = tick;
    bridge->have_inputs = true;
    return push_event(bridge, MDKR_SESSION_EVENT_INPUT_ACCEPTED, tick,
                      inputs->confirmed_mask);
}

bool mdkr_session_bridge_submit_local_inputs(
    MdkrSessionBridge *bridge, uint32_t tick,
    const MdkrPadSample *local_samples, unsigned local_sample_count,
    const MdkrInputSet *transport_base) {
    MdkrInputSample canonical_base[MDKR_MATCH_SLOTS];
    MdkrInputSample local[MDKR_MATCH_SLOTS];
    MdkrCanonicalInputFrame merged;
    MdkrInputSet next;
    unsigned index;

    if (bridge == NULL || transport_base == NULL ||
        !input_set_valid(bridge, transport_base) ||
        local_sample_count > MDKR_MATCH_SLOTS ||
        local_sample_count != bridge->roster.local_seat_count ||
        (local_sample_count != 0u && local_samples == NULL)) {
        return false;
    }
    memset(canonical_base, 0, sizeof(canonical_base));
    memset(local, 0, sizeof(local));
    for (index = 0u; index < MDKR_MATCH_SLOTS; index++) {
        canonical_base[index].buttons = transport_base->slots[index].buttons;
        canonical_base[index].stick_x = transport_base->slots[index].stick_x;
        canonical_base[index].stick_y = transport_base->slots[index].stick_y;
        canonical_base[index].present = transport_base->slots[index].present != 0u;
    }
    for (index = 0u; index < local_sample_count; index++) {
        local[index].buttons = local_samples[index].buttons;
        local[index].stick_x = local_samples[index].stick_x;
        local[index].stick_y = local_samples[index].stick_y;
        local[index].present = local_samples[index].present != 0u;
    }
    if (!mdkr_net_local_input_merge(
            &bridge->roster, local, local_sample_count,
            canonical_base, &merged)) {
        return false;
    }

    next = *transport_base;
    next.present_mask = 0u;
    next.confirmed_mask = (uint8_t)(
        transport_base->confirmed_mask | merged.locally_authored_mask);
    for (index = 0u; index < MDKR_MATCH_SLOTS; index++) {
        next.slots[index].buttons = merged.slots[index].buttons;
        next.slots[index].stick_x = merged.slots[index].stick_x;
        next.slots[index].stick_y = merged.slots[index].stick_y;
        next.slots[index].present = merged.slots[index].present ? 1u : 0u;
        if (merged.slots[index].present) {
            next.present_mask |= (uint8_t)(1u << index);
        }
    }
    return mdkr_session_bridge_submit_inputs(bridge, tick, &next);
}

bool mdkr_session_bridge_poll_event(
    MdkrSessionBridge *bridge, MdkrSessionEvent *event) {
    if (bridge == NULL || event == NULL || bridge->event_count == 0u) {
        return false;
    }
    *event = bridge->events[bridge->event_head];
    bridge->event_head = (uint8_t)(((unsigned)bridge->event_head + 1u) %
                                   MDKR_SESSION_EVENT_CAPACITY);
    bridge->event_count--;
    return true;
}

bool mdkr_session_bridge_set_engine_phase(
    MdkrSessionBridge *bridge, MdkrEnginePhase phase) {
    if (bridge == NULL || !bridge->have_manifest ||
        bridge->event_count >= MDKR_SESSION_EVENT_CAPACITY ||
        phase > MDKR_ENGINE_FAILED ||
        !phase_transition_allowed(bridge->engine_phase, phase)) {
        return false;
    }
    bridge->engine_phase = phase;
    if (phase == MDKR_ENGINE_STOPPED) {
        bridge->have_inputs = false;
    }
    return push_event(bridge, MDKR_SESSION_EVENT_ENGINE_PHASE,
                      bridge->latest_tick, (uint32_t)phase);
}

bool mdkr_session_bridge_publish_result(
    MdkrSessionBridge *bridge, const MdkrMatchResultV1 *result) {
    bool seen[MDKR_SESSION_MAX_PLAYERS] = { false, false, false, false };
    if (bridge == NULL || result == NULL || !bridge->have_manifest ||
        bridge->event_count >= MDKR_SESSION_EVENT_CAPACITY ||
        bridge->engine_phase != MDKR_ENGINE_FINISHED ||
        result->version != MDKR_MATCH_RESULT_VERSION ||
        result->size != sizeof(*result) ||
        result->match_epoch != bridge->manifest.match_epoch ||
        result->player_count != bridge->manifest.slot_count ||
        result->synchronized > 1u || result->reserved[0] != 0u ||
        result->reserved[1] != 0u) {
        return false;
    }
    for (unsigned i = 0; i < result->player_count; ++i) {
        if (result->finishing_order[i] >= result->player_count ||
            seen[result->finishing_order[i]]) {
            return false;
        }
        seen[result->finishing_order[i]] = true;
    }
    bridge->result = *result;
    bridge->have_result = true;
    return push_event(bridge, MDKR_SESSION_EVENT_RESULT_READY,
                      result->terminal_tick, result->synchronized);
}

bool mdkr_session_bridge_export_result(
    const MdkrSessionBridge *bridge, MdkrMatchResultV1 *result) {
    if (bridge == NULL || result == NULL || !bridge->have_result) return false;
    *result = bridge->result;
    return true;
}

const MdkrMatchManifestV1 *mdkr_session_bridge_manifest(
    const MdkrSessionBridge *bridge) {
    return bridge != NULL && bridge->have_manifest ? &bridge->manifest : NULL;
}

const MdkrMatchLaunchDescriptorV1 *mdkr_session_bridge_launch_descriptor(
    const MdkrSessionBridge *bridge) {
    return bridge != NULL && bridge->have_launch_descriptor
        ? &bridge->launch_descriptor : NULL;
}

uint8_t mdkr_session_bridge_local_slot_mask(
    const MdkrSessionBridge *bridge) {
    return bridge != NULL && bridge->have_manifest
        ? bridge->local_slot_mask : 0u;
}

uint8_t mdkr_session_bridge_viewport_slot_mask(
    const MdkrSessionBridge *bridge) {
    return bridge != NULL && bridge->have_manifest
        ? bridge->viewport_slot_mask : 0u;
}

const MdkrNetRoster *mdkr_session_bridge_roster(
    const MdkrSessionBridge *bridge) {
    return bridge != NULL && bridge->have_manifest ? &bridge->roster : NULL;
}

const MdkrInputSet *mdkr_session_bridge_inputs(
    const MdkrSessionBridge *bridge, uint32_t *tick) {
    if (bridge == NULL || !bridge->have_inputs) return NULL;
    if (tick != NULL) *tick = bridge->latest_tick;
    return &bridge->latest_inputs;
}
