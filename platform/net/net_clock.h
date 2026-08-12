/* Deterministic host-clock/opportunity schedules for rollback tests. */
#ifndef MDKR_NET_CLOCK_H
#define MDKR_NET_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "net_impairment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrNetClockProfile {
    int64_t initial_offset_us;
    int32_t drift_ppm;
    uint32_t nominal_step_us;
    uint32_t long_frame_every;
    uint32_t long_frame_extra_us;
    uint32_t skip_every;
    uint32_t sleep_start_opportunity;
    uint32_t sleep_opportunities;
} MdkrNetClockProfile;

typedef struct MdkrNetClockStep {
    int64_t monotonic_us;
    uint32_t host_opportunity;
    bool tick_offered;
    bool sleeping;
    bool woke;
    bool long_frame;
} MdkrNetClockStep;

typedef struct MdkrNetClock {
    MdkrNetClockProfile profile;
    int64_t monotonic_us;
    int64_t drift_remainder;
    uint32_t host_opportunities;
    uint32_t authored_offers;
    uint32_t skipped_offers;
    uint32_t long_frames;
    uint32_t sleep_skips;
} MdkrNetClock;

bool mdkr_net_clock_named_profile(
    MdkrNetImpairmentProfileName name, uint16_t cadence_hz,
    unsigned endpoint, MdkrNetClockProfile *output);
bool mdkr_net_clock_init(
    MdkrNetClock *clock, const MdkrNetClockProfile *profile);
MdkrNetClockStep mdkr_net_clock_step(MdkrNetClock *clock);

#ifdef __cplusplus
}
#endif
#endif
