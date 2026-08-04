#ifndef MDKR64_TAJ_MOD_STATE_FILE_H
#define MDKR64_TAJ_MOD_STATE_FILE_H

#include "taj_mod_state.h"

/* Global sidecar beside eeprom.bin; it intentionally has no save-slot data. */
const TajModStateStorage *taj_mod_state_file_storage(void);

#endif /* MDKR64_TAJ_MOD_STATE_FILE_H */
