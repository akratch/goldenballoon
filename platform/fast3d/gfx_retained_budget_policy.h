/* Deterministic retained-arena budget policy. Environment lookup and
 * process-lifetime caching belong to gfx_retained_task.c; this helper only
 * validates the supplied text, so hostile values can be tested ROM-free. */
#ifndef MDKR_GFX_RETAINED_BUDGET_POLICY_H
#define MDKR_GFX_RETAINED_BUDGET_POLICY_H

#include <stddef.h>

#define GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT (16u * 1024u * 1024u)
#define GFX_RETAINED_ARENA_COPY_BUDGET_MAX (16u * 1024u * 1024u)
/*
 * Smallest budget that is still a budget. Below this no capture can ever be
 * admitted, so the value does not shed load -- it turns replay off, and it does
 * so through the path that treats rejection as expected (gfx_retained_task.c's
 * commit returns false WITHOUT counting a failure), which means the run reports
 * zero capture failures while producing no retained tasks at all. "0" is the
 * spelling that reaches that state most easily. Such values fail closed to the
 * production default, exactly like every other input this parser refuses.
 */
#define GFX_RETAINED_ARENA_COPY_BUDGET_MIN (1024u)

_Static_assert(GFX_RETAINED_ARENA_COPY_BUDGET_MIN <=
                   GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT &&
               GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT <=
                   GFX_RETAINED_ARENA_COPY_BUDGET_MAX,
               "retained arena copy budget bounds must be ordered "
               "MIN <= DEFAULT <= MAX");

static inline size_t gfx_retained_arena_copy_budget_parse(const char *value) {
    size_t parsed = 0u;
    const unsigned char *cursor = (const unsigned char *)value;

    if (cursor == NULL || *cursor == '\0') {
        return GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT;
    }
    while (*cursor != '\0') {
        unsigned digit;
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
            return GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT;
        }
        digit = (unsigned)(*cursor - (unsigned char)'0');
        if (parsed > GFX_RETAINED_ARENA_COPY_BUDGET_MAX / 10u ||
            (parsed == GFX_RETAINED_ARENA_COPY_BUDGET_MAX / 10u &&
             digit > GFX_RETAINED_ARENA_COPY_BUDGET_MAX % 10u)) {
            return GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT;
        }
        parsed = parsed * 10u + digit;
        cursor++;
    }
    if (parsed < GFX_RETAINED_ARENA_COPY_BUDGET_MIN) {
        return GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT;
    }
    return parsed;
}

#endif /* MDKR_GFX_RETAINED_BUDGET_POLICY_H */
