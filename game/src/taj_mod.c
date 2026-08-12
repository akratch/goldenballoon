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

typedef struct ModRacerRuntimeState {
    TajModPersistentState persisted;
    TajModPersistentState pending_previous;
    TajModPersistentState pending_candidate;
    TajModPersistentState retry_candidate;
    const TajModStateStorage *storage;
    ModRacerIdentity player_identity[TAJ_MOD_MAX_PLAYERS];
    ModRacerIdentity racer_identity[TAJ_MOD_MAX_PLAYERS];
    unsigned int unlock_announcement_mask;
    unsigned int enabled_mask;
    int persistence_failed;
    TajModPersistenceIssue persistence_issue;
    TajModPersistenceIssue pending_issue;
    TajModPersistenceIssue retry_issue;
    unsigned int pending_enabled_mask;
    unsigned int pending_unlock_announcement_mask;
    int pending_store;
    int retry_pending;
    unsigned int pending_generation;
    unsigned int next_generation;
#ifdef TAJ_MOD_TESTING
    int test_async_persistence;
#endif
    int racer_bindings_active;
    int booted;
} ModRacerRuntimeState;

static ModRacerRuntimeState s_roster;

static int mod_racer_valid_player(int player_index) {
    return player_index >= 0 && player_index < TAJ_MOD_MAX_PLAYERS;
}

static int mod_racer_valid_identity(ModRacerIdentity identity) {
    return identity > MOD_RACER_RETAIL &&
           identity < MOD_RACER_IDENTITY_COUNT;
}

static unsigned int mod_racer_identity_bit(ModRacerIdentity identity) {
    static const unsigned int bits[MOD_RACER_IDENTITY_COUNT] = {
        0u, 0x1u, 0x2u, 0x4u
    };
    return mod_racer_valid_identity(identity) ? bits[identity] : 0u;
}

static int mod_racer_persisted_unlocked(const TajModPersistentState *state,
                                         ModRacerIdentity identity) {
    if (state == NULL) return 0;
    switch (identity) {
        case MOD_RACER_TAJ:
            return state->taj_unlocked != 0;
        case MOD_RACER_WIZPIG:
            return state->wizpig_unlocked != 0;
        case MOD_RACER_TERRY:
            return state->terry_unlocked != 0;
        default:
            return 0;
    }
}

static int mod_racer_persisted_migrated(const TajModPersistentState *state,
                                         ModRacerIdentity identity) {
    if (state == NULL) return 0;
    switch (identity) {
        case MOD_RACER_TAJ:
            return state->adventure_migration_complete != 0;
        case MOD_RACER_WIZPIG:
            return state->wizpig_migration_complete != 0;
        case MOD_RACER_TERRY:
            return state->terry_migration_complete != 0;
        default:
            return 0;
    }
}

static void mod_racer_set_persisted_unlock(TajModPersistentState *state,
                                            ModRacerIdentity identity,
                                            int unlocked, int migrated) {
    if (state == NULL) return;
    switch (identity) {
        case MOD_RACER_TAJ:
            state->taj_unlocked = unlocked != 0;
            state->adventure_migration_complete = migrated != 0;
            break;
        case MOD_RACER_WIZPIG:
            state->wizpig_unlocked = unlocked != 0;
            state->wizpig_migration_complete = migrated != 0;
            break;
        case MOD_RACER_TERRY:
            state->terry_unlocked = unlocked != 0;
            state->terry_migration_complete = migrated != 0;
            break;
        default:
            break;
    }
}

static unsigned int mod_racer_enabled_mask_from_state(
    const TajModPersistentState *state) {
    unsigned int mask = 0;
    ModRacerIdentity identity;
    for (identity = MOD_RACER_TAJ; identity < MOD_RACER_IDENTITY_COUNT;
         identity++) {
        if (mod_racer_persisted_unlocked(state, identity)) {
            mask |= mod_racer_identity_bit(identity);
        }
    }
    return mask;
}

int taj_mod_valid_live_player(int player_index) {
    return mod_racer_valid_player(player_index);
}

