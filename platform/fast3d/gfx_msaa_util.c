/*
 * gfx_msaa_util.c — MSAA sample-count resolution (FID-0018).
 * See gfx_msaa_util.h for the finding and the cross-backend contract.
 */
#include "gfx_msaa_util.h"

int gfxMsaaResolveSampleCount(int requested, unsigned supported_mask) {
    /* 0/1 (and any nonsense < 2) => MSAA off. Metal's single-sample value is 1. */
    if (requested < 2) {
        return 1;
    }
    /* Largest ladder count <= requested that the device supports. The ladder is
     * {8,4,2}, identical to gfx_opengl_effective_msaa_samples(). */
    if (requested >= 8 && (supported_mask & GFX_MSAA_SUP_8)) {
        return 8;
    }
    if (requested >= 4 && (supported_mask & GFX_MSAA_SUP_4)) {
        return 4;
    }
    if (requested >= 2 && (supported_mask & GFX_MSAA_SUP_2)) {
        return 2;
    }
    /* Requested a level the device can't provide, and no smaller supported step
     * fits under it -> fall back to off rather than silently over-sampling. */
    return 1;
}

int gfxMsaaPipelineCountsConsistent(int pass_samples,
                                    const int *pipeline_samples, int count) {
    if (count > 0 && pipeline_samples == 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (pipeline_samples[i] != pass_samples) {
            return 0;
        }
    }
    return 1;
}
