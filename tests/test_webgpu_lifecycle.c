#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "fast3d/gfx_webgpu_lifecycle.h"
#include "fast3d/gfx_webgpu_shader.h"

static void expect_action(const char *name, enum MdkrWgpuSurfaceAction got,
                          enum MdkrWgpuSurfaceAction want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d, want %d\n", name, (int)got, (int)want);
        exit(1);
    }
}

static void expect_true(const char *name, bool value) {
    if (!value) {
        fprintf(stderr, "%s: expected true\n", name);
        exit(1);
    }
}

int main(void) {
    struct MdkrWgpuLifecycle state;
    struct MdkrWgpuSurfacePolicy policy;
    const uint64_t render = 1u << 0;
    const uint64_t dst = 1u << 1;
    const uint64_t src = 1u << 2;

    mdkr_wgpu_lifecycle_init(&state);
    expect_action("optimal", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_OPTIMAL), MDKR_WGPU_SURFACE_PRESENT);
    for (int i = 0; i < 29; ++i) {
        expect_action("suboptimal remains presentable", mdkr_wgpu_lifecycle_acquire(
            &state, MDKR_WGPU_ACQUIRE_SUBOPTIMAL), MDKR_WGPU_SURFACE_PRESENT);
    }
    expect_action("persistent suboptimal schedules repair",
        mdkr_wgpu_lifecycle_acquire(&state, MDKR_WGPU_ACQUIRE_SUBOPTIMAL),
        MDKR_WGPU_SURFACE_PRESENT_RECONFIGURE);
    expect_action("optimal after repair", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_OPTIMAL), MDKR_WGPU_SURFACE_PRESENT);
    expect_action("timeout skips", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_TIMEOUT), MDKR_WGPU_SURFACE_SKIP);
    expect_action("timeout recovers", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_OPTIMAL), MDKR_WGPU_SURFACE_PRESENT);
    expect_action("outdated reconfigures", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_OUTDATED), MDKR_WGPU_SURFACE_RECONFIGURE);
    expect_action("lost recreates", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_LOST), MDKR_WGPU_SURFACE_RECREATE);
    for (int i = 0; i < 59; ++i) {
        expect_action("bounded errors skip", mdkr_wgpu_lifecycle_acquire(
            &state, MDKR_WGPU_ACQUIRE_ERROR), MDKR_WGPU_SURFACE_SKIP);
    }
    expect_action("persistent errors escalate", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_ERROR), MDKR_WGPU_SURFACE_RECOVER_DEVICE);
    expect_action("success resets errors", mdkr_wgpu_lifecycle_acquire(
        &state, MDKR_WGPU_ACQUIRE_OPTIMAL), MDKR_WGPU_SURFACE_PRESENT);

    expect_true("copy policy", mdkr_wgpu_surface_policy(
        &policy, render | dst | src, render, dst, src, true));
    expect_true("copy present selected", policy.copy_present);
    expect_true("surface readback selected", policy.surface_readback);
    expect_true("all supported usages requested",
                policy.configured_usages == (render | dst | src));

    expect_true("render-blit policy", mdkr_wgpu_surface_policy(
        &policy, render, render, dst, src, true));
    expect_true("render blit selected", policy.render_blit_present);
    expect_true("unsupported readback omitted", !policy.surface_readback);
    expect_true("only attachment requested", policy.configured_usages == render);

    expect_true("missing attachment rejected", !mdkr_wgpu_surface_policy(
        &policy, dst | src, render, dst, src, false));

    expect_true("shader attribute capacity is shared",
        sizeof(((struct WgpuShaderInfo *)0)->attrs) /
            sizeof(((struct WgpuShaderInfo *)0)->attrs[0]) ==
        MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY);
    expect_true("shader layout accepted", mdkr_wgpu_shader_layout_supported(
        8, 7, MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY, 16, 16));
    expect_true("zero-attribute shader rejected",
        !mdkr_wgpu_shader_layout_supported(
            0, 0, MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY, 16, 16));
    expect_true("CPU attribute overflow rejected",
        !mdkr_wgpu_shader_layout_supported(
            MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY + 1, 8,
            MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY, 64, 64));
    expect_true("device attribute overflow rejected",
        !mdkr_wgpu_shader_layout_supported(
            17, 8, MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY, 16, 16));
    expect_true("device varying overflow rejected",
        !mdkr_wgpu_shader_layout_supported(
            16, 17, MDKR_WGPU_SHADER_ATTRIBUTE_CAPACITY, 16, 16));

    puts("test_webgpu_lifecycle: PASS");
    return 0;
}
