/*
 * Link support for the camera unit tests that exercise the PRODUCTION matrix
 * recipe in game/src/hasm/math_util.c.
 *
 * Those tests must not carry a second copy of mtxf_from_transform: a test-local
 * reimplementation compares a recipe against itself and cannot fail when the
 * production one changes. Linking the real translation unit is what makes the
 * oracle falsifiable, and it drags in the engine-wide symbols math_util.c
 * references from code paths the camera tests never enter.
 *
 * Everything below therefore exists only to satisfy the linker. Nothing here is
 * a stand-in for behavior under test: the RNG seeds, arctangent table, and
 * interrupt flag belong to functions no camera test calls, and the trig
 * forwarders defer to the sine oracle each test already defines. If a camera
 * test ever starts depending on one of these, that dependency belongs in the
 * test, not here.
 */

#include "macros.h"
#include "types.h"
#include "structs.h"

s32 coss_s16(s16 angle);
s32 sins_s16(s16 angle);

u32 gCurrentRNGSeed;
u32 gPrevRNGSeed;
s32 gIntDisFlag;
s16 gArcTanTable[1026];

f32 sins_f(s16 angle) {
    return (f32) sins_s16(angle) * (1.0f / 65536.0f);
}

f32 coss_f(s16 angle) {
    return (f32) coss_s16(angle) * (1.0f / 65536.0f);
}

u32 __osDisableInt(void) {
    return 0;
}

void __osRestoreInt(UNUSED u32 mask) {
}

s32 mdkr_mips_round_w_s(f32 value) {
    return (s32) (value < 0.0f ? value - 0.5f : value + 0.5f);
}

u32 mdkr_mips_atan_index(UNUSED u32 numerator, UNUSED u32 denominator) {
    return 0;
}

int mdkr_rotpy_legacy(void) {
    return 0;
}

void mdkr_rotpy_observe(UNUSED float x, UNUSED float y, UNUSED float z) {
}
