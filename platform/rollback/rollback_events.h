#ifndef MDKR_ROLLBACK_EVENTS_H
#define MDKR_ROLLBACK_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_EVENT_CAPACITY 512u

typedef enum MdkrRollbackEffectPolicy {
    MDKR_ROLLBACK_EFFECT_REVERSIBLE = 0,
    MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY
} MdkrRollbackEffectPolicy;

typedef struct MdkrRollbackEventId {
    uint32_t tick;
    uint32_t emitter;
    uint16_t ordinal;
    uint16_t kind;
} MdkrRollbackEventId;

typedef struct MdkrRollbackEvent {
    MdkrRollbackEventId id;
    uint32_t value;
    uint8_t policy;
    bool previewed;
    bool confirmed;
    bool live_in_timeline;
} MdkrRollbackEvent;

typedef void (*MdkrRollbackEffectFn)(
    const MdkrRollbackEvent *event, void *context);

typedef struct MdkrRollbackEventStats {
    uint64_t emitted;
    uint64_t duplicates;
    uint64_t committed;
    uint64_t cancelled;
    uint64_t overflows;
    uint64_t forbidden_io;
} MdkrRollbackEventStats;

typedef struct MdkrRollbackEventJournal {
    MdkrRollbackEvent entries[MDKR_ROLLBACK_EVENT_CAPACITY];
    unsigned count;
    bool resimulating;
    bool online_match;
    MdkrRollbackEffectFn preview;
    MdkrRollbackEffectFn commit;
    MdkrRollbackEffectFn cancel;
    void *context;
    MdkrRollbackEventStats stats;
} MdkrRollbackEventJournal;

void mdkr_rollback_events_init(
    MdkrRollbackEventJournal *journal, bool online_match,
    MdkrRollbackEffectFn preview, MdkrRollbackEffectFn commit,
    MdkrRollbackEffectFn cancel, void *context);
void mdkr_rollback_events_set_resimulating(
    MdkrRollbackEventJournal *journal, bool enabled);
bool mdkr_rollback_events_emit(
    MdkrRollbackEventJournal *journal, MdkrRollbackEventId id,
    MdkrRollbackEffectPolicy policy, uint32_t value);
void mdkr_rollback_events_begin_rewrite(
    MdkrRollbackEventJournal *journal, uint32_t from_tick);
void mdkr_rollback_events_end_rewrite(MdkrRollbackEventJournal *journal);
void mdkr_rollback_events_confirm_through(
    MdkrRollbackEventJournal *journal, uint32_t tick);
void mdkr_rollback_events_force_clear(MdkrRollbackEventJournal *journal);
bool mdkr_rollback_events_host_io_allowed(
    MdkrRollbackEventJournal *journal, bool progression_write);

#ifdef __cplusplus
}
#endif
#endif
