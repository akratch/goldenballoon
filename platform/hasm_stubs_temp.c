/**
 * hasm_stubs_temp.c — native instrumentation and non-gameplay diagnostics that
 * once shared a file with temporary hand-asm fallbacks.
 *
 * Required gameplay providers are deliberately absent here. obj_animate,
 * obj_shade_fast, calc_dynamic_lighting_for_object_2, gzip_inflate_block, and
 * func_80049794 all have strong production definitions; omitting one must fail
 * the link rather than select a successful no-op.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "structs.h"
#include "objects.h"
#include "racer.h"
#include "gzip.h"

#ifdef _WIN32
/* PE/COFF has no true weak definitions. GCC lowers __attribute__((weak)) to a
 * COFF "weak external" — an UNDEFINED symbol plus a mangled alias
 * (.weak.rmonPrintf.*) — and binutils' ld does not fall back to that alias when
 * nothing else defines the symbol, so the two stubs at the bottom of this file
 * link as `undefined reference to rmonPrintf/__assert`. Neither
 * game/libultra/src/libc/rmonPrintf.c nor game/libultra/src/debug/assert.c is
 * compiled into the target on ANY platform (only libultra/src/audio is), so the
 * override the weak attribute protects against cannot occur; a strong
 * definition is what genuinely links. Keep the attribute on ELF, where it is
 * both meaningful and free. */
#define WEAK
#else
#define WEAK __attribute__((weak))
#endif

/* ---- other still-hand-asm game functions (undecompiled .s) -------------- */
/* func_80017A18 -- THE STUB THAT USED TO LIVE HERE IS GONE. Wave "objcoll"
 * adopted upstream's matched body (decomp commit 9da89ecb) into
 * game/src/objects.c, so the strong definition there is what links now and
 * object-model collision genuinely reports hits.
 *
 * Recorded because the removed comment asserted three things that were all
 * false by the time anyone acted on them, and the next person should not
 * re-derive that: (1) "upstream labels the body NON_EQUIVALENT / there is no
 * ground truth" -- superseded, it is matched; (2) "it does not compile under
 * NATIVE_PORT because collisionFacets is a dkrptr32 token" -- true, and DKR_PTR
 * is the whole fix, two lines; (3) the implied worry that the collision data
 * needs a big-endian swizzle -- it does not, object_models.c generates
 * collisionPlanes/collisionFacets at runtime from vertex cross-products.
 *
 * The counter stays, but it now counts HITS rather than stubbed misses, so
 * [OBJCOLL] remains a live measurement of how much object collision the routes
 * actually exercise. See docs/OPEN_ITEMS.md wave "objcoll". */
static unsigned long s_objCollHits;

/* MDKR_OBJCOLL=trace prints the frame of every hit. Attribution needs this: when
 * a route's outcome changes, "9 hits happened somewhere in 9400 frames" cannot
 * distinguish a collision that directly caused it from one whose 2-unit nudge
 * amplified over the next 4000 frames. Off by default; noisy on hub routes
 * (1730 hits) and near-silent on race tracks (9 on boss 38, 0 on 5/32/15). */
extern int g_frameCounter; /* platform/platform_os.h */

static int mdkr_objcoll_trace(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_OBJCOLL");
        cached = (e != NULL && e[0] == 't') ? 1 : 0;
    }
    return cached;
}

void mdkr_objcoll_hit(void) {
    s_objCollHits++;
    if (mdkr_objcoll_trace()) {
        fprintf(stderr, "[TRACE] [OBJCOLL] hit #%lu frame=%d\n", s_objCollHits, g_frameCounter);
    }
}

/* MDKR_OBJCOLL=legacy makes func_80017A18 return 0 immediately, exactly as the
 * removed WEAK stub did. That is what lets ONE binary drive both arms of the
 * positive control in tests/check_door_blocks.py -- without it, "the door blocks
 * now" is unfalsifiable, because the failure mode is silence (you simply drive
 * through). Same convention as MDKR_COLLTEX=legacy below. */
int mdkr_objcoll_legacy(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_OBJCOLL");
        cached = (e != NULL && e[0] == 'l') ? 1 : 0;
    }
    return cached;
}

/* ---- untextured-collision-batch A/B toggle + probe ----------------------- *
 * game/src/hasm/collision.c's generate_collision_candidates() reaches a batch
 * with textureIndex == 255. The ROM gives it SURFACE_DEFAULT and collides with
 * it; the upstream NON_MATCHING C `continue`d and dropped it. MDKR_COLLTEX=legacy
 * restores the skip so one binary can drive both arms, and the counter below is
 * what shows the case is reached by real level data rather than being
 * theoretical. See the long comment at the use site. */
static unsigned long s_untexturedBatches;

int mdkr_colltex_legacy(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_COLLTEX");
        cached = (e != NULL && e[0] == 'l') ? 1 : 0;
    }
    return cached;
}

/* MDKR_COLLTEX_FORCE=1 pretends every batch is untextured. The real case is not
 * reached by any measured level, so this is what makes a comparison of the two
 * arms mean anything. Deliberately a SEPARATE variable from MDKR_COLLTEX so the
 * forced and legacy arms can be combined -- that pairing is the one that shows
 * the harm, because skipping every batch leaves nothing to stand on. */
int mdkr_colltex_force(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_COLLTEX_FORCE");
        cached = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return cached;
}

void mdkr_coll_untextured(void) {
    s_untexturedBatches++;
}

__attribute__((destructor))
static void mdkr64_colltex_report(void) {
    const char *e = getenv("MDKR_TRACE");
    if (e != NULL && e[0] != '\0' && e[0] != '0') {
        fprintf(stderr, "[TRACE] [COLLTEX] untexturedBatches=%lu legacy=%d\n",
                s_untexturedBatches, mdkr_colltex_legacy());
        fprintf(stderr, "[TRACE] [OBJCOLL] objectmodel_collision_hits=%lu\n",
                s_objCollHits);
        fflush(stderr);
    }
}

/* ---- libultra libc bits not compiled natively -------------------------- */
WEAK void rmonPrintf(const char *format, ...) {
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
}
WEAK void __assert(const char *exp, const char *filename, int line) {
    fprintf(stderr, "[assert] %s:%d: %s\n", filename ? filename : "?", line, exp ? exp : "?");
}
