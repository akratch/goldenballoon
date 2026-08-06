#include "gfx_webgpu_surface_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    TEST_ALPHA_AUTO = 1,
    TEST_ALPHA_OPAQUE = 2,
};

static int require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 1;
    }
    return 0;
}

/*
 * The whole latched-policy matrix, from the value a player picks to the mode
 * the surface is configured with, against every combination of advertised
 * capabilities. The property: no policy and no capability set reaches
 * Immediate without the explicit opt-in.
 */
static const struct PresentCase {
    const char *name;
    MdkrPresentPolicyKind kind;
    unsigned rate;
    unsigned display_rate;
    bool allow_tearing;
    bool mailbox;
    bool immediate;
    GfxWebgpuPresentMode expected;
} kPresentCases[] = {
#define PRESENT_CASE(NAME, KIND, RATE, HZ, TEAR, MAILBOX, IMMEDIATE, WANT) \
    { NAME, MDKR_PRESENT_##KIND, RATE, HZ, TEAR, MAILBOX, IMMEDIATE, \
      GFX_WEBGPU_PRESENT_##WANT }
    /*                       policy     rate   Hz  tear  mbox  immed  mode */
    PRESENT_CASE("original",
                 ORIGINAL,      0u,  60u, false, true,  true,  FIFO),
    PRESENT_CASE("display",
                 DISPLAY,       0u,  60u, false, true,  true,  FIFO),
    PRESENT_CASE("cap below the refresh",
                 CAPPED,       30u,  60u, false, true,  true,  FIFO),
    PRESENT_CASE("cap at the refresh",
                 CAPPED,       60u,  60u, false, true,  true,  FIFO),
    PRESENT_CASE("cap under a faster refresh",
                 CAPPED,      120u, 144u, false, true,  true,  FIFO),
    PRESENT_CASE("cap with an unknown refresh",
                 CAPPED,      120u,   0u, false, true,  true,  FIFO),
    PRESENT_CASE("cap above the refresh",
                 CAPPED,      120u,  60u, false, true,  true,  MAILBOX),
    PRESENT_CASE("cap above the refresh without mailbox",
                 CAPPED,      120u,  60u, false, false, true,  FIFO),
    PRESENT_CASE("uncapped",
                 UNCAPPED,      0u,  60u, false, true,  true,  MAILBOX),
    PRESENT_CASE("uncapped without mailbox",
                 UNCAPPED,      0u,  60u, false, false, true,  FIFO),
    PRESENT_CASE("uncapped with nothing advertised",
                 UNCAPPED,      0u,  60u, false, false, false, FIFO),
    PRESENT_CASE("original opted in",
                 ORIGINAL,      0u,  60u, true,  true,  true,  IMMEDIATE),
    PRESENT_CASE("cap below the refresh opted in",
                 CAPPED,       30u,  60u, true,  true,  true,  IMMEDIATE),
    PRESENT_CASE("uncapped opted in",
                 UNCAPPED,      0u,  60u, true,  true,  true,  IMMEDIATE),
    PRESENT_CASE("opted in without immediate keeps the policy's own mode",
                 UNCAPPED,      0u,  60u, true,  true,  false, MAILBOX),
    PRESENT_CASE("opted in on a blocking policy without immediate",
                 DISPLAY,       0u,  60u, true,  true,  false, FIFO),
    PRESENT_CASE("opted in with nothing advertised",
                 UNCAPPED,      0u,  60u, true,  false, false, FIFO),
#undef PRESENT_CASE
};

static int check_matrix(void) {
    int failures = 0;
    size_t i;

    for (i = 0; i < sizeof(kPresentCases) / sizeof(kPresentCases[0]); ++i) {
        const struct PresentCase *c = &kPresentCases[i];
        const MdkrPresentPolicy policy = { c->kind, c->rate };
        const MdkrPresentSync sync =
            mdkr_present_policy_sync(&policy, c->display_rate);
        const GfxWebgpuPresentSupport advertised = { c->mailbox, c->immediate };
        const GfxWebgpuPresentMode chosen =
            gfx_webgpu_surface_rank_present(sync, c->allow_tearing, advertised);

        if (chosen != c->expected) {
            fprintf(stderr, "FAIL: %s selected %s, expected %s\n", c->name,
                    gfx_webgpu_surface_present_name(chosen),
                    gfx_webgpu_surface_present_name(c->expected));
            failures++;
        }
        if (!c->allow_tearing && chosen == GFX_WEBGPU_PRESENT_IMMEDIATE) {
            fprintf(stderr, "FAIL: %s tore without an opt-in\n", c->name);
            failures++;
        }
        /* The requested mode is what the row reports before capabilities are
         * consulted, so a fallback has to be visible as requested != effective
         * rather than silently logged as satisfied. */
        if (gfx_webgpu_surface_request_present(sync, c->allow_tearing) !=
                chosen &&
            (advertised.mailbox && advertised.immediate)) {
            fprintf(stderr,
                    "FAIL: %s fell back with every mode advertised\n", c->name);
            failures++;
        }
    }
    failures += require_true(
        mdkr_present_policy_sync(NULL, 60u) == MDKR_PRESENT_SYNC_BLOCKING,
        "a missing policy must fail closed onto the blocking queue");
    return failures;
}

/*
 * GE007_WEBGPU_PRESENT names a mode rather than a policy, so it is ranked
 * against the same capabilities: an advertised mode is honoured, and an
 * unadvertised one falls back to FIFO rather than being configured and
 * rejected. Support is resolved per capability generation.
 */
static const struct OverrideCase {
    const char *name;
    GfxWebgpuPresentMode requested;
    bool mailbox;
    bool immediate;
    GfxWebgpuPresentMode expected;
} kOverrideCases[] = {
    { "fifo is always available",
      GFX_WEBGPU_PRESENT_FIFO,      false, false, GFX_WEBGPU_PRESENT_FIFO },
    { "generation A advertises mailbox",
      GFX_WEBGPU_PRESENT_MAILBOX,   true,  true,  GFX_WEBGPU_PRESENT_MAILBOX },
    { "generation B withdraws mailbox",
      GFX_WEBGPU_PRESENT_MAILBOX,   false, true,  GFX_WEBGPU_PRESENT_FIFO },
    { "generation A advertises immediate",
      GFX_WEBGPU_PRESENT_IMMEDIATE, true,  true,  GFX_WEBGPU_PRESENT_IMMEDIATE },
    { "generation B withdraws immediate",
      GFX_WEBGPU_PRESENT_IMMEDIATE, true,  false, GFX_WEBGPU_PRESENT_FIFO },
    { "nothing advertised fails closed to FIFO",
      GFX_WEBGPU_PRESENT_IMMEDIATE, false, false, GFX_WEBGPU_PRESENT_FIFO },
};

static int check_override(void) {
    int failures = 0;
    size_t i;

    for (i = 0; i < sizeof(kOverrideCases) / sizeof(kOverrideCases[0]); ++i) {
        const struct OverrideCase *c = &kOverrideCases[i];
        const GfxWebgpuPresentSupport advertised = { c->mailbox, c->immediate };
        const GfxWebgpuPresentMode chosen =
            gfx_webgpu_surface_rank_override(c->requested, advertised);

        if (chosen != c->expected) {
            fprintf(stderr, "FAIL: override %s selected %s, expected %s\n",
                    c->name, gfx_webgpu_surface_present_name(chosen),
                    gfx_webgpu_surface_present_name(c->expected));
            failures++;
        }
        if (c->requested != GFX_WEBGPU_PRESENT_IMMEDIATE &&
            chosen == GFX_WEBGPU_PRESENT_IMMEDIATE) {
            fprintf(stderr, "FAIL: override %s tore without asking\n", c->name);
            failures++;
        }
    }
    return failures;
}

int main(void) {
    int failures = 0;

    failures += require_true(
        gfx_webgpu_surface_select_alpha(
            TEST_ALPHA_AUTO,
            TEST_ALPHA_OPAQUE,
            true) == TEST_ALPHA_OPAQUE,
        "generation A should select advertised opaque alpha");

    /* Generation B deliberately narrows its capabilities. No state from the
     * prior generation may survive this second resolution. */
    failures += require_true(
        gfx_webgpu_surface_select_alpha(
            TEST_ALPHA_AUTO,
            TEST_ALPHA_OPAQUE,
            false) == TEST_ALPHA_AUTO,
        "generation B should fall back to automatic alpha");

    failures += check_matrix();
    failures += check_override();

    if (failures != 0) {
        return 1;
    }
    puts("PASS: WebGPU surface policy is resolved per capability generation");
    return 0;
}
