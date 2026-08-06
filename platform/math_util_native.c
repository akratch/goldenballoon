/**
 * math_util_native.c — strong native implementations for the math_util.s
 * hand-asm routines and their data. Their table, conversion, RNG, and gameplay
 * behavior is covered by the math/runtime gates; a missing or duplicate
 * production provider must be a link error.
 *
 * DKR angle unit: s16 where a full turn is 0x10000 (so 0x4000 == 90 degrees);
 * radians = angle * (2*pi / 65536).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "structs.h"
#include "math_util.h"
#include "platform_os.h"

#define DKR_ANG_TO_RAD (3.14159265358979323846f / 32768.0f)

/* ---- trig ------------------------------------------------------------------ *
 * DKR fixed-point trig amplitude is 0x10000 (65536), NOT 0x7FFF. The real
 * hand-asm sins_s16/coss_s16 (src/hasm/ido/math_util.s) read a u16 sine table
 * whose peak is 0x8000 and then `sll v0,1` (x2) it, so 1.0 maps to 0x10000. All
 * callers assume that scale: matrix builders do `sins_s16(a) * (1.0f/0x10000)`
 * (hasm/math_util.c) and integer callers do `(sins_s16(a) * v) >> 16`. Scaling
 * by 32767 instead halved every sine/cosine, so every rotation matrix's 3x3
 * came out at 0.5x (and products like cos*cos at 0.25x) — shrinking rendered
 * models AND the racer velocity/inverse-transform math (constant crawl speed).
 * 1.0 -> 65536 makes both the float (/0x10000) and integer (>>16) paths exact.
 *
 * The ROM does NOT evaluate a sine, though: it interpolates a 1025-entry
 * quarter-turn table, and the DEFAULT here now reproduces that exactly. See the
 * transcription below. `MDKR_TRIG=libm` restores the old libm approximation for
 * A/B measurement; it is a divergence, not a fallback. */

/* gSineTable — quarter turn, 1025 live entries, peak 0x8000, generated at load
 * time (see the constructor). `game/src/hasm/math_util.c` declares it extern and
 * the hand-asm reads it; nothing else in the tree defines it, because in the ROM
 * it lives in the .data section of game/src/hasm/ido/math_util.s, which this
 * build does not assemble.
 *
 * sins_s16 indexes [0, 1023] and also reads index+1, so 1025 entries are live —
 * exactly the count in the .s. Entry 1024 is 0x8000, i.e. -32768 as s16; the
 * assembly loads the table with `lhu`, so every read below goes through (u16). */
#define SINE_LIVE 1025
s16 gSineTable[SINE_LIVE];

/* MDKR_TRIG=libm -> 1: the pre-fix libm approximation. Set by the constructor
 * (which runs before main and therefore before any caller), so the hot path is a
 * plain load and not a getenv. */
static int s_trigLibm = 0;

/**
 * `XLEAF(sins_s16)` / `LEAF(coss_s16)`, game/src/hasm/ido/math_util.s:2427.
 *
 *     sll  v0, a0, 17           # test bit 14: which half of the half-turn
 *     bgez v0, .first_half
 *     xori a0, 0x7FFF           #   mirror inside the half turn
 *   .first_half:
 *     srl  t2, a0, 3
 *     andi t2, 0x7FE            # byte offset -> entry (a0 >> 4) & 0x3FF
 *     la   v0, gSineTable
 *     addu v0, t2
 *     lhu  t2, 0x2(v0)          # hi = table[idx + 1]
 *     lhu  v0, 0x0(v0)          # lo = table[idx]
 *     andi t1, a0, 0xF          # frac
 *     subu t2, v0               # hi - lo
 *     mul  t2, t1               # (hi - lo) * frac
 *     srl  t2, 3                #   / 8   (the /16 of the lerp, x2 for the scale)
 *     sll  v0, 1                # lo * 2
 *     addu v0, t2
 *     sll  a0, 16
 *     bgez a0, .positive        # test bit 15: second half of the turn
 *     negu v0
 *
 * Read in `.set reorder` terms — the assembler fills the delay slots — so the
 * instruction after each `bgez` runs only when the branch is NOT taken.
 *
 * Every shift that the assembly does with `srl` is done here on an unsigned
 * value, not with `>>` on a signed one: coss_s16 adds 0x4000 to a
 * sign-extended s16, so `a` is genuinely negative for a whole quadrant of
 * inputs, and `srl` vs `sra` is exactly the class of transcription defect the
 * "hasmaudit" wave was looking for.
 */
