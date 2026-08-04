#include "taj_mod.h"

#if defined(NATIVE_PORT) || defined(TAJ_MOD_TESTING)

#include "mdkr_trace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define TAJ_MOD_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define TAJ_MOD_KEEPALIVE
#endif

typedef struct TajModRuntimeState {
    TajModPersistentState persisted;
    TajModPersistentState pending_previous;
    TajModPersistentState pending_candidate;
    TajModPersistentState retry_candidate;
    const TajModStateStorage *storage;
    unsigned int player_mask;
    unsigned int racer_mask;
    int unlock_announcement;
    int enabled;
    int persistence_failed;
    TajModPersistenceIssue persistence_issue;
    TajModPersistenceIssue pending_issue;
    TajModPersistenceIssue retry_issue;
    int pending_enabled;
    int pending_unlock_announcement;
    int pending_store;
    int retry_pending;
    unsigned int pending_generation;
    unsigned int next_generation;
#ifdef TAJ_MOD_TESTING
    int test_async_persistence;
#endif
    int racer_bindings_active;
    int booted;
} TajModRuntimeState;

static TajModRuntimeState s_taj;

static int taj_mod_valid_player(int player_index) {
    return player_index >= 0 && player_index < TAJ_MOD_MAX_PLAYERS;
}

unsigned int taj_mod_player_bit(int player_index) {
    static const unsigned int bits[TAJ_MOD_MAX_PLAYERS] = {
        0x1u, 0x2u, 0x4u, 0x8u
    };
    return taj_mod_valid_player(player_index) ? bits[player_index] : 0u;
}

static int taj_mod_uses_async_persistence(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#elif defined(TAJ_MOD_TESTING)
    return s_taj.test_async_persistence;
#else
    return 0;
#endif
}

static unsigned int taj_mod_next_generation(void) {
    s_taj.next_generation++;
    if (s_taj.next_generation == 0) {
        s_taj.next_generation++;
    }
    return s_taj.next_generation;
}

static void taj_mod_queue_retry(const TajModPersistentState *candidate,
                                TajModPersistenceIssue issue) {
    if (candidate == NULL) {
        return;
    }
    s_taj.retry_candidate = *candidate;
    s_taj.retry_issue = issue;
    s_taj.retry_pending = 1;
}

#ifdef NATIVE_PORT
static void taj_mod_apply_test_player_from_environment(void) {
    const char *text = getenv("MDKR_TAJ_TEST_PLAYER");
    char *end = NULL;
    long player;

    if (text == NULL) {
        return;
    }
    errno = 0;
    player = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        player < 0 || player >= TAJ_MOD_MAX_PLAYERS) {
        fprintf(stderr,
                "[TAJ] ignoring invalid MDKR_TAJ_TEST_PLAYER value: %s\n",
                text);
        return;
    }
    /* This is an opt-in native test bootstrap, not an unlock route: it never
     * writes the sidecar and cannot activate without the exact bounded value. */
    s_taj.persisted.version = TAJ_MOD_STATE_VERSION;
    s_taj.persisted.taj_unlocked = 1;
    s_taj.persisted.adventure_migration_complete = 1;
    s_taj.enabled = 1;
    s_taj.player_mask = taj_mod_player_bit((int)player);
    MDKR_TRACE("taj_test_player: player=%d enabled=1", (int)player);
}
#endif

