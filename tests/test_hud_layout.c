#include "hud_layout.h"

#include <math.h>
#include <stdio.h>

static int failures;

static void expect_near(const char *name, float actual, float expected) {
    if (fabsf(actual - expected) > 0.001f) {
        fprintf(stderr, "%s: got %.6f, expected %.6f\n",
                name, actual, expected);
        failures++;
    }
}

int main(void) {
    expect_near("disabled", mdkr_hud_horizontal_offset(
                    16.0f / 9.0f, 0, MDKR_HUD_ANCHOR_RIGHT), 0.0f);
    expect_near("authored", mdkr_hud_horizontal_offset(
                    4.0f / 3.0f, 1, MDKR_HUD_ANCHOR_LEFT), 0.0f);
    expect_near("portrait", mdkr_hud_horizontal_offset(
                    9.0f / 16.0f, 1, MDKR_HUD_ANCHOR_LEFT), 0.0f);
    expect_near("center", mdkr_hud_horizontal_offset(
                    21.0f / 9.0f, 1, MDKR_HUD_ANCHOR_CENTER), 0.0f);
    expect_near("16:9 left", mdkr_hud_horizontal_offset(
                    16.0f / 9.0f, 1, MDKR_HUD_ANCHOR_LEFT), -160.0f / 3.0f);
    expect_near("16:9 right", mdkr_hud_horizontal_offset(
                    16.0f / 9.0f, 1, MDKR_HUD_ANCHOR_RIGHT), 160.0f / 3.0f);
    return failures != 0;
}
