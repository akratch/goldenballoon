/* Exact, bounded host-time clock for PCM synthesis opportunities. */
#ifndef MDKR_AUDIO_SERVICE_CLOCK_H
#define MDKR_AUDIO_SERVICE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrAudioServiceClockStats {
    uint64_t advances;
    uint64_t elapsed_units;
    uint64_t elapsed_fields;
    uint64_t due_quanta;
    uint64_t serviced_quanta;
    uint64_t retired_quanta;
    uint64_t rebases;
    uint64_t max_pending_quanta;
} MdkrAudioServiceClockStats;

typedef struct MdkrAudioServiceClock {
    unsigned fields_per_quantum;
    uint64_t residual_units;
    uint64_t pending_quanta;
    MdkrAudioServiceClockStats stats;
} MdkrAudioServiceClock;

void mdkr_audio_service_clock_init(
    MdkrAudioServiceClock *clock, unsigned fields_per_quantum);

/* Add elapsed source fields. A suspension rebase retires old residue/debt
 * before the supplied fields begin a fresh audio interval. */
unsigned mdkr_audio_service_clock_advance(
    MdkrAudioServiceClock *clock, unsigned fields, bool rebase);

/* One source field is exactly 1e9 units, matching SimSched. Sub-field present
 * opportunities feed this form so 120/144/uncapped schedules do not round
 * audio time up to a whole VI field per image. */
unsigned mdkr_audio_service_clock_advance_units(
    MdkrAudioServiceClock *clock, uint64_t units, bool rebase);

/* Consume one due PCM quantum. Presentation opportunities call no equivalent:
 * without elapsed audio time there is deliberately no minimum block. */
bool mdkr_audio_service_clock_take(MdkrAudioServiceClock *clock);

uint64_t mdkr_audio_service_clock_pending(
    const MdkrAudioServiceClock *clock);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDIO_SERVICE_CLOCK_H */
