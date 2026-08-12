#include "net_impairment.h"

#include <string.h>

static uint32_t random_u32(MdkrNetImpairment *simulator) {
    uint64_t value = simulator->rng;
    value ^= value >> 12; value ^= value << 25; value ^= value >> 27;
    simulator->rng = value;
    return (uint32_t)((value * UINT64_C(2685821657736338717)) >> 32);
}

static bool schedule(MdkrNetImpairment *simulator, uint32_t sender_tick,
                     unsigned source, unsigned destination,
                     const void *bytes, size_t length, uint32_t extra_delay,
                     bool corrupt) {
    unsigned index;
    uint32_t delay = simulator->profile.latency_ticks;
    if (simulator->profile.jitter_ticks != 0u) {
        const uint32_t span = simulator->profile.jitter_ticks * 2u + 1u;
        const int32_t offset = (int32_t)(random_u32(simulator) % span) -
            (int32_t)simulator->profile.jitter_ticks;
        delay = offset < 0 && (uint32_t)(-offset) > delay
            ? 0u : (uint32_t)((int32_t)delay + offset);
    }
    for (index = 0u; index < MDKR_NET_SIM_CAPACITY; index++) {
        MdkrNetSimPacket *packet = &simulator->packets[index];
        if (packet->occupied) continue;
        memset(packet, 0, sizeof(*packet));
        memcpy(packet->bytes, bytes, length);
        if (corrupt) packet->bytes[length - 1u] ^= UINT8_C(0x80);
        packet->delivery_tick = sender_tick + delay + extra_delay;
        packet->sequence = simulator->next_sequence++;
        packet->length = (uint8_t)length;
        packet->source = (uint8_t)source;
        packet->destination = (uint8_t)destination;
        packet->occupied = true;
        return true;
    }
    simulator->overflow++;
    return false;
}

bool mdkr_net_impairment_named_profile(
    MdkrNetImpairmentProfileName name, uint16_t cadence_hz,
    MdkrNetImpairmentProfile *output) {
    MdkrNetImpairmentProfile value;
    if (output == NULL || name >= MDKR_NET_PROFILE_COUNT ||
        (cadence_hz != 25u && cadence_hz != 30u)) return false;
    memset(&value, 0, sizeof(value));
    switch (name) {
        case MDKR_NET_PROFILE_LAN:
            value.latency_ticks = 1u;
            value.max_deliveries_per_tick = 32u;
            break;
        case MDKR_NET_PROFILE_REGIONAL_GOOD:
            value.latency_ticks = 2u;
            value.jitter_ticks = 1u;
            value.duplicate_per_thousand = 5u;
            value.reorder_per_thousand = 5u;
            value.reorder_extra_ticks = 2u;
            value.max_deliveries_per_tick = 16u;
            break;
        case MDKR_NET_PROFILE_REGIONAL_VARIABLE:
            value.latency_ticks = 3u;
            value.jitter_ticks = 3u;
            value.loss_per_thousand = 5u;
            value.duplicate_per_thousand = 10u;
            value.reorder_per_thousand = 75u;
            value.reorder_extra_ticks = 4u;
            value.max_deliveries_per_tick = 8u;
            break;
        case MDKR_NET_PROFILE_POOR:
            value.latency_ticks = 5u;
            value.jitter_ticks = 5u;
            value.loss_per_thousand = 25u;
            value.duplicate_per_thousand = 20u;
            value.reorder_per_thousand = 150u;
            value.reorder_extra_ticks = 8u;
            value.max_deliveries_per_tick = 3u;
            break;
        case MDKR_NET_PROFILE_TWO_SECOND_OUTAGE:
            value.latency_ticks = 2u;
            value.jitter_ticks = 1u;
            value.max_deliveries_per_tick = 16u;
            value.outage_start_tick = cadence_hz;
            value.outage_end_tick = cadence_hz * 3u;
            break;
        case MDKR_NET_PROFILE_ADVERSARIAL:
            value.latency_ticks = 1u;
            value.jitter_ticks = 2u;
            value.loss_per_thousand = 50u;
            value.duplicate_per_thousand = 100u;
            value.reorder_per_thousand = 250u;
            value.malformed_per_thousand = 100u;
            value.reorder_extra_ticks = 10u;
            value.max_deliveries_per_tick = 1u;
            break;
        default: return false;
    }
    *output = value;
    return true;
}

