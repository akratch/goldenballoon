#include "steering_compat.h"

#include <math.h>
#include <stdio.h>

static int failures;

static void expect_near(const char *name, float actual, float expected) {
    if (fabsf(actual - expected) > 0.0001f) {
        fprintf(stderr, "%s: got %.9g, expected %.9g\n",
                name, actual, expected);
        failures++;
    }
}

static void test_equivalent_half_steps(float lateral, float force,
                                       float traction, float pitch) {
    float retail = mdkr_lateral_traction_step(
        lateral, force, traction, pitch, 0);
    float half = mdkr_lateral_traction_step(
        lateral, force, traction, pitch, 1);
    half = mdkr_lateral_traction_step(half, force, traction, pitch, 1);
    expect_near("two Enhanced steps equal one retail step", half, retail);
}

int main(void) {
    test_equivalent_half_steps(0.0f, -12.0f, 0.8f, 1.0f);
    test_equivalent_half_steps(4.5f, 2.25f, 0.5f, 0.97f);
    test_equivalent_half_steps(-9.0f, 0.0f, 0.85f, 0.8f);
    test_equivalent_half_steps(3.0f, -1.0f, 1.0f, 1.0f);
    expect_near("zero retention", mdkr_lateral_traction_step(
                    12.0f, 5.0f, 0.0f, 1.0f, 1), 0.0f);
    return failures != 0;
}
