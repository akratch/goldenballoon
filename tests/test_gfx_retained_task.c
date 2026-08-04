#include "gfx_retained_task.h"
#include "gfx_retained_budget_policy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void test_arena_budget_parse_policy(void) {
    expect("valid low budget parses exactly",
           gfx_retained_arena_copy_budget_parse("1024") == 1024u);
    expect("empty and absent budgets use the production default",
           gfx_retained_arena_copy_budget_parse("") ==
               GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT &&
               gfx_retained_arena_copy_budget_parse(NULL) ==
                   GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT);
    expect("malformed budget fails closed to the production default",
           gfx_retained_arena_copy_budget_parse("16MiB") ==
               GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT);
    expect("over-budget and numeric-overflow inputs fail closed",
           gfx_retained_arena_copy_budget_parse("16777217") ==
               GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT &&
               gfx_retained_arena_copy_budget_parse(
                   "184467440737095516160000") ==
                   GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT);
}

static void test_immutable_transaction(void) {
    uint8_t arena[256];
    uint8_t external[12];
    uint8_t expected_external[12];
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS] = { 0 };
    GfxRetainedTaskView view;
    const void *dependency = NULL;
    const uint64_t tick = UINT64_C(73);
    size_t index;
    size_t dependency_room = 0u;
    GfxRetainedTaskStats stats;

    for (index = 0u; index < sizeof(arena); index++) {
        arena[index] = (uint8_t)(index ^ 0x5au);
    }
    for (index = 0u; index < sizeof(external); index++) {
        external[index] = (uint8_t)(0xa0u + index);
    }
    memcpy(expected_external, external, sizeof(external));
    segments[0] = (uintptr_t)(arena + 32);
    segments[1] = (uintptr_t)external;

    expect("capture begins", gfx_retained_task_capture_begin(
        tick, arena, sizeof(arena)));
    expect("arena dependency is covered", gfx_retained_task_capture_dependency(
        arena + 40, arena + 40, 16));
    expect("external dependency is copied",
           gfx_retained_task_capture_dependency(
               external, external, sizeof(external)));
    expect("commit succeeds", gfx_retained_task_capture_commit(
        arena + 64, segments));

    memset(arena, 0xcc, sizeof(arena));
    memset(external, 0xdd, sizeof(external));

    expect("wrong tick rejects",
           !gfx_retained_task_acquire(tick + 1u, &view));
    expect("exact tick acquires", gfx_retained_task_acquire(tick, &view));
    expect("arena has private ownership", view.retained_arena != arena);
    expect("display list rebased",
           view.display_list == (const uint8_t *)view.retained_arena + 64);
    expect("segment rebased",
           view.segments[0] == (uintptr_t)view.retained_arena + 32u);
    expect("external segment identity retained",
           view.segments[1] == (uintptr_t)external);
    expect("arena image survived live mutation",
           ((const uint8_t *)view.retained_arena)[91] == (uint8_t)(91 ^ 0x5a));
    expect("retained address maps to original identity",
           gfx_retained_task_original_address(
               (const uint8_t *)view.retained_arena + 91) == arena + 91);
    expect("external dependency lookup succeeds",
           gfx_retained_task_lookup_dependency(
               external, sizeof(external), &dependency));
    expect("external bytes survived live mutation",
           dependency != NULL &&
               memcmp(dependency, expected_external, sizeof(external)) == 0);
    expect("retained external bytes map to original identity",
           gfx_retained_task_original_address(dependency) == external &&
               gfx_retained_task_original_address(
                   (const uint8_t *)dependency + 5) == external + 5);
    dependency = NULL;
    expect("external interior span lookup succeeds",
           gfx_retained_task_lookup_dependency(
               external + 3, 4, &dependency));
    expect("external interior span preserves bytes and identity",
           dependency != NULL &&
               memcmp(dependency, expected_external + 3, 4) == 0 &&
               gfx_retained_task_original_address(dependency) == external + 3);
    expect("external retained span reports bounded readable room",
           gfx_retained_task_dependency_room(dependency, &dependency_room) &&
               dependency_room == sizeof(external) - 3u);
    expect("unretained address has no private readable room",
           !gfx_retained_task_dependency_room(arena, &dependency_room));

    {
        uint32_t raw = (uint32_t)(uintptr_t)(arena + 101);
        uint32_t flipped = raw ^ UINT32_C(0x80000000);
        expect("raw arena token resolves into private image",
               gfx_retained_task_resolve_arena_token(raw) ==
                   (uint8_t *)view.retained_arena + 101);
        expect("flipped arena token resolves into private image",
               gfx_retained_task_resolve_arena_token(flipped) ==
                   (uint8_t *)view.retained_arena + 101);
    }
    memset(&stats, 0, sizeof(stats));
    gfx_retained_task_get_stats(&stats);
    expect("resolution telemetry names both immutable sources",
           stats.arena_resolutions == 2u &&
               stats.external_resolutions == 2u);
}

