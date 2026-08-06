#include "gfx_webgpu_surface_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    TEST_ALPHA_AUTO = 1,
    TEST_ALPHA_OPAQUE = 2,
    TEST_PRESENT_FIFO = 10,
    TEST_PRESENT_IMMEDIATE = 11,
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
 * capabilities. The property under test is the one the tearing defect violated:
 * no policy, and no capability set, reaches Immediate without the explicit
 * opt-in.
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

int main(void) {
    int failures = 0;

    failures += require_true(
        gfx_webgpu_surface_select_alpha(
            TEST_ALPHA_AUTO,
            TEST_ALPHA_OPAQUE,
            true) == TEST_ALPHA_OPAQUE,
        "generation A should select advertised opaque alpha");
    failures += require_true(
        gfx_webgpu_surface_select_present(
            TEST_PRESENT_FIFO,
            TEST_PRESENT_IMMEDIATE,
            true) == TEST_PRESENT_IMMEDIATE,
        "generation A should select advertised immediate presentation");

    /* Generation B deliberately narrows its capabilities. No state from the
     * prior generation may survive this second resolution. */
    failures += require_true(
        gfx_webgpu_surface_select_alpha(
            TEST_ALPHA_AUTO,
            TEST_ALPHA_OPAQUE,
            false) == TEST_ALPHA_AUTO,
        "generation B should fall back to automatic alpha");
    failures += require_true(
        gfx_webgpu_surface_select_present(
            TEST_PRESENT_FIFO,
            TEST_PRESENT_IMMEDIATE,
            false) == TEST_PRESENT_FIFO,
        "generation B should fall back to FIFO presentation");
    failures += require_true(
        gfx_webgpu_surface_select_present(
            TEST_PRESENT_FIFO,
            TEST_PRESENT_IMMEDIATE,
            false) == TEST_PRESENT_FIFO,
        "unavailable capabilities should fail closed to FIFO");

    failures += check_matrix();

    if (failures != 0) {
        return 1;
    }
    puts("PASS: WebGPU surface policy is resolved per capability generation");
    return 0;
}