static int taj_mod_store_candidate(const TajModPersistentState *candidate,
                                   TajModPersistenceIssue issue) {
    if (s_taj.storage == NULL) {
        s_taj.persistence_failed = 0;
        s_taj.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
        s_taj.retry_pending = 0;
        return 1;
    }
    if (taj_mod_uses_async_persistence()) {
        /* IDBFS commits asynchronously. Keep exactly one transaction in
         * flight so its rollback snapshot cannot be replaced by a later menu
         * action. A second action is refused; it must be explicitly retried. */
        if (s_taj.pending_store) {
            s_taj.persistence_failed = 1;
            s_taj.persistence_issue = issue;
            fprintf(stderr,
                    "[TAJ] persistence is still busy; retry this action shortly\n");
            return 0;
        }
        s_taj.pending_previous = s_taj.persisted;
        s_taj.pending_enabled = s_taj.enabled;
        s_taj.pending_unlock_announcement = s_taj.unlock_announcement;
        s_taj.pending_issue = issue;
        s_taj.pending_candidate = *candidate;
        s_taj.pending_generation = taj_mod_next_generation();
        s_taj.pending_store = 1;
    }
    if (taj_mod_state_store(candidate, s_taj.storage) != 1) {
        s_taj.pending_store = 0;
        s_taj.pending_generation = 0;
        taj_mod_queue_retry(candidate, issue);
        s_taj.persistence_failed = 1;
        s_taj.persistence_issue = issue;
        fprintf(stderr,
                issue == TAJ_MOD_PERSISTENCE_ERASE
                    ? "[TAJ] bonus erase was not persisted; keeping the prior unlock\n"
                    : "[TAJ] global unlock state was not persisted; session remains active\n");
        return 0;
    }
    /* Desktop storage is durable on return. Web storage reports its later
     * IDBFS result through the keepalive callbacks below. */
    s_taj.persistence_failed = 0;
    s_taj.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
    if (!taj_mod_uses_async_persistence()) {
        s_taj.retry_pending = 0;
    }
    return 1;
}

static void taj_mod_unlock(void) {
    TajModPersistentState candidate = s_taj.persisted;
    int newly_unlocked = !s_taj.persisted.taj_unlocked;
    int persistence_changed = newly_unlocked ||
        !s_taj.persisted.adventure_migration_complete;

    candidate.taj_unlocked = 1;
    candidate.adventure_migration_complete = 1;
    if (persistence_changed ||
        (s_taj.retry_pending &&
         s_taj.retry_issue == TAJ_MOD_PERSISTENCE_UNLOCK)) {
        (void)taj_mod_store_candidate(&candidate,
                                      TAJ_MOD_PERSISTENCE_UNLOCK);
    }
    /* An unlock remains useful for this session even if storage failed. */
    s_taj.persisted = candidate;
    if (newly_unlocked) {
        s_taj.unlock_announcement = 1;
    }
    s_taj.enabled = 1;
}

void taj_mod_boot(const TajModStateStorage *storage) {
    int result;

    if (s_taj.booted) {
        return;
    }
    memset(&s_taj, 0, sizeof(s_taj));
    taj_mod_state_defaults(&s_taj.persisted);
    s_taj.storage = storage;
    if (storage != NULL) {
        result = taj_mod_state_load(&s_taj.persisted, storage);
        if (result < 0) {
            taj_mod_state_defaults(&s_taj.persisted);
            s_taj.persistence_failed = 1;
            s_taj.persistence_issue = TAJ_MOD_PERSISTENCE_LOAD;
            fprintf(stderr, "[TAJ] global unlock state could not be loaded; using defaults\n");
        }
    }
    s_taj.enabled = s_taj.persisted.taj_unlocked;
    s_taj.booted = 1;
}

void taj_mod_on_title_return(void) { taj_mod_reset_player_selections(); }

int taj_mod_persistence_failed(void) { return s_taj.persistence_failed; }
TajModPersistenceIssue taj_mod_persistence_issue(void) {
    return s_taj.persistence_issue;
}
int taj_mod_persistence_pending(void) { return s_taj.pending_store; }

int taj_mod_retry_persistence(void) {
    TajModPersistentState candidate;
    TajModPersistenceIssue issue;

    if (s_taj.pending_store || !s_taj.retry_pending) {
        return 0;
    }
    candidate = s_taj.retry_candidate;
    issue = s_taj.retry_issue;
    if (!taj_mod_store_candidate(&candidate, issue)) {
        return 0;
    }
    s_taj.persisted = candidate;
    s_taj.enabled = candidate.taj_unlocked != 0;
    if (!s_taj.enabled) {
        s_taj.unlock_announcement = 0;
        taj_mod_reset_player_selections();
    }
    return 1;
}
int taj_mod_is_unlocked(void) { return s_taj.persisted.taj_unlocked != 0; }
int taj_mod_is_enabled(void) { return taj_mod_is_unlocked() && s_taj.enabled; }
void taj_mod_set_enabled(int enabled) {
    s_taj.enabled = taj_mod_is_unlocked() && enabled != 0;
    if (!s_taj.enabled) {
        taj_mod_reset_player_selections();
    }
}
int taj_mod_consume_unlock_announcement(void) {
    int announcement = s_taj.unlock_announcement;
    s_taj.unlock_announcement = 0;
    return announcement;
}

