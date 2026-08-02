/*
 * test_mip_chain.c — ROM-free, GPU-free unit tests for the mip chain builder.
 *
 * platform/fast3d/gfx_mipgen.c is deliberately pure: it takes an RGBA8 buffer
 * and produces the reduced levels, touching no GL/WebGPU/Metal state. That is
 * what lets every correctness rule below be asserted in milliseconds without a
 * device, and it is why the chain is built once in the HLE rather than three
 * times in three backends.
 */
#include "fast3d/gfx_mipgen.h"
#include "fast3d/gfx_texture_edge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

static void expect_int(const char *name, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %-46s actual=%d expected=%d\n", name, actual, expected);
        s_failures++;
    }
}

static void expect_near_u8(const char *name, int actual, int expected, int tol) {
    if (abs(actual - expected) > tol) {
        fprintf(stderr, "FAIL %-46s actual=%d expected=%d (+/-%d)\n",
                name, actual, expected, tol);
        s_failures++;
    }
}

static void expect_true(const char *name, int condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* ---- level counting and sizing ---------------------------------------- */

static void test_level_count(void) {
    /* Each level halves both axes, floored at 1, until both are 1. */
    expect_int("64x32 levels", gfx_mip_level_count(64, 32), 7);
    expect_int("32x48 levels", gfx_mip_level_count(32, 48), 6);
    expect_int("1x1 levels", gfx_mip_level_count(1, 1), 1);
    expect_int("2x1 levels", gfx_mip_level_count(2, 1), 2);
    expect_int("degenerate 0x0", gfx_mip_level_count(0, 0), 0);

    /* Sizing covers levels 1..N-1 only; level 0 is the caller's own buffer. */
    expect_int("2x2 scratch bytes", (int) gfx_mip_chain_bytes(2, 2), 4);
    expect_int("1x1 scratch bytes", (int) gfx_mip_chain_bytes(1, 1), 0);
}

/* ---- a flat colour must survive every reduction exactly ---------------- */

static void test_uniform_is_lossless(void) {
    enum { W = 8, H = 8 };
    uint8_t src[W * H * 4];
    uint8_t scratch[4096];
    GfxMipChain chain;

    for (int i = 0; i < W * H; i++) {
        src[i * 4 + 0] = 173;
        src[i * 4 + 1] = 22;
        src[i * 4 + 2] = 91;
        src[i * 4 + 3] = 200;
    }
    expect_true("uniform build ok",
                gfx_mip_build(src, W, H, scratch, sizeof(scratch), &chain));
    expect_int("uniform level count", chain.level_count, 4);

    for (int l = 0; l < chain.level_count; l++) {
        const uint8_t *px = chain.level[l];
        int n = chain.width[l] * chain.height[l];
        for (int i = 0; i < n; i++) {
            /* Round-tripping through linear space must not drift the value. */
            expect_near_u8("uniform R preserved", px[i * 4 + 0], 173, 1);
            expect_near_u8("uniform G preserved", px[i * 4 + 1], 22, 1);
            expect_near_u8("uniform B preserved", px[i * 4 + 2], 91, 1);
            expect_int("uniform A preserved", px[i * 4 + 3], 200);
        }
    }
}

/* ---- the reason this filters in linear space --------------------------- */

static void test_linear_space_average(void) {
    /* A 2x1 black/white pair reduces to one texel.
     *
     * Averaging the sRGB-encoded bytes gives 127. Averaging the LIGHT they
     * represent -- linearize, mean, re-encode -- gives ~188. The second is
     * correct, and the difference is exactly the "mipmaps look muddy" artifact.
     */
    uint8_t src[2 * 1 * 4] = {
        0,   0,   0,   255,
        255, 255, 255, 255,
    };
    uint8_t scratch[64];
    GfxMipChain chain;

    expect_true("2x1 build ok", gfx_mip_build(src, 2, 1, scratch, sizeof(scratch), &chain));
    expect_int("2x1 level count", chain.level_count, 2);
    expect_int("level 1 is 1x1", chain.width[1] * chain.height[1], 1);

    expect_near_u8("linear-space mean of black and white",
                   chain.level[1][0], 188, 2);
    expect_true("NOT the naive sRGB mean", abs((int) chain.level[1][0] - 127) > 40);

    /* Alpha is a coverage fraction, not a light value: it averages linearly. */
    expect_int("alpha averaged without gamma", chain.level[1][3], 255);
}

static void test_alpha_is_linear(void) {
    uint8_t src[2 * 1 * 4] = {
        10, 10, 10, 0,
        10, 10, 10, 200,
    };
    uint8_t scratch[64];
    GfxMipChain chain;

    expect_true("alpha build ok", gfx_mip_build(src, 2, 1, scratch, sizeof(scratch), &chain));
    expect_near_u8("alpha is a plain mean", chain.level[1][3], 100, 1);
}

/* ---- NPOT: the area box must weight partial source texels -------------- */

static void test_npot_area_box(void) {
    /* 3x1 -> 1x1. An exact-area box averages all three source texels with
     * equal weight; a naive 2-tap box would silently drop the third. */
    uint8_t src[3 * 1 * 4] = {
        0,   0,   0,   30,
        0,   0,   0,   60,
        0,   0,   0,   90,
    };
    uint8_t scratch[64];
    GfxMipChain chain;

    expect_true("3x1 build ok", gfx_mip_build(src, 3, 1, scratch, sizeof(scratch), &chain));
    expect_int("3x1 level count", chain.level_count, 2);
    expect_near_u8("all three source texels contribute", chain.level[1][3], 60, 1);
}

static void test_npot_dimensions(void) {
    /* 32x48 is a real DKR texture shape and the one that made
     * glGenerateMipmap fail silently on macOS Metal. Built on the CPU it is
     * unremarkable -- which is the point of not delegating to the driver. */
    enum { W = 32, H = 48 };
    uint8_t *src = calloc((size_t) W * H * 4, 1);
    uint8_t *scratch = calloc(gfx_mip_chain_bytes(W, H) + 16, 1);
    GfxMipChain chain;
    static const int expect_w[] = { 32, 16, 8, 4, 2, 1 };
    static const int expect_h[] = { 48, 24, 12, 6, 3, 1 };

    expect_true("npot build ok",
                gfx_mip_build(src, W, H, scratch, gfx_mip_chain_bytes(W, H), &chain));
    expect_int("npot level count", chain.level_count, 6);
    for (int l = 0; l < chain.level_count && l < 6; l++) {
        expect_int("npot level width", chain.width[l], expect_w[l]);
        expect_int("npot level height", chain.height[l], expect_h[l]);
    }
    free(src);
    free(scratch);
}

/* ---- contracts the HLE relies on -------------------------------------- */

static void test_contracts(void) {
    uint8_t src[4 * 4 * 4];
    uint8_t scratch[512];
    GfxMipChain chain;
    memset(src, 0x40, sizeof(src));

    /* Level 0 must alias the caller's buffer, not a copy: the HLE hands the
     * decoded texture straight through and must not pay to duplicate it. */
    expect_true("build ok", gfx_mip_build(src, 4, 4, scratch, sizeof(scratch), &chain));
    expect_true("level 0 aliases the source", chain.level[0] == src);

    /* Insufficient scratch is refused, not overrun. */
    expect_true("short scratch refused",
                !gfx_mip_build(src, 4, 4, scratch, 3, &chain));

    /* Degenerate inputs are refused rather than trusted. */
    expect_true("null src refused",
                !gfx_mip_build(NULL, 4, 4, scratch, sizeof(scratch), &chain));
    expect_true("zero dims refused",
                !gfx_mip_build(src, 0, 4, scratch, sizeof(scratch), &chain));
    expect_true("null out refused",
                !gfx_mip_build(src, 4, 4, scratch, sizeof(scratch), NULL));
}

/* ---- cutout alpha coverage --------------------------------------------- */

/* A picket fence: 2 opaque columns in every 8. Coverage = 0.25. This is the
 * shape that erodes into nothing under a plain box filter. */
static void make_fence(uint8_t *px, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *o = px + ((size_t) y * (size_t) w + (size_t) x) * 4u;
            int opaque = (x % 8) < 2;
            o[0] = 120; o[1] = 90; o[2] = 60;
            o[3] = opaque ? 255 : 0;
        }
    }
}

