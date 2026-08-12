/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "platform/rollback/rollback_events.h"

typedef struct Effects { int preview; int commit; int cancel; } Effects;
static void preview(const MdkrRollbackEvent *event, void *context) {
    (void)event; ((Effects *)context)->preview++;
}
static void commit(const MdkrRollbackEvent *event, void *context) {
    (void)event; ((Effects *)context)->commit++;
}
static void cancel(const MdkrRollbackEvent *event, void *context) {
    (void)event; ((Effects *)context)->cancel++;
}

int main(void) {
    Effects effects = {0, 0, 0};
    MdkrRollbackEventJournal journal;
    const MdkrRollbackEventId audio = {10u, 4u, 0u, 1u};
    const MdkrRollbackEventId corrected_audio = {12u, 4u, 1u, 1u};
    const MdkrRollbackEventId save = {11u, 0u, 0u, 2u};
    mdkr_rollback_events_init(&journal, true, preview, commit, cancel, &effects);
    assert(mdkr_rollback_events_emit(&journal, audio,
        MDKR_ROLLBACK_EFFECT_REVERSIBLE, 7u));
    assert(effects.preview == 1);
    assert(mdkr_rollback_events_emit(&journal, audio,
        MDKR_ROLLBACK_EFFECT_REVERSIBLE, 7u));
    assert(effects.preview == 1 && journal.stats.duplicates == 1u);
    assert(mdkr_rollback_events_emit(&journal, save,
        MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY, 99u));
    assert(effects.commit == 0);
    mdkr_rollback_events_begin_rewrite(&journal, 10u);
    assert(!mdkr_rollback_events_host_io_allowed(&journal, false));
    /* Audio disappears on the corrected timeline; save remains, but does not
     * perform host I/O during resimulation. */
    assert(mdkr_rollback_events_emit(&journal, save,
        MDKR_ROLLBACK_EFFECT_CONFIRMED_ONLY, 99u));
    assert(mdkr_rollback_events_emit(&journal, corrected_audio,
        MDKR_ROLLBACK_EFFECT_REVERSIBLE, 8u));
    assert(effects.preview == 1);
    mdkr_rollback_events_end_rewrite(&journal);
    assert(effects.cancel == 1 && effects.preview == 2 && journal.count == 2u);
    mdkr_rollback_events_confirm_through(&journal, 11u);
    assert(effects.commit == 1);
    assert(journal.count == 1u);
    assert(!mdkr_rollback_events_host_io_allowed(&journal, true));
    assert(mdkr_rollback_events_host_io_allowed(&journal, false));
    mdkr_rollback_events_force_clear(&journal);
    assert(journal.count == 0u);
    puts("test_rollback_events: PASS");
    return 0;
}
