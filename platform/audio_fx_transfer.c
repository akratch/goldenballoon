#include "audio_fx_transfer.h"

typedef char
    PortAudioFxUnsignedIntMustBe32Bits[sizeof(unsigned int) == 4 ? 1 : -1];
typedef char PortAudioFxTailSlackMustCoverDma
    [PORT_AUDIO_FX_TAIL_SLACK_SAMPLES * sizeof(short) >= 8 ? 1 : -1];

static unsigned int s_guard_trips;

static unsigned long long portAudioFxRoundSamplesToDma(unsigned int samples) {
    return ((unsigned long long)samples + 3ULL) & ~3ULL;
}

static unsigned int portAudioFxClampSpan(unsigned int offset,
                                         unsigned int samples,
                                         unsigned int allocated_samples,
                                         int *clamped) {
    unsigned int available;

    if (samples == 0) {
        return 0;
    }
    if (offset >= allocated_samples) {
        *clamped = 1;
        return 0;
    }

    available = allocated_samples - offset;
    if (portAudioFxRoundSamplesToDma(samples) <= available) {
        return samples;
    }

    *clamped = 1;
    samples = available & ~3u;
    return samples;
}

int portAudioFxPlanTransfer(unsigned int logical_length,
                            unsigned int allocated_samples,
                            unsigned int current_offset,
                            unsigned int requested_samples,
                            struct PortAudioFxTransferPlan *out) {
    unsigned int remaining;

    if (out == 0) {
        return 0;
    }
    out->first_offset = 0;
    out->first_samples = 0;
    out->second_offset = 0;
    out->second_samples = 0;
    out->planned_samples = 0;
    out->clamped = 0;

    if (logical_length == 0 || allocated_samples < logical_length ||
        current_offset > logical_length || requested_samples == 0) {
        return 0;
    }

    if (current_offset == logical_length) {
        current_offset = 0;
    }

    remaining = requested_samples;
    if (remaining > logical_length) {
        remaining = logical_length;
        out->clamped = 1;
    }

    out->first_offset = current_offset;
    out->first_samples = logical_length - current_offset;
    if (out->first_samples > remaining) {
        out->first_samples = remaining;
    }
    remaining -= out->first_samples;
    out->second_offset = 0;
    out->second_samples = remaining;

    out->first_samples =
        portAudioFxClampSpan(out->first_offset, out->first_samples,
                             allocated_samples, &out->clamped);
    out->second_samples =
        portAudioFxClampSpan(out->second_offset, out->second_samples,
                             allocated_samples, &out->clamped);
    out->planned_samples = out->first_samples + out->second_samples;
    return out->planned_samples != 0;
}

void portAudioFxRecordGuardTrip(void) {
    if (s_guard_trips != ~0u) {
        s_guard_trips++;
    }
}

unsigned int portAudioFxGuardTripCount(void) { return s_guard_trips; }
