/*
 * gfx_msaa_util.h — MSAA sample-count resolution (FID-0018).
 *
 * Pure (ROM-free, Metal-free, SDL-free) so the native Metal backend
 * (src/platform/fast3d/gfx_metal.mm) and the ROM-free unit test
 * (tests/test_msaa_sample_count.c) exercise the exact same clamp arithmetic.
 *
 * FINDING (FID-0018): the Metal backend hardcoded rasterSampleCount = 1 on every
 * pipeline (gfx_metal.mm:1097 caller passed 1) and never built a multisample
 * scene target, so Video.MSAA (0/2/4/8) was silently ignored on Apple/Metal —
 * geometry edges aliased regardless of the user's setting, and alpha-tested
 * cutout surfaces (foliage, reticle, decals) shimmered with no alpha-to-coverage.
 * The GL backend honors the same setting via gfx_opengl_effective_msaa_samples()
 * (a requested->GL_MAX_SAMPLES clamp) plus GL_SAMPLE_ALPHA_TO_COVERAGE. This
 * helper is the Metal mirror of GL's clamp ladder, factored out so the selection
 * logic is unit-testable without a live MTLDevice.
 *
 * Cross-backend contract intentionally matched to GL:
 *   - the ladder is {8,4,2}: pick the largest supported count <= requested;
 *   - a requested level of 0 or 1 means MSAA OFF;
 *   - when MSAA is on the alpha-tested material path enables A2C, and SSAO is
 *     disabled (GL disables SSAO under MSAA — see gfx_opengl.c / the "SSAO off
 *     under MSAA" limit; Metal reinstates it only when MSAA is actually on).
 *
 * The Metal caller reports "off" as rasterSampleCount == 1 (Metal's single-sample
 * value), not 0 (GL's), so this helper returns 1 for off.
 */
#ifndef MGB64_GFX_MSAA_UTIL_H
#define MGB64_GFX_MSAA_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Device-supported sample-count bitmask (what MTLDevice supportsTextureSampleCount:
 * reports). The caller ORs these together for the counts the device supports. */
#define GFX_MSAA_SUP_2 0x1u
#define GFX_MSAA_SUP_4 0x2u
#define GFX_MSAA_SUP_8 0x4u

/* Resolve a requested MSAA level to an effective Metal rasterSampleCount.
 *
 *   requested       — the Video.MSAA setting (0/1/2/4/8; arbitrary ints tolerated).
 *   supported_mask  — OR of GFX_MSAA_SUP_* for the counts the device supports.
 *
 * Returns 1 (MSAA OFF, single-sample) when requested < 2, or when no ladder
 * candidate <= requested is device-supported. Otherwise the largest of {8,4,2}
 * that is both <= requested AND present in supported_mask. Mirrors GL's
 * gfx_opengl_effective_msaa_samples() ladder exactly (which returns 0 for off).
 */
int gfxMsaaResolveSampleCount(int requested, unsigned supported_mask);

/* Regression guard for Metal's hard rule that every pipeline rendering into a
 * single render pass must share that pass's rasterSampleCount (a mismatch aborts
 * at draw time — the reason the count was hardcoded to 1 before this fix).
 * Returns 1 iff all `count` entries of pipeline_samples equal pass_samples
 * (count == 0 is vacuously consistent). */
int gfxMsaaPipelineCountsConsistent(int pass_samples,
                                    const int *pipeline_samples, int count);

#ifdef __cplusplus
}
#endif

#endif /* MGB64_GFX_MSAA_UTIL_H */
