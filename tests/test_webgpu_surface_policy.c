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

    if (failures != 0) {
        return 1;
    }
    puts("PASS: WebGPU surface policy is resolved per capability generation");
    return 0;
}
