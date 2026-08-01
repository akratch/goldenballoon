#ifndef MDKR_AUDI_PORT_DKR_H
#define MDKR_AUDI_PORT_DKR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dkr_audio_out_init(void);
void dkr_audio_out_shutdown(void);

/* Host clock input and ordered post-game-tick service are deliberately split.
 * A presentation may advance host time, but it cannot synthesize PCM merely
 * because it was presented. */
void dkr_audio_advance_fields(unsigned fields, bool rebase);
void dkr_audio_advance_units(uint64_t units, bool rebase);
void dkr_audio_service_tick(void);
void dkr_audio_service_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDI_PORT_DKR_H */
