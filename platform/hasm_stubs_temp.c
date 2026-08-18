/**
 * hasm_stubs_temp.c — native instrumentation and non-gameplay diagnostics that
 * once shared a file with temporary hand-asm fallbacks.
 *
 * Required gameplay providers are deliberately absent here. obj_animate,
 * obj_shade_fast, calc_dynamic_lighting_for_object_2, gzip_inflate_block, and
 * racer_update_plane (func_80049794) all have strong production definitions; omitting one must fail
 * the link rather than select a successful no-op.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
/* object_model_test_collisions (func_80017A18) -- THE STUB THAT USED TO LIVE
 * HERE IS GONE. Wave "objcoll"
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
        /* Substring, not first-character: the wedge gate needs per-hit frames in
         * BOTH of its arms, and one of those arms is `norecover`. A single
         * variable cannot carry two first characters, so the values compose --
         * MDKR_OBJCOLL=norecover+trace selects both. `legacy` and `norecover`
         * read the same way below for the same reason. */
        cached = (e != NULL && strstr(e, "trace") != NULL) ? 1 : 0;
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
        cached = (e != NULL && strstr(e, "legacy") != NULL) ? 1 : 0;
    }
    return cached;
}

/* MDKR_OBJCOLL=norecover disables the embedded-point recovery pass added to
 * func_80017A18() -- the pass that ejects a wheel point which arrived INSIDE a
 * collision mesh, mirroring resolve_collisions()' Step 2 for terrain. Same
 * one-binary-two-arms convention as `legacy` above and MDKR_COLLTEX=legacy: the
 * fix's failure mode is silence (a point that is never ejected is simply never
 * mentioned again by the one-sided facet walk), so the check needs the
 * un-recovered arm from the same binary to have anything to compare against.
 *
 * A new VALUE on an existing seam, deliberately not a new variable. Off unless
 * set, and composable with `trace` (see mdkr_objcoll_trace above), which the
 * wedge gate needs because it asserts on hit frames in this arm too. */
int mdkr_objcoll_norecover(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_OBJCOLL");
        cached = (e != NULL && strstr(e, "norecover") != NULL) ? 1 : 0;
    }
    return cached;
}

/* MDKR_DOORCARRY=legacy restores the authored carry-frame pairing for RISING
 * sliding doors in collision_objectmodel() -- the pairing that welded a door's
 * vertical step onto laterally blocked racer points and launched the kart to
 * the alcove ceiling on the post-race lobby return (issue #41). Same
 * one-binary-two-arms convention as MDKR_OBJCOLL above: the fix's failure mode
 * is an ordinary-looking drive with nothing on stderr, so
 * tests/check_postrace_door_fling.py needs the launching arm from the same
 * binary to prove its altitude bound still bites. Inert unless set. */
int mdkr_doorcarry_legacy(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_DOORCARRY");
        cached = (e != NULL && strstr(e, "legacy") != NULL) ? 1 : 0;
    }
    return cached;
}

/* Count of points the recovery pass ejected, reported at headless exit as
 * "[OBJRECOVER] points=N iters=M embedded=K". Unconditional, like [OBJCOLL]'s
 * own summary, so a check can read it without turning tracing on. `points`
 * counts distinct (point, tick, object) recoveries; `iters` counts push-out
 * iterations, so iters/points is the mean iteration depth -- the number that
 * says whether the pass is converging in one step or grinding toward its bail.
 * Measured on the wedge gate's seeded contact: 1 point, 1 iteration. */
static unsigned long s_objRecoverIters;
static unsigned long s_objRecoverPoints;

void mdkr_objcoll_recovered(int iterations) {
    s_objRecoverPoints++;
    s_objRecoverIters += (unsigned long)(iterations > 0 ? iterations : 0);
}

unsigned long mdkr_objcoll_recover_iters(void)  { return s_objRecoverIters; }
unsigned long mdkr_objcoll_recover_points(void) { return s_objRecoverPoints; }

