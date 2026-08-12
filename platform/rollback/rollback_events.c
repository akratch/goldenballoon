#include "rollback_events.h"

#include <string.h>

static bool same_id(MdkrRollbackEventId left, MdkrRollbackEventId right) {
    return left.tick == right.tick && left.emitter == right.emitter &&
        left.ordinal == right.ordinal && left.kind == right.kind;
}

void mdkr_rollback_events_init(
    MdkrRollbackEventJournal *journal, bool online_match,
    MdkrRollbackEffectFn preview, MdkrRollbackEffectFn commit,
    MdkrRollbackEffectFn cancel, void *context) {
    if (journal == NULL) return;
    memset(journal, 0, sizeof(*journal));
    journal->online_match = online_match;
    journal->preview = preview;
    journal->commit = commit;
    journal->cancel = cancel;
    journal->context = context;
}

void mdkr_rollback_events_set_resimulating(
    MdkrRollbackEventJournal *journal, bool enabled) {
    if (journal != NULL) journal->resimulating = enabled;
}

bool mdkr_rollback_events_emit(
    MdkrRollbackEventJournal *journal, MdkrRollbackEventId id,
    MdkrRollbackEffectPolicy policy, uint32_t value) {
    unsigned index;
    MdkrRollbackEvent *event;
    if (journal == NULL || id.kind == 0u ||
        (policy != MDKR_ROLLBACK_EFFECT_REVERSIBLE &&
         policy != MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY)) return false;
    for (index = 0u; index < journal->count; index++) {
        event = &journal->entries[index];
        if (!same_id(event->id, id)) continue;
        if (event->value != value || event->policy != (uint8_t)policy) return false;
        event->live_in_timeline = true;
        journal->stats.duplicates++;
        return true;
    }
    if (journal->count >= MDKR_ROLLBACK_EVENT_CAPACITY) {
        journal->stats.overflows++;
        return false;
    }
    event = &journal->entries[journal->count++];
    memset(event, 0, sizeof(*event));
    event->id = id;
    event->value = value;
    event->policy = (uint8_t)policy;
    event->live_in_timeline = true;
    journal->stats.emitted++;
    if (policy == MDKR_ROLLBACK_EFFECT_REVERSIBLE && !journal->resimulating) {
        event->previewed = true;
        if (journal->preview != NULL) journal->preview(event, journal->context);
    }
    return true;
}

void mdkr_rollback_events_begin_rewrite(
    MdkrRollbackEventJournal *journal, uint32_t from_tick) {
    unsigned index;
    if (journal == NULL) return;
    journal->resimulating = true;
    for (index = 0u; index < journal->count; index++) {
        MdkrRollbackEvent *event = &journal->entries[index];
        if (!event->confirmed && event->id.tick >= from_tick) {
            event->live_in_timeline = false;
        }
    }
}

void mdkr_rollback_events_end_rewrite(MdkrRollbackEventJournal *journal) {
    unsigned read_index;
    unsigned write_index = 0u;
    if (journal == NULL) return;
    /* Reconciliation is complete before adapters preview corrected effects or
     * cancel vanished ones. Those callbacks are the deliberate host-I/O seam,
     * so they must observe ordinary (non-resimulating) policy. */
    journal->resimulating = false;
    for (read_index = 0u; read_index < journal->count; read_index++) {
        MdkrRollbackEvent *event = &journal->entries[read_index];
        if (!event->live_in_timeline && !event->confirmed) {
            if (event->previewed && journal->cancel != NULL) {
                journal->cancel(event, journal->context);
            }
            journal->stats.cancelled++;
            continue;
        }
        if (event->live_in_timeline && !event->confirmed &&
            event->policy == MDKR_ROLLBACK_EFFECT_REVERSIBLE &&
            !event->previewed) {
            event->previewed = true;
            if (journal->preview != NULL) {
                journal->preview(event, journal->context);
            }
        }
        if (write_index != read_index) journal->entries[write_index] = *event;
        write_index++;
    }
    journal->count = write_index;
}

void mdkr_rollback_events_confirm_through(
    MdkrRollbackEventJournal *journal, uint32_t tick) {
    unsigned index;
    unsigned write_index = 0u;
    if (journal == NULL) return;
    for (index = 0u; index < journal->count; index++) {
        MdkrRollbackEvent *event = &journal->entries[index];
        if (event->confirmed || !event->live_in_timeline || event->id.tick > tick) continue;
        event->confirmed = true;
        if (event->policy == MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY &&
            journal->commit != NULL) journal->commit(event, journal->context);
        journal->stats.committed++;
    }
    /* Confirmed history is outside the legal rollback window. Retaining it
     * would turn a long match into an eventual fixed-capacity failure without
     * adding any dedupe or cancellation value. */
    for (index = 0u; index < journal->count; index++) {
        if (journal->entries[index].confirmed) continue;
        if (write_index != index) {
            journal->entries[write_index] = journal->entries[index];
        }
        write_index++;
    }
    journal->count = write_index;
}

void mdkr_rollback_events_force_clear(MdkrRollbackEventJournal *journal) {
    unsigned index;
    if (journal == NULL) return;
    for (index = 0u; index < journal->count; index++) {
        MdkrRollbackEvent *event = &journal->entries[index];
        if (event->previewed && event->live_in_timeline && journal->cancel != NULL) {
            journal->cancel(event, journal->context);
        }
    }
    journal->count = 0u;
    journal->resimulating = false;
}

bool mdkr_rollback_events_host_io_allowed(
    MdkrRollbackEventJournal *journal, bool progression_write) {
    if (journal == NULL) return false;
    if (journal->resimulating || (journal->online_match && progression_write)) {
        journal->stats.forbidden_io++;
        return false;
    }
    return true;
}