unsigned int taj_mod_player_bit(int player_index) {
    static const unsigned int bits[TAJ_MOD_MAX_PLAYERS] = {
        0x1u, 0x2u, 0x4u, 0x8u
    };
    return mod_racer_valid_player(player_index) ? bits[player_index] : 0u;
}

static int mod_racer_uses_async_persistence(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#elif defined(TAJ_MOD_TESTING)
    return s_roster.test_async_persistence;
#else
    return 0;
#endif
}

static unsigned int mod_racer_next_generation(void) {
    s_roster.next_generation++;
    if (s_roster.next_generation == 0) s_roster.next_generation++;
    return s_roster.next_generation;
}

static void mod_racer_queue_retry(const TajModPersistentState *candidate,
                                   TajModPersistenceIssue issue) {
    if (candidate == NULL) return;
    s_roster.retry_candidate = *candidate;
    s_roster.retry_issue = issue;
    s_roster.retry_pending = 1;
}

#ifdef NATIVE_PORT
static int mod_racer_test_player(const char *variable, const char *label) {
    const char *text = getenv(variable);
    char *end = NULL;
    long player;

    if (text == NULL) return -1;
    errno = 0;
    player = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || player < 0 ||
        player >= TAJ_MOD_MAX_PLAYERS) {
        fprintf(stderr, "[ROSTER] ignoring invalid %s value: %s\n", variable,
                text);
        return -1;
    }
    MDKR_TRACE("%s_test_player: player=%d enabled=1", label, (int)player);
    return (int)player;
}

static ModRacerIdentity mod_racer_test_identity(int player_index) {
    static int taj_player = -2;
    static int wizpig_player = -2;
    static int terry_player = -2;
    static int warned_conflict;
    if (taj_player == -2) {
        taj_player = mod_racer_test_player("MDKR_TAJ_TEST_PLAYER", "taj");
        wizpig_player = mod_racer_test_player("MDKR_WIZPIG_TEST_PLAYER",
                                               "wizpig");
        terry_player = mod_racer_test_player("MDKR_TERRY_TEST_PLAYER",
                                              "terry");
    }
    if (player_index == taj_player &&
        (player_index == wizpig_player || player_index == terry_player)) {
        if (!warned_conflict) {
            fprintf(stderr,
                    "[ROSTER] multiple test racers target player %d; Taj wins\n",
                    player_index);
            warned_conflict = 1;
        }
        return MOD_RACER_TAJ;
    }
    if (player_index == taj_player) return MOD_RACER_TAJ;
    if (player_index == wizpig_player && player_index == terry_player) {
        if (!warned_conflict) {
            fprintf(stderr,
                    "[ROSTER] Wizpig and Terry target player %d; Wizpig wins\n",
                    player_index);
            warned_conflict = 1;
        }
        return MOD_RACER_WIZPIG;
    }
    if (player_index == wizpig_player) return MOD_RACER_WIZPIG;
    if (player_index == terry_player) return MOD_RACER_TERRY;
    return MOD_RACER_RETAIL;
}
#else
static ModRacerIdentity mod_racer_test_identity(int player_index) {
    (void)player_index;
    return MOD_RACER_RETAIL;
}
#endif

static int mod_racer_test_identity_active(ModRacerIdentity identity) {
    int player;
    for (player = 0; player < TAJ_MOD_MAX_PLAYERS; player++) {
        if (mod_racer_test_identity(player) == identity) return 1;
    }
    return 0;
}

static ModRacerIdentity mod_racer_effective_player_identity(int player_index) {
    ModRacerIdentity identity;
    if (!mod_racer_valid_player(player_index)) return MOD_RACER_RETAIL;
    identity = s_roster.player_identity[player_index];
    return identity == MOD_RACER_RETAIL
               ? mod_racer_test_identity(player_index)
               : identity;
}

