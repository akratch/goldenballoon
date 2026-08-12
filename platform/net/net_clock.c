#include "net_clock.h"

#include <limits.h>
#include <string.h>

bool mdkr_net_clock_named_profile(
    MdkrNetImpairmentProfileName name, uint16_t cadence_hz,
    unsigned endpoint, MdkrNetClockProfile *output) {
    static const int32_t variable_drift[4] = {-150, 75, 200, -50};
    MdkrNetClockProfile value;
    if (output == NULL || name >= MDKR_NET_PROFILE_COUNT || endpoint >= 4u ||
        (cadence_hz != 25u && cadence_hz != 30u)) return false;
    memset(&value, 0, sizeof(value));
    value.nominal_step_us = UINT32_C(1000000) / cadence_hz;
    value.initial_offset_us = (int64_t)endpoint * 5000;
    switch (name) {
        case MDKR_NET_PROFILE_LAN:
            value.initial_offset_us = (int64_t)endpoint * 100;
            break;
        case MDKR_NET_PROFILE_REGIONAL_GOOD:
            value.drift_ppm = ((int32_t)endpoint - 1) * 25;
            break;
        case MDKR_NET_PROFILE_REGIONAL_VARIABLE:
            value.drift_ppm = variable_drift[endpoint];
            value.long_frame_every = 97u;
            value.long_frame_extra_us = value.nominal_step_us * 2u;
            value.skip_every = 53u;
            break;
        case MDKR_NET_PROFILE_POOR:
            value.drift_ppm = variable_drift[endpoint] * 4;
            value.long_frame_every = 37u;
            value.long_frame_extra_us = value.nominal_step_us * 5u;
            value.skip_every = 11u;
            break;
        case MDKR_NET_PROFILE_TWO_SECOND_OUTAGE:
            value.drift_ppm = variable_drift[endpoint];
            value.sleep_start_opportunity = cadence_hz;
            value.sleep_opportunities = cadence_hz * 2u;
            break;
        case MDKR_NET_PROFILE_ADVERSARIAL:
            value.drift_ppm = variable_drift[endpoint] * 8;
            value.long_frame_every = 13u;
            value.long_frame_extra_us = value.nominal_step_us * 10u;
            value.skip_every = 5u;
            value.sleep_start_opportunity = cadence_hz;
            value.sleep_opportunities = cadence_hz / 2u;
            break;
        default: return false;
    }
    *output = value;
    return true;
}

bool mdkr_net_clock_init(
    MdkrNetClock *clock, const MdkrNetClockProfile *profile) {
    if (clock == NULL || profile == NULL || profile->nominal_step_us == 0u ||
        profile->drift_ppm < -100000 || profile->drift_ppm > 100000 ||
        (profile->long_frame_every == 0u &&
         profile->long_frame_extra_us != 0u) ||
        (profile->sleep_opportunities != 0u &&
         profile->sleep_start_opportunity >
             UINT32_MAX - profile->sleep_opportunities)) return false;
    memset(clock, 0, sizeof(*clock));
    clock->profile = *profile;
    clock->monotonic_us = profile->initial_offset_us;
    return true;
}

MdkrNetClockStep mdkr_net_clock_step(MdkrNetClock *clock) {
    MdkrNetClockStep step;
    int64_t drift_scaled;
    int64_t advance;
    uint32_t index;
    memset(&step, 0, sizeof(step));
    if (clock == NULL || clock->profile.nominal_step_us == 0u) return step;
    index = clock->host_opportunities;
    advance = clock->profile.nominal_step_us;
    drift_scaled = (int64_t)clock->profile.nominal_step_us *
        clock->profile.drift_ppm + clock->drift_remainder;
    advance += drift_scaled / INT64_C(1000000);
    clock->drift_remainder = drift_scaled % INT64_C(1000000);
    if (clock->profile.long_frame_every != 0u && index != 0u &&
        index % clock->profile.long_frame_every == 0u) {
        advance += clock->profile.long_frame_extra_us;
        step.long_frame = true;
        clock->long_frames++;
    }
    step.sleeping = clock->profile.sleep_opportunities != 0u &&
        index >= clock->profile.sleep_start_opportunity &&
        index < clock->profile.sleep_start_opportunity +
            clock->profile.sleep_opportunities;
    step.woke = clock->profile.sleep_opportunities != 0u &&
        index == clock->profile.sleep_start_opportunity +
            clock->profile.sleep_opportunities;
    if (step.sleeping) {
        clock->sleep_skips++;
    } else if (clock->profile.skip_every != 0u &&
               (index + 1u) % clock->profile.skip_every == 0u) {
        clock->skipped_offers++;
    } else {
        step.tick_offered = true;
        clock->authored_offers++;
    }
    clock->monotonic_us += advance;
    clock->host_opportunities++;
    step.monotonic_us = clock->monotonic_us;
    step.host_opportunity = index;
    return step;
}