static void test_failed_publication_retains_last_complete(void) {
    uint8_t arena[64];
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS] = { 0 };
    GfxRetainedTaskView before;
    GfxRetainedTaskView after;

    memset(arena, 0x31, sizeof(arena));
    expect("baseline begin", gfx_retained_task_capture_begin(
        UINT64_C(101), arena, sizeof(arena)));
    expect("baseline commit", gfx_retained_task_capture_commit(
        arena + 8, segments));
    expect("baseline acquire", gfx_retained_task_acquire(101, &before));

    expect("next begin", gfx_retained_task_capture_begin(
        UINT64_C(102), arena, sizeof(arena)));
    expect("cross-arena dependency fails transaction",
           !gfx_retained_task_capture_dependency(
               arena + sizeof(arena) - 4u,
               arena + sizeof(arena) - 4u, 8u));
    expect("failed dependency prevents commit",
           !gfx_retained_task_capture_commit(arena + 8, segments));
    expect("failed publication keeps prior task",
           gfx_retained_task_acquire(101, &after));
    expect("prior retained bytes unchanged",
           before.retained_arena == after.retained_arena &&
               ((const uint8_t *)after.retained_arena)[0] == 0x31);
    expect("failed task was never visible",
           !gfx_retained_task_acquire(102, &after));
}

static void test_arena_budget_holds_last_complete(void) {
    uint8_t arena[64];
    uintptr_t segments[GFX_RETAINED_TASK_SEGMENTS] = { 0 };
    GfxRetainedTaskView before;
    GfxRetainedTaskView after;
    GfxRetainedTaskStats stats;
    uint8_t *oversized;
    size_t oversized_size;
    uint64_t copied_before;

    memset(arena, 0x7a, sizeof(arena));
    expect("budget baseline begins", gfx_retained_task_capture_begin(
        UINT64_C(201), arena, sizeof(arena)));
    expect("budget baseline commits", gfx_retained_task_capture_commit(
        arena + 4, segments));
    expect("budget baseline acquires", gfx_retained_task_acquire(201, &before));
    gfx_retained_task_get_stats(&stats);
    expect("production replay budget covers the complete DKR arena",
           stats.arena_copy_budget ==
               GFX_RETAINED_ARENA_COPY_BUDGET_DEFAULT);
    oversized_size = stats.arena_copy_budget + 1u;
    oversized = (uint8_t *)malloc(oversized_size);
    expect("oversized test arena allocates", oversized != NULL);
    if (oversized == NULL) {
        return;
    }
    memset(oversized, 0x19, oversized_size);
    copied_before = stats.arena_bytes;
    expect("over-budget capture is refused before copying",
           !gfx_retained_task_capture_begin(202, oversized, oversized_size));
    expect("over-budget capture cannot publish",
           !gfx_retained_task_capture_commit(oversized, segments));
    expect("over-budget capture preserves exact previous task",
           gfx_retained_task_acquire(201, &after) &&
               after.retained_arena == before.retained_arena &&
               after.display_list == before.display_list);
    gfx_retained_task_get_stats(&stats);
    expect("budget rejection has explicit telemetry",
           stats.arena_budget_rejections > 0u &&
               stats.arena_bytes == copied_before &&
               stats.arena_peak <= stats.arena_copy_budget &&
               stats.resident_peak <= stats.arena_copy_budget * 2u);
    free(oversized);
}

int main(void) {
    test_arena_budget_parse_policy();
    test_immutable_transaction();
    test_failed_publication_retains_last_complete();
    test_arena_budget_holds_last_complete();
    gfx_retained_task_shutdown();
    if (failures != 0) {
        fprintf(stderr, "gfx retained task: %d failure(s)\n", failures);
        return 1;
    }
    puts("gfx retained task: PASS");
    return 0;
}