static s32 dkr_sins_interp(s32 a) {
    s32 idx, frac, lo, hi, v;
    if ((((u32) a) >> 14) & 1) {
        a ^= 0x7FFF;
    }
    idx = (s32) ((((u32) a) >> 4) & 0x3FF);
    frac = a & 0xF;
    lo = (s32) (u16) gSineTable[idx];
    hi = (s32) (u16) gSineTable[idx + 1];
    v = (lo << 1) + (s32) (((u32) ((hi - lo) * frac)) >> 3);
    if ((((u32) a) >> 15) & 1) {
        v = -v;
    }
    return v;
}

/** `XLEAF(sins_2)` (math_util.s:2467) — the same walk with NO interpolation. */
static s32 dkr_sins_step(s32 a) {
    s32 idx, v;
    if ((((u32) a) >> 14) & 1) {
        a ^= 0x7FFF;
    }
    idx = (s32) ((((u32) a) >> 4) & 0x3FF);
    v = (s32) (u16) gSineTable[idx] << 1;
    if ((((u32) a) >> 15) & 1) {
        v = -v;
    }
    return v;
}

s32 sins_s16(s16 angle) {
    if (s_trigLibm) {
        return (s32) (sinf((f32) angle * DKR_ANG_TO_RAD) * 65536.0f);
    }
    return dkr_sins_interp((s32) angle);
}

/* `LEAF(coss_s16)` is `addiu a0, 0x4000` and then a fall-through into
 * sins_s16 — on the 32-bit sign-extended argument, so no s16 wrap happens. */
s32 coss_s16(s16 angle) {
    if (s_trigLibm) {
        return (s32) (cosf((f32) angle * DKR_ANG_TO_RAD) * 65536.0f);
    }
    return dkr_sins_interp((s32) angle + 0x4000);
}

s32 sins_2(s16 angle) {
    if (s_trigLibm) {
        return (s32) (sinf((f32) angle * DKR_ANG_TO_RAD) * 65536.0f);
    }
    return dkr_sins_step((s32) angle);
}

/* `LEAF(sins_f)` / `LEAF(coss_f)` (math_util.s:2380/2411) are literally
 * `jal sins_s16` / `jal coss_s16`, `cvt.s.w`, `mul.s` by 1/0x10000 — so they
 * inherit the table walk rather than evaluating a sine of their own. Calling
 * libm here (which is what shipped) made sins_f and sins_s16 disagree with each
 * other as well as with the ROM. */
f32 sins_f(s16 angle) {
    if (s_trigLibm) {
        return sinf((f32) angle * DKR_ANG_TO_RAD);
    }
    return (f32) sins_s16(angle) * (1.0f / 65536.0f);
}

f32 coss_f(s16 angle) {
    if (s_trigLibm) {
        return cosf((f32) angle * DKR_ANG_TO_RAD);
    }
    return (f32) coss_s16(angle) * (1.0f / 65536.0f);
}

/* ---- A/B hook + probe for the vec3f_rotate_py transposition fix ----------- *
 * MDKR_ROTPY=legacy restores the pre-fix (transposed pitch/yaw) arithmetic in
 * game/src/hasm/math_util.c, so one binary can drive both arms. Cached on first
 * call; no-op unless set. */
int mdkr_rotpy_legacy(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MDKR_ROTPY");
        cached = (e != NULL && e[0] == 'l') ? 1 : 0;
    }
    return cached;
}

/* Reachability probe. The fix is behaviour-neutral for the racer simulation --
 * measured 0 of 359 [PACE] rows changed -- because every call site feeds
 * particles, lights, the lens flare or sprite placement rather than physics. So
 * "did it change the race" cannot show it is reached, and this counts the calls
 * and folds every result into a hash instead. Only armed under MDKR_TRACE. */
static int s_rotpyProbe = -1;
static unsigned long long s_rotpyCalls;
static unsigned long long s_rotpyHash = 1469598103934665603ULL; /* FNV-1a 64 */

static void rotpy_fold(unsigned int bits) {
    int k;
    for (k = 0; k < 4; k++) {
        s_rotpyHash ^= (bits >> (k * 8)) & 0xFF;
        s_rotpyHash *= 1099511628211ULL;
    }
}