static int mod_racer_store_candidate(const TajModPersistentState *candidate,
                                      TajModPersistenceIssue issue) {
    if (s_roster.storage == NULL) {
        s_roster.persistence_failed = 0;
        s_roster.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
        s_roster.retry_pending = 0;
        return 1;
    }
    if (mod_racer_uses_async_persistence()) {
        if (s_roster.pending_store) {
            mod_racer_queue_retry(candidate, issue);
            fprintf(stderr,
                    "[ROSTER] persistence is busy; retry this action shortly\n");
            return 0;
        }
        s_roster.pending_previous = s_roster.persisted;
        s_roster.pending_enabled_mask = s_roster.enabled_mask;
        s_roster.pending_unlock_announcement_mask =
            s_roster.unlock_announcement_mask;
        s_roster.pending_issue = issue;
        s_roster.pending_candidate = *candidate;
        s_roster.pending_generation = mod_racer_next_generation();
        s_roster.pending_store = 1;
    }
    if (taj_mod_state_store(candidate, s_roster.storage) != 1) {
        s_roster.pending_store = 0;
        s_roster.pending_generation = 0;
        mod_racer_queue_retry(candidate, issue);
        s_roster.persistence_failed = 1;
        s_roster.persistence_issue = issue;
        fprintf(stderr,
                issue == TAJ_MOD_PERSISTENCE_ERASE
                    ? "[ROSTER] bonus erase was not persisted; keeping the prior unlocks\n"
                    : "[ROSTER] unlock state was not persisted; session remains active\n");
        return 0;
    }
    s_roster.persistence_failed = 0;
    s_roster.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
    if (!mod_racer_uses_async_persistence()) {
        s_roster.retry_pending = 0;
    } else if (issue == TAJ_MOD_PERSISTENCE_UNLOCK && s_roster.retry_pending &&
               s_roster.retry_issue == TAJ_MOD_PERSISTENCE_UNLOCK) {
        /* This candidate was built from s_roster.persisted, which already carries every unlock the
         * queued retry was holding, so it strictly supersedes it. Dropping the queue entry here is
         * what stops taj_mod_report_persistence_success() from later replaying an OLDER unlock set
         * over this newer one -- which silently un-unlocked whichever identity was granted second.
         * Only unlocks coalesce: an ERASE must still be retried on its own terms. */
        s_roster.retry_pending = 0;
    }
    return 1;
}

static int mod_racer_unlock(ModRacerIdentity identity) {
    TajModPersistentState candidate = s_roster.persisted;
    int newly_unlocked;
    int persistence_changed;
    int stored = 1;

    if (!mod_racer_valid_identity(identity)) return 0;
    newly_unlocked = !mod_racer_persisted_unlocked(&s_roster.persisted,
                                                    identity);
    persistence_changed = newly_unlocked ||
        !mod_racer_persisted_migrated(&s_roster.persisted, identity);
    mod_racer_set_persisted_unlock(&candidate, identity, 1, 1);
    if (persistence_changed ||
        (s_roster.retry_pending &&
         s_roster.retry_issue == TAJ_MOD_PERSISTENCE_UNLOCK)) {
        stored = mod_racer_store_candidate(&candidate,
                                            TAJ_MOD_PERSISTENCE_UNLOCK);
    }
    mod_racer_set_persisted_unlock(&s_roster.persisted, identity, 1,
        stored ? 1 : mod_racer_persisted_migrated(&s_roster.persisted,
                                                   identity));
    s_roster.persisted.version = candidate.version;
    if (newly_unlocked) {
        s_roster.unlock_announcement_mask |= mod_racer_identity_bit(identity);
    }
    s_roster.enabled_mask |= mod_racer_identity_bit(identity);
    return newly_unlocked;
}

void taj_mod_boot(const TajModStateStorage *storage) {
    int result;
    if (s_roster.booted) return;
    memset(&s_roster, 0, sizeof(s_roster));
    taj_mod_state_defaults(&s_roster.persisted);
    s_roster.storage = storage;
    if (storage != NULL) {
        result = taj_mod_state_load(&s_roster.persisted, storage);
        if (result < 0) {
            taj_mod_state_defaults(&s_roster.persisted);
            s_roster.persistence_failed = 1;
            s_roster.persistence_issue = TAJ_MOD_PERSISTENCE_LOAD;
            fprintf(stderr,
                    "[ROSTER] unlock state could not be loaded; using defaults\n");
        }
    }
    s_roster.enabled_mask =
        mod_racer_enabled_mask_from_state(&s_roster.persisted);
    s_roster.booted = 1;
}

