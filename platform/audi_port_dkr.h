#ifndef MDKR_AUDI_PORT_DKR_H
#define MDKR_AUDI_PORT_DKR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void dkr_audio_out_init(void);
void dkr_audio_out_shutdown(void);

/* Fixed-ticket clock input and ordered post-game-tick service are deliberately
 * split. Presentation count cannot change the amount of credited audio time. */
void dkr_audio_advance_fields(unsigned fields, bool rebase);
void dkr_audio_service_tick(void);
void dkr_audio_service_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_AUDI_PORT_DKR_H */