/* ---- wedge gate: the versioned internal test capability ------------------ *
 * Everything below is inert unless
 *   MDKR_INTERNAL_TEST_TOKEN=mdkr64-objcoll-wedge-v1
 * is present, the same versioned-capability pattern present_sched.c uses for
 * its replay arms. Two hooks share it:
 *
 *   - the arrival probe in func_80017A18(), which answers "did a wheel point
 *     start this tick already inside a collision mesh?" and is the only
 *     arm-independent observable for the defect;
 *   - MDKR_TEST_OBJCOLL_EMBED=<frame>, which SEEDS that state deterministically.
 *
 * The seed exists because a drive route cannot reach the state on demand: four
 * measured approach angles into the hub's Dino Domain door leaf (head-on and
 * three glancing) all separated correctly, so the embedded state is reachable in
 * play -- the reporter reached it -- but not on a schedule a gate can assert on.
 * Seeding it is what makes the recovery pass falsifiable rather than a change
 * nothing measures. It is a TEST MUTATION and it says so: it moves wheel points,
 * which no production path here does.
 */
int mdkr_objcoll_wedge_test_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *token = getenv("MDKR_INTERNAL_TEST_TOKEN");
        cached = (token != NULL && strcmp(token, "mdkr64-objcoll-wedge-v1") == 0) ? 1 : 0;
    }
    return cached;
}

/* Per-tick embedded-point count, traced only on ticks where it is nonzero (the
 * [COLPEAK] convention) plus a headless-exit total. A sustained nonzero run is
 * the wedge; a single tick followed by zeros is the recovery working. */
static unsigned long s_objEmbedPoints;
static int           s_objEmbedFrame = -1;
static int           s_objEmbedInFrame;

void mdkr_objcoll_embed_probe(int inside) {
    if (!inside) {
        return;
    }
    s_objEmbedPoints++;
    if (g_frameCounter != s_objEmbedFrame) {
        s_objEmbedFrame = g_frameCounter;
        s_objEmbedInFrame = 0;
    }
    s_objEmbedInFrame++;
    fprintf(stderr, "[TRACE] [OBJEMB] frame=%d embedded=%d total=%lu\n",
            g_frameCounter, s_objEmbedInFrame, s_objEmbedPoints);
}

unsigned long mdkr_objcoll_embed_total(void) { return s_objEmbedPoints; }

/* MDKR_TEST_OBJCOLL_EMBED=<frame>: from <frame> onward, once, displace the
 * racer's target AND origin wheel points toward the collision-meshed object's
 * centre by MDKR_TEST_OBJCOLL_EMBED_DEPTH units. Returns the depth to apply, or
 * 0 for "do nothing" -- which is every frame unless armed.
 *
 * The default is 8, and the ceiling is not a matter of taste: the recovery pass
 * only claims penetrations shallower than one collision radius plus three units,
 * so a seed deeper than that lands OUTSIDE the band and is deliberately ignored
 * as "far side of the object" rather than ejected. Measured: depth 24 produced
 * zero probe readings for exactly that reason, which is the containment working,
 * not the seed failing. Keep it well inside the band. */
float mdkr_objcoll_embed_seed_depth(void) {
    static int   cached = -1;
    static int   frame;
    static float depth;
    static int   fired;

    if (!mdkr_objcoll_wedge_test_enabled()) {
        return 0.0f;
    }
    if (cached < 0) {
        const char *e = getenv("MDKR_TEST_OBJCOLL_EMBED");
        const char *d = getenv("MDKR_TEST_OBJCOLL_EMBED_DEPTH");
        frame  = (e != NULL && e[0] != '\0') ? atoi(e) : -1;
        depth  = (d != NULL && d[0] != '\0') ? (float)atof(d) : 8.0f;
        cached = 1;
    }
    if (frame < 0 || fired || g_frameCounter < frame) {
        return 0.0f;
    }
    fired = 1;
    fprintf(stderr, "[TRACE] [OBJEMB] SEED frame=%d depth=%.1f\n", g_frameCounter, (double)depth);
    return depth;
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
