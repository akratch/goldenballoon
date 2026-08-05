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

int taj_mod_valid_live_player(int player_index) {
    return taj_mod_valid_player(player_index);
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
/* MDKR_TAJ_TEST_PLAYER is an opt-in native bootstrap that makes one port play
 * as Taj without driving the menus. It is a SHADOW: it is resolved once, it is
 * never written into `persisted` and never into `player_mask`, and every read
 * site ORs it in through the accessors below.
 *
 * It used to assign persisted.taj_unlocked / .adventure_migration_complete and
 * player_mask directly. Both were real defects. Writing the persisted struct
 * made a later genuine ABRACADABRA compute persistence_changed == 0, so the
 * unlock was never stored and the player never saw the banner -- the test hook
 * silently disabled the shipping unlock path. Writing player_mask re-applied it
 * on EVERY taj_mod_begin_racer_bindings(), so it also clobbered whatever the
 * player had actually chosen at every race init. */
static int taj_mod_test_player(void) {
    static int cached = -2; /* -2 unparsed, -1 absent/invalid */
    const char *text;
    char *end = NULL;
    long player;

    if (cached != -2) {
        return cached;
    }
    cached = -1;
    text = getenv("MDKR_TAJ_TEST_PLAYER");
    if (text == NULL) {
        return cached;
    }
    errno = 0;
    player = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        player < 0 || player >= TAJ_MOD_MAX_PLAYERS) {
        fprintf(stderr,
                "[TAJ] ignoring invalid MDKR_TAJ_TEST_PLAYER value: %s\n",
                text);
        return cached;
    }
    cached = (int)player;
    MDKR_TRACE("taj_test_player: player=%d enabled=1", cached);
    return cached;
}

static unsigned int taj_mod_test_player_bit(void) {
    int player = taj_mod_test_player();
    return player < 0 ? 0u : taj_mod_player_bit(player);
}
#else
#define taj_mod_test_player_bit() 0u
#endif

/* Every read of the selection mask goes through here so the test shadow is
 * applied in exactly one place and can never leak into stored state. */
static unsigned int taj_mod_effective_player_mask(void) {
    return s_taj.player_mask | taj_mod_test_player_bit();
}

/* The hook must also make Taj reachable, but only for reads: an unlock it
 * implies is a session fact, never a persisted one. */
static int taj_mod_test_player_active(void) {
    return taj_mod_test_player_bit() != 0u;
}

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
            /* REFUSED, not failed. The in-flight transaction is still going to
             * succeed or fail on its own and report through the keepalive
             * callbacks; latching persistence_failed here raised a banner for a
             * write that had not been attempted yet, and it overwrote the issue
             * belonging to the transaction actually in flight. Queue the
             * candidate so taj_mod_retry_persistence() can pick it up once the
             * commit settles, and leave the reported state to the owner of the
             * transaction. */
            taj_mod_queue_retry(candidate, issue);
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
    int stored = 1;

    candidate.taj_unlocked = 1;
    candidate.adventure_migration_complete = 1;
    if (persistence_changed ||
        (s_taj.retry_pending &&
         s_taj.retry_issue == TAJ_MOD_PERSISTENCE_UNLOCK)) {
        stored = taj_mod_store_candidate(&candidate,
                                         TAJ_MOD_PERSISTENCE_UNLOCK);
    }
    /* An unlock remains useful for this session even if storage failed. */
    s_taj.persisted.taj_unlocked = 1;
    s_taj.persisted.version = candidate.version;
    /* adventure_migration_complete is NOT a session fact: it is the record that
     * the one-time import reconcile has been written down. Committing it in RAM
     * after a refused or failed store made taj_mod_reconcile_imported_taj_flags()
     * early-out for the rest of the session, so the retry that was supposed to
     * write the unlock never had a reason to run again. Only advance it when
     * the bytes really went out. */
    if (stored) {
        s_taj.persisted.adventure_migration_complete =
            candidate.adventure_migration_complete;
    }
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
int taj_mod_is_unlocked(void) {
    return s_taj.persisted.taj_unlocked != 0 || taj_mod_test_player_active();
}
int taj_mod_is_enabled(void) {
    return taj_mod_is_unlocked() &&
           (s_taj.enabled || taj_mod_test_player_active());
}
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
    if (taj_mod_effective_player_mask() & selected_bit) {
        s_taj.racer_mask |= live_bit;
    } else {
        s_taj.racer_mask &= ~live_bit;
    }
}

int taj_mod_player_selected(int player_index) {
    return taj_mod_valid_player(player_index) &&
           (taj_mod_effective_player_mask() &
            taj_mod_player_bit(player_index)) != 0;
}

int taj_mod_racer_is_taj(int player_index) {
    unsigned int mask;

    mask = s_taj.racer_bindings_active ? s_taj.racer_mask
                                      : taj_mod_effective_player_mask();
    return taj_mod_is_enabled() && taj_mod_valid_player(player_index) &&
           (mask & taj_mod_player_bit(player_index)) != 0;
}

int taj_mod_resolve_race_character(int player_index, int requested_character) {
    /* This resolver is called before live racer bindings exist. */
    if (taj_mod_is_enabled() && taj_mod_valid_player(player_index) &&
        (taj_mod_effective_player_mask() &
         taj_mod_player_bit(player_index))) {
        return TAJ_MOD_DONOR_CHARACTER;
    }
    if (requested_character < 0 || requested_character > TAJ_MOD_DONOR_CHARACTER) {
        /* Out of range means the caller has a corrupt or uninitialised slot,
         * not that this player picked Taj. Returning the DONOR here handed the
         * donor character to a player who never selected it and made a garbage
         * value indistinguishable from a genuine Taj pick. Fail to the neutral
         * default the rest of the menu uses instead. */
        return TAJ_MOD_NEUTRAL_CHARACTER;
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