void taj_mod_on_title_return(void) { taj_mod_reset_player_selections(); }
int taj_mod_persistence_failed(void) { return s_roster.persistence_failed; }
TajModPersistenceIssue taj_mod_persistence_issue(void) {
    return s_roster.persistence_issue;
}
int taj_mod_persistence_pending(void) { return s_roster.pending_store; }

int taj_mod_retry_persistence(void) {
    TajModPersistentState candidate;
    TajModPersistenceIssue issue;
    if (s_roster.pending_store || !s_roster.retry_pending) return 0;
    candidate = s_roster.retry_candidate;
    issue = s_roster.retry_issue;
    /* This queue entry now owns the in-flight attempt. A failure will enqueue
     * it again; clearing first prevents a successful callback from replaying
     * the same transaction forever. */
    s_roster.retry_pending = 0;
    if (!mod_racer_store_candidate(&candidate, issue)) return 0;
    s_roster.persisted = candidate;
    s_roster.enabled_mask &= mod_racer_enabled_mask_from_state(&candidate);
    if (s_roster.enabled_mask == 0) {
        s_roster.unlock_announcement_mask = 0;
        taj_mod_reset_player_selections();
    }
    return 1;
}

int mod_racer_is_unlocked(ModRacerIdentity identity) {
    return mod_racer_valid_identity(identity) &&
           (mod_racer_persisted_unlocked(&s_roster.persisted, identity) ||
            mod_racer_test_identity_active(identity));
}

int mod_racer_is_enabled(ModRacerIdentity identity) {
    return mod_racer_is_unlocked(identity) &&
           ((s_roster.enabled_mask & mod_racer_identity_bit(identity)) != 0 ||
            mod_racer_test_identity_active(identity));
}

void mod_racer_set_enabled(ModRacerIdentity identity, int enabled) {
    int player;
    unsigned int bit = mod_racer_identity_bit(identity);
    if (bit == 0) return;
    if (enabled && mod_racer_is_unlocked(identity)) {
        s_roster.enabled_mask |= bit;
        return;
    }
    s_roster.enabled_mask &= ~bit;
    for (player = 0; player < TAJ_MOD_MAX_PLAYERS; player++) {
        if (s_roster.player_identity[player] == identity) {
            s_roster.player_identity[player] = MOD_RACER_RETAIL;
        }
        if (s_roster.racer_identity[player] == identity) {
            s_roster.racer_identity[player] = MOD_RACER_RETAIL;
        }
    }
}

int mod_racer_consume_unlock_announcement(ModRacerIdentity identity) {
    unsigned int bit = mod_racer_identity_bit(identity);
    int result = (s_roster.unlock_announcement_mask & bit) != 0;
    s_roster.unlock_announcement_mask &= ~bit;
    return result;
}

ModRacerIdentity mod_racer_submit_magic_code(const char *input) {
    if (input == NULL) return MOD_RACER_RETAIL;
    if (strcmp(input, "ABRACADABRA") == 0) {
        mod_racer_unlock(MOD_RACER_TAJ);
        return MOD_RACER_TAJ;
    }
    if (strcmp(input, "WIZPIGPOWER") == 0) {
        mod_racer_unlock(MOD_RACER_WIZPIG);
        return MOD_RACER_WIZPIG;
    }
    if (strcmp(input, "TERRYFLY") == 0) {
        mod_racer_unlock(MOD_RACER_TERRY);
        return MOD_RACER_TERRY;
    }
    return MOD_RACER_RETAIL;
}

int mod_racer_unlock_from_adventure_progress(ModRacerIdentity identity,
                                              unsigned int progress) {
    unsigned int required;
    if (identity == MOD_RACER_TAJ) {
        required = TAJ_MOD_COMPLETED_CHALLENGES;
    } else if (identity == MOD_RACER_WIZPIG) {
        required = WIZPIG_MOD_COMPLETED_BOSSES;
    } else if (identity == MOD_RACER_TERRY) {
        required = TERRY_MOD_COMPLETED_BOSSES;
    } else {
        return 0;
    }
    if ((progress & required) != required) return 0;
    return mod_racer_unlock(identity);
}

