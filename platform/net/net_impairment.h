/* Seeded endpoint-clock packet simulator for deterministic netcode tests. */
#ifndef MDKR_NET_IMPAIRMENT_H
#define MDKR_NET_IMPAIRMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NET_SIM_CAPACITY 2048u
#define MDKR_NET_SIM_MAX_BYTES 64u

typedef struct MdkrNetImpairmentProfile {
    uint16_t latency_ticks;
    uint16_t jitter_ticks;
    uint16_t loss_per_thousand;
    uint16_t duplicate_per_thousand;
    uint16_t reorder_per_thousand;
    uint16_t malformed_per_thousand;
    uint16_t max_deliveries_per_tick;
    uint16_t reorder_extra_ticks;
    uint32_t outage_start_tick;
    uint32_t outage_end_tick;
} MdkrNetImpairmentProfile;

typedef enum MdkrNetImpairmentProfileName {
    MDKR_NET_PROFILE_LAN = 0,
    MDKR_NET_PROFILE_REGIONAL_GOOD,
    MDKR_NET_PROFILE_REGIONAL_VARIABLE,
    MDKR_NET_PROFILE_POOR,
    MDKR_NET_PROFILE_TWO_SECOND_OUTAGE,
    MDKR_NET_PROFILE_ADVERSARIAL,
    MDKR_NET_PROFILE_COUNT
} MdkrNetImpairmentProfileName;

typedef struct MdkrNetSimPacket {
    uint8_t bytes[MDKR_NET_SIM_MAX_BYTES];
    uint32_t delivery_tick;
    uint32_t sequence;
    uint8_t length;
    uint8_t source;
    uint8_t destination;
    bool occupied;
} MdkrNetSimPacket;

typedef struct MdkrNetImpairment {
    MdkrNetSimPacket packets[MDKR_NET_SIM_CAPACITY];
    MdkrNetImpairmentProfile profile;
    uint64_t rng;
    uint32_t next_sequence;
    uint64_t sent;
    uint64_t dropped;
    uint64_t duplicated;
    uint64_t reordered;
    uint64_t corrupted;
    uint64_t outage_dropped;
    uint64_t throttled;
    uint64_t overflow;
    uint32_t receive_tick[4];
    uint16_t deliveries_this_tick[4];
} MdkrNetImpairment;

bool mdkr_net_impairment_named_profile(
    MdkrNetImpairmentProfileName name, uint16_t cadence_hz,
    MdkrNetImpairmentProfile *output);

void mdkr_net_impairment_init(
    MdkrNetImpairment *simulator, uint64_t seed,
    MdkrNetImpairmentProfile profile);
bool mdkr_net_impairment_send(
    MdkrNetImpairment *simulator, uint32_t sender_tick,
    unsigned source, unsigned destination, const void *bytes, size_t length);
bool mdkr_net_impairment_receive(
    MdkrNetImpairment *simulator, uint32_t receiver_tick,
    unsigned destination, MdkrNetSimPacket *output);

#ifdef __cplusplus
}
#endif
#endif