void mdkr_rotpy_observe(float x, float y, float z) {
    union { float f; unsigned int u; } c;
    if (s_rotpyProbe < 0) {
        const char *e = getenv("MDKR_TRACE");
        s_rotpyProbe = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (!s_rotpyProbe) {
        return;
    }
    s_rotpyCalls++;
    c.f = x; rotpy_fold(c.u);
    c.f = y; rotpy_fold(c.u);
    c.f = z; rotpy_fold(c.u);
}

__attribute__((destructor))
static void mdkr64_rotpy_report(void) {
    if (s_rotpyProbe > 0) {
        fprintf(stderr, "[TRACE] [ROTPY] calls=%llu hash=0x%016llx legacy=%d\n",
                s_rotpyCalls, s_rotpyHash, mdkr_rotpy_legacy());
        fflush(stderr);
    }
}

/* ---- rotate a direction vector by the upper-left 3x3 of a float matrix ---- */
void mtxf_transform_dir(MtxF *mf, Vec3f *in, Vec3f *out) {
    f32 x = in->f[0], y = in->f[1], z = in->f[2];
    out->f[0] = x * (*mf)[0][0] + y * (*mf)[1][0] + z * (*mf)[2][0];
    out->f[1] = x * (*mf)[0][1] + y * (*mf)[1][1] + z * (*mf)[2][1];
    out->f[2] = x * (*mf)[0][2] + y * (*mf)[1][2] + z * (*mf)[2][2];
}

/* ---- data tables/globals that live in the excluded math_util data .s ---- *
 * gArcTanTable feeds atan2s() (a real C body in math_util.c). Rebuilt at load
 * time rather than copied, because the ROM's copy lives in the .data section of
 * game/src/hasm/ido/math_util.s, which this build does not assemble.
 * Index range is [0, 1024] -- atan2_lookup()'s worst case is
 * (s32)(1.0f * 1024 + 0.5f) == 1024, so 1025 entries are live. */
#define ARCTAN_LIVE 1025

s16 gArcTanTable[1026];

/* THE ROM'S BOOT SEEDS, from the .data section of game/src/hasm/ido/math_util.s:
 *
 *     EXPORT(gCurrentRNGSeed)
 *         .word 0x5141564D   / 'QAVM' /
 *     EXPORT(gPrevRNGSeed)
 *         .word 0x5141564D   / 'QAVM' /
 *
 * Those are the LIVE starting seeds, not placeholders: set_rng_seed() has exactly
 * one caller in the whole game (game/src/waves.c:364, `set_rng_seed('WAVF')`,
 * bracketed by save_rng_seed()/load_rng_seed()), so nothing re-seeds the
 * generator at boot and every one of a run's rand_range() draws descends from it
 * -- 98 call sites, including racer.c and particles.c.
 *
 * This file shipped 0x00051234 / 0 from the first platform commit until the
 * "closedloop" wave, invented only to make the link succeed. That put the port on
 * a completely different random sequence from frame 0, silently: the run was
 * still perfectly deterministic, just deterministically wrong, which is why
 * tests/check_determinism.py could never see it.
 *
 * It is now the DEFAULT. It was deferred for one wave because it is reached and
 * material -- 80 of 359 [PACE] racer rows changed on Ancient Lake, from row 279 --
 * and the two route-calibrated fixtures could not survive that shift. Both are
 * closed-loop now (tests/check_race_drive.py drives with DKR's own AI;
 * tests/check_collision_gridmask.py states its positive control against the
 * fixed arm's own ceiling instead of a hand-timed frame count), so the shift no
 * longer breaks them. `MDKR_RNGSEED=legacy` restores the invented seeds for A/B;
 * `=rom` is accepted and selects the default. */
#define DKR_RNG_SEED_ROM      0x5141564D /* 'QAVM' -- the ROM's .data value */
#define DKR_RNG_SEED_LEGACY   0x00051234 /* what this port used before "closedloop" */

s32 gCurrentRNGSeed = DKR_RNG_SEED_ROM;
s32 gPrevRNGSeed    = DKR_RNG_SEED_ROM;
static u32 gPresentationRNGSeed = DKR_RNG_SEED_ROM ^ 0x50524553u;
u8  gIntDisFlag     = 0; /* EXPORT(gIntDisFlag) .byte 0x00 -- matches */

/* Renderer/HUD-only randomness. It intentionally uses the ROM generator's
 * exact step and inclusive range semantics, but advances a separate stream so
 * presentation count and visibility cannot steer authoritative gameplay. */
s32 presentation_rand_range(s32 min, s32 max) {
    u64 temp;
    u32 seed = gPresentationRNGSeed;
    u32 span;

    temp = seed;
    temp = (temp << 32) | (temp >> 1);
    temp ^= ((u64)(seed & 0xFFFFFu) << 12);
    seed = (u32)(temp ^ ((temp >> 20) & 0xFFFu));
    gPresentationRNGSeed = seed;
    if (max < min) {
        s32 swap = min;
        min = max;
        max = swap;
    }
    span = (u32)max - (u32)min + 1u;
    if (span == 0u) {
        return (s32)seed;
    }
    return (s32)((seed - (u32)min) % span + (u32)min);
}

/* These rolls were render-authored in the ROM but were intentionally moved to
 * the presentation stream by the native enhanced-cadence port. Preserve both
 * compatibility targets: byte-exact ROM ordering at the shipping original
 * cadence, and the pre-FPS native gameplay stream at opt-in enhanced cadence. */
s32 cadence_compat_rand_range(s32 min, s32 max) {
    if (platform_sim_tick_fields() == 2) {
        return rand_range(min, max);
    }
    return presentation_rand_range(min, max);
}

static unsigned int mdkr_fnv1a32_u16(const s16 *vals, int n) {
    unsigned int h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        unsigned int v = (unsigned int) (unsigned short) vals[i];
        h = (h ^ (v & 0xFF)) * 16777619u;
        h = (h ^ ((v >> 8) & 0xFF)) * 16777619u;
    }
    return h;
}

