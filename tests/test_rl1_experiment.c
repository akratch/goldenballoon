#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx_rl1_experiment.h"

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    /* Upward-facing, non-degenerate triangle. */
    const float xyz[9] = {
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f,
    };
    const uint8_t source[12] = {
        40, 60, 80, 10,
        90, 100, 110, 20,
        180, 160, 140, 30,
    };
    uint8_t baked[12];
    uint8_t additive[12];
    uint8_t superseded[12];

    memcpy(baked, source, sizeof(baked));
    expect(
        !gfx_rl1_apply_triangle(GFX_RL1_BAKED, xyz, baked),
        "baked arm reports no transformation");
    expect(
        memcmp(baked, source, sizeof(baked)) == 0,
        "baked arm is byte-identical");

    memcpy(additive, source, sizeof(additive));
    expect(
        gfx_rl1_apply_triangle(GFX_RL1_BAKED_SUN, xyz, additive),
        "baked-sun transforms a valid triangle");
    expect(
        additive[0] != additive[4],
        "baked-sun retains baked vertex variation");
    expect(
        additive[3] == 10 && additive[7] == 20 && additive[11] == 30,
        "baked-sun preserves alpha");

    memcpy(superseded, source, sizeof(superseded));
    expect(
        gfx_rl1_apply_triangle(GFX_RL1_SUPERSEDE, xyz, superseded),
        "supersede transforms a valid triangle");
    expect(
        superseded[0] == superseded[4] &&
        superseded[4] == superseded[8],
        "supersede discards baked vertex variation");
    expect(
        superseded[0] == superseded[1] &&
        superseded[1] == superseded[2],
        "supersede emits neutral directional light");
    expect(
        superseded[3] == 10 &&
        superseded[7] == 20 &&
        superseded[11] == 30,
        "supersede preserves alpha");

    {
        const float degenerate[9] = { 0 };
        uint8_t unchanged[12];
        memcpy(unchanged, source, sizeof(unchanged));
        expect(
            !gfx_rl1_apply_triangle(
                GFX_RL1_SUPERSEDE, degenerate, unchanged),
            "degenerate triangle is rejected");
        expect(
            memcmp(unchanged, source, sizeof(unchanged)) == 0,
            "degenerate triangle leaves colour unchanged");
    }
    {
        float nonfinite[9];
        uint8_t unchanged[12];
        memcpy(nonfinite, xyz, sizeof(nonfinite));
        nonfinite[4] = NAN;
        memcpy(unchanged, source, sizeof(unchanged));
        expect(
            !gfx_rl1_apply_triangle(
                GFX_RL1_BAKED_SUN, nonfinite, unchanged),
            "non-finite triangle is rejected");
        expect(
            memcmp(unchanged, source, sizeof(unchanged)) == 0,
            "non-finite triangle leaves colour unchanged");
    }
    expect(
        strcmp(gfx_rl1_arm_name(GFX_RL1_BAKED), "baked") == 0 &&
        strcmp(gfx_rl1_arm_name(GFX_RL1_BAKED_SUN), "baked-sun") == 0 &&
        strcmp(gfx_rl1_arm_name(GFX_RL1_SUPERSEDE), "supersede") == 0,
        "arm names are stable telemetry tokens");

    if (failures != 0) {
        fprintf(stderr, "%d RL-1 experiment test(s) failed\n", failures);
        return 1;
    }
    puts("PASS rl1 experiment");
    return 0;
}