int mod_racer_reconcile_imported_progress(unsigned int taj_flags,
                                           unsigned int bosses) {
    int unlocked = 0;
    if (!mod_racer_persisted_migrated(&s_roster.persisted, MOD_RACER_TAJ) &&
        (taj_flags & TAJ_MOD_COMPLETED_CHALLENGES) ==
            TAJ_MOD_COMPLETED_CHALLENGES) {
        unlocked |= mod_racer_unlock(MOD_RACER_TAJ);
    }
    if (!mod_racer_persisted_migrated(&s_roster.persisted,
                                      MOD_RACER_WIZPIG) &&
        (bosses & WIZPIG_MOD_COMPLETED_BOSSES) ==
            WIZPIG_MOD_COMPLETED_BOSSES) {
        unlocked |= mod_racer_unlock(MOD_RACER_WIZPIG);
    }
    if (!mod_racer_persisted_migrated(&s_roster.persisted,
                                      MOD_RACER_TERRY) &&
        (bosses & TERRY_MOD_COMPLETED_BOSSES) ==
            TERRY_MOD_COMPLETED_BOSSES) {
        unlocked |= mod_racer_unlock(MOD_RACER_TERRY);
    }
    return unlocked;
}

/* Maps a 0-based row offset within the bonus block of the Magic Codes list to the identity that
 * owns it. The list shows one row per UNLOCKED bonus identity, appended after the retail rows, in
 * enum order, so the mapping is "skip locked identities, then count".
 *
 * This lives here rather than inline in menu.c because it was previously open-coded twice --
 * cheatlist_render() and menu_magic_codes_list_loop() -- and both copies consumed Wizpig's row only
 * when it was the row being resolved. With Taj and Wizpig both unlocked, Terry's row resolved to
 * MOD_RACER_RETAIL: it still drew the "CONTROL TERRY" label, but reported OFF forever and its
 * toggle was a silent no-op. One definition, unit-tested across all eight unlock combinations. */
ModRacerIdentity mod_racer_identity_for_cheat_row(int virtual_row) {
    ModRacerIdentity identity;

    if (virtual_row < 0) return MOD_RACER_RETAIL;
    for (identity = MOD_RACER_TAJ; identity < MOD_RACER_IDENTITY_COUNT; identity++) {
        if (!mod_racer_is_unlocked(identity)) continue;
        if (virtual_row == 0) return identity;
        virtual_row--;
    }
    return MOD_RACER_RETAIL;
}

int mod_racer_unlocked_count(void) {
    ModRacerIdentity identity;
    int count = 0;

    for (identity = MOD_RACER_TAJ; identity < MOD_RACER_IDENTITY_COUNT; identity++) {
        if (mod_racer_is_unlocked(identity)) count++;
    }
    return count;
}

int taj_mod_is_unlocked(void) { return mod_racer_is_unlocked(MOD_RACER_TAJ); }
int taj_mod_is_enabled(void) { return mod_racer_is_enabled(MOD_RACER_TAJ); }
void taj_mod_set_enabled(int enabled) {
    mod_racer_set_enabled(MOD_RACER_TAJ, enabled);
}
int taj_mod_consume_unlock_announcement(void) {
    return mod_racer_consume_unlock_announcement(MOD_RACER_TAJ);
}
int taj_mod_submit_magic_code(const char *input) {
    return mod_racer_submit_magic_code(input) == MOD_RACER_TAJ;
}
int taj_mod_unlock_from_taj_flags(unsigned int taj_flags) {
    return mod_racer_unlock_from_adventure_progress(MOD_RACER_TAJ, taj_flags);
}
int taj_mod_reconcile_imported_taj_flags(unsigned int taj_flags) {
    if (mod_racer_persisted_migrated(&s_roster.persisted, MOD_RACER_TAJ) ||
        (taj_flags & TAJ_MOD_COMPLETED_CHALLENGES) !=
            TAJ_MOD_COMPLETED_CHALLENGES) {
        return 0;
    }
    return mod_racer_unlock(MOD_RACER_TAJ);
}

void taj_mod_reset_player_selections(void) {
    memset(s_roster.player_identity, 0, sizeof(s_roster.player_identity));
    memset(s_roster.racer_identity, 0, sizeof(s_roster.racer_identity));
    s_roster.racer_bindings_active = 0;
}

