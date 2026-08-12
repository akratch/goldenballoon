#ifndef MDKR64_MAGIC_CODES_STATE_FILE_H
#define MDKR64_MAGIC_CODES_STATE_FILE_H

#include "magic_codes_state.h"

/* Global sidecar beside eeprom.bin; it intentionally has no save-slot data. */
const MagicCodesStateStorage *magic_codes_state_file_storage(void);

#endif /* MDKR64_MAGIC_CODES_STATE_FILE_H */
