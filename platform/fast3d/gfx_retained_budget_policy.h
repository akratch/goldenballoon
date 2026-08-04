/* Deterministic retained-arena budget policy. Environment lookup and
 * process-lifetime caching belong to gfx_retained_task.c; this helper only
 * validates the supplied text, so hostile values can be tested ROM-free. */
#ifndef MDKR_GFX_RETAINED_BUDGET_POLICY_H
#define MDKR_GFX_RETAINED_BUDGET_POLICY_H

#include <stddef.h>

#define GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT (16u * 1024u * 1024u)
#define GFX_RETAINED_ARENA_COPY_BUDGET_MAX (16u * 1024u * 1024u)

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
    return parsed;
}

#endif /* MDKR_GFX_RETAINED_BUDGET_POLICY_H */
