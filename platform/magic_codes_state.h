/* Versioned global persistence for the retail Magic Code menu. */
#ifndef MDKR64_MAGIC_CODES_STATE_H
#define MDKR64_MAGIC_CODES_STATE_H

#include "text_state_file.h"

#include <stddef.h>
#include <stdint.h>

enum {
    MAGIC_CODES_STATE_VERSION = 1,
    MAGIC_CODES_STATE_TEXT_CAPACITY = 128
};

/* Progression/debug/one-shot codes are deliberately absent. The sidecar owns
 * only ordinary player-selected modifiers; TT and Drumstick remain save data. */
#define MAGIC_CODES_PERSISTED_MASK UINT32_C(0x03FFFBFC)

typedef struct MagicCodesPersistentState {
    uint32_t version;
    uint32_t unlocked;
    uint32_t active;
} MagicCodesPersistentState;

typedef MdkrTextStateStorage MagicCodesStateStorage;

void magic_codes_state_defaults(MagicCodesPersistentState *state);
int magic_codes_state_is_valid(const MagicCodesPersistentState *state);
int magic_codes_state_parse(MagicCodesPersistentState *state, const char *text,
                            size_t length);
int magic_codes_state_serialize(const MagicCodesPersistentState *state,
                                char *text, size_t capacity, size_t *length);
int magic_codes_state_load(MagicCodesPersistentState *state,
                           const MagicCodesStateStorage *storage);
int magic_codes_state_store(const MagicCodesPersistentState *state,
                            const MagicCodesStateStorage *storage);

/* Process-lifetime coordinator. Desktop writes are durable on return. Web
 * writes are serialized and coalesced until their IDBFS generation settles. */
void magic_codes_persistence_boot(const MagicCodesStateStorage *storage,
                                  uint32_t *unlocked, uint32_t *active);
int magic_codes_persistence_update(uint32_t unlocked, uint32_t active);
int magic_codes_persistence_failed(void);
int magic_codes_persistence_pending(void);
unsigned int magic_codes_persistence_pending_generation(void);
void magic_codes_report_persistence_success(unsigned int generation);
void magic_codes_report_persistence_failure(unsigned int generation);

#ifdef MAGIC_CODES_STATE_TESTING
void magic_codes_persistence_reset_for_test(void);
void magic_codes_persistence_set_async_for_test(int enabled);
#endif

#endif /* MDKR64_MAGIC_CODES_STATE_H */
