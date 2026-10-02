/* Per-walk budget for uploading pack replacement textures (issue #66), and
 * the warm-load window that exempts already-decoded ones from it.
 *
 * gfx_pc_dkr.c owns one of these and drives it from the display-list walk;
 * the rules live here, free of the renderer, so a unit test can hold them to
 * account without a GPU (tests/test_gfx_texpack_upload_budget.c).
 *
 * The budget. Installing textures a few per frame bounds the uploads of NEW
 * replacements, but not the re-uploads of resident ones: a level load
 * invalidates the renderer's texture cache (arena reuse), and a warm return
 * to a course re-uploaded every replacement it draws in its first frame --
 * 124 of them, 33.7 MiB, 4.7 ms on WebGPU and 10.2 ms on GL in the T12a
 * measurement. So replacement uploads share one per-walk time budget; past
 * it a replacement is drawn from ROM this walk and refilled in place on a
 * later one. At least one is uploaded per walk, so nothing starves. A loader
 * with no off-frame work (INLINE, or no pack) has no budget: the 1.7
 * behaviour.
 *
 * The warm-load window. Right after a level load or a pack toggle the
 * renderer has forgotten its entries, so the scene's replacements are
 * uploaded again as they are drawn. Deferring THOSE showed the original
 * textures for several frames on every warm level load -- 498 of 651
 * deferrals over the T12a route -- for pixels the store already held. So for
 * the first GFX_TEXPACK_WARM_WALKS real walks after either event, uploads of
 * RESIDENT replacements (a miss the store answered with pixels, or a refill
 * of an entry deferred while resident) are outside the budget and do not
 * spend it. Newly decoded textures (a pending entry's refill) stay budgeted.
 * On the T12a route the re-uploads after a boundary land on its 2nd-4th
 * presented frames; ten walks covers that with margin. */
#ifndef MDKR_GFX_TEXPACK_UPLOAD_BUDGET_H
#define MDKR_GFX_TEXPACK_UPLOAD_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

#define GFX_TEXPACK_UPLOAD_BUDGET_NS UINT64_C(2000000)
#define GFX_TEXPACK_WARM_WALKS 10

typedef struct GfxTexpackUploadBudget {
    uint64_t walk_ns;           /* budgeted upload time spent this walk */
    uint32_t walk_uploads;      /* ...and how many uploads spent it */
    int      warm_walks;        /* walks left in the warm-load window */
    bool     walk_warm;         /* this walk is inside the window */
    uint32_t seen_generation;   /* the override generation last seen */
} GfxTexpackUploadBudget;

/* How one replacement upload goes this walk. */
typedef enum GfxTexpackUpload {
    /* Past this walk's budget: draw the original now, refill later. */
    GFX_TEXPACK_UPLOAD_DEFER = 0,
    /* Upload now, and charge its time to the budget. */
    GFX_TEXPACK_UPLOAD_SPEND,
    /* Upload now, outside the budget: a resident replacement inside the
     * warm-load window. */
    GFX_TEXPACK_UPLOAD_WARM,
    /* Upload now: this loader has no budget at all. */
    GFX_TEXPACK_UPLOAD_FREE
} GfxTexpackUpload;

/* A level load: the window opens for the next GFX_TEXPACK_WARM_WALKS walks. */
static inline void gfx_texpack_budget_level_boundary(
    GfxTexpackUploadBudget *budget) {
    budget->warm_walks = GFX_TEXPACK_WARM_WALKS;
}

/* Once per real walk, before any bind: a fresh budget, and the window opened
 * if the override generation moved (a pack toggle) since the last walk. */
static inline void gfx_texpack_budget_begin_walk(
    GfxTexpackUploadBudget *budget, uint32_t override_generation) {
    budget->walk_ns = 0;
    budget->walk_uploads = 0;
    if (override_generation != budget->seen_generation) {
        budget->seen_generation = override_generation;
        budget->warm_walks = GFX_TEXPACK_WARM_WALKS;
    }
    budget->walk_warm = budget->warm_walks > 0;
    if (budget->warm_walks > 0) budget->warm_walks--;
}

/* The decision for one replacement upload. `loader_budgeted`: the store reads
 * and decodes off the frame (WORKER or BUDGET), so uploads are budgeted at
 * all. `resident`: the store already held these pixels before this walk asked
 * (a miss it answered, or a refill of an entry deferred while resident), as
 * against pixels it has just finished decoding (a pending entry's refill). */
static inline GfxTexpackUpload gfx_texpack_budget_decide(
    const GfxTexpackUploadBudget *budget, bool loader_budgeted,
    bool resident) {
    if (!loader_budgeted) return GFX_TEXPACK_UPLOAD_FREE;
    if (resident && budget->walk_warm) return GFX_TEXPACK_UPLOAD_WARM;
    if (budget->walk_uploads == 0 ||
        budget->walk_ns < GFX_TEXPACK_UPLOAD_BUDGET_NS) {
        return GFX_TEXPACK_UPLOAD_SPEND;
    }
    return GFX_TEXPACK_UPLOAD_DEFER;
}

/* After an upload decided as SPEND: what it took. */
static inline void gfx_texpack_budget_spend(GfxTexpackUploadBudget *budget,
                                            uint64_t ns) {
    budget->walk_ns += ns;
    budget->walk_uploads++;
}

#endif /* MDKR_GFX_TEXPACK_UPLOAD_BUDGET_H */
