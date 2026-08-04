#include "gfx_retained_task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void set_budget_env(const char *value) {
#ifdef _WIN32
    (void)_putenv_s("MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES", value);
#else
    (void)setenv("MDKR_RETAINED_ARENA_COPY_BUDGET_BYTES", value, 1);
#endif
}

int main(void) {
    uint8_t arena[1025];
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS] = { 0 };
    GfxRetainedTaskStats stats;

    gfx_retained_task_get_stats(&stats);
    expect("ctest environment selects constrained budget",
           stats.arena_copy_budget == 1024u);
    set_budget_env("2048");
    gfx_retained_task_get_stats(&stats);
    expect("budget environment is cached for the process",
           stats.arena_copy_budget == 1024u);
    expect("cached constrained budget rejects before arena copy",
           !gfx_retained_task_capture_begin(1u, arena, sizeof(arena)) &&
               !gfx_retained_task_capture_commit(arena, segments));
    gfx_retained_task_get_stats(&stats);
    expect("budget rejection remains observable",
           stats.arena_budget_rejections == 1u && stats.arena_bytes == 0u);
    gfx_retained_task_shutdown();

    if (failures != 0) {
        fprintf(stderr, "gfx retained task budget: %d failure(s)\n", failures);
        return 1;
    }
    puts("gfx retained task budget: PASS");
    return 0;
}
