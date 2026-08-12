#ifndef DKR_TAJ_MOD_H
#define DKR_TAJ_MOD_H

#include "taj_mod_state.h"

enum {
    TAJ_MOD_MAX_PLAYERS = 4,
    TAJ_MOD_DONOR_CHARACTER = 9,
    WIZPIG_MOD_DONOR_CHARACTER = 0,
    TERRY_MOD_DONOR_CHARACTER = 0,
    /* Where an unusable requested character falls back to. Deliberately NOT
     * the donor: a corrupt slot must never read as a Taj selection. */
    TAJ_MOD_NEUTRAL_CHARACTER = 0,
    TAJ_MOD_COMPLETED_CHALLENGES = 0x38,
    WIZPIG_MOD_COMPLETED_BOSSES = 0x20,
    /* Tricky's rematch is the thematic Dino Domain reward. */
    TERRY_MOD_COMPLETED_BOSSES = 0x80
};

/* Virtual racers deliberately do not extend Character or the retail ten-wide
 * asset tables. This identity travels beside the donor character instead. */
typedef enum ModRacerIdentity {
    MOD_RACER_RETAIL = 0,
    MOD_RACER_TAJ,
    MOD_RACER_WIZPIG,
    MOD_RACER_TERRY,
    MOD_RACER_IDENTITY_COUNT
} ModRacerIdentity;

typedef enum TajModPersistenceIssue {
    TAJ_MOD_PERSISTENCE_NONE = 0,
    TAJ_MOD_PERSISTENCE_LOAD,
    TAJ_MOD_PERSISTENCE_UNLOCK,
    TAJ_MOD_PERSISTENCE_ERASE
} TajModPersistenceIssue;

void taj_mod_boot(const TajModStateStorage *storage);
/* A boot is idempotent for the process. Title/selection returns clear only the
 * per-player sidecar, never the global unlock. */
void taj_mod_on_title_return(void);
int taj_mod_persistence_failed(void);
TajModPersistenceIssue taj_mod_persistence_issue(void);
/* A failed sidecar update never silently loses the current session choice.
 * Callers may offer this as an explicit Retry action once no web commit is in
 * flight. The return value reports whether a new store was accepted. */
int taj_mod_retry_persistence(void);
int taj_mod_persistence_pending(void);
int mod_racer_is_unlocked(ModRacerIdentity identity);
int mod_racer_is_enabled(ModRacerIdentity identity);
void mod_racer_set_enabled(ModRacerIdentity identity, int enabled);
int mod_racer_consume_unlock_announcement(ModRacerIdentity identity);
ModRacerIdentity mod_racer_submit_magic_code(const char *input);
int mod_racer_unlock_from_adventure_progress(ModRacerIdentity identity,
                                              unsigned int progress);
int mod_racer_reconcile_imported_progress(unsigned int taj_flags,
                                           unsigned int bosses);
/* Magic Codes list: which identity owns row `virtual_row` of the bonus block, and how many bonus
 * rows the list has. Single source of truth for the render and the input loop. */
ModRacerIdentity mod_racer_identity_for_cheat_row(int virtual_row);
int mod_racer_unlocked_count(void);
int taj_mod_is_unlocked(void);
int taj_mod_is_enabled(void);
void taj_mod_set_enabled(int enabled);
int taj_mod_consume_unlock_announcement(void);
int taj_mod_submit_magic_code(const char *input);
int taj_mod_unlock_from_taj_flags(unsigned int taj_flags);
int taj_mod_reconcile_imported_taj_flags(unsigned int taj_flags);
void taj_mod_clear_session_codes(void);
/* Erase is transactional: failure leaves the prior unlock live and durable. */
int taj_mod_erase_all_bonuses(void);
void taj_mod_on_adventure_file_deleted(void);

void taj_mod_reset_player_selections(void);
/* Checked four-player mask conversion shared by gameplay and presentation.
 * Invalid indices return zero; callers never perform a variable-width shift. */
unsigned int taj_mod_player_bit(int player_index);
/* The same bound taj_mod_bind_racer_player() applies, exposed so callers can
 * report a rejected binding instead of losing it silently. */
int taj_mod_valid_live_player(int player_index);
void taj_mod_set_player_selected(int player_index, int selected);
void mod_racer_set_player_identity(int player_index,
                                   ModRacerIdentity identity);
ModRacerIdentity mod_racer_player_identity(int player_index);
/* DKR swaps the complete P1/P2 settings rows when Adventure leadership moves.
 * Keep the settings-slot sidecar aligned without disturbing live racers from
 * the race that just ended. */
void taj_mod_swap_player_selections(int first_player, int second_player);
void taj_mod_begin_racer_bindings(void);
void taj_mod_bind_racer_player(int selected_player_index,
                               int live_player_index);
/* Settings/results slot identity. Live racer identity may be remapped in
 * two-player Adventure and is queried through taj_mod_racer_is_taj(). */
int taj_mod_player_selected(int player_index);
int taj_mod_racer_is_taj(int player_index);
ModRacerIdentity mod_racer_live_identity(int player_index);
int mod_racer_live_is(int player_index, ModRacerIdentity identity);
int mod_racer_resolve_race_character(int player_index,
                                     int requested_character);
int taj_mod_resolve_race_character(int player_index, int requested_character);
/* Browser callbacks carry the exact write generation. A delayed callback from
 * an older IDBFS transaction must never settle a newer Taj state change. */
unsigned int taj_mod_persistence_pending_generation(void);
void taj_mod_report_persistence_failure(unsigned int generation);
void taj_mod_report_persistence_success(unsigned int generation);

#ifdef TAJ_MOD_TESTING
void taj_mod_reset_for_test(void);
void taj_mod_set_async_persistence_for_test(int enabled);
unsigned int taj_mod_pending_generation_for_test(void);
#endif

#endif /* DKR_TAJ_MOD_H */
