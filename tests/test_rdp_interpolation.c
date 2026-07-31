#include "fast3d/gfx_cc.h"
#include "fast3d/gfx_rdp_interpolation.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int s_failures;

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

static void expect_u8(const char *name, uint8_t actual, uint8_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %s: got %u, expected %u\n",
                name, (unsigned)actual, (unsigned)expected);
        s_failures++;
    }
}

static uint8_t legacy_lerp_u8(uint8_t a, uint8_t b, float t) {
    float value = (float)a + ((float)b - (float)a) * t;
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)(value + 0.5f);
}

static void test_shader_options(void) {
    uint32_t fixed_fog = gfx_rdp_interpolation_options(true, false);
    uint32_t fixed_clear = gfx_rdp_interpolation_options(false, false);

    expect_true("shade is screen-linear",
                (fixed_fog & SHADER_OPT_NOPERSPECTIVE_INPUTS) != 0);
    expect_true("fog is screen-linear",
                (fixed_fog & SHADER_OPT_NOPERSPECTIVE_FOG) != 0);
    expect_true("fog qualifier omitted when unused",
                (fixed_clear & SHADER_OPT_NOPERSPECTIVE_FOG) == 0);
    expect_true("shade qualifier remains without fog",
                (fixed_clear & SHADER_OPT_NOPERSPECTIVE_INPUTS) != 0);
    expect_true("legacy control has old qualifiers",
                gfx_rdp_interpolation_options(true, true) == 0);
}

static void test_fog_coordinate(void) {
    expect_u8("ordinary fog",
              gfx_rdp_fog_from_clip(-0.25f, 0.5f, 100, 128), 78);
    expect_u8("lower clamp",
              gfx_rdp_fog_from_clip(-2.0f, 1.0f, 100, 128), 0);
    expect_u8("upper clamp",
              gfx_rdp_fog_from_clip(2.0f, 1.0f, 100, 128), 255);
    expect_u8("non-finite fails closed",
              gfx_rdp_fog_from_clip(NAN, 1.0f, 100, 128), 0);
}

static void test_clipped_vertex_positive_control(void) {
    /*
     * Edge A-to-B crosses z+w=0 at t=4/9. Endpoint fog bytes are 108 and
     * 0. Their old byte interpolation gives 60, while the generated position is
     * (z,w)=(-1,1), whose actual fog coordinate is 28.
     */
    const float t = 4.0f / 9.0f;
    const uint8_t fog_a = gfx_rdp_fog_from_clip(-0.2f, 1.0f, 100, 128);
    const uint8_t fog_b = gfx_rdp_fog_from_clip(-2.0f, 1.0f, 100, 128);
    const float clipped_z = -0.2f + (-2.0f - -0.2f) * t;
    const float clipped_w = 1.0f;
    const uint8_t legacy = legacy_lerp_u8(fog_a, fog_b, t);
    const uint8_t fixed =
        gfx_rdp_fog_from_clip(clipped_z, clipped_w, 100, 128);

    expect_u8("positive-control endpoint A", fog_a, 108);
    expect_u8("positive-control endpoint B", fog_b, 0);
    expect_u8("legacy clipped fog", legacy, 60);
    expect_u8("recomputed clipped fog", fixed, 28);
    expect_true("control proves endpoint interpolation is wrong",
                fixed != legacy);
}

int main(void) {
    test_shader_options();
    test_fog_coordinate();
    test_clipped_vertex_positive_control();

    if (s_failures != 0) {
        fprintf(stderr, "%d RDP interpolation test(s) failed\n", s_failures);
        return 1;
    }
    printf("RDP interpolation tests passed\n");
    return 0;
}