__attribute__((constructor))
static void mdkr64_fill_math_tables(void) {
    /* A/B HOOKS for the three ROM-fidelity corrections this file carries. All
     * three are now the DEFAULT; each env var selects the superseded behaviour so
     * one binary can still drive both arms:
     *
     *   MDKR_RNGSEED=legacy   the invented 0x00051234 / 0 boot seeds
     *   MDKR_ARCTAN=trunc     truncate the arctan curve instead of rounding it
     *   MDKR_TRIG=libm        evaluate sines with libm instead of the ROM's
     *                         table + lerp
     *
     * `=rom` / `=round` / `=rom` are accepted and select the default, so an older
     * invocation that asked for the fix still gets it. Anything else is ignored,
     * which means a typo gets the ROM-faithful default rather than silently
     * selecting a divergence. tests/check_math_tables.py asserts both arms.  */
    const char *seedMode = getenv("MDKR_RNGSEED");
    const char *tableMode = getenv("MDKR_ARCTAN");
    const char *trigMode = getenv("MDKR_TRIG");
    const char *trace;
    int truncMode = (tableMode != NULL && tableMode[0] == 't');
    int i;

    if (seedMode != NULL && seedMode[0] == 'l') {
        gCurrentRNGSeed = DKR_RNG_SEED_LEGACY;
        gPrevRNGSeed = 0;
    }
    s_trigLibm = (trigMode != NULL && trigMode[0] == 'l');

    /* gSineTable — the ROM's quarter-turn sine table, reproduced with NO ROM
     * data: round(sin(i * pi/2 / 1024) * 0x8000) matches EXPORT(gSineTable) in
     * game/src/hasm/ido/math_util.s on all 1025 entries (0 differ, asserted by
     * tests/check_math_tables.py against the .half directives).
     *
     * Entry 1024 is 0x8000, which is -32768 as s16; the assembly reads the table
     * with `lhu`, and so does dkr_sins_interp, so the wrap is the ROM's own. */
    for (i = 0; i < SINE_LIVE; i++) {
        double s = sin((double) i * (3.14159265358979323846 / 2.0) / 1024.0);
        gSineTable[i] = (s16) (int) (s * 32768.0 + 0.5);
    }

    /* value = atan(i/1024) mapped so 90deg == 0x4000 (i.e. * 0x8000/pi).
     *
     * The ROM's table holds the ROUNDED curve. Truncating -- which this port did
     * until the "closedloop" wave -- leaves 491 of the 1025 live entries one unit
     * low. Verified entry by entry against EXPORT(gArcTanTable) in
     * game/src/hasm/ido/math_util.s: truncating mismatches 491/1025, `+ 0.5f`
     * mismatches 0/1025, i.e. rounding reproduces the ROM's table exactly.
     *
     * One unit is 1/65536 of a turn, so nothing ever looked wrong; it just made
     * every atan2s()/arctan2_f() result disagree with the ROM by up to an LSB, at
     * 72 call sites including the AI's steering and the camera. Material -- 110 of
     * 359 [PACE] rows changed, from row 233 -- which is why it waited for the
     * fixtures to become closed-loop. `MDKR_ARCTAN=trunc` restores it. */
    for (i = 0; i < 1026; i++) {
        float a = atanf((float)i / 1024.0f) * (32768.0f / 3.14159265358979323846f);
        gArcTanTable[i] = (s16)(truncMode ? a : a + 0.5f);
    }

    /* Probe, so a headless check can assert on the tables, the seed and the trig
     * themselves rather than on downstream pixels. FNV-1a over the live entries as
     * u16; tests/check_math_tables.py recomputes both hashes from the .s and
     * compares. `sinFnv` folds sins_s16 over ALL 65536 angles, so it covers the
     * table walk (mirroring, interpolation, sign) and not just the table.
     *
     * MDKR_TRACE is read directly instead of via mdkr_trace_enabled(): that
     * helper memoises on first call, and under Emscripten its miss path reads
     * `Module.__mdkrTrace`, which the shell has not set this early. Calling it
     * from a constructor would cache 0 and silently disable tracing for the whole
     * browser session. */
    trace = getenv("MDKR_TRACE");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        unsigned int h = mdkr_fnv1a32_u16(gArcTanTable, ARCTAN_LIVE);
        unsigned int hs = mdkr_fnv1a32_u16(gSineTable, SINE_LIVE);
        unsigned int hsin = 2166136261u;
        for (i = 0; i < 65536; i++) {
            unsigned int v = (unsigned int) sins_s16((s16) i);
            int k;
            for (k = 0; k < 4; k++) {
                hsin = (hsin ^ ((v >> (k * 8)) & 0xFF)) * 16777619u;
            }
        }
        fprintf(stderr,
                "[TRACE] [MATH] rngSeed=0x%08x prevSeed=0x%08x arctan=%s "
                "arctanN=%d arctanFnv=0x%08x trig=%s sineN=%d sineFnv=0x%08x "
                "sinFnv=0x%08x\n",
                (unsigned int)gCurrentRNGSeed, (unsigned int)gPrevRNGSeed,
                truncMode ? "trunc" : "round", ARCTAN_LIVE, h,
                s_trigLibm ? "libm" : "table", SINE_LIVE, hs, hsin);

        /* vec3f_rotate_py self-test. Route-independent: it asserts the FORMULA
         * rather than any downstream pixel, which matters because the fix is
         * behaviour-neutral for the racer simulation and so cannot be seen in a
         * [PACE] stream. Angles are DKR s16 (full turn 0x10000).
         *
         * Case A is the maximally discriminating one -- pitch 0, yaw 90deg turns
         * a purely forward vector into a purely sideways one:
         *   ROM/fixed  x=z*cos(0)*sin(90) = z,  y=-z*sin(0)  = 0,  z=0
         *   transposed x=z*cos(90)*sin(0) = 0,  y=-z*sin(90) = -z, z=0
         * i.e. a horizontal direction becomes vertical. */
        {
            static const struct { int pitch, yaw; } cases[2] = {
                { 0x0000, 0x4000 },   /* A: 0deg pitch, 90deg yaw */
                { 0x2000, 0x1000 },   /* B: 45deg pitch, 22.5deg yaw */
            };
            int c;
            for (c = 0; c < 2; c++) {
                Vec3s rot;
                Vec3f v, g;
                rot.y_rotation = (s16)cases[c].yaw;
                rot.x_rotation = (s16)cases[c].pitch;
                rot.z_rotation = 0;
                v.x = 0.0f; v.y = 0.0f; v.z = 100.0f;
                vec3f_rotate_py(&rot, &v);

                /* Independent oracle, needing no golden numbers: vec3f_rotate()
                 * -- which matches ITS assembly -- applied to (0, 0, z) with the
                 * same angles and zero roll IS vec3f_rotate_py() by definition
                 * (that is what the function's own docstring says it is). With
                 * the transposition present the two disagree; with it fixed they
                 * agree to the last bit the float format allows. The check
                 * asserts the agreement, so it does not depend on any reading
                 * of the assembly being right. */
                g.x = 0.0f; g.y = 0.0f; g.z = 100.0f;
                vec3f_rotate(&rot, &g);

                fprintf(stderr,
                        "[TRACE] [ROTPY] selftest case=%c pitch=0x%04X yaw=0x%04X "
                        "z=100 -> x=%.5f y=%.5f z=%.5f | rotate3=%.5f,%.5f,%.5f\n",
                        'A' + c, (unsigned)cases[c].pitch & 0xFFFF,
                        (unsigned)cases[c].yaw & 0xFFFF, v.x, v.y, v.z,
                        g.x, g.y, g.z);
            }
            /* The self-test must not be mistaken for gameplay reachability. */
            s_rotpyCalls = 0;
            s_rotpyHash = 1469598103934665603ULL;
        }
        fflush(stderr);
    }
}
