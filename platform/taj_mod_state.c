#include "taj_mod_state.h"

#include <stdio.h>
#include <string.h>

void taj_mod_state_defaults(TajModPersistentState *state) {
    if (state != NULL) {
        state->version = TAJ_MOD_STATE_VERSION;
        state->taj_unlocked = 0;
        state->adventure_migration_complete = 0;
    }
}

int taj_mod_state_parse(TajModPersistentState *state, const char *text,
                        size_t length) {
    char buffer[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t parse_length;
    unsigned int version;
    unsigned int unlocked;
    unsigned int migration_complete;
    int consumed = 0;

    if (state == NULL || text == NULL || length == 0 ||
        length >= sizeof(buffer)) {
        return 0;
    }
    /* Accept the conventional single LF/CRLF file terminator. Keep the parser
     * otherwise exact: blank lines, trailing keys, and arbitrary suffixes must
     * still fail instead of being silently ignored. */
    parse_length = length;
    if (text[parse_length - 1] == '\n') {
        parse_length--;
        if (parse_length > 0 && text[parse_length - 1] == '\r') {
            parse_length--;
        }
    }
    if (parse_length == 0) {
        return 0;
    }
    memcpy(buffer, text, parse_length);
    buffer[parse_length] = '\0';
    if (sscanf(buffer,
               "taj_mod_version=%u\ntaj_unlocked=%u\n"
               "adventure_migration_complete=%u%n",
               &version, &unlocked, &migration_complete, &consumed) != 3 ||
        (size_t)consumed != parse_length || version != TAJ_MOD_STATE_VERSION ||
        unlocked > 1u || migration_complete > 1u) {
        return 0;
    }
    state->version = version;
    state->taj_unlocked = (int)unlocked;
    state->adventure_migration_complete = (int)migration_complete;
    return 1;
}

int taj_mod_state_serialize(const TajModPersistentState *state, char *text,
                            size_t capacity, size_t *length) {
    int written;

    if (state == NULL || text == NULL || capacity == 0 || length == NULL ||
        state->version != TAJ_MOD_STATE_VERSION) {
        return 0;
    }
    written = snprintf(text, capacity,
                       "taj_mod_version=%u\ntaj_unlocked=%u\n"
                       "adventure_migration_complete=%u",
                       state->version, state->taj_unlocked != 0 ? 1u : 0u,
                       state->adventure_migration_complete != 0 ? 1u : 0u);
    if (written < 0 || (size_t)written >= capacity) {
        return 0;
    }
    *length = (size_t)written;
    return 1;
}

int taj_mod_state_load(TajModPersistentState *state,
                       const TajModStateStorage *storage) {
    char text[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t length = 0;
    int result;

    if (state == NULL || storage == NULL || storage->read == NULL) {
        return -1;
    }
    taj_mod_state_defaults(state);
    result = storage->read(storage->context, text, sizeof(text), &length);
    if (result <= 0) {
        return result;
    }
    if (length >= sizeof(text)) {
        return -1;
    }
    text[length] = '\0';
    return taj_mod_state_parse(state, text, length) ? 1 : -1;
}

int taj_mod_state_store(const TajModPersistentState *state,
                        const TajModStateStorage *storage) {
    char text[TAJ_MOD_STATE_TEXT_CAPACITY];
    size_t length;

    if (storage == NULL || storage->write == NULL ||
        !taj_mod_state_serialize(state, text, sizeof(text), &length)) {
        return -1;
    }
    return storage->write(storage->context, text, length);
}
