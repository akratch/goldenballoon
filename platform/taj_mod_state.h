/* Versioned, platform-neutral persistence contract for the Taj mod sidecar. */
#ifndef MDKR64_TAJ_MOD_STATE_H
#define MDKR64_TAJ_MOD_STATE_H

#include <stddef.h>

enum {
    TAJ_MOD_STATE_VERSION = 1,
    TAJ_MOD_STATE_TEXT_CAPACITY = 96
};

typedef struct TajModPersistentState {
    unsigned int version;
    int taj_unlocked;
    int adventure_migration_complete;
} TajModPersistentState;

/* A backend returns 1 for a completed operation, 0 for an absent state, and
 * -1 for an I/O failure.  The core deliberately owns no filesystem policy. */
typedef int (*TajModStateRead)(void *context, char *text, size_t capacity,
                               size_t *length);
typedef int (*TajModStateWrite)(void *context, const char *text, size_t length);

typedef struct TajModStateStorage {
    void *context;
    TajModStateRead read;
    TajModStateWrite write;
} TajModStateStorage;

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