void mdkr_net_impairment_init(
    MdkrNetImpairment *simulator, uint64_t seed,
    MdkrNetImpairmentProfile profile) {
    if (simulator == NULL) return;
    memset(simulator, 0, sizeof(*simulator));
    simulator->profile = profile;
    if (simulator->profile.loss_per_thousand > 1000u)
        simulator->profile.loss_per_thousand = 1000u;
    if (simulator->profile.duplicate_per_thousand > 1000u)
        simulator->profile.duplicate_per_thousand = 1000u;
    if (simulator->profile.reorder_per_thousand > 1000u)
        simulator->profile.reorder_per_thousand = 1000u;
    if (simulator->profile.malformed_per_thousand > 1000u)
        simulator->profile.malformed_per_thousand = 1000u;
    if (simulator->profile.outage_end_tick <=
        simulator->profile.outage_start_tick) {
        simulator->profile.outage_start_tick = 0u;
        simulator->profile.outage_end_tick = 0u;
    }
    for (unsigned destination = 0u; destination < 4u; destination++) {
        simulator->receive_tick[destination] = UINT32_MAX;
    }
    simulator->rng = seed != 0u ? seed : UINT64_C(0x9e3779b97f4a7c15);
}

bool mdkr_net_impairment_send(
    MdkrNetImpairment *simulator, uint32_t sender_tick,
    unsigned source, unsigned destination, const void *bytes, size_t length) {
    if (simulator == NULL || bytes == NULL || length == 0u ||
        length > MDKR_NET_SIM_MAX_BYTES || source >= 4u || destination >= 4u) return false;
    uint32_t reorder_delay = 0u;
    bool corrupt;
    simulator->sent++;
    if (simulator->profile.outage_end_tick != 0u &&
        sender_tick >= simulator->profile.outage_start_tick &&
        sender_tick < simulator->profile.outage_end_tick) {
        simulator->dropped++;
        simulator->outage_dropped++;
        return true;
    }
    if (random_u32(simulator) % 1000u < simulator->profile.loss_per_thousand) {
        simulator->dropped++;
        return true;
    }
    if (random_u32(simulator) % 1000u <
        simulator->profile.reorder_per_thousand) {
        reorder_delay = simulator->profile.reorder_extra_ticks != 0u
            ? simulator->profile.reorder_extra_ticks : 1u;
        simulator->reordered++;
    }
    corrupt = random_u32(simulator) % 1000u <
        simulator->profile.malformed_per_thousand;
    if (corrupt) simulator->corrupted++;
    if (!schedule(simulator, sender_tick, source, destination, bytes, length,
                  reorder_delay, corrupt)) return false;
    if (random_u32(simulator) % 1000u < simulator->profile.duplicate_per_thousand) {
        if (!schedule(simulator, sender_tick + 1u, source, destination,
                      bytes, length, reorder_delay, corrupt)) return false;
        simulator->duplicated++;
    }
    return true;
}

bool mdkr_net_impairment_receive(
    MdkrNetImpairment *simulator, uint32_t receiver_tick,
    unsigned destination, MdkrNetSimPacket *output) {
    unsigned index;
    unsigned selected = MDKR_NET_SIM_CAPACITY;
    if (simulator == NULL || output == NULL || destination >= 4u) return false;
    if (simulator->receive_tick[destination] != receiver_tick) {
        simulator->receive_tick[destination] = receiver_tick;
        simulator->deliveries_this_tick[destination] = 0u;
    }
    for (index = 0u; index < MDKR_NET_SIM_CAPACITY; index++) {
        const MdkrNetSimPacket *packet = &simulator->packets[index];
        if (!packet->occupied || packet->destination != destination ||
            (int32_t)(receiver_tick - packet->delivery_tick) < 0) continue;
        if (selected == MDKR_NET_SIM_CAPACITY ||
            packet->delivery_tick < simulator->packets[selected].delivery_tick ||
            (packet->delivery_tick == simulator->packets[selected].delivery_tick &&
             packet->sequence < simulator->packets[selected].sequence)) selected = index;
    }
    if (selected == MDKR_NET_SIM_CAPACITY) return false;
    if (simulator->profile.max_deliveries_per_tick != 0u &&
        simulator->deliveries_this_tick[destination] >=
            simulator->profile.max_deliveries_per_tick) {
        simulator->throttled++;
        return false;
    }
    *output = simulator->packets[selected];
    simulator->packets[selected].occupied = false;
    simulator->deliveries_this_tick[destination]++;
    return true;
}
