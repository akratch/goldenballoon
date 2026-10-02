/*
 * test_gfx_texpack_upload_budget.c -- the replacement-upload budget and its
 * warm-load window (platform/fast3d/gfx_texpack_upload_budget.h), ROM- and
 * GPU-free.
 *
 * T12b's fix round 2 stopped warm level loads and pack toggles from drawing
 * the original textures for several frames: right after either, re-uploads of
 * replacements the store already holds go up outside the per-walk budget.
 * Nothing checked that afterwards (T12d). These cases fail if a resident
 * re-upload is budgeted -- deferred, or charged -- on the walks after a level
 * load or a toggle, and also if the window leaks: newly decoded textures stay
 * budgeted inside it, and everything is budgeted again once it closes.
 */
#include "gfx_texpack_upload_budget.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

#define MS UINT64_C(1000000)

/* What a walk does with `count` resident replacements of `cost` each: the
 * number deferred, and the number uploaded outside the budget. */
typedef struct WalkResult {
    int deferred;
    int warm;
    int spent;
} WalkResult;

static WalkResult draw(GfxTexpackUploadBudget *budget, int count,
                       uint64_t cost, int resident) {
    WalkResult result = { 0, 0, 0 };
    int        index;
    for (index = 0; index < count; index++) {
        switch (gfx_texpack_budget_decide(budget, true, resident != 0)) {
        case GFX_TEXPACK_UPLOAD_DEFER: result.deferred++; break;
        case GFX_TEXPACK_UPLOAD_WARM:  result.warm++;     break;
        case GFX_TEXPACK_UPLOAD_SPEND:
            gfx_texpack_budget_spend(budget, cost);
            result.spent++;
            break;
        case GFX_TEXPACK_UPLOAD_FREE:  break;
        }
    }
    return result;
}

/* 1. Away from any boundary the budget holds: the first upload always goes
 * (nothing starves), more go while under 2 ms, and the rest are deferred. */
static void test_budget_outside_the_window(void) {
    GfxTexpackUploadBudget budget;
    WalkResult             walk;

    memset(&budget, 0, sizeof budget);
    gfx_texpack_budget_begin_walk(&budget, 0);
    walk = draw(&budget, 20, MS, 1);
    expect(walk.spent == 2 && walk.deferred == 18 && walk.warm == 0,
           "20 resident 1 ms uploads: two fit the 2 ms budget, 18 wait");
    gfx_texpack_budget_begin_walk(&budget, 0);
    walk = draw(&budget, 1, 5 * MS, 1);
    expect(walk.spent == 1 && walk.deferred == 0,
           "the first upload of a walk goes even when it alone is over budget");
    expect(gfx_texpack_budget_decide(&budget, true, true) ==
               GFX_TEXPACK_UPLOAD_DEFER,
           "and the next one waits");
    expect(gfx_texpack_budget_decide(&budget, false, true) ==
               GFX_TEXPACK_UPLOAD_FREE,
           "a loader with no off-frame work has no budget at all");
}

/* 2. After a level load, resident replacements go up at once and spend
 * nothing, for exactly GFX_TEXPACK_WARM_WALKS walks; then the budget is back. */
static void test_a_level_load_opens_the_window(void) {
    GfxTexpackUploadBudget budget;
    WalkResult             walk;
    int                    index;
    int                    all_warm = 1;

    memset(&budget, 0, sizeof budget);
    gfx_texpack_budget_begin_walk(&budget, 0);
    gfx_texpack_budget_level_boundary(&budget);
    for (index = 0; index < GFX_TEXPACK_WARM_WALKS; index++) {
        gfx_texpack_budget_begin_walk(&budget, 0);
        walk = draw(&budget, 124, MS, 1);
        if (walk.warm != 124 || walk.deferred != 0 || budget.walk_ns != 0) {
            all_warm = 0;
        }
    }
    expect(all_warm,
           "for ten walks after a level load, 124 resident re-uploads all go "
           "up at once and spend no budget");
    gfx_texpack_budget_begin_walk(&budget, 0);
    walk = draw(&budget, 20, MS, 1);
    expect(walk.warm == 0 && walk.spent == 2 && walk.deferred == 18,
           "the eleventh walk is budgeted again");
}

/* 3. A pack toggle -- the override generation moving -- opens it the same
 * way, seen by the next walk. */
static void test_a_toggle_opens_the_window(void) {
    GfxTexpackUploadBudget budget;
    WalkResult             walk;
    int                    index;

    /* A game that has been running at generation 7 for a while. */
    memset(&budget, 0, sizeof budget);
    for (index = 0; index <= GFX_TEXPACK_WARM_WALKS; index++) {
        gfx_texpack_budget_begin_walk(&budget, 7);
    }
    walk = draw(&budget, 20, MS, 1);
    expect(walk.deferred == 18, "control: no window without a toggle");
    gfx_texpack_budget_begin_walk(&budget, 8);
    walk = draw(&budget, 20, MS, 1);
    expect(walk.warm == 20 && walk.deferred == 0,
           "the walk after a toggle re-uploads every resident replacement");
    for (index = 0; index < GFX_TEXPACK_WARM_WALKS; index++) {
        gfx_texpack_budget_begin_walk(&budget, 8);
    }
    walk = draw(&budget, 20, MS, 1);
    expect(walk.warm == 0 && walk.deferred == 18,
           "it closes after ten walks with no further toggle");
    gfx_texpack_budget_begin_walk(&budget, 9);
    walk = draw(&budget, 20, MS, 1);
    expect(walk.warm == 20, "and the toggle back reopens it");
}

/* 4. Inside the window, only RESIDENT pixels are exempt: a texture that has
 * just finished decoding is new work and stays inside the budget, sharing it
 * with nothing the warm uploads spent. */
static void test_new_decodes_stay_budgeted_in_the_window(void) {
    GfxTexpackUploadBudget budget;
    WalkResult             warm;
    WalkResult             fresh;

    memset(&budget, 0, sizeof budget);
    gfx_texpack_budget_level_boundary(&budget);
    gfx_texpack_budget_begin_walk(&budget, 0);
    warm = draw(&budget, 50, MS, 1);
    fresh = draw(&budget, 20, MS, 0);
    expect(warm.warm == 50 && fresh.spent == 2 && fresh.deferred == 18,
           "new decodes are budgeted in the window, and warm uploads did not "
           "use that budget up");
}

/* 5. A level load inside the window restarts it rather than being lost. */
static void test_a_second_boundary_restarts_the_window(void) {
    GfxTexpackUploadBudget budget;
    WalkResult             walk;
    int                    index;

    memset(&budget, 0, sizeof budget);
    gfx_texpack_budget_level_boundary(&budget);
    for (index = 0; index < GFX_TEXPACK_WARM_WALKS - 1; index++) {
        gfx_texpack_budget_begin_walk(&budget, 0);
    }
    gfx_texpack_budget_level_boundary(&budget);
    for (index = 0; index < GFX_TEXPACK_WARM_WALKS; index++) {
        gfx_texpack_budget_begin_walk(&budget, 0);
    }
    walk = draw(&budget, 20, MS, 1);
    expect(walk.warm == 20,
           "a level load late in the window gives the new scene its own ten");
}

int main(void) {
    test_budget_outside_the_window();
    test_a_level_load_opens_the_window();
    test_a_toggle_opens_the_window();
    test_new_decodes_stay_budgeted_in_the_window();
    test_a_second_boundary_restarts_the_window();
    printf(failures ? "FAILURES: %d\n"
                    : "all texture-pack upload budget assertions passed\n",
           failures);
    return failures ? 1 : 0;
}
