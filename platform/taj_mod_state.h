/* Versioned, platform-neutral persistence contract for the Taj mod sidecar. */
#ifndef MDKR64_TAJ_MOD_STATE_H
#define MDKR64_TAJ_MOD_STATE_H

#include "text_state_file.h"

enum {
    TAJ_MOD_STATE_VERSION = 3,
    TAJ_MOD_STATE_TEXT_CAPACITY = 256
};

typedef struct TajModPersistentState {
    unsigned int version;
    int taj_unlocked;
    /* Kept under its original C name so existing Taj transaction code remains
     * source-compatible. Version 2+ calls this taj_migration_complete on disk. */
    int adventure_migration_complete;
    int wizpig_unlocked;
    int wizpig_migration_complete;
    int terry_unlocked;
    int terry_migration_complete;
} TajModPersistentState;

typedef MdkrTextStateStorage TajModStateStorage;

void taj_mod_state_defaults(TajModPersistentState *state);
int taj_mod_state_parse(TajModPersistentState *state, const char *text,
                        size_t length);
int taj_mod_state_serialize(const TajModPersistentState *state, char *text,
                            size_t capacity, size_t *length);
int taj_mod_state_load(TajModPersistentState *state,
                       const TajModStateStorage *storage);
int taj_mod_state_store(const TajModPersistentState *state,
                        const TajModStateStorage *storage);

#endif /* MDKR64_TAJ_MOD_STATE_H */