static void test_alpha_coverage_preserved(void) {
    /*
     * Threshold 192 rather than 128 is deliberate. At 128 the box average of an
     * opaque/transparent pair rounds to exactly 128 and clears the test by
     * luck, so plain and cutout agree at every level and the test would assert
     * nothing. At 192 the averaged texels fall below the threshold and the
     * fence disappears entirely under a plain box — which is the artifact this
     * pass exists to prevent, and the only setting where it can be shown to.
     */
    enum { W = 64, H = 64, THRESH = 192 };
    static uint8_t src[W * H * 4];
    static uint8_t scratch_plain[W * H * 4];
    static uint8_t scratch_cutout[W * H * 4];
    GfxMipChain plain, cutout;
    float base;

    make_fence(src, W, H);
    base = gfx_mip_alpha_coverage(src, W, H, THRESH);
    expect_true("fence base coverage is 0.25", base > 0.24f && base < 0.26f);

    expect_true("plain build ok",
                gfx_mip_build(src, W, H, scratch_plain, sizeof(scratch_plain), &plain));
    expect_true("cutout build ok",
                gfx_mip_build_cutout(src, W, H, scratch_cutout, sizeof(scratch_cutout),
                                     THRESH, &cutout));

    /* Level 1 still resolves the pattern; coverage must be exact there. */
    {
        float c1 = gfx_mip_alpha_coverage(cutout.level[1], cutout.width[1],
                                          cutout.height[1], THRESH);
        if (c1 < base - 0.05f || c1 > base + 0.05f) {
            fprintf(stderr, "FAIL level 1 coverage %.3f vs base %.3f\n",
                    (double) c1, (double) base);
            s_failures++;
        }
    }

    /*
     * Level 2 is the payoff, and it carries its own control. Under a plain box
     * the averaged texels fall below the alpha threshold and the fence vanishes
     * completely; the cutout pass must keep it on screen. If the plain arm ever
     * stops vanishing, the cutout assertion beside it proves nothing — so both
     * are asserted together.
     */
    {
        float p2 = gfx_mip_alpha_coverage(plain.level[2], plain.width[2],
                                          plain.height[2], THRESH);
        float c2 = gfx_mip_alpha_coverage(cutout.level[2], cutout.width[2],
                                          cutout.height[2], THRESH);
        expect_true("CONTROL: plain box loses the fence entirely at level 2",
                    p2 < 0.01f);
        expect_true("cutout keeps the fence visible at level 2", c2 > 0.20f);
    }

    /*
     * Below Nyquist the fence has genuinely aliased away and the original
     * coverage is no longer representable — a uniform level can only be 0 or 1.
     * What must NEVER happen is running to 1.0: a fence that becomes a solid
     * wall is a worse artifact than one that fades, and it is what a naive
     * "smallest scale reaching target" search produces.
     */
    for (int l = 1; l < cutout.level_count; l++) {
        float c = gfx_mip_alpha_coverage(cutout.level[l], cutout.width[l],
                                         cutout.height[l], THRESH);
        if (c > 0.75f) {
            fprintf(stderr, "FAIL level %d (%dx%d) ran to %.3f (base %.3f) "
                            "-- a fence must not become a wall\n",
                    l, cutout.width[l], cutout.height[l], (double) c, (double) base);
            s_failures++;
        }
    }

    /* Colour must be untouched by the alpha pass. */
    expect_int("cutout preserves colour", cutout.level[1][0], plain.level[1][0]);

    /* threshold 0 must be exactly the plain path. */
    {
        static uint8_t scratch_off[W * H * 4];
        GfxMipChain off;
        expect_true("threshold 0 build ok",
                    gfx_mip_build_cutout(src, W, H, scratch_off, sizeof(scratch_off), 0, &off));
        expect_int("threshold 0 == plain build",
                   memcmp(scratch_off, scratch_plain, sizeof(scratch_off)), 0);
    }
}

static void test_texture_edge_threshold_contract(void) {
    const float rejected = (float) (GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_U8 - 1u) / 255.0f;
    const float accepted = (float) GFX_TEXTURE_EDGE_ALPHA_THRESHOLD_U8 / 255.0f;

    expect_true("texture-edge previous byte is rejected",
                rejected <= GFX_TEXTURE_EDGE_ALPHA_THRESHOLD);
    expect_true("texture-edge threshold byte is accepted",
                accepted > GFX_TEXTURE_EDGE_ALPHA_THRESHOLD);
}

int main(void) {
    test_alpha_coverage_preserved();
    test_texture_edge_threshold_contract();
    test_level_count();
    test_uniform_is_lossless();
    test_linear_space_average();
    test_alpha_is_linear();
    test_npot_area_box();
    test_npot_dimensions();
    test_contracts();

    if (s_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("all mip_chain tests passed\n");
    return 0;
}