void taj_mod_clear_session_codes(void) {
    ModRacerIdentity identity;
    for (identity = MOD_RACER_TAJ; identity < MOD_RACER_IDENTITY_COUNT;
         identity++) {
        mod_racer_set_enabled(identity, 0);
    }
}

int taj_mod_erase_all_bonuses(void) {
    TajModPersistentState candidate = s_roster.persisted;
    mod_racer_set_persisted_unlock(&candidate, MOD_RACER_TAJ, 0, 1);
    mod_racer_set_persisted_unlock(&candidate, MOD_RACER_WIZPIG, 0, 1);
    mod_racer_set_persisted_unlock(&candidate, MOD_RACER_TERRY, 0, 1);
    if (!mod_racer_store_candidate(&candidate, TAJ_MOD_PERSISTENCE_ERASE)) {
        return 0;
    }
    s_roster.persisted = candidate;
    s_roster.enabled_mask = 0;
    s_roster.unlock_announcement_mask = 0;
    taj_mod_reset_player_selections();
    return 1;
}

void taj_mod_on_adventure_file_deleted(void) {
    taj_mod_reset_player_selections();
}

void mod_racer_set_player_identity(int player_index,
                                   ModRacerIdentity identity) {
    if (!mod_racer_valid_player(player_index)) return;
    if (identity != MOD_RACER_RETAIL && !mod_racer_is_enabled(identity)) return;
    s_roster.player_identity[player_index] = identity;
}

ModRacerIdentity mod_racer_player_identity(int player_index) {
    return mod_racer_effective_player_identity(player_index);
}

void taj_mod_set_player_selected(int player_index, int selected) {
    if (!mod_racer_valid_player(player_index)) return;
    if (selected) {
        mod_racer_set_player_identity(player_index, MOD_RACER_TAJ);
    } else if (s_roster.player_identity[player_index] == MOD_RACER_TAJ) {
        s_roster.player_identity[player_index] = MOD_RACER_RETAIL;
    }
}

void taj_mod_swap_player_selections(int first_player, int second_player) {
    ModRacerIdentity swap;
    if (!mod_racer_valid_player(first_player) ||
        !mod_racer_valid_player(second_player) || first_player == second_player) {
        return;
    }
    swap = s_roster.player_identity[first_player];
    s_roster.player_identity[first_player] =
        s_roster.player_identity[second_player];
    s_roster.player_identity[second_player] = swap;
}

void taj_mod_begin_racer_bindings(void) {
    memset(s_roster.racer_identity, 0, sizeof(s_roster.racer_identity));
    s_roster.racer_bindings_active = 1;
}

void taj_mod_bind_racer_player(int selected_player_index,
                               int live_player_index) {
    if (!s_roster.racer_bindings_active ||
        !mod_racer_valid_player(selected_player_index) ||
        !mod_racer_valid_player(live_player_index)) {
        return;
    }
    s_roster.racer_identity[live_player_index] =
        mod_racer_effective_player_identity(selected_player_index);
}

ModRacerIdentity mod_racer_live_identity(int player_index) {
    ModRacerIdentity identity;
    if (!mod_racer_valid_player(player_index)) return MOD_RACER_RETAIL;
    identity = s_roster.racer_bindings_active
                   ? s_roster.racer_identity[player_index]
                   : mod_racer_effective_player_identity(player_index);
    return mod_racer_is_enabled(identity) ? identity : MOD_RACER_RETAIL;
}

int mod_racer_live_is(int player_index, ModRacerIdentity identity) {
    return mod_racer_valid_identity(identity) &&
           mod_racer_live_identity(player_index) == identity;
}

int taj_mod_player_selected(int player_index) {
    return mod_racer_player_identity(player_index) == MOD_RACER_TAJ;
}
int taj_mod_racer_is_taj(int player_index) {
    return mod_racer_live_is(player_index, MOD_RACER_TAJ);
}