int taj_mod_submit_magic_code(const char *input) {
    if (input == NULL || strcmp(input, "ABRACADABRA") != 0) {
        return 0;
    }
    taj_mod_unlock();
    return 1;
}

int taj_mod_unlock_from_taj_flags(unsigned int taj_flags) {
    if ((taj_flags & TAJ_MOD_COMPLETED_CHALLENGES) !=
        TAJ_MOD_COMPLETED_CHALLENGES) {
        return 0;
    }
    if (!taj_mod_is_unlocked()) {
        taj_mod_unlock();
        return 1;
    }
    return 0;
}

int taj_mod_reconcile_imported_taj_flags(unsigned int taj_flags) {
    if (s_taj.persisted.adventure_migration_complete ||
        (taj_flags & TAJ_MOD_COMPLETED_CHALLENGES) !=
            TAJ_MOD_COMPLETED_CHALLENGES) {
        return 0;
    }
    return taj_mod_unlock_from_taj_flags(taj_flags);
}

void taj_mod_reset_player_selections(void) {
    s_taj.player_mask = 0;
    s_taj.racer_mask = 0;
    s_taj.racer_bindings_active = 0;
}

void taj_mod_clear_session_codes(void) {
    taj_mod_set_enabled(0);
}

int taj_mod_erase_all_bonuses(void) {
    TajModPersistentState candidate = s_taj.persisted;

    candidate.taj_unlocked = 0;
    candidate.adventure_migration_complete = 1;
    if (!taj_mod_store_candidate(&candidate, TAJ_MOD_PERSISTENCE_ERASE)) {
        return 0;
    }
    s_taj.persisted = candidate;
    s_taj.enabled = 0;
    s_taj.unlock_announcement = 0;
    taj_mod_reset_player_selections();
    return 1;
}

void taj_mod_on_adventure_file_deleted(void) {
    /* This global sidecar intentionally survives individual Adventure saves. */
    taj_mod_reset_player_selections();
}

void taj_mod_set_player_selected(int player_index, int selected) {
    unsigned int player_bit = taj_mod_player_bit(player_index);
    if (!taj_mod_valid_player(player_index)) {
        return;
    }
    if (selected && taj_mod_is_enabled()) {
        s_taj.player_mask |= player_bit;
    } else {
        s_taj.player_mask &= ~player_bit;
    }
}

void taj_mod_swap_player_selections(int first_player, int second_player) {
    unsigned int first_bit;
    unsigned int second_bit;
    unsigned int first_selected;
    unsigned int second_selected;

    if (!taj_mod_valid_player(first_player) ||
        !taj_mod_valid_player(second_player) ||
        first_player == second_player) {
        return;
    }
    first_bit = taj_mod_player_bit(first_player);
    second_bit = taj_mod_player_bit(second_player);
    first_selected = s_taj.player_mask & first_bit;
    second_selected = s_taj.player_mask & second_bit;
    s_taj.player_mask &= ~(first_bit | second_bit);
    if (first_selected) {
        s_taj.player_mask |= second_bit;
    }
    if (second_selected) {
        s_taj.player_mask |= first_bit;
    }
}

void taj_mod_begin_racer_bindings(void) {
    s_taj.racer_mask = 0;
    s_taj.racer_bindings_active = 1;
#ifdef NATIVE_PORT
    taj_mod_apply_test_player_from_environment();
#endif
}

void taj_mod_bind_racer_player(int selected_player_index,
                               int live_player_index) {
    unsigned int selected_bit;
    unsigned int live_bit;
    if (!s_taj.racer_bindings_active ||
        !taj_mod_valid_player(selected_player_index) ||
        !taj_mod_valid_player(live_player_index)) {
        return;
    }
    selected_bit = taj_mod_player_bit(selected_player_index);
    live_bit = taj_mod_player_bit(live_player_index);
    if (s_taj.player_mask & selected_bit) {
        s_taj.racer_mask |= live_bit;
    } else {
        s_taj.racer_mask &= ~live_bit;
    }
}

