#include "audio_service_clock.h"

#include <limits.h>
#include <string.h>

#define AUDIO_FIELD_UNITS UINT64_C(1000000000)

void mdkr_audio_service_clock_init(
    MdkrAudioServiceClock *clock, unsigned fields_per_quantum) {
    if (clock == NULL) {
        return;
    }
    memset(clock, 0, sizeof(*clock));
    clock->fields_per_quantum = fields_per_quantum != 0u
        ? fields_per_quantum : 1u;
}

unsigned mdkr_audio_service_clock_advance(
    MdkrAudioServiceClock *clock, unsigned fields, bool rebase) {
    uint64_t units;

    if ((uint64_t)fields > UINT64_MAX / AUDIO_FIELD_UNITS) {
        units = UINT64_MAX;
    } else {
        units = (uint64_t)fields * AUDIO_FIELD_UNITS;
    }
    return mdkr_audio_service_clock_advance_units(clock, units, rebase);
}

unsigned mdkr_audio_service_clock_advance_units(
    MdkrAudioServiceClock *clock, uint64_t units, bool rebase) {
    uint64_t unit_total;
    uint64_t quantum_units;
    uint64_t due;

    if (clock == NULL || clock->fields_per_quantum == 0u) {
        return 0u;
    }
    clock->stats.advances++;
    if (rebase) {
        clock->stats.retired_quanta += clock->pending_quanta;
        clock->pending_quanta = 0u;
        clock->residual_units = 0u;
        clock->stats.rebases++;
    }
    if (UINT64_MAX - clock->stats.elapsed_units < units) {
        clock->stats.elapsed_units = UINT64_MAX;
    } else {
        clock->stats.elapsed_units += units;
    }
    clock->stats.elapsed_fields =
        clock->stats.elapsed_units / AUDIO_FIELD_UNITS;
    quantum_units =
        (uint64_t)clock->fields_per_quantum * AUDIO_FIELD_UNITS;
    if (UINT64_MAX - clock->residual_units < units) {
        unit_total = UINT64_MAX;
    } else {
        unit_total = clock->residual_units + units;
    }
    due = unit_total / quantum_units;
    clock->residual_units = unit_total % quantum_units;
    if (UINT64_MAX - clock->pending_quanta < due) {
        clock->pending_quanta = UINT64_MAX;
    } else {
        clock->pending_quanta += due;
    }
    clock->stats.due_quanta += due;
    if (clock->pending_quanta > clock->stats.max_pending_quanta) {
        clock->stats.max_pending_quanta = clock->pending_quanta;
    }
    return due > UINT_MAX ? UINT_MAX : (unsigned)due;
}

bool mdkr_audio_service_clock_take(MdkrAudioServiceClock *clock) {
    if (clock == NULL || clock->pending_quanta == 0u) {
        return false;
    }
    clock->pending_quanta--;
    clock->stats.serviced_quanta++;
    return true;
}

uint64_t mdkr_audio_service_clock_pending(
    const MdkrAudioServiceClock *clock) {
    return clock != NULL ? clock->pending_quanta : 0u;
}