int mod_racer_resolve_race_character(int player_index,
                                     int requested_character) {
    /* The native test identity is intentionally routed through the same donor
     * resolver as a menu selection. Otherwise a real-ROM presentation run can
     * claim Terry/Wizpig while leaving an unrelated retail model underneath,
     * which exercises only the fail-visible fallback rather than gameplay. */
    ModRacerIdentity identity = mod_racer_effective_player_identity(player_index);
    if (mod_racer_is_enabled(identity)) {
        if (identity == MOD_RACER_TAJ) return TAJ_MOD_DONOR_CHARACTER;
        if (identity == MOD_RACER_WIZPIG) return WIZPIG_MOD_DONOR_CHARACTER;
        if (identity == MOD_RACER_TERRY) return TERRY_MOD_DONOR_CHARACTER;
    }
    if (requested_character < 0 ||
        requested_character > TAJ_MOD_DONOR_CHARACTER) {
        return TAJ_MOD_NEUTRAL_CHARACTER;
    }
    return requested_character;
}

int taj_mod_resolve_race_character(int player_index, int requested_character) {
    return mod_racer_resolve_race_character(player_index, requested_character);
}

unsigned int taj_mod_persistence_pending_generation(void) {
    return s_roster.pending_store ? s_roster.pending_generation : 0;
}

TAJ_MOD_KEEPALIVE void taj_mod_report_persistence_failure(
    unsigned int generation) {
    TajModPersistenceIssue issue;
    TajModPersistentState previous;
    TajModPersistentState candidate;
    int restored = 1;
    if (!s_roster.pending_store || generation == 0 ||
        generation != s_roster.pending_generation) return;
    issue = s_roster.pending_issue;
    previous = s_roster.pending_previous;
    candidate = s_roster.pending_candidate;
    if (issue == TAJ_MOD_PERSISTENCE_ERASE) {
        restored = taj_mod_state_store(&previous, s_roster.storage) == 1;
        if (restored) {
            s_roster.persisted = previous;
            s_roster.enabled_mask = s_roster.pending_enabled_mask &
                mod_racer_enabled_mask_from_state(&s_roster.persisted);
            s_roster.unlock_announcement_mask =
                s_roster.pending_unlock_announcement_mask;
            taj_mod_reset_player_selections();
        }
    }
    fprintf(stderr,
            issue == TAJ_MOD_PERSISTENCE_ERASE
                ? (restored
                    ? "[ROSTER] bonus erase could not be committed; keeping prior unlocks\n"
                    : "[ROSTER] bonus erase could not be committed; retry before restarting\n")
                : "[ROSTER] unlock state could not be committed; session remains active\n");
    s_roster.pending_store = 0;
    s_roster.pending_generation = 0;
    /* Two unlocks can be discovered during one save-file listing pass. The
     * later candidate contains both, so retain that coalesced write if the
     * first browser commit fails. Destructive actions deliberately do not
     * coalesce with unlocks. */
    if (!(issue == TAJ_MOD_PERSISTENCE_UNLOCK && s_roster.retry_pending &&
          s_roster.retry_issue == TAJ_MOD_PERSISTENCE_UNLOCK)) {
        mod_racer_queue_retry(&candidate, issue);
    }
    s_roster.persistence_failed = 1;
    s_roster.persistence_issue = issue;
}

TAJ_MOD_KEEPALIVE void taj_mod_report_persistence_success(
    unsigned int generation) {
    int continue_queue;
    if (!s_roster.pending_store || generation == 0 ||
        generation != s_roster.pending_generation) return;
    continue_queue = s_roster.retry_pending;
    s_roster.pending_store = 0;
    s_roster.pending_generation = 0;
    s_roster.persistence_failed = 0;
    s_roster.persistence_issue = TAJ_MOD_PERSISTENCE_NONE;
    /* A refused second unlock is not lost merely because the transaction in
     * front of it succeeded. Start it only after the first generation settles
     * so rollback snapshots can never overlap. */
    if (continue_queue) (void)taj_mod_retry_persistence();
}

#ifdef TAJ_MOD_TESTING
void taj_mod_reset_for_test(void) { memset(&s_roster, 0, sizeof(s_roster)); }
void taj_mod_set_async_persistence_for_test(int enabled) {
    s_roster.test_async_persistence = enabled != 0;
}
unsigned int taj_mod_pending_generation_for_test(void) {
    return taj_mod_persistence_pending_generation();
}
#endif

#endif /* NATIVE_PORT || TAJ_MOD_TESTING */