int taj_mod_player_selected(int player_index) {
    return taj_mod_valid_player(player_index) &&
           (s_taj.player_mask & taj_mod_player_bit(player_index)) != 0;
}

int taj_mod_racer_is_taj(int player_index) {
    unsigned int mask;

    mask = s_taj.racer_bindings_active ? s_taj.racer_mask : s_taj.player_mask;
    return taj_mod_is_enabled() && taj_mod_valid_player(player_index) &&
           (mask & taj_mod_player_bit(player_index)) != 0;
}

int taj_mod_resolve_race_character(int player_index, int requested_character) {
    /* This resolver is called before live racer bindings exist. */
    if (taj_mod_is_enabled() && taj_mod_valid_player(player_index) &&
        (s_taj.player_mask & taj_mod_player_bit(player_index))) {
        return TAJ_MOD_DONOR_CHARACTER;
    }
    if (requested_character < 0 || requested_character > TAJ_MOD_DONOR_CHARACTER) {
        return TAJ_MOD_DONOR_CHARACTER;
    }
    return requested_character;
}

unsigned int taj_mod_persistence_pending_generation(void) {
    return s_taj.pending_store ? s_taj.pending_generation : 0;
}

TAJ_MOD_KEEPALIVE void taj_mod_report_persistence_failure(unsigned int generation) {
    TajModPersistenceIssue issue;
    TajModPersistentState previous;
    TajModPersistentState candidate;
    int restored = 1;

    /* Ignore a duplicate or stale browser callback. It must never overwrite a
     * later load error or a failure already reported to the player. */
    if (!s_taj.pending_store || generation == 0 ||
        generation != s_taj.pending_generation) return;
    issue = s_taj.pending_issue;
    previous = s_taj.pending_previous;
    candidate = s_taj.pending_candidate;

    if (issue == TAJ_MOD_PERSISTENCE_ERASE) {
        /* The browser's in-memory write succeeded but its durable IDBFS flush
         * did not. Put the previous bytes back before the shell's independent
         * retry chain can flush the failed erase on a later timer tick. */
        restored = taj_mod_state_store(&previous, s_taj.storage) == 1;
        if (restored) {
            s_taj.persisted = previous;
            s_taj.enabled = s_taj.pending_enabled &&
                            s_taj.persisted.taj_unlocked;
            s_taj.unlock_announcement = s_taj.pending_unlock_announcement;
            taj_mod_reset_player_selections();
        }
    }
    fprintf(stderr,
            issue == TAJ_MOD_PERSISTENCE_ERASE
                ? (restored
                    ? "[TAJ] bonus erase could not be committed; keeping the prior unlock\n"
                    : "[TAJ] bonus erase could not be committed; retry before restarting\n")
                : "[TAJ] global unlock state could not be committed; session remains active\n");
    s_taj.pending_store = 0;
    s_taj.pending_generation = 0;
    taj_mod_queue_retry(&candidate, issue);
    s_taj.persistence_failed = 1;
    s_taj.persistence_issue = issue;
}

TAJ_MOD_KEEPALIVE void taj_mod_report_persistence_success(unsigned int generation) {
    if (!s_taj.pending_store || generation == 0 ||
        generation != s_taj.pending_generation) return;
    s_taj.pending_store = 0;
    s_taj.pending_generation = 0;
    s_taj.retry_pending = 0;
    s_taj.persistence_failed = 0;
    s_taj.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
}

#ifdef TAJ_MOD_TESTING
void taj_mod_reset_for_test(void) { memset(&s_taj, 0, sizeof(s_taj)); }
void taj_mod_set_async_persistence_for_test(int enabled) {
    s_taj.test_async_persistence = enabled != 0;
}
unsigned int taj_mod_pending_generation_for_test(void) {
    return taj_mod_persistence_pending_generation();
}
#endif

#endif /* NATIVE_PORT || TAJ_MOD_TESTING */
