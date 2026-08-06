#include "camera.h"
#include "audio.h"
#include "game.h"
#include "libultra/src/libc/rmonPrintf.h"
#include "math_util.h"
#include "menu.h"
#include "objects.h"
#include "PRinternal/piint.h"
#include "PRinternal/viint.h"
#include "textures_sprites.h"
#include "tracks.h"
#include "video.h"
#include "weather.h"

#ifdef NATIVE_PORT
#include "camera_obstruction_runtime.h"
#include "display_config.h"
#include "fast3d/gfx_presentation_packet.h"
#include "fast3d/gfx_shadow_frame.h"
#include "presentation_snapshot.h"
#include "thread3_main.h"
#include <math.h>
#include <stdio.h>  /* fprintf — the NULL-sprite assert below */
#include <stdlib.h> /* abort   — ditto */
#include <string.h> /* memcpy  — mdkr_camera_replay_mvp */
#endif

#define CAMERA_MODEL_STACK_SIZE 5

/************ .data ************/

s8 gAntiPiracyViewport = FALSE;

// x1, y1, x2, y2
// posX, posY, width, height
// scissorX1, scissorY1, scissorX2, scissorY2
// flags
#define DEFAULT_VIEWPORT                                                                                         \
    0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH_HALF, SCREEN_HEIGHT_HALF, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, \
        SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, 0

#define SCISSOR_INTERLACE G_SC_NON_INTERLACE

#ifdef NATIVE_PORT
/*
 * LAYOUT (split-array class). gActiveCameraID reaches 4 — viewport_reset() sets it
 * to 4 by hand — and both viewport_reset() and viewport_rsp_set() then read
 * `gScreenViewports[gActiveCameraID].flags`. The array is declared [4], so that is
 * an out-of-bounds read of whatever the linker parked after it. UBSan
 * -fsanitize=array-bounds reports it on EVERY route this project has
 * (camera.c:1005 and camera.c:1027 pre-fix, 99 of 99 runs in the sweep).
 *
 * What it actually reads today, on all three targets, is gViewportStack[2]:
 *
 *   target   &gScreenViewports  +256 lands on          bit 0 of that halfword
 *   N64      0x800DD068         gViewportStack[2]      vscale[1] = halfHeight*4
 *   native   0x1008ec048        gViewportStack[2]      vscale[0] = tempWidth*4
 *   wasm32   0x00019840         gViewportStack[2]      vscale[0] = tempWidth*4
 *
 * VIEWPORT_EXTRA_BG is 0x0001 and viewport_rsp_set() only ever stores multiples of
 * 4 into vscale, so the aliased halfword's bit 0 is always 0 and the read always
 * yields "no extra background". That is why this has been silent: it is benign by
 * arithmetic accident, not by design, and it is one relink away from flipping
 * viewport_reset() and viewport_rsp_set() onto the wrong branch for the
 * full-screen camera.
 *
 * Give index 4 a real element. This is provably behaviour-identical: every write
 * path (camEnableUserView / camDisableUserView / viewport_menu_set, all called
 * with index 0, plus copy_viewports_to_stack(), which loops i < 4) uses viewport
 * index 0..3, so element 4 keeps its initialiser forever, and DEFAULT_VIEWPORT's
 * flags field is 0 — exactly what the out-of-bounds read returns. It also makes
 * the (currently unreachable) scissor read at camera.c:665 return sane defaults
 * instead of gViewportStack bytes.
 *
 * Detector: tests/check_array_bounds_sweep.py fails if this goes back out of
 * bounds (verified — reverting this block makes it report
 * `camera.c index 4 out of bounds for type 'ScreenViewport[4]'`).
 */
ScreenViewport gScreenViewports[5] = {
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT }, /* gActiveCameraID == 4: the full-screen / menu camera */
};
_Static_assert(ARRAY_COUNT(gScreenViewports) > 4,
               "viewport_reset() sets gActiveCameraID = 4 and then indexes this array with it, "
               "so index 4 must be a real element and not whatever the linker placed after it");
static ViewportWorldRegion sViewportWorldRegions[ARRAY_COUNT(gScreenViewports)];
#else
ScreenViewport gScreenViewports[4] = {
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
    { DEFAULT_VIEWPORT },
};
#endif

u32 gViewportWithBG = FALSE;

Vertex gVehiclePartVertex = { 0, 0, 0, 255, 255, 255, 255 };

// The viewport z-range below is half of the max (511)
#define G_HALFZ (G_MAXZ / 2) /* 9 bits of integer screen-Z precision */

// RSP Viewports
Vp gViewportStack[20] = {
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
    { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } }, { { { 0, 0, G_HALFZ, 0 }, { 0, 0, G_HALFZ, 0 } } },
};

ObjectTransform D_800DD288 = {
    { { { 0, 0, 0 } } },
    { 0 },
    1.0f,
    { { { { 0.0f, 0.0f, -281.0f } } } },
};

ObjectTransform D_800DD2A0 = {
    { { { 0, 0, 0 } } },
    { 0 },
    1.0f,
    { { { { 0.0f, 0.0f, 0.0f } } } },
};

MtxF gOrthoMatrixF = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 160.0f },
};

u8 gCameraZoomLevels[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/*******************************/

/************ .bss ************/

Camera gCameras[8];
s32 gViewportLayout;
s32 gActiveCameraID;
s32 gNumCameras;
UNUSED s32 D_80120CEC;
ObjectTransform gCameraTransform;
s32 gMtxOriginID;
s32 gSpriteAnimMode;
f32 gCurCamFOV;
s8 gCutsceneCameraActive;
s8 gAdjustViewportHeight;
s32 gNoCamShake;
s32 gModelMatrixStackPos;
s32 gCameraMatrixPos;
UNUSED s32 D_80120D24;
f32 gCameraRelPosStackX[CAMERA_MODEL_STACK_SIZE + 1];
f32 gCameraRelPosStackY[CAMERA_MODEL_STACK_SIZE + 1];
#ifdef NATIVE_PORT
/*
 * CONTAINMENT (split-array class), not a demonstrated bug — read the measurement
 * before removing it. mtx_cam_push() writes all THREE of these at the same index
 * and only afterwards warns `if (gCameraMatrixPos > CAMERA_MODEL_STACK_SIZE)`, so
 * the code's own contract is that index 5 is legal. X and Y are sized for it; Z is
 * one element short, so a Z write at index 5 lands in the next object, and which
 * object that is differs per target:
 *
 *   native  gCameraRelPosStackZ 0x1009ae004 + 20 == 0x1009ae018 == gCameraTransform
 *   wasm32  gCameraRelPosStackZ 0x00028e80  + 20 == 0x00028e94  == gTransitionsDisabled
 *   N64     Z + 20 == perspNorm (the gSPPerspNormalize value)
 *
 * i.e. on wasm32 it is a silent 4-byte write into an unrelated subsystem's flag.
 * gCameraMatrixPos was measured to never come near 5: a peak probe over 9 tracks
 * (including every wave track and boss 38), the attract demo, 2P split-screen, the
 * Adventure hub loop and the menu graph recorded **max gCameraMatrixPos = 1** and
 * max gModelMatrixStackPos = 1, and UBSan -fsanitize=array-bounds reported nothing
 * here across the whole 99-run sweep. So this is unreachable today and sizing Z
 * like X and Y is a no-op; it exists so that if some future track does push a
 * sixth matrix, the write stays inside its own array instead of corrupting
 * whatever the linker happened to put next.
 *
 * (Related and deliberately NOT touched: gModelMatrixF is [6] but cam_init() only
 * fills [0..4] — D_80120DA0 only has storage for 5 matrices — so gModelMatrixF[5]
 * is permanently NULL and mtx_cam_push()'s `gModelMatrixF[pos + 1]` would write
 * through it at pos == 4. That is equally true on the N64, so the real game cannot
 * reach pos 4 either; measured max 1. Faithful, latent, left alone.)
 */
f32 gCameraRelPosStackZ[CAMERA_MODEL_STACK_SIZE + 1];
_Static_assert(ARRAY_COUNT(gCameraRelPosStackZ) == ARRAY_COUNT(gCameraRelPosStackX),
               "mtx_cam_push() writes X, Y and Z at the same index — they must be the same length");
#else
f32 gCameraRelPosStackZ[CAMERA_MODEL_STACK_SIZE];
#endif
u16 perspNorm;
MtxF *gModelMatrixF[CAMERA_MODEL_STACK_SIZE + 1];
Mtx *gModelMatrix[CAMERA_MODEL_STACK_SIZE + 1];
f32 D_80120DA0[CAMERA_MODEL_STACK_SIZE * 16];
MtxF gPerspectiveMatrixF;
MtxF gViewProjMatrixF;
MtxF gViewMatrixF;
MtxF gInverseViewMatrixF;
Mtx gPerspectiveMatrix;
UNUSED Mtx gInverseViewMatrix;
MtxF gCurrentModelMatrixF;
MtxF gCurrentMVPMatrixF;
#ifdef NATIVE_PORT
/*
 * The N64 display list carries only a model-view-projection matrix. Modern
 * directional shadows also need the world transform, but replaying game
 * rendering to recover it would advance animation/RNG twice. Copy the
 * world/view-projection pair into a host side registry keyed by the exact Mtx
 * allocation already referenced by G_MTX. The HLE consumes the copy while
 * walking the one real display list.
 */
/*
 * Which camera gViewProjMatrixF currently belongs to, and whether it is the
 * GAMEPLAY view-projection at all.
 *
 * This distinction is load-bearing for Phase 3 Wave B and invisible from the
 * registry side: gViewProjMatrixF is not one matrix per frame. cam_build_view_basis()
 * installs the active camera's VP, but mtx_perspective() then overwrites it
 * with a fixed z=-281 transform for the 2D/model-viewer path and mtx_ortho()
 * replaces it with the ortho matrix outright. A presentation replay that
 * substituted an interpolated gameplay camera into entries registered under
 * either of those would swim the HUD, the menus and the transition masks
 * against a still world. Tagging at the registration site is the only place
 * that knows.
 */
static s32 sShadowRegisterViewport = 0;
static s32 sShadowRegisterGameplayVp = 0;
/* Owner recipe parallel to the game's model stack. It contains copied POD
 * only; the address is an identity token paired with a spawn generation and
 * is never dereferenced by the replay. */
static GfxPresentationMatrixOwner
    sPresentationOwnerStack[CAMERA_MODEL_STACK_SIZE + 1];

static bool mdkr_presentation_owner_root(
    GfxPresentationMatrixOwner *out, const ObjectTransform *transform,
    f32 scaleY, f32 offsetY, const MtxF *parentWorld) {
    uint64_t generation = 0;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (transform == NULL || parentWorld == NULL ||
        !presentation_snapshot_identity_generation(transform, &generation)) {
        return false;
    }
    out->address = transform;
    out->generation = generation;
    out->matrix_class = GFX_PRESENTATION_MATRIX_ROOT;
    out->source_position[0] = transform->x_position;
    out->source_position[1] = transform->y_position;
    out->source_position[2] = transform->z_position;
    out->source_scale = transform->scale;
    out->source_rotation[0] = transform->rotation.y_rotation;
    out->source_rotation[1] = transform->rotation.x_rotation;
    out->source_rotation[2] = transform->rotation.z_rotation;
    out->scale_y = scaleY;
    out->offset_y = offsetY;
    memcpy(out->parent_world, parentWorld, sizeof(out->parent_world));
    out->valid = true;
    return true;
}

static bool mdkr_presentation_owner_child(
    GfxPresentationMatrixOwner *out,
    const GfxPresentationMatrixOwner *root,
    const MtxF *childLocal) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (root == NULL || !root->valid || childLocal == NULL) {
        return false;
    }
    *out = *root;
    out->matrix_class = GFX_PRESENTATION_MATRIX_CHILD;
    memcpy(out->child_local, childLocal, sizeof(out->child_local));
    out->valid = true;
    return true;
}

static void mdkr_shadow_register_matrix(
    Mtx *key,
    const MtxF *world,
    GfxShadowMobility mobility,
    s32 site,
    const GfxPresentationMatrixOwner *owner) {
    if (key != NULL && world != NULL) {
        gfx_shadow_matrix_set_context(
            (int)sShadowRegisterViewport, sShadowRegisterGameplayVp != 0);
        gfx_shadow_matrix_set_site((int)site);
        gfx_shadow_matrix_set_presentation_owner(owner);
        (void)gfx_shadow_matrix_register(
            key, *world, gViewProjMatrixF, mobility);
    }
}

/*
 * Recompose one registered matrix into the N64 fixed-point form a G_MTX command
 * carries, for a presentation replay (Phase 3 Wave B).
 *
 * This lives in camera.c, and calls mtxf_mul/mtxf_to_mtx rather than
 * reimplementing them in the renderer, ON PURPOSE. When the replay uses the
 * captured view-projection unchanged, the result MUST be bit-identical to the
 * matrix the display list already holds, or a "zero-delta" replay would not be
 * zero-delta and the slice-1 gate would be measuring rounding instead of
 * correctness. mtxf_mul's addition order is deliberately reassociated to match
 * the original hardware (see math_util.c) and mtxf_to_mtx rounds through
 * mdkr_mips_round_w_s; a second implementation in another translation unit
 * would be at the mercy of that unit's FP contraction settings. Calling the
 * same two functions with the same inputs makes bit-identity structural.
 */
void mdkr_camera_replay_mvp(
    const f32 world[4][4], const f32 viewProjection[4][4], void *outMtx) {
    MtxF worldF;
    MtxF viewProjectionF;
    MtxF mvp;

    if (world == NULL || viewProjection == NULL || outMtx == NULL) {
        return;
    }
    memcpy(worldF, world, sizeof(worldF));
    memcpy(viewProjectionF, viewProjection, sizeof(viewProjectionF));
    mtxf_mul(&worldF, &viewProjectionF, &mvp);
    mtxf_to_mtx(&mvp, (Mtx *)outMtx);
}

/*
 * Rebuild an object-owned world matrix from immutable presentation state.
 * The address is used only with the generation-keyed snapshot index; no live
 * Object is dereferenced and no authoritative memory is written.
 *
 * mtx_cam_push can observe temporary render-only adjustments (racer tumble,
 * bob and model scale) which have been restored by snapshot time. Carry the
 * exact residual from the registered transform onto the interpolated pose.
 *
 * The retained display list was authored from the PREVIOUS snapshot: DKR
 * submits one list while it builds the next. The residual must therefore be
 * measured against alpha zero, not the newer snapshot. Measuring it against
 * alpha one extrapolates every moving object backward at alpha zero instead of
 * reproducing the retained list's authored endpoint.
 */
bool mdkr_camera_replay_object_world(
    const GfxPresentationMatrixOwner *owner, u64 numerator, u64 denominator,
    f32 outWorld[4][4]) {
    PresentationObjectPose authored;
    PresentationObjectPose target;
    ObjectTransform transform;
    MtxF rootLocal;
    MtxF rootWorld;

    if (owner == NULL || !owner->valid || outWorld == NULL ||
        (owner->matrix_class != GFX_PRESENTATION_MATRIX_ROOT &&
         owner->matrix_class != GFX_PRESENTATION_MATRIX_CHILD)) {
        return false;
    }
    if (!presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation, 0u, denominator,
            &authored) ||
        !presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation, numerator, denominator,
            &target) ||
        !target.interpolated) {
        return false;
    }

    memset(&transform, 0, sizeof(transform));
    transform.x_position =
        target.position[0] +
        (owner->source_position[0] - authored.position[0]);
    transform.y_position =
        target.position[1] +
        (owner->source_position[1] - authored.position[1]);
    transform.z_position =
        target.position[2] +
        (owner->source_position[2] - authored.position[2]);
    transform.rotation.y_rotation = (s16)(
        target.rotation_y +
        (s16)(owner->source_rotation[0] - authored.rotation_y));
    transform.rotation.x_rotation = (s16)(
        target.rotation_x +
        (s16)(owner->source_rotation[1] - authored.rotation_x));
    transform.rotation.z_rotation = (s16)(
        target.rotation_z +
        (s16)(owner->source_rotation[2] - authored.rotation_z));
    if (authored.scale != 0.0f) {
        transform.scale =
            target.scale * (owner->source_scale / authored.scale);
    } else {
        transform.scale = target.scale +
                          (owner->source_scale - authored.scale);
    }

    mtxf_from_transform(&rootLocal, &transform);
    if (owner->offset_y != 0.0f) {
        mtxf_translate_y(&rootLocal, owner->offset_y);
    }
    if (owner->scale_y != 1.0f) {
        mtxf_scale_y(&rootLocal, owner->scale_y);
    }
    mtxf_mul(&rootLocal, (MtxF *)&owner->parent_world, &rootWorld);
    if (owner->matrix_class == GFX_PRESENTATION_MATRIX_CHILD) {
        mtxf_mul((MtxF *)&owner->child_local, &rootWorld,
                 (MtxF *)outWorld);
    } else {
        memcpy(outWorld, rootWorld, sizeof(rootWorld));
    }
    return true;
}

static bool mdkr_camera_replay_object_transform(
    const GfxPresentationMatrixOwner *owner, u64 numerator, u64 denominator,
    ObjectTransform *out) {
    PresentationObjectPose authored;
    PresentationObjectPose target;

    if (owner == NULL || !owner->valid || out == NULL ||
        !presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation, 0u, denominator,
            &authored) ||
        !presentation_snapshot_resolve_object_generation(
            owner->address, owner->generation, numerator, denominator,
            &target) ||
        !target.interpolated) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->x_position =
        target.position[0] +
        (owner->source_position[0] - authored.position[0]);
    out->y_position =
        target.position[1] +
        (owner->source_position[1] - authored.position[1]);
    out->z_position =
        target.position[2] +
        (owner->source_position[2] - authored.position[2]);
    out->rotation.y_rotation = (s16)(
        target.rotation_y +
        (s16)(owner->source_rotation[0] - authored.rotation_y));
    out->rotation.x_rotation = (s16)(
        target.rotation_x +
        (s16)(owner->source_rotation[1] - authored.rotation_x));
    out->rotation.z_rotation = (s16)(
        target.rotation_z +
        (s16)(owner->source_rotation[2] - authored.rotation_z));
    if (authored.scale != 0.0f) {
        out->scale = target.scale * (owner->source_scale / authored.scale);
    } else {
        out->scale = target.scale + (owner->source_scale - authored.scale);
    }
    return true;
}

/* Rebuild the authored shield/magnet matrix from semantic parameters. This
 * mirrors mtx_shear_push's expression order instead of lerping matrix cells:
 * the local recipe remains a rotation/scale/shear and the current endpoint is
 * still verified against the display-list matrix before replay trusts it. */
static void mdkr_camera_effect_world_from_transforms(
    const ObjectTransform *effect, const ObjectTransform *base, f32 shear,
    f32 out[4][4]) {
    const f32 cxe = coss_f(effect->rotation.x_rotation);
    const f32 sxe = sins_f(effect->rotation.x_rotation);
    const f32 cye = coss_f(effect->rotation.y_rotation);
    const f32 sye = sins_f(effect->rotation.y_rotation);
    const f32 czb = coss_f(base->rotation.z_rotation);
    const f32 szb = sins_f(base->rotation.z_rotation);
    const f32 cxb = coss_f(base->rotation.x_rotation);
    const f32 sxb = sins_f(base->rotation.x_rotation);
    const f32 cyb = coss_f(base->rotation.y_rotation);
    const f32 syb = sins_f(base->rotation.y_rotation);
    const f32 scale = effect->scale;

    out[0][0] = ((((czb * cyb) + (szb * (sxb * syb))) * cye) +
                 (-sye * (cxb * syb))) * scale;
    out[0][1] = (((szb * cxb) * cye) + (-sye * -sxb)) * scale;
    out[0][2] = ((((-syb * czb) + (szb * (sxb * cyb))) * cye) +
                 (-sye * (cxb * cyb))) * scale;
    out[0][3] = 0.0f;
    out[1][0] = ((((-szb * cyb) + (czb * (sxb * syb))) * cxe) +
                 (sxe * ((sye * ((czb * cyb) + (szb * (sxb * syb)))) +
                         (cye * (cxb * syb))))) * shear;
    out[1][1] = (((czb * cxb) * cxe) +
                 (sxe * ((sye * (szb * cxb)) + (cye * -sxb)))) *
                shear;
    out[1][2] = ((((-szb * -syb) + (czb * (sxb * cyb))) * cxe) +
                 (sxe * ((sye * ((-syb * czb) + (szb * (sxb * cyb)))) +
                         (cye * (cxb * cyb))))) * shear;
    out[1][3] = 0.0f;
    out[2][0] = ((-sxe * ((-szb * cyb) + (czb * (sxb * syb)))) +
                 (cxe * ((sye * ((czb * cyb) + (szb * (sxb * syb)))) +
                         (cye * (cxb * syb))))) * scale;
    out[2][1] = ((-sxe * (czb * cxb)) +
                 (cxe * ((sye * (szb * cxb)) + (cye * -sxb)))) * scale;
    out[2][2] = ((-sxe * ((-szb * -syb) + (czb * (sxb * cyb)))) +
                 (cxe * ((sye * ((-syb * czb) + (szb * (sxb * cyb)))) +
                         (cye * (cxb * cyb))))) * scale;
    out[2][3] = 0.0f;
    out[3][0] = (((czb * cyb) + (szb * (sxb * syb))) *
                     effect->x_position) +
                (effect->y_position *
                 ((-szb * cyb) + (czb * (sxb * syb)))) +
                (effect->z_position * (cxb * syb)) + base->x_position;
    out[3][1] = ((szb * cxb) * effect->x_position) +
                (effect->y_position * (czb * cxb)) +
                (effect->z_position * -sxb) + base->y_position;
    out[3][2] = (((-syb * czb) + (szb * (sxb * cyb))) *
                     effect->x_position) +
                (effect->y_position *
                 ((-szb * -syb) + (czb * (sxb * cyb)))) +
                (effect->z_position * (cxb * cyb)) + base->z_position;
    out[3][3] = 1.0f;
}

bool mdkr_camera_replay_effect_world(
    const GfxPresentationMatrixOwner *previous,
    const GfxPresentationMatrixOwner *current, u64 numerator,
    u64 denominator, f32 outWorld[4][4]) {
    ObjectTransform base;
    ObjectTransform effect;
    f32 shear;

    if (previous == NULL || current == NULL || outWorld == NULL ||
        previous->matrix_class != GFX_PRESENTATION_MATRIX_EFFECT ||
        current->matrix_class != GFX_PRESENTATION_MATRIX_EFFECT ||
        previous->address != current->address ||
        previous->generation != current->generation ||
        previous->secondary_address == NULL ||
        previous->secondary_address != current->secondary_address ||
        previous->secondary_generation == 0u ||
        previous->secondary_generation != current->secondary_generation ||
        !mdkr_camera_replay_object_transform(
            current, numerator, denominator, &base)) {
        return false;
    }
    memset(&effect, 0, sizeof(effect));
    presentation_lerp3(previous->effect_position, current->effect_position,
                       numerator, denominator, effect.position.f);
    effect.scale = presentation_lerp1(
        previous->effect_scale, current->effect_scale,
        numerator, denominator);
    effect.rotation.y_rotation = presentation_lerp_angle(
        previous->effect_rotation[0], current->effect_rotation[0],
        numerator, denominator);
    effect.rotation.x_rotation = presentation_lerp_angle(
        previous->effect_rotation[1], current->effect_rotation[1],
        numerator, denominator);
    effect.rotation.z_rotation = presentation_lerp_angle(
        previous->effect_rotation[2], current->effect_rotation[2],
        numerator, denominator);
    shear = presentation_lerp1(
        previous->effect_shear, current->effect_shear,
        numerator, denominator);
    mdkr_camera_effect_world_from_transforms(
        &effect, &base, shear, outWorld);
    return true;
}

bool mdkr_camera_replay_billboard_anchor(
    const GfxPresentationMatrixOwner *owner, u64 numerator, u64 denominator,
    f32 outPosition[3]) {
    ObjectTransform transform;

    if (outPosition == NULL ||
        !mdkr_camera_replay_object_transform(
            owner, numerator, denominator, &transform)) {
        return false;
    }
    outPosition[0] = transform.x_position;
    outPosition[1] = transform.y_position;
    outPosition[2] = transform.z_position;
    return true;
}

bool mdkr_camera_replay_billboard_matrix(
    const GfxPresentationMatrixOwner *owner, s32 viewport, u64 numerator,
    u64 denominator, void *outMtx) {
    ObjectTransform transform;
    PresentationCameraPose camera;
    MtxF billboard;
    s16 tilt;
    MdkrBillboardCorrection correction;

    if (outMtx == NULL || viewport < 0 ||
        !mdkr_camera_replay_object_transform(
            owner, numerator, denominator, &transform) ||
        !presentation_snapshot_resolve_camera(
            viewport, numerator, denominator, &camera)) {
        return false;
    }
    tilt = (s16)(camera.rotation_z + transform.rotation.z_rotation);
    mtxf_billboard(&billboard, tilt, transform.scale, gVideoAspectRatio);
    correction = mdkr_display_calculate_billboard_correction(
        camera.fov, camera.vertical_fov, gVideoAspectRatio, camera.aspect,
        mdkr_display_widescreen_enabled());
    mdkr_display_apply_billboard_correction(billboard, correction);
    mtxf_to_mtx(&billboard, (Mtx *)outMtx);
    return true;
}


/*
 * The authored FOV remains gCurCamFOV.  These are the projection actually used
 * for the current host aspect/viewport after the optional horizontal safety cap.
 * tracks.c consumes the horizontal value to widen its CPU culling planes in
 * lockstep with the lens.
 */
f32 gEffectiveCamVFOV = CAMERA_DEFAULT_FOV;
f32 gEffectiveCamHFOV = 75.17818f; /* 60-degree vertical lens at 4:3 */
f32 gEffectiveCamAspect = CAMERA_ASPECT;
static MdkrCameraProjection sNativeProjectionByViewport[4];
static bool sNativeProjectionValid[4];
static s32 sNativeOrthoDrawSpace = G_MTX_DKR_SPACE_SAFE_2D;
#endif

/******************************/

extern s32 D_B0000578; // Used as a symbol for anti-piracy checks in the game.

#ifdef NATIVE_PORT
/**
 * Produce the exact record a fixed-tick camera resolver will later latch.  This
 * is deliberately callable before any display-list work; no viewport scissor or
 * renderer global participates in lens selection.
 *
 * DKR clips two-player views with scissors while retaining a full-height RSP
 * viewport, so the logical dimensions belong to the RSP viewport, not the
 * scissor rectangle; display_config owns that mapping.  The safe 4:3 region is
 * the caller's channel selection, not a property read here, because render and
 * the resolver ask for different regions of the same viewport; a safe-region
 * view is never a gameplay lens.
 */
static bool cam_projection_for_viewport_region(
    s32 viewport, s32 cameraID, bool gameplayCamera, s32 safeWorldRegion,
    MdkrCameraProjection *out) {
    MdkrDisplayLayout layout;
    MdkrCameraProjectionRequest request;

    layout = mdkr_display_layout();
    memset(&request, 0, sizeof(request));
    request.authored_vertical_fov = gCurCamFOV;
    request.presentation_aspect =
        safeWorldRegion ? CAMERA_ASPECT : layout.presentation_aspect;
    request.gameplay_vertical_fov = mdkr_display_gameplay_fov();
    request.maximum_horizontal_fov = mdkr_display_max_horizontal_fov();
    request.near_plane = CAMERA_NEAR;
    request.far_plane = CAMERA_FAR;
    request.display_generation = mdkr_display_config_generation();
    request.viewport_layout = gViewportLayout;
    request.viewport = viewport;
    request.camera_id = cameraID;
    request.widescreen_enabled = mdkr_display_widescreen_enabled();
    /* Camera IDs 4..7 are the scripted/cutscene bank.  Their authored lens is
     * not scaled by the gameplay FOV option even if a cutscene flag changes
     * between camera selection and this query. */
    request.gameplay_camera = gameplayCamera && !safeWorldRegion;
    return mdkr_display_calculate_camera_projection(&request, out);
}

bool cam_effective_projection_for_viewport_context(
    s32 viewport, s32 cameraID, bool gameplayCamera,
    MdkrCameraProjection *out) {
    return cam_projection_for_viewport_region(
        viewport, cameraID, gameplayCamera,
        viewport_world_region_uses_safe_aperture(viewport), out);
}

/**
 * The obstruction resolver's lens channel.
 *
 * A framed view narrows the drawn image, not the viewport it is drawn into: the
 * frame retracts inside one authored image and the safe aperture is a
 * presentation property the resolver never observes. Guarding the presentation
 * lens keeps the resolver's guard a superset of every image the same viewport
 * can publish, so a shot validated behind the frame is still valid without it.
 * Render must keep using the latched safe-aperture record instead.
 */
bool cam_resolver_projection_for_viewport_context(
    s32 viewport, s32 cameraID, bool gameplayCamera,
    MdkrCameraProjection *out) {
    return cam_projection_for_viewport_region(
        viewport, cameraID, gameplayCamera, FALSE, out);
}

bool cam_effective_projection_for_viewport(
    s32 viewport, s32 cameraID, MdkrCameraProjection *out) {
    const bool gameplayCamera =
        cameraID >= 0 && cameraID < 4 && get_game_mode() == GAMEMODE_INGAME &&
        !gCutsceneCameraActive;
    return cam_effective_projection_for_viewport_context(
        viewport, cameraID, gameplayCamera, out);
}

bool cam_latch_effective_projection_for_viewport_context(
    s32 viewport, s32 cameraID, bool gameplayCamera,
    MdkrCameraProjection *out) {
    MdkrCameraProjection projection;

    if (viewport < 0 || viewport >= ARRAY_COUNT(sNativeProjectionByViewport) ||
        !cam_effective_projection_for_viewport_context(
            viewport, cameraID, gameplayCamera, &projection)) {
        return false;
    }
    sNativeProjectionByViewport[viewport] = projection;
    sNativeProjectionValid[viewport] = true;
    if (out != NULL) {
        *out = projection;
    }
    return true;
}

bool cam_latch_effective_projection_for_viewport(
    s32 viewport, s32 cameraID, MdkrCameraProjection *out) {
    const bool gameplayCamera =
        cameraID >= 0 && cameraID < 4 && get_game_mode() == GAMEMODE_INGAME &&
        !gCutsceneCameraActive;
    return cam_latch_effective_projection_for_viewport_context(
        viewport, cameraID, gameplayCamera, out);
}

bool cam_get_latched_effective_projection_for_viewport(
    s32 viewport, MdkrCameraProjection *out) {
    if (out == NULL || viewport < 0 || viewport >= ARRAY_COUNT(sNativeProjectionByViewport) ||
        !sNativeProjectionValid[viewport]) {
        return false;
    }
    *out = sNativeProjectionByViewport[viewport];
    return true;
}

bool cam_restore_latched_effective_projection_for_viewport(
    s32 viewport, s32 cameraID, const MdkrCameraProjection *projection) {
    if (projection == NULL || viewport < 0 ||
        viewport >= ARRAY_COUNT(sNativeProjectionByViewport) ||
        projection->viewport != viewport || projection->camera_id != cameraID ||
        projection->camera_bank != cameraID / 4 ||
        projection->generation == 0U || projection->display_generation == 0U ||
        projection->display_generation != mdkr_display_config_generation() ||
        !isfinite(projection->logical_viewport_width) ||
        projection->logical_viewport_width <= 0.0f ||
        !isfinite(projection->logical_viewport_height) ||
        projection->logical_viewport_height <= 0.0f ||
        !isfinite(projection->aspect) || projection->aspect <= 0.0f ||
        !isfinite(projection->vertical_fov) || projection->vertical_fov <= 0.0f ||
        projection->vertical_fov >= 180.0f ||
        !isfinite(projection->horizontal_fov) || projection->horizontal_fov <= 0.0f ||
        projection->horizontal_fov >= 180.0f ||
        !isfinite(projection->near_plane) || projection->near_plane <= 0.0f ||
        !isfinite(projection->far_plane) ||
        projection->far_plane <= projection->near_plane ||
        (projection->horizontal_fov_capped != 0 &&
         projection->horizontal_fov_capped != 1)) {
        return false;
    }
    sNativeProjectionByViewport[viewport] = *projection;
    sNativeProjectionValid[viewport] = true;
    return true;
}

/* Render receives the record latched by the fixed-tick finalizer, never loose
 * aspect/FOV values. Do not turn this into a query: rendering an unvalidated
 * wider projection is precisely the mismatch this contract prevents. */
static void cam_rebuild_native_projection(s32 viewport, s32 cameraID) {
    MdkrCameraProjection projection;

    if (!cam_get_latched_effective_projection_for_viewport(viewport, &projection) ||
        projection.camera_id != cameraID ||
        !camera_obstruction_projection_matches_render(
            viewport, cameraID, projection.generation)) {
        return;
    }

    gEffectiveCamVFOV = projection.vertical_fov;
    gEffectiveCamHFOV = projection.horizontal_fov;
    gEffectiveCamAspect = projection.aspect;
    guPerspectiveF(gPerspectiveMatrixF, &perspNorm, projection.vertical_fov,
                   projection.aspect, projection.near_plane, projection.far_plane,
                   CAMERA_SCALE);
    mtxf_to_mtx(&gPerspectiveMatrixF, &gPerspectiveMatrix);
}

f32 cam_get_effective_horizontal_fov(void) {
    return gEffectiveCamHFOV;
}

f32 cam_get_effective_vertical_fov(void) {
    return gEffectiveCamVFOV;
}

f32 cam_get_effective_aspect(void) {
    return gEffectiveCamAspect;
}
#endif

/**
 * Official Name: camInit
 */
void cam_init(void) {
    s32 i;
    s32 j;
    u32 stat;

    // clang-format off
    for (i = 0; i < 5; i++) { gModelMatrixF[i] = (MtxF*)&D_80120DA0[i << 4]; }
    // clang-format on

    for (j = 0; j < 8; j++) {
        gActiveCameraID = j;
        camera_reset(200, 200, 200, 0, 0, 180);
    }

    gCutsceneCameraActive = FALSE;
    gActiveCameraID = 0;
    gModelMatrixStackPos = 0;
    gCameraMatrixPos = 0;
    gViewportLayout = 0;
    gSpriteAnimMode = SPRITE_ANIM_NORMALIZED;
    gNoCamShake = FALSE;
    gAdjustViewportHeight = FALSE;
    gAntiPiracyViewport = FALSE;

#ifdef NATIVE_PORT
    /* Before the latch below: a level/menu-background load starts a new scene,
     * and the projection it latches must not inherit the previous scene's
     * aperture. */
    viewport_world_regions_reset();
#endif

#ifndef NATIVE_PORT
    WAIT_ON_IOBUSY(stat);

    // 0xB0000578 is a direct read from the ROM as opposed to RAM
    if (((D_B0000578 & 0xFFFF) & 0xFFFF) != 0x8965) {
        gAntiPiracyViewport = TRUE;
    }
#else
    /* Anti-piracy: WAIT_ON_IOBUSY spins on the PI status register (0xA4600010)
     * and D_B0000578 reads the cartridge ROM domain (0xB0000578) — both are N64
     * hardware addresses that fault on the host. On genuine hardware this leaves
     * gAntiPiracyViewport FALSE, which is exactly its value from line above, so
     * skipping the whole check is behaviour-preserving for a legitimate ROM. */
    (void) stat;
#endif

    gCurCamFOV = CAMERA_DEFAULT_FOV;
#ifdef NATIVE_PORT
    (void) cam_latch_effective_projection_for_viewport(0, 0, NULL);
    cam_rebuild_native_projection(0, 0);
#else
    guPerspectiveF(gPerspectiveMatrixF, &perspNorm, CAMERA_DEFAULT_FOV, CAMERA_ASPECT, CAMERA_NEAR, CAMERA_FAR,
                   CAMERA_SCALE);
    mtxf_to_mtx(&gPerspectiveMatrixF, &gPerspectiveMatrix);
#endif
}

void cam_set_zoom(s32 cameraID, s32 zoomLevel) {
    if (cameraID >= 0 && cameraID <= 3) {
        gCameraZoomLevels[cameraID] = zoomLevel;
        gCameras[cameraID].zoom = zoomLevel;
    }
}

/**
 * Set gAdjustViewportHeight to PAL mode if necessary, if setting is 1.
 * Otherwise, set it to 0, regardless of TV type.
 */
void enable_pal_viewport_height_adjust(s8 setting) {
    if (osTvType == OS_TV_TYPE_PAL) {
        gAdjustViewportHeight = setting;
    }
}

/**
 * Disable camera shake.
 */
void cam_shake_off(void) {
    gNoCamShake = TRUE;
}

/**
 * Enable camera shake. Camera wiggles up and down with it enabled.
 */
void cam_shake_on(void) {
    gNoCamShake = FALSE;
}

/**
 * Unused function that will return the current camera's FoV.
 * Official Name: camGetFOV
 */
UNUSED f32 cam_get_fov(void) {
    return gCurCamFOV;
}

/**
 * Set the FoV of the viewspace, then recalculate the perspective matrix.
 * Official Name: camSetFOV
 */
void cam_set_fov(f32 camFieldOfView) {
    if (CAMERA_MIN_FOV < camFieldOfView && camFieldOfView < CAMERA_MAX_FOV && camFieldOfView != gCurCamFOV) {
        gCurCamFOV = camFieldOfView;
#ifdef NATIVE_PORT
        s32 cameraID = gActiveCameraID + (gCutsceneCameraActive ? 4 : 0);
        (void) cam_latch_effective_projection_for_viewport(gActiveCameraID, cameraID, NULL);
        cam_rebuild_native_projection(gActiveCameraID, cameraID);
#else
        guPerspectiveF(gPerspectiveMatrixF, &perspNorm, camFieldOfView, CAMERA_ASPECT, CAMERA_NEAR, CAMERA_FAR,
                       CAMERA_SCALE);
        mtxf_to_mtx(&gPerspectiveMatrixF, &gPerspectiveMatrix);
#endif
    }
}

/**
 * Unused function that recalculates the perspective matrix.
 */
UNUSED void cam_reset_fov(void) {
#ifdef NATIVE_PORT
    gCurCamFOV = CAMERA_DEFAULT_FOV;
    {
        s32 cameraID = gActiveCameraID + (gCutsceneCameraActive ? 4 : 0);
        (void) cam_latch_effective_projection_for_viewport(gActiveCameraID, cameraID, NULL);
        cam_rebuild_native_projection(gActiveCameraID, cameraID);
    }
#else
    guPerspectiveF(gPerspectiveMatrixF, &perspNorm, CAMERA_DEFAULT_FOV, CAMERA_ASPECT, CAMERA_NEAR, CAMERA_FAR,
                   CAMERA_SCALE);
    mtxf_to_mtx(&gPerspectiveMatrixF, &gPerspectiveMatrix);
#endif
}

/**
 * Return the current fixed point model matrix.
 */
UNUSED MtxF *mtx_get_modelmtx_s16(void) {
    return &gCurrentMVPMatrixF;
}

/**
 * Returns the number of active viewports.
 */
s32 cam_get_viewport_layout(void) {
    return gViewportLayout;
}

/**
 * Return the index of the active view.
 * 0-3 is players 1-4, and 4-7 is the same, but with 4 added on for cutscenes.
 * Official Name: camGetMode
 */
s32 get_current_viewport(void) {
    return gActiveCameraID;
}

/**
 * Initialises the camera object for the tracks menu.
 */
void camera_init_tracks_menu(Gfx **dList, Mtx **mtxS) {
    Camera *cam;
    s16 angleY;
    s16 angleX;
    s16 angleZ;
    s16 sp24;
    f32 posX;
    f32 posY;
    f32 posZ;

    cam_set_layout(VIEWPORT_LAYOUT_1_PLAYER);
    set_active_camera(0);
    cam = cam_get_active_camera();
    angleY = cam->trans.rotation.y_rotation;
    angleX = cam->trans.rotation.x_rotation;
    angleZ = cam->trans.rotation.z_rotation;
    posX = cam->trans.x_position;
    posY = cam->trans.y_position;
    posZ = cam->trans.z_position;
    sp24 = cam->pitch;
    cam->trans.rotation.z_rotation = 0;
    cam->trans.rotation.x_rotation = 0;
    cam->trans.rotation.y_rotation = -0x8000;
    cam->pitch = 0;
    cam->trans.x_position = 0.0f;
    cam->trans.y_position = 0.0f;
    cam->trans.z_position = 0.0f;
    update_envmap_position(0.0f, 0.0f, -1.0f);
    viewport_main(dList, mtxS);
    cam->pitch = sp24;
    cam->trans.rotation.y_rotation = angleY;
    cam->trans.rotation.x_rotation = angleX;
    cam->trans.rotation.z_rotation = angleZ;
    cam->trans.x_position = posX;
    cam->trans.y_position = posY;
    cam->trans.z_position = posZ;
}

/**
 * Compare the coordinates passed through to the active camera and return the distance between them.
 */
f32 get_distance_to_active_camera(f32 xPos, f32 yPos, f32 zPos) {
    s32 index;
    f32 dx, dz, dy;
    Camera *camera;

    index = gActiveCameraID;

    if (gCutsceneCameraActive) {
        index += 4;
    }

    camera = camera_obstruction_camera_for_slot(index);
    dz = zPos - camera->trans.z_position;
    dx = xPos - camera->trans.x_position;
    dy = yPos - camera->trans.y_position;
    return sqrtf((dz * dz) + ((dx * dx) + (dy * dy)));
}

/**
 * Sets the position and angle of the active camera.
 * Also sets the other properties of the camera to a default.
 */
void camera_reset(s32 xPos, s32 yPos, s32 zPos, s32 angleZ, s32 angleX, s32 angleY) {
    gCameras[gActiveCameraID].trans.rotation.z_rotation = angleZ * (0x7FFF / 180);
    gCameras[gActiveCameraID].trans.x_position = xPos;
    gCameras[gActiveCameraID].trans.y_position = yPos;
    gCameras[gActiveCameraID].trans.z_position = zPos;
    gCameras[gActiveCameraID].trans.rotation.x_rotation = angleX * (0x7FFF / 180);
    gCameras[gActiveCameraID].pitch = 0;
    gCameras[gActiveCameraID].x_velocity = 0.0f;
    gCameras[gActiveCameraID].y_velocity = 0.0f;
    gCameras[gActiveCameraID].z_velocity = 0.0f;
    gCameras[gActiveCameraID].shakeMagnitude = 0.0f;
    gCameras[gActiveCameraID].boomLength = 160.0f;
    gCameras[gActiveCameraID].trans.rotation.y_rotation = angleY * (0x7FFF / 180);
    gCameras[gActiveCameraID].zoom = gCameraZoomLevels[gActiveCameraID];
}

/**
 * Write directly to the second set of object stack indeces.
 * The first 4 are reserved for the 4 player viewports, so the misc views, used in the title screen
 * and course previews instead use the next 4.
 */
void write_to_object_render_stack(s32 stackPos, f32 xPos, f32 yPos, f32 zPos, s16 arg4, s16 arg5, s16 arg6) {
    stackPos += 4;
    gCameras[stackPos].pitch = 0;
    gCameras[stackPos].trans.x_position = xPos;
    gCameras[stackPos].trans.y_position = yPos;
    gCameras[stackPos].trans.z_position = zPos;
    gCameras[stackPos].trans.rotation.y_rotation = arg4;
    gCameras[stackPos].trans.rotation.x_rotation = arg5;
    gCameras[stackPos].trans.rotation.z_rotation = arg6;
    gCameras[stackPos].cameraSegmentID = get_level_segment_index_from_position(xPos, yPos, zPos);
    gCutsceneCameraActive = TRUE;
#ifdef NATIVE_PORT
    {
        MdkrCameraIntent intent;
        const f32 horizontal = coss_f(arg5);

        memset(&intent, 0, sizeof(intent));
        intent.camera_id = stackPos;
        intent.authored_mode = gCameras[stackPos].mode;
        intent.family = MDKR_CAMERA_INTENT_FAMILY_SCRIPTED_CUTSCENE;
        intent.desired_eye.x = xPos;
        intent.desired_eye.y = yPos;
        intent.desired_eye.z = zPos;
        intent.pivot = intent.desired_eye;
        /* Scripted transforms have no universal subject. Preserve the exact
         * final orientation as a forward ray instead of inventing a target. */
        intent.forward.x = sins_f(arg4) * horizontal;
        intent.forward.y = -sins_f(arg5);
        intent.forward.z = coss_f(arg4) * horizontal;
        intent.forward_valid = TRUE;
        camera_obstruction_intent_capture(&intent);
    }
#endif
}

/**
 * Check if the misc camera view is active.
 * Official name: camIsUserView
 */
s8 check_if_showing_cutscene_camera(void) {
    return gCutsceneCameraActive;
}

/**
 * Disable the cutscene camera, returning it to the conventional mode.
 */
void disable_cutscene_camera(void) {
    gCutsceneCameraActive = FALSE;
}

/**
 * Sets the current layout and returns the number of active cameras for that layout.
 * The layoutID argument must be from the ViewportCount enumeration.
 */
s32 cam_set_layout(s32 layoutID) {
    if (layoutID >= VIEWPORT_LAYOUT_1_PLAYER && layoutID <= VIEWPORT_LAYOUT_4_PLAYERS) {
        gViewportLayout = layoutID;
    } else {
        gViewportLayout = VIEWPORT_LAYOUT_1_PLAYER;
    }
    switch (gViewportLayout) {
        case VIEWPORT_LAYOUT_1_PLAYER:
            gNumCameras = 1;
            break;
        case VIEWPORT_LAYOUT_2_PLAYERS:
            gNumCameras = 2;
            break;
        case VIEWPORT_LAYOUT_3_PLAYERS:
            gNumCameras = 3;
            break;
        case VIEWPORT_LAYOUT_4_PLAYERS:
            gNumCameras = 4;
            break;
    }
    if (gActiveCameraID >= gNumCameras) {
        stubbed_printf("Camera Error: Illegal mode!\n");
        gActiveCameraID = 0;
    }
    return gNumCameras;
}

/**
 * Sets the active viewport ID to the passed number.
 * If it's not within 1-4, then it's set to 0.
 * Official name: camSetView
 */
void set_active_camera(s32 num) {
    if (num >= 0 && num < 4) {
        gActiveCameraID = num;
    } else {
        stubbed_printf("Camera Error: Illegal player no!\n");
        gActiveCameraID = 0;
    }
}

/**
 * Takes the size of each view frame and writes them to the viewport stack, using values compatable with the RSP.
 * Only does this if extended backgrounds are enabled.
 */
void copy_viewports_to_stack(void) {
    s32 width;
    s32 height;
    s32 port;
    s32 yPos;
    s32 xPos;
    s32 i;

    gViewportWithBG = 1 - gViewportWithBG;
    for (i = 0; i < 4; i++) {
        if (gScreenViewports[i].flags & VIEWPORT_UNK_04) {
            gScreenViewports[i].flags &= ~VIEWPORT_EXTRA_BG;
        } else if (gScreenViewports[i].flags & VIEWPORT_UNK_02) {
            gScreenViewports[i].flags |= VIEWPORT_EXTRA_BG;
        }
        gScreenViewports[i].flags &= ~(VIEWPORT_UNK_02 | VIEWPORT_UNK_04);

        if (gScreenViewports[i].flags & VIEWPORT_EXTRA_BG) {
            if (!(gScreenViewports[i].flags & VIEWPORT_X_CUSTOM)) {
                xPos = (((gScreenViewports[i].x2 - gScreenViewports[i].x1) + 1) << 1) + (gScreenViewports[i].x1 * 4);
            } else {
                xPos = gScreenViewports[i].posX;
                xPos *= 4;
            }
            if (!(gScreenViewports[i].flags & VIEWPORT_Y_CUSTOM)) {
                yPos = (((gScreenViewports[i].y2 - gScreenViewports[i].y1 + 1)) << 1) + (gScreenViewports[i].y1 * 4);
            } else {
                yPos = gScreenViewports[i].posY;
                yPos *= 4;
            }
            if (!(gScreenViewports[i].flags & VIEWPORT_WIDTH_CUSTOM)) {
                width = gScreenViewports[i].x2 - gScreenViewports[i].x1;
                width += 1;
                width *= 2;
            } else {
                width = gScreenViewports[i].width;
                width *= 2;
            }
            if (!(gScreenViewports[i].flags & VIEWPORT_HEIGHT_CUSTOM)) {
                height = (gScreenViewports[i].y2 - gScreenViewports[i].y1) + 1;
                height *= 2;
            } else {
                height = gScreenViewports[i].height;
                height *= 2;
            }
            port = i + (gViewportWithBG * 5);
            port += 10;
            if (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) {
                if (0) {} // Fakematch
                width = -width;
            }
            gViewportStack[port].vp.vtrans[0] = xPos;
            gViewportStack[port].vp.vtrans[1] = yPos;
            gViewportStack[port].vp.vscale[0] = width;
            gViewportStack[port].vp.vscale[1] = height;
        }
    }
}

void camEnableUserView(s32 viewPortIndex, s32 arg1) {
    if (arg1) {
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_EXTRA_BG;
    } else {
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_UNK_02;
    }
    gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_UNK_04;
}

void camDisableUserView(s32 viewPortIndex, s32 arg1) {
    if (arg1) {
        gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_EXTRA_BG;
    } else {
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_UNK_04;
    }
    gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_UNK_02;
}

/**
 * Return's the current viewport's flag status for extended backgrounds.
 * Required to draw some extra things used in menus.
 */
s32 check_viewport_background_flag(s32 viewPortIndex) {
    return gScreenViewports[viewPortIndex].flags & VIEWPORT_EXTRA_BG;
}

#ifdef NATIVE_PORT
/**
 * Choose how a user viewport's live world maps onto a widescreen drawable.
 *
 * VIEWPORT_EXTRA_BG is an original engine rendering mechanism, not a layout
 * contract: logos and credits use it for a vertically clipped cinematic while
 * Track Select and the later post-race screens use it for a wooden aperture.
 * Keep that distinction explicit rather than inferring it from the legacy flag.
 */
void viewport_world_region_set(s32 viewPortIndex, ViewportWorldRegion region) {
    if (viewPortIndex < 0 ||
        viewPortIndex >= (s32) ARRAY_COUNT(sViewportWorldRegions)) {
        return;
    }
    sViewportWorldRegions[viewPortIndex] =
        region == VIEWPORT_WORLD_REGION_SAFE_APERTURE
            ? VIEWPORT_WORLD_REGION_SAFE_APERTURE
            : VIEWPORT_WORLD_REGION_PRESENTATION;
}

/**
 * Return every viewport to the unframed presentation region.
 *
 * The safe aperture belongs to the scene that draws a frame around its live
 * view, not to the viewport.  Scene entry (cam_init, menu_init) resets the
 * region; the screens that own an aperture -- Track Select and the later
 * post-race pages -- restate it on every frame that draws the framed view.
 */
void viewport_world_regions_reset(void) {
    s32 i;

    for (i = 0; i < (s32) ARRAY_COUNT(sViewportWorldRegions); i++) {
        sViewportWorldRegions[i] = VIEWPORT_WORLD_REGION_PRESENTATION;
    }
}

s32 viewport_world_region_uses_safe_aperture(s32 viewPortIndex) {
    if (viewPortIndex < 0 ||
        viewPortIndex >= (s32) ARRAY_COUNT(sViewportWorldRegions)) {
        return FALSE;
    }
    return sViewportWorldRegions[viewPortIndex] ==
           VIEWPORT_WORLD_REGION_SAFE_APERTURE;
}
#endif

/**
 * Sets the intended viewport to the size passed through by arguments.
 * Official Name: camSetUserView
 */
void viewport_menu_set(s32 viewPortIndex, s32 x1, s32 y1, s32 x2, s32 y2) {
    s32 widthAndHeight, width, height;
    s32 temp;

    widthAndHeight = fb_size();
    height = GET_VIDEO_HEIGHT(widthAndHeight) & 0xFFFF;
    width = GET_VIDEO_WIDTH(widthAndHeight);

    if (x2 < x1) {
        temp = x1;
        x1 = x2;
        x2 = temp;
    }
    if (y2 < y1) {
        temp = y1;
        y1 = y2;
        y2 = temp;
    }

    if (x1 >= width || x2 < 0 || y1 >= height || y2 < 0) {
        gScreenViewports[viewPortIndex].scissorX1 = 0;
        gScreenViewports[viewPortIndex].scissorY1 = 0;
        gScreenViewports[viewPortIndex].scissorX2 = 0;
        gScreenViewports[viewPortIndex].scissorY2 = 0;
    } else {
        if (x1 < 0) {
            gScreenViewports[viewPortIndex].scissorX1 = 0;
        } else {
            gScreenViewports[viewPortIndex].scissorX1 = x1;
        }
        if (y1 < 0) {
            gScreenViewports[viewPortIndex].scissorY1 = 0;
        } else {
            gScreenViewports[viewPortIndex].scissorY1 = y1;
        }
        if (x2 >= width) {
            gScreenViewports[viewPortIndex].scissorX2 = width - 1;
        } else {
            gScreenViewports[viewPortIndex].scissorX2 = x2;
        }
        if (y2 >= height) {
            gScreenViewports[viewPortIndex].scissorY2 = height - 1;
        } else {
            gScreenViewports[viewPortIndex].scissorY2 = y2;
        }
    }
    gScreenViewports[viewPortIndex].y1 = y1;
    gScreenViewports[viewPortIndex].x1 = x1;
    gScreenViewports[viewPortIndex].x2 = x2;
    gScreenViewports[viewPortIndex].y2 = y2;
}

/**
 * Set the selected viewport's coordinate offsets and view size.
 * If you pass VIEWPORT_AUTO through, then the property will be automatically set when the game creates the viewports.
 */
void set_viewport_properties(s32 viewPortIndex, s32 posX, s32 posY, s32 width, s32 height) {
    if (posX != VIEWPORT_AUTO) {
        gScreenViewports[viewPortIndex].posX = posX;
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_X_CUSTOM;
    } else {
        gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_X_CUSTOM;
    }
    if (posY != VIEWPORT_AUTO) {
        //!@bug Viewport Y writes to the X value. Luckily, all cases this function is called use VIEWPORT_AUTO,
        // so this bug doesn't happen in practice.
        gScreenViewports[viewPortIndex].posX = posY;
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_Y_CUSTOM;
    } else {
        gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_Y_CUSTOM;
    }
    if (width != VIEWPORT_AUTO) {
        gScreenViewports[viewPortIndex].width = width;
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_WIDTH_CUSTOM;
    } else {
        gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_WIDTH_CUSTOM;
    }
    if (height != VIEWPORT_AUTO) {
        gScreenViewports[viewPortIndex].height = height;
        gScreenViewports[viewPortIndex].flags |= VIEWPORT_HEIGHT_CUSTOM;
    } else {
        gScreenViewports[viewPortIndex].flags &= ~VIEWPORT_HEIGHT_CUSTOM;
    }
}

/**
 * Sets the passed values to be equal to the selected viewports scissor size, before drawing the background elements.
 * Usually, this is the same size as the viewport's size.
 * Official name: camGetVisibleUserView
 */
s32 copy_viewport_background_size_to_coords(s32 viewPortIndex, s32 *x1, s32 *y1, s32 *x2, s32 *y2) {
    // gDPFillRectangle values
    *x1 = gScreenViewports[viewPortIndex].scissorX1;
    *x2 = gScreenViewports[viewPortIndex].scissorX2;
    *y1 = gScreenViewports[viewPortIndex].scissorY1;
    *y2 = gScreenViewports[viewPortIndex].scissorY2;
    if ((*x1 | *x2 | *y1 | *y2) == 0) {
        return 0;
    }
    return 1;
}

/**
 * Sets the passed values to the coordinate size of the passed viewport.
 * Official name: camGetUserView
 */
void copy_viewport_frame_size_to_coords(s32 viewPortIndex, s32 *x1, s32 *y1, s32 *x2, s32 *y2) {
    *x1 = gScreenViewports[viewPortIndex].x1;
    *y1 = gScreenViewports[viewPortIndex].y1;
    *x2 = gScreenViewports[viewPortIndex].x2;
    *y2 = gScreenViewports[viewPortIndex].y2;
}

/**
 * Unused function that sets the passed values to the framebuffer's size in coordinates.
 * Official name: camGetWindowLimits
 */
UNUSED void copy_framebuffer_size_to_coords(s32 *x1, s32 *y1, s32 *x2, s32 *y2) {
    u32 widthAndHeight = fb_size();
    *x1 = 0;
    *y1 = 0;
    *x2 = GET_VIDEO_WIDTH(widthAndHeight);
    *y2 = GET_VIDEO_HEIGHT(widthAndHeight);
}

void viewport_main(Gfx **dlist, Mtx **mats) {
    u32 y;
    u32 x;
    u32 tempX;
    u32 sp58_height;
    u32 sp54_width;
    u32 posY;
    u32 posX;
    u32 tempY;
    u32 videoHeight;
    u32 videoWidth;
    u32 widthAndHeight;
    s32 viewports;
    s32 originalCameraID;
    s32 savedCameraID;
    s32 tempCameraID;

    originalCameraID = gActiveCameraID;
    savedCameraID = gActiveCameraID;

    if (is_player_two_in_control() && gViewportLayout == VIEWPORT_LAYOUT_1_PLAYER) {
        gActiveCameraID = 1;
        savedCameraID = 0;
    }
    widthAndHeight = fb_size();
    videoHeight = GET_VIDEO_HEIGHT(widthAndHeight);
    videoWidth = GET_VIDEO_WIDTH(widthAndHeight);
    if (gScreenViewports[savedCameraID].flags & VIEWPORT_EXTRA_BG) {
#ifdef NATIVE_PORT
        /* The world region is the viewport's own persistent property, so the
         * latched record already carries the safe-aperture lens; render only
         * has to tell the renderer which region this image draws into. */
        s32 cameraID = gActiveCameraID + (gCutsceneCameraActive ? 4 : 0);

        cam_rebuild_native_projection(savedCameraID, cameraID);
        gDkrSetWorldRegion((*dlist)++,
                           viewport_world_region_uses_safe_aperture(savedCameraID));
#endif
        tempCameraID = gActiveCameraID;
        gActiveCameraID = savedCameraID;
        gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, gScreenViewports[gActiveCameraID].scissorX1,
                      gScreenViewports[gActiveCameraID].scissorY1, gScreenViewports[gActiveCameraID].scissorX2,
                      gScreenViewports[gActiveCameraID].scissorY2);
        viewport_rsp_set(dlist, 0, 0, 0, 0);
        gActiveCameraID = tempCameraID;
        if (mats != NULL) {
            func_80067D3C(dlist, mats);
        }
        gActiveCameraID = originalCameraID;
        return;
    }

    viewports = gViewportLayout;
    if (viewports == VIEWPORT_LAYOUT_3_PLAYERS) {
        viewports = VIEWPORT_LAYOUT_4_PLAYERS;
    }
    x = videoWidth >> 1;
    sp54_width = x;
    y = videoHeight >> 1;
    sp58_height = y;
    posX = sp54_width;
    posY = sp58_height;

    if (osTvType == 0) {
        sp58_height = 145;
    }

    switch (viewports) {
        case VIEWPORT_LAYOUT_1_PLAYER:
            posX = sp54_width;
            posY = sp58_height;
            if (osTvType == OS_TV_TYPE_PAL) {
                posY -= 18;
            }
            gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, 0, videoWidth, videoHeight);
            break;
        case VIEWPORT_LAYOUT_2_PLAYERS:
            // 2 players = split screen horizontally
            // first player has top half
            posX = sp54_width;
            posY = gActiveCameraID;
            if (posY == 0) {
                posY = videoHeight >> 2;
                if (osTvType == OS_TV_TYPE_PAL) {
                    posY -= 12;
                }
                gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, 0, videoWidth, y - (videoHeight >> 7));
            } else {
                // second player has bottom half
                posY = y;
                posY += videoHeight >> 2;
                tempY = y;
                tempY += (videoHeight >> 7);
                gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, tempY, videoWidth, videoHeight - (videoHeight >> 7));
            }

            break;
        // this is probably never reached because of an if above that sets the viewport to 4 players if its currently 3
        // players
        case VIEWPORT_LAYOUT_3_PLAYERS:
            posY = sp58_height;
            // 3 player splits screen in 4 parts, first player = top left, second = top right, third = bottom left and
            // bottom right has map of race track
            if (gActiveCameraID == 0) {
                posX = videoWidth >> 2;
                gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, 0, x - (videoWidth >> 8), videoHeight);
            } else {
                posX = x;
                posX += (videoWidth >> 2);
                tempX = x;
                tempX += (videoWidth >> 8);
                gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, tempX, 0, videoWidth - (videoWidth >> 8), videoHeight);
            }
            break;
        case VIEWPORT_LAYOUT_4_PLAYERS:
            sp58_height >>= 1;
            sp54_width >>= 1;
            tempY = 0;
            tempX = 0;
            switch (gActiveCameraID) {
                case 0:
                    // Using tempX and tempY here is not smart since IDO can't optimize out the zero now.
                    // Why here of all places did they do this instead of just setting zero like everywhere else?
                    gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, tempX, tempY, x - (videoWidth >> 8),
                                  y - (videoHeight >> 7));
                    break;
                case 1:
                    tempY = x;
                    posX = x;
                    posX += (videoWidth >> 8);
                    gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, posX, 0, (x + x) - (videoWidth >> 8),
                                  y - (videoHeight >> 7));
                    break;
                case 2:
                    tempX = y;
                    posY = y;
                    posY += (videoHeight >> 7);
                    posX = x;
                    posX -= (videoWidth >> 8);
                    gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, posY, posX, (y + y) - (videoHeight >> 7));
                    break;
                case 3:
                    tempY = x;
                    tempX = y;
                    posX = x;
                    posX += (videoWidth >> 8);
                    gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, posX, y + (videoHeight >> 7),
                                  (x + x) - (videoWidth >> 8), (y + y) - (videoHeight >> 7));
                    break;
                default:
                    gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, 0,
                                  x - (videoWidth >> 8),
                                  y - (videoHeight >> 7));
                    break;
            }
            posY = tempX + sp58_height;
            posX = tempY + sp54_width;
            if (osTvType == OS_TV_TYPE_PAL) {
                if (gActiveCameraID <= 1) {
                    posY -= 20;
                } else {
                    posY -= 6;
                }
            }
            break;
        default:
            gDPSetScissor((*dlist)++, SCISSOR_INTERLACE, 0, 0,
                          videoWidth, videoHeight);
            break;
    }

    if (osTvType == OS_TV_TYPE_PAL) {
        posX -= 4;
    }
#ifdef NATIVE_PORT
    {
        s32 cameraID = gActiveCameraID + (gCutsceneCameraActive ? 4 : 0);
        cam_rebuild_native_projection(savedCameraID, cameraID);
    }
    gDkrSetWorldRegion((*dlist)++, FALSE);
#endif
    viewport_rsp_set(dlist, sp54_width, sp58_height, posX, posY);
    if (mats != NULL) {
        func_80067D3C(dlist, mats);
    }
    gActiveCameraID = originalCameraID;
}

/**
 * Takes the size of the screen as depicted by the active menu viewport, then sets the RDP scissor to match it.
 * Official Name: camSetScissor
 */
void viewport_scissor(Gfx **dList) {
    s32 size;
    s32 lrx;
    s32 lry;
    s32 ulx;
    s32 uly;
    s32 numViewports;
    s32 temp;
    s32 temp2;
    s32 temp3;
    s32 temp4;
    s32 width;
    s32 height;

    size = fb_size();
    height = (u16) GET_VIDEO_HEIGHT(size);
    width = (u16) size;
    numViewports = gViewportLayout;

    if (numViewports != VIEWPORT_LAYOUT_1_PLAYER) {
        if (numViewports == VIEWPORT_LAYOUT_3_PLAYERS) {
            numViewports = VIEWPORT_LAYOUT_4_PLAYERS;
        }
        lrx = ulx = 0;
        lry = uly = 0;
        // clang-format off
        lrx += width;\
        lry += height;
        // clang-format on
        temp = lry >> 7;
        temp2 = lrx >> 8;
        temp4 = lrx >> 1;
        temp3 = lry >> 1;
        switch (numViewports) {
            case 1:
                switch (gActiveCameraID) {
                    case 0:
                        lry = temp3 - temp;
                        break;
                    default:
                        uly = temp3 + temp;
                        lry -= temp;
                        break;
                }
                break;
            case 2:
                switch (gActiveCameraID) {
                    case 0:
                        lrx = temp4 - temp2;
                        break;
                    default:
                        ulx = temp4 + temp2;
                        lrx -= temp2;
                        break;
                }
                break;
            case 3:
                // clang-format off
                switch (gActiveCameraID) {
                    case 0:
                        lrx = temp4 - temp2;\
                        lry = temp3 - temp;
                        break;
                    case 1:
                        ulx = temp4 + temp2;
                        lrx -= temp2;
                        lry = temp3 - temp;
                        break;
                    case 2:
                        uly = temp3 + temp;
                        lrx = temp4 - temp2;\
                        lry -= temp;
                        break;
                    case 3:
                        ulx = temp4 + temp2;\
                        uly = temp3 + temp;
                        lrx -= temp2;\
                        lry -= temp;
                        break;
                }
                // clang-format on
                break;
        }
        gDPSetScissor((*dList)++, 0, ulx, uly, lrx, lry);
    } else {
        gDPSetScissor((*dList)++, 0, 0, 0, width, height);
    }
}

#ifdef NATIVE_PORT
/* Fidelity Phase 2b -- the pure half of func_80067D3C: everything that derives the
 * view / inverse-view basis from the active camera, with no display-list emission,
 * so the fixed step can reconstruct the basis render will use later in the same
 * frame (obj_sort_tick needs gViewMatrixF, the visibility prepass needs
 * gInverseViewMatrixF for the cull planes).
 *
 * Split rather than shared with func_80067D3C below, following the waves_tick
 * pattern: the #ifndef NATIVE_PORT arm keeps the original function body verbatim,
 * so the matching build is untouched. */
static void mdkr_snapshot_authored_camera_record(s32 viewport,
                                                 s32 cameraId) {
    const Camera *camera;
    const ScreenViewport *screen;
    PresentationCameraEntry sample;
    MdkrCameraProjection projection;

    if (viewport < 0 || viewport >= PRESENTATION_SNAPSHOT_MAX_VIEWPORTS ||
        cameraId < 0 || cameraId >= PRESENTATION_SNAPSHOT_MAX_CAMERAS) {
        return;
    }
    /* The pose and the matrix in one record must describe the same lens.
     * cam_build_view_basis() authored gViewProjMatrixF from the resolved
     * camera, so recording the unresolved gCameras entry here would make an
     * interpolated replay diverge from the image it interpolates between. */
    camera = camera_obstruction_snapshot_camera_for_slot(cameraId);
    if (camera == NULL) {
        return;
    }
    screen = &gScreenViewports[viewport];
    memset(&sample, 0, sizeof(sample));
    sample.camera_id = cameraId;
    sample.viewport_index = viewport;
    sample.position[0] = camera->trans.x_position;
    sample.position[1] = camera->trans.y_position;
    sample.position[2] = camera->trans.z_position;
    sample.rotation_x = camera->trans.rotation.x_rotation;
    sample.rotation_y = camera->trans.rotation.y_rotation;
    sample.rotation_z = camera->trans.rotation.z_rotation;
    sample.pitch = camera->pitch;
    sample.shake_magnitude = camera->shakeMagnitude;
    sample.apply_shake = gNoCamShake;
    sample.discontinuity =
        (uint8_t) camera_obstruction_snapshot_discontinuous(cameraId);
    sample.fov = gCurCamFOV;
    if (cam_get_latched_effective_projection_for_viewport(viewport, &projection) &&
        projection.camera_id == cameraId) {
        sample.vertical_fov = projection.vertical_fov;
        sample.aspect = projection.aspect;
        sample.near_plane = projection.near_plane;
        sample.far_plane = projection.far_plane;
    } else {
        /* Snapshot publication is observational; retain the established
         * projection fallback if no validated authored record exists. */
        sample.vertical_fov = gEffectiveCamVFOV;
        sample.aspect = gEffectiveCamAspect;
        sample.near_plane = CAMERA_NEAR;
        sample.far_plane = CAMERA_FAR;
    }
    sample.world_region =
        viewport_world_region_uses_safe_aperture(viewport) ? 1u : 0u;
    sample.viewport[0] = (f32)screen->posX;
    sample.viewport[1] = (f32)screen->posY;
    sample.viewport[2] = (f32)screen->width;
    sample.viewport[3] = (f32)screen->height;
    memcpy(sample.authored_view_projection, gViewProjMatrixF,
           sizeof(sample.authored_view_projection));
    (void)presentation_snapshot_authored_camera_record(&sample);
}

void cam_build_view_basis(void) {
    s32 originalCamID;
    Camera *activeCamera;

    originalCamID = gActiveCameraID;
    if (gCutsceneCameraActive) {
        gActiveCameraID += 4;
    }
    activeCamera = camera_obstruction_camera_for_slot(gActiveCameraID);

    gCameraTransform.rotation.y_rotation = 0x8000 + activeCamera->trans.rotation.y_rotation;
    gCameraTransform.rotation.x_rotation =
        activeCamera->trans.rotation.x_rotation + activeCamera->pitch;
    gCameraTransform.rotation.z_rotation = activeCamera->trans.rotation.z_rotation;

    gCameraTransform.x_position = -activeCamera->trans.x_position;
    gCameraTransform.y_position = -activeCamera->trans.y_position;
    if (gNoCamShake) {
        gCameraTransform.y_position -= activeCamera->shakeMagnitude;
    }
    gCameraTransform.z_position = -activeCamera->trans.z_position;

    mtxf_from_inverse_transform(&gViewMatrixF, &gCameraTransform);
    mtxf_mul(&gViewMatrixF, &gPerspectiveMatrixF, &gViewProjMatrixF);
    /* From here until something else overwrites gViewProjMatrixF, every
     * registered matrix belongs to this viewport's gameplay camera and is a
     * legal target for an interpolated view-projection (Wave B). */
    sShadowRegisterViewport = originalCamID;
    sShadowRegisterGameplayVp = 1;

    gCameraTransform.rotation.y_rotation = -0x8000 - activeCamera->trans.rotation.y_rotation;
    gCameraTransform.rotation.x_rotation =
        -(activeCamera->trans.rotation.x_rotation + activeCamera->pitch);
    gCameraTransform.rotation.z_rotation = -activeCamera->trans.rotation.z_rotation;
    gCameraTransform.scale = 1.0f;
    gCameraTransform.x_position = activeCamera->trans.x_position;
    gCameraTransform.y_position = activeCamera->trans.y_position;
    if (gNoCamShake) {
        gCameraTransform.y_position += activeCamera->shakeMagnitude;
    }
    gCameraTransform.z_position = activeCamera->trans.z_position;

    mtxf_from_transform(&gInverseViewMatrixF, &gCameraTransform);
    mtxf_to_mtx(&gInverseViewMatrixF, &gInverseViewMatrix);

    gActiveCameraID = originalCamID;
}
#endif

// Official Name: camGetPlayerProjMtx / camSetProjMtx - ??
void func_80067D3C(Gfx **dList, UNUSED Mtx **mtx) {
#ifdef NATIVE_PORT
    const s32 viewport = gActiveCameraID;
    const s32 cameraId =
        gActiveCameraID + (gCutsceneCameraActive ? 4 : 0);

    gSPPerspNormalize((*dList)++, perspNorm);
    cam_build_view_basis();
    /* Unlike logic-only cam_build_view_basis() calls, this path emits the
     * projection normalization into the task and supplies the VP used by its
     * registered gameplay matrices. Latch only this authored recipe. */
    mdkr_snapshot_authored_camera_record(viewport, cameraId);
#else
    s32 originalCamID;

    gSPPerspNormalize((*dList)++, perspNorm);

    originalCamID = gActiveCameraID;
    if (gCutsceneCameraActive) {
        gActiveCameraID += 4;
    }

    gCameraTransform.rotation.y_rotation = 0x8000 + gCameras[gActiveCameraID].trans.rotation.y_rotation;
    gCameraTransform.rotation.x_rotation =
        gCameras[gActiveCameraID].trans.rotation.x_rotation + gCameras[gActiveCameraID].pitch;
    gCameraTransform.rotation.z_rotation = gCameras[gActiveCameraID].trans.rotation.z_rotation;

    gCameraTransform.x_position = -gCameras[gActiveCameraID].trans.x_position;
    gCameraTransform.y_position = -gCameras[gActiveCameraID].trans.y_position;
    if (gNoCamShake) {
        gCameraTransform.y_position -= gCameras[gActiveCameraID].shakeMagnitude;
    }
    gCameraTransform.z_position = -gCameras[gActiveCameraID].trans.z_position;

    mtxf_from_inverse_transform(&gViewMatrixF, &gCameraTransform);
    mtxf_mul(&gViewMatrixF, &gPerspectiveMatrixF, &gViewProjMatrixF);

    gCameraTransform.rotation.y_rotation = -0x8000 - gCameras[gActiveCameraID].trans.rotation.y_rotation;
    gCameraTransform.rotation.x_rotation =
        -(gCameras[gActiveCameraID].trans.rotation.x_rotation + gCameras[gActiveCameraID].pitch);
    gCameraTransform.rotation.z_rotation = -gCameras[gActiveCameraID].trans.rotation.z_rotation;
    gCameraTransform.scale = 1.0f;
    gCameraTransform.x_position = gCameras[gActiveCameraID].trans.x_position;
    gCameraTransform.y_position = gCameras[gActiveCameraID].trans.y_position;
    if (gNoCamShake) {
        gCameraTransform.y_position += gCameras[gActiveCameraID].shakeMagnitude;
    }
    gCameraTransform.z_position = gCameras[gActiveCameraID].trans.z_position;

    mtxf_from_transform(&gInverseViewMatrixF, &gCameraTransform);
    mtxf_to_mtx(&gInverseViewMatrixF, &gInverseViewMatrix);

    gActiveCameraID = originalCamID;
#endif
}

/**
 * Sets the Y value of the Y axis in the matrix to the passed value.
 * This is used to vertically scale ortho geometry to look identical across NTSC and PAL systems.
 * Official Name: camOrthoYAspect
 */
void set_ortho_matrix_height(f32 value) {
    gOrthoMatrixF[1][1] = value;
}

/**
 * Sets the current matrix to represent an orthogonal view.
 * Used for drawing triangles on screen as HUD.
 * Official Name: camStandardOrtho
 */
void mtx_ortho(Gfx **dList, Mtx **mtx) {
    u32 widthAndHeight;
    s32 width, height;
    s32 i, j;

    widthAndHeight = fb_size();
    height = GET_VIDEO_HEIGHT(widthAndHeight);
    width = GET_VIDEO_WIDTH(widthAndHeight);
    mtxf_to_mtx(&gOrthoMatrixF, *mtx);
    gModelMatrix[0] = *mtx;
    gViewportStack[gActiveCameraID + 5].vp.vscale[0] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vscale[1] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vtrans[0] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vtrans[1] = height * 2;
    gSPViewport((*dList)++, OS_K0_TO_PHYSICAL(&gViewportStack[gActiveCameraID + 5]));
#ifdef NATIVE_PORT
    gSPMatrixDKRTagged((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_0,
                       sNativeOrthoDrawSpace);
    sNativeOrthoDrawSpace = G_MTX_DKR_SPACE_SAFE_2D;
#else
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_0);
#endif
    gModelMatrixStackPos = 0;
    gMtxOriginID = G_MTX_DKR_INDEX_0;

    for (i = 0; i < 4; i++) {
        // clang-format off
        // Required to be one line, but the "\" fixes that.
        for (j = 0; j < 4; j++) { \
            gViewProjMatrixF[i][j] = gOrthoMatrixF[i][j];
        }
        // clang-format on
    }
#ifdef NATIVE_PORT
    /* gViewProjMatrixF is now the ortho matrix; matrices registered against it
     * are HUD/menu/transition space, never the gameplay camera (Wave B). */
    sShadowRegisterGameplayVp = 0;
#endif
}

#ifdef NATIVE_PORT
/**
 * Transition masks are authored in 4:3 but must cover every host pixel.  The
 * renderer maps this tagged ortho matrix with an aspect-preserving "cover"
 * transform, so circles stay circular and bars cannot expose side gutters.
 */
void mtx_ortho_fullscreen(Gfx **dList, Mtx **mtx) {
    sNativeOrthoDrawSpace = G_MTX_DKR_SPACE_FULLBLEED;
    mtx_ortho(dList, mtx);
}

/**
 * Draw one 320-wide decorative background tile into presentation space while
 * retaining the safe area's uniform pixel scale and vertical registration.
 * Adjacent tiles therefore extend fixed menu art without stretching it or
 * introducing a cover-crop seam at the safe-area boundary.
 */
void mtx_ortho_wide_background(Gfx **dList, Mtx **mtx,
                               f32 authoredTileOffset) {
    MdkrDisplayLayout layout = mdkr_display_layout();
    MtxF wideOrtho;
    f32 horizontalScale = 1.0f;
    u32 widthAndHeight;
    s32 width;
    s32 height;
    s32 i;
    s32 j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            wideOrtho[i][j] = gOrthoMatrixF[i][j];
        }
    }
    if (layout.presentation.width > 0.0f) {
        horizontalScale = layout.safe.width / layout.presentation.width;
    }
    wideOrtho[0][0] *= horizontalScale;
    wideOrtho[3][0] += authoredTileOffset * horizontalScale;

    widthAndHeight = fb_size();
    height = GET_VIDEO_HEIGHT(widthAndHeight);
    width = GET_VIDEO_WIDTH(widthAndHeight);
    mtxf_to_mtx(&wideOrtho, *mtx);
    gModelMatrix[0] = *mtx;
    gViewportStack[gActiveCameraID + 5].vp.vscale[0] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vscale[1] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vtrans[0] = width * 2;
    gViewportStack[gActiveCameraID + 5].vp.vtrans[1] = height * 2;
    gSPViewport((*dList)++, OS_K0_TO_PHYSICAL(
        &gViewportStack[gActiveCameraID + 5]));
    gSPMatrixDKRTagged((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++),
                       G_MTX_DKR_INDEX_0, G_MTX_DKR_SPACE_WIDE_BG);
    gModelMatrixStackPos = 0;
    gMtxOriginID = G_MTX_DKR_INDEX_0;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            gViewProjMatrixF[i][j] = wideOrtho[i][j];
        }
    }
    sShadowRegisterGameplayVp = 0;
}
#endif

/*
 * Build each viewport's view-projection from the INTERPOLATED camera inputs
 * (Phase 3 Wave B slice 2, spec §7 "camera: interpolate before deriving the
 * view matrix").
 *
 * This deliberately mirrors cam_build_view_basis() above, statement for
 * statement, rather than calling it: that function reads gCameras[] — live
 * authoritative state — and writes gViewMatrixF/gViewProjMatrixF/
 * gInverseViewMatrixF/gCameraTransform, all of which the tick's own render
 * depends on. A presentation frame may not touch any of them (spec §4.5). So
 * the inputs come from the presentation snapshot pair and every intermediate
 * is a local; the only output is the caller's array.
 *
 * The arithmetic is the same math_util path the authoritative build uses. The
 * alpha-zero replay observer compares this result byte-for-byte with the view-
 * projection captured when the retained task was authored, and carries the
 * alpha-one target forward to prove it becomes the next task's endpoint.
 */
static void mdkr_camera_pose_view_projection(
    const PresentationCameraPose *pose, f32 out[4][4]) {
    ObjectTransform transform;
    MtxF view;
    MtxF perspective;
    u16 norm;

    guPerspectiveF(perspective, &norm, pose->vertical_fov, pose->aspect,
                   pose->near_plane, pose->far_plane, CAMERA_SCALE);

    memset(&transform, 0, sizeof(transform));
    transform.scale = 1.0f;
    transform.rotation.y_rotation = 0x8000 + pose->rotation_y;
    transform.rotation.x_rotation = pose->rotation_x + pose->pitch;
    transform.rotation.z_rotation = pose->rotation_z;
    transform.x_position = -pose->position[0];
    transform.y_position = -pose->position[1];
    if (pose->apply_shake) {
        transform.y_position -= pose->shake_magnitude;
    }
    transform.z_position = -pose->position[2];

    mtxf_from_inverse_transform(&view, &transform);
    mtxf_mul(&view, &perspective, (MtxF *)out);
}

size_t mdkr_camera_interpolated_view_projections(
    u64 numerator, u64 denominator,
    GfxShadowReplayViewProjection *out, size_t capacity) {
    const PresentationSnapshot *current = presentation_snapshot_current();
    const PresentationSnapshot *previous = presentation_snapshot_previous();
    size_t produced = 0;
    size_t viewport;

    if (out == NULL || current == NULL || previous == NULL ||
        !current->valid || !previous->valid ||
        current->stage_generation != previous->stage_generation ||
        previous->authored_tick == UINT64_MAX ||
        current->authored_tick != previous->authored_tick + 1u) {
        return 0;
    }
    for (viewport = 0; viewport < capacity; viewport++) {
        PresentationCameraPose pose;
        PresentationCameraPose next;

        memset(&out[viewport], 0, sizeof(out[viewport]));
        out[viewport].camera_id = -1;
        out[viewport].authored_tick = previous->authored_tick;
        out[viewport].next_authored_tick = current->authored_tick;
        out[viewport].numerator = numerator;
        out[viewport].denominator = denominator;
        if (!presentation_snapshot_resolve_camera(
                (int) viewport, numerator, denominator, &pose)) {
            continue;
        }
        /* Nothing to substitute when the pair could not be interpolated (spawn,
         * discontinuity, no published previous): that viewport keeps the
         * display list's own camera, which is the authoritative one. */
        if (!pose.interpolated) {
            continue;
        }

        if (numerator == 0u) {
            memcpy(out[viewport].view_projection,
                   previous->cameras[viewport].authored_view_projection,
                   sizeof(out[viewport].view_projection));
        } else if (numerator >= denominator) {
            memcpy(out[viewport].view_projection,
                   current->cameras[viewport].authored_view_projection,
                   sizeof(out[viewport].view_projection));
        } else {
            mdkr_camera_pose_view_projection(
                &pose, out[viewport].view_projection);
        }
        out[viewport].valid = TRUE;
        out[viewport].camera_id = pose.camera_id;
        if (presentation_snapshot_resolve_camera(
                (int)viewport, denominator, denominator, &next) &&
            next.interpolated && next.camera_id == pose.camera_id) {
            memcpy(out[viewport].next_view_projection,
                   current->cameras[viewport].authored_view_projection,
                   sizeof(out[viewport].next_view_projection));
            out[viewport].next_valid = TRUE;
        }
        produced = viewport + 1;
    }
    return produced;
}

/**
 * Sets the current matrix to represent a perspective view.
 * Necessary for setting up any 3D scene.
 * Official Name: camStandardPersp?
 */
void mtx_perspective(Gfx **dList, Mtx **mtx) {
#ifdef NATIVE_PORT
    /* This overwrites gViewProjMatrixF with a FIXED z=-281 transform: from here
     * on, registered matrices are NOT the gameplay camera's (Wave B). */
    sShadowRegisterGameplayVp = 0;
#endif
    mtxf_from_inverse_transform(&gCurrentModelMatrixF, &D_800DD288);
    mtxf_mul(&gCurrentModelMatrixF, &gPerspectiveMatrixF, &gViewProjMatrixF);
    mtxf_from_inverse_transform(gModelMatrixF[0], &D_800DD2A0);
    mtxf_mul(gModelMatrixF[0], &gViewProjMatrixF, &gCurrentModelMatrixF);
    mtxf_to_mtx(&gCurrentModelMatrixF, *mtx);
#ifdef NATIVE_PORT
    mdkr_shadow_register_matrix(
        *mtx, gModelMatrixF[0], GFX_SHADOW_MOBILITY_STATIC,
        GFX_SHADOW_SITE_PERSPECTIVE, NULL);
    gSPMatrixDKRTagged((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_0,
                       G_MTX_DKR_SPACE_WORLD);
#else
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_0);
#endif
    gModelMatrixStackPos = 0;
    gMtxOriginID = G_MTX_DKR_INDEX_0;
}

/**
 * Sets the RSP viewport onscreen to the given properties.
 * Viewports have a centre position and a scale factor, rather than a standard four corners.
 * Official Name: camSetViewport
 */
void viewport_rsp_set(Gfx **dList, s32 halfWidth, s32 halfHeight, s32 centerX, s32 centerY) {
    s32 tempWidth = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) ? -halfWidth : halfWidth;
#ifdef ANTI_TAMPER
    // Antipiracy measure. Flips the screen upside down.
    if (gAntiPiracyViewport) {
        halfHeight = -halfHeight;
        tempWidth = -halfWidth;
    }
#endif
    if (!(gScreenViewports[gActiveCameraID].flags & VIEWPORT_EXTRA_BG)) {
        gViewportStack[gActiveCameraID].vp.vtrans[0] = centerX * 4;
        gViewportStack[gActiveCameraID].vp.vtrans[1] = centerY * 4;
        gViewportStack[gActiveCameraID].vp.vscale[0] = tempWidth * 4;
        gViewportStack[gActiveCameraID].vp.vscale[1] = halfHeight * 4;
        gSPViewport((*dList)++, OS_K0_TO_PHYSICAL(&gViewportStack[gActiveCameraID]));
    } else {
        gSPViewport((*dList)++, OS_K0_TO_PHYSICAL(&gViewportStack[gActiveCameraID + 10 + (gViewportWithBG * 5)]));
    }
}

/**
 * Resets the viewport back to default.
 * If in the track menu, or post-race, set it to a small screen view instead.
 * Official Name: camResetView?
 */
void viewport_reset(Gfx **dList) {
    u32 widthAndHeight, width, height;
    gActiveCameraID = 4;
    widthAndHeight = fb_size();
    height = GET_VIDEO_HEIGHT(widthAndHeight);
    width = GET_VIDEO_WIDTH(widthAndHeight);
    if (!(gScreenViewports[gActiveCameraID].flags & VIEWPORT_EXTRA_BG)) {
        gDPSetScissor((*dList)++, G_SC_NON_INTERLACE, 0, 0, width - 1, height - 1);
        viewport_rsp_set(dList, width >> 1, height >> 1, width >> 1, height >> 1);
    } else {
        viewport_scissor(dList);
        viewport_rsp_set(dList, 0, 0, 0, 0);
    }
    gActiveCameraID = 0;
}

UNUSED const char D_800E6F44[] = "cameraPushSprMtx: model stack overflow!!\n";

/**
 * Sets the matrix position to the world origin (0, 0, 0)
 * Used when the next thing rendered relies on there not being any matrix offset.
 * Official Name: camOffsetZero?
 */
void mtx_world_origin(Gfx **dList, Mtx **mtx) {
    mtxf_from_translation(gModelMatrixF[gModelMatrixStackPos], 0.0f, 0.0f, 0.0f);
    mtxf_mul(gModelMatrixF[gModelMatrixStackPos], &gViewProjMatrixF, &gCurrentModelMatrixF);
    mtxf_to_mtx(&gCurrentModelMatrixF, *mtx);
    gModelMatrix[gModelMatrixStackPos] = *mtx;
#ifdef NATIVE_PORT
    mdkr_shadow_register_matrix(
        *mtx, gModelMatrixF[gModelMatrixStackPos],
        GFX_SHADOW_MOBILITY_STATIC, GFX_SHADOW_SITE_WORLD_ORIGIN, NULL);
    gSPMatrixDKRTagged((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), gMtxOriginID,
                       G_MTX_DKR_SPACE_WORLD);
#else
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), gMtxOriginID);
#endif
}

/**
 * Sets the sprite animation mode.
 * This determines how the animation frame is interpreted: either as a frame index,
 * or as a normalized progress value from 0 to 255.
 */
void cam_set_sprite_anim_mode(s32 setting) {
    gSpriteAnimMode = setting;
}

/**
 * Renders a sprite in 3D space as a billboard.
 *
 * If the sprite represents a vehicle part (e.g., wheel, propeller, fan),
 * the function calculates the appropriate animation frame based on the object's
 * orientation relative to the camera, ensuring the sprite correctly matches
 * the viewing angle. It also orients the sprite properly in 3D space with tilt.
 *
 * For regular sprites, it sets up a standard billboard that always faces the camera.
 */
#ifdef NATIVE_PORT
static s32 render_sprite_billboard_transform_impl(Gfx **dList, Mtx **mtx, Vertex **vtx,
                                                  const ObjectTransform *transform, s16 animFrame,
                                                  Sprite *sprite, s32 flags) {
#define MDKR_BILLBOARD_TRANSFORM transform
#define MDKR_BILLBOARD_ANIM_FRAME animFrame
#else
s32 render_sprite_billboard(Gfx **dList, Mtx **mtx, Vertex **vtx, Object *obj, Sprite *sprite, s32 flags) {
#define MDKR_BILLBOARD_TRANSFORM (&obj->trans)
#define MDKR_BILLBOARD_ANIM_FRAME obj->animFrame
#endif
    f32 diffX;
    f32 diffY;
    Vertex *v;
    f32 lateralDist;
    f32 sineY;
    f32 cosY;
    f32 temp;
    f32 diffZ;
    s32 tanX;
    s32 tanY;
    s32 tiltAngle;
    s32 result;
    s32 frameID;
#ifdef NATIVE_PORT
    GfxPresentationMatrixOwner billboardOwner;
    bool billboardOwnerValid = false;
    MtxF vehiclePartLocal;
#endif

#ifdef NATIVE_PORT
    if (transform == NULL) {
#else
    if (obj == NULL) {
#endif
        stubbed_printf("\nCam do 2D sprite called with NULL pointer!");
    }

#ifdef NATIVE_PORT
    /* Every path below dereferences `sprite` unconditionally (numberOfFrames,
     * drawFlags, the frame table), and the N64 original has no NULL check
     * either -- so on real hardware a NULL sprite here is already a bug, not a
     * survivable condition. Fail LOUDLY rather than silently skipping the draw:
     * a silently-dropped sprite is exactly the class of defect that hid the
     * ASSET_MISC_20 boost-table decode bug (garbage sprite IDs -> a NULL from
     * tex_load_sprite(); see docs/OPEN_ITEMS.md and objects.c dkr_boost_table).
     * This used to be a capped `[GFX] NULL sprite -- skipped` warning while
     * that decode was broken; the decode is fixed, so a NULL here now means a
     * NEW asset/loader regression and must surface immediately. */
    if (sprite == NULL) {
        fprintf(stderr, "[FATAL] render_sprite_billboard: NULL sprite (flags=%d) — a sprite failed to load\n",
                (int) flags);
        fflush(stderr);
        abort();
    }
    if (transform != NULL) {
        MtxF identity;
        mtxf_from_translation(&identity, 0.0f, 0.0f, 0.0f);
        billboardOwnerValid = mdkr_presentation_owner_root(
            &billboardOwner, transform, 1.0f, 0.0f, &identity);
        if (billboardOwnerValid) {
            billboardOwner.matrix_class =
                GFX_PRESENTATION_MATRIX_BILLBOARD;
        }
    }
#endif

    result = TRUE;
    if (flags & RENDER_VEHICLE_PART) {
        // Vehicle parts like wheels, propellers, and fans are implemented as sprites,
        // each with 16 frames representing different orientations.
        // This requires calculating the correct frame based on the relative orientation
        // between the object and the camera, as well as properly orienting the sprite in 3D space.

        // Calculate camera position relative to the object's position
        diffX = gCameraRelPosStackX[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->x_position;
        diffY = gCameraRelPosStackY[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->y_position;
        diffZ = gCameraRelPosStackZ[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->z_position;

        // Rotate camera coordinates by the object's yaw rotation
        // to get the camera orientation relative to the vehicle,
        // e.g., to determine if we view the wheel from the side.
        sineY = sins_f(MDKR_BILLBOARD_TRANSFORM->rotation.y_rotation);
        cosY = coss_f(MDKR_BILLBOARD_TRANSFORM->rotation.y_rotation);

        temp = (diffX * cosY) + (diffZ * sineY);
        diffZ = (diffZ * cosY) - (diffX * sineY);
        diffX = temp;

        // Calculate the angle between the camera direction and the vertical plane of the vehicle
        tanY = arctan2_f(diffX, sqrtf((diffY * diffY) + (diffZ * diffZ)));

        tanX = -sins_s16(arctan2_f(diffX, diffZ)) >> 8;
        if (diffZ < 0.0f) {
            diffZ = -diffZ;
            tanX = 1 - tanX;
            tanY = -tanY;
        }

        tiltAngle = arctan2_f(diffY, diffZ);
        if (tiltAngle > 0x8000) {
            tiltAngle -= 0x10000;
        }
        tiltAngle = (tiltAngle * tanX) >> 8;
        frameID = (tanY >> 7) & 0xFF;
        if (frameID > 127) {
            stubbed_printf("CamDo2DSprite FrameNo Overflow !!!\n");
            frameID = 255 - frameID;
            tiltAngle += 0x8000;
            result = FALSE;
        }
        frameID *= 2;

        // Construct the model matrix for the sprite,
        // orienting it perpendicular to the camera and applying the calculated tilt angle.
        diffX = gCameraRelPosStackX[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->x_position;
        diffY = gCameraRelPosStackY[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->y_position;
        diffZ = gCameraRelPosStackZ[gCameraMatrixPos] - MDKR_BILLBOARD_TRANSFORM->z_position;
        lateralDist = sqrtf((diffX * diffX) + (diffZ * diffZ));
        gCameraTransform.rotation.y_rotation = arctan2_f(diffX, diffZ);
        gCameraTransform.rotation.x_rotation = -arctan2_f(diffY, lateralDist);
        gCameraTransform.rotation.z_rotation = tiltAngle;
        gCameraTransform.scale = MDKR_BILLBOARD_TRANSFORM->scale;
        gCameraTransform.x_position = MDKR_BILLBOARD_TRANSFORM->x_position;
        gCameraTransform.y_position = MDKR_BILLBOARD_TRANSFORM->y_position;
        gCameraTransform.z_position = MDKR_BILLBOARD_TRANSFORM->z_position;
        mtxf_from_transform(&gCurrentModelMatrixF, &gCameraTransform);
#ifdef NATIVE_PORT
        /* Preserve the actual child-local recipe before gCurrentModelMatrixF
         * is reused below as the final child-world x view-projection matrix.
         * Passing that final MVP as child_local makes presentation replay
         * multiply an already projected matrix by the interpolated racer root;
         * vehicle-part sprites (most visibly the kart wheels) then land outside
         * the viewport on every intermediate present. */
        memcpy(vehiclePartLocal, gCurrentModelMatrixF,
               sizeof(vehiclePartLocal));
#endif
        gModelMatrixStackPos++;
        mtxf_mul(&gCurrentModelMatrixF, gModelMatrixF[gModelMatrixStackPos - 1], gModelMatrixF[gModelMatrixStackPos]);
        mtxf_mul(gModelMatrixF[gModelMatrixStackPos], &gViewProjMatrixF, &gCurrentModelMatrixF);
        mtxf_to_mtx(&gCurrentModelMatrixF, *mtx);
        gModelMatrix[gModelMatrixStackPos] = *mtx;
#ifdef NATIVE_PORT
        /*
         * CLAIM THIS TENANCY. This push re-uses a matrix arena address whose
         * previous tenant is very likely still registered, and without a
         * registration of its own the registry would keep answering for the
         * dead one. The stale-tenant guard in gfx_pc_dkr.c makes that answer a
         * miss rather than a lie, but a miss costs this geometry its shadow --
         * and unlike the two billboard pushes below, vehicle parts (wheels,
         * propellers, fans) deliberately do NOT enable billboard mode, so they
         * are real caster geometry that should be casting.
         *
         * Registering is exact here and only here: the list matrix one line
         * above is gModelMatrixF[pos] x gViewProjMatrixF, the same
         * world-times-view-projection shape mtx_cam_push registers, so the
         * replay's recomposition reproduces it bit-for-bit. The billboard push
         * at the end of this function and the one in
         * render_ortho_triangle_image are NOT that shape -- they upload a local
         * billboard/scale matrix with no view-projection composed into it at
         * all -- so there is no (world, view_projection) pair that describes
         * them and they are deliberately left unregistered for the guard to
         * refuse.
         */
        {
            GfxPresentationMatrixOwner childOwner;
            const GfxPresentationMatrixOwner *owner = NULL;
            if (mdkr_presentation_owner_child(
                    &childOwner,
                    &sPresentationOwnerStack[gModelMatrixStackPos - 1],
                    &vehiclePartLocal)) {
                owner = &childOwner;
            }
            mdkr_shadow_register_matrix(
                *mtx, gModelMatrixF[gModelMatrixStackPos],
                GFX_SHADOW_MOBILITY_DYNAMIC,
                GFX_SHADOW_SITE_SPRITE_PART, owner);
        }
#endif
        gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_2);
        // Push an empty vertex because sprite vertices are hardcoded to start from index 1,
        // and billboard mode is not enabled for vehicle parts.
        gSPVertexDKR((*dList)++, OS_K0_TO_PHYSICAL(&gVehiclePartVertex), 1, 0);
    } else {
        // For non-vehicle parts, set up a standard billboard.

        // Push the anchor vertex at the object's position
        v = *vtx;
        v->x = MDKR_BILLBOARD_TRANSFORM->x_position;
        v->y = MDKR_BILLBOARD_TRANSFORM->y_position;
        v->z = MDKR_BILLBOARD_TRANSFORM->z_position;
        v->r = 255; // These don't actually do anything since vertex colours are disabled anyway.
        v->g = 255;
        v->b = 255;
        v->a = 255;
#ifdef NATIVE_PORT
        /* With a parent object matrix in force this anchor is local to that
         * already-interpolated root, so moving it again would double-apply
         * owner motion. Stack zero is the world-space object/particle case. */
        if (billboardOwnerValid &&
            !sPresentationOwnerStack[gModelMatrixStackPos].valid) {
            (void)gfx_presentation_packet_register_vertex(
                v, sizeof(*v), (int)sShadowRegisterViewport,
                &billboardOwner);
        }
#endif
        gSPVertexDKR((*dList)++, OS_K0_TO_PHYSICAL(*vtx), 1, 0);
        (*vtx)++;

        // Create a billboard matrix that compensates for camera tilt,
        // so the sprite tilts consistently with other objects relative to the camera.
        // Aspect ratio compensation is applied to maintain proper sprite proportions on screen.
        tiltAngle =
            cam_get_active_camera()->trans.rotation.z_rotation +
            MDKR_BILLBOARD_TRANSFORM->rotation.z_rotation;
        frameID = MDKR_BILLBOARD_ANIM_FRAME;
        gModelMatrixStackPos++;
        mtxf_billboard(
            gModelMatrixF[gModelMatrixStackPos], tiltAngle,
            MDKR_BILLBOARD_TRANSFORM->scale, gVideoAspectRatio);
#ifdef NATIVE_PORT
        {
            MdkrBillboardCorrection correction =
                mdkr_display_calculate_billboard_correction(
                    gCurCamFOV, gEffectiveCamVFOV, gVideoAspectRatio,
                    gEffectiveCamAspect, mdkr_display_widescreen_enabled());

            /*
             * The projected anchor has already passed through the perspective
             * matrix, but F3DDKR adds this matrix's local X/Y afterward in clip
             * space. Correct the output columns independently so host aspect and
             * FOV changes preserve the original pixel-space shape, rotation and
             * apparent world scale. The pure helper returns 1,1 for stock 4:3
             * and leaves the aspect term disabled in exact legacy-stretch mode.
             */
            mdkr_display_apply_billboard_correction(
                *gModelMatrixF[gModelMatrixStackPos], correction);
        }
#endif
        mtxf_to_mtx(gModelMatrixF[gModelMatrixStackPos], *mtx);
        gModelMatrix[gModelMatrixStackPos] = *mtx;
#ifdef NATIVE_PORT
        if (billboardOwnerValid) {
            (void)gfx_presentation_packet_register_matrix(
                *mtx, sizeof(**mtx), (int)sShadowRegisterViewport,
                &billboardOwner);
        }
#endif
        gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_2);
        // Enable billboard mode; subsequent vertices will be rendered relative to the anchor point
        gDkrEnableBillboard((*dList)++);
    }
    // Calculate the correct animation frame based on the current animation mode.
    if (gSpriteAnimMode == SPRITE_ANIM_NORMALIZED) {
        frameID = ((frameID & 0xFF) * sprite->numberOfFrames) >> 8;
    }

    // The RENDER_ANTI_ALIASING flag shares the same value as RENDER_VEHICLE_PART,
    // so we must clear RENDER_VEHICLE_PART from flags to avoid conflicts.
    // Anti-aliasing is automatically enabled for semi-transparent sprites.
    flags &= ~RENDER_VEHICLE_PART;
    if (flags & RENDER_SEMI_TRANSPARENT) {
        flags |= RENDER_ANTI_ALIASING;
    }

    // Load material and set render flags including fog, transparency, z-compare, and anti-aliasing
    material_load_simple(dList, sprite->drawFlags | (flags & (RENDER_FOG_ACTIVE | RENDER_SEMI_TRANSPARENT |
                                                              RENDER_Z_COMPARE | RENDER_ANTI_ALIASING)));

    if (!(flags & RENDER_Z_UPDATE)) {
        gDPSetPrimColor((*dList)++, 0, 0, 255, 255, 255, 255);
    }
    gSPDisplayList((*dList)++, sprite->frames[frameID]);

    // Pop the model matrix stack and select the appropriate matrix index for rendering
    gModelMatrixStackPos--;
    if (gModelMatrixStackPos == 0) {
        frameID = G_MTX_DKR_INDEX_0;
    } else {
        frameID = G_MTX_DKR_INDEX_1;
    }
    gSPSelectMatrixDKR((*dList)++, frameID);

    // Disable billboard mode
    gDkrDisableBillboard((*dList)++);

    return result;
}
#undef MDKR_BILLBOARD_TRANSFORM
#undef MDKR_BILLBOARD_ANIM_FRAME

#ifdef NATIVE_PORT
s32 render_sprite_billboard(Gfx **dList, Mtx **mtx, Vertex **vtx, Object *obj, Sprite *sprite, s32 flags) {
    if (obj == NULL) {
        return render_sprite_billboard_transform_impl(
            dList, mtx, vtx, NULL, 0, sprite, flags);
    }
    return render_sprite_billboard_transform_impl(
        dList, mtx, vtx, &obj->trans, obj->animFrame, sprite, flags);
}

s32 render_sprite_billboard_transform(Gfx **dList, Mtx **mtx, Vertex **vtx,
                                      const ObjectTransform *transform, s16 animFrame,
                                      Sprite *sprite, s32 flags) {
    return render_sprite_billboard_transform_impl(
        dList, mtx, vtx, transform, animFrame, sprite, flags);
}
#endif

/**
 * Sets transform and scale matrices to set position and size, loads the texture, sets the rendermodes, then draws the
 * result onscreen.
 */
#ifdef NATIVE_PORT
static void render_ortho_triangle_image_transform_impl(Gfx **dList, Mtx **mtx, Vertex **vtx,
                                                       const ObjectTransform *transform, s16 animFrame,
                                                       Sprite *sprite, s32 flags) {
#define MDKR_ORTHO_TRANSFORM transform
#define MDKR_ORTHO_ANIM_FRAME animFrame
#else
void render_ortho_triangle_image(Gfx **dList, Mtx **mtx, Vertex **vtx, ObjectSegment *segment, Sprite *sprite,
                                 s32 flags) {
#define MDKR_ORTHO_TRANSFORM (&segment->trans)
#define MDKR_ORTHO_ANIM_FRAME segment->animFrame
#endif
    UNUSED s32 pad;
    f32 scale;
    s32 index;
    Vertex *vertex;
    MtxF aspectMtxF;
    MtxF scaleMtxF;

    if (sprite == NULL) {
        return;
    }

    vertex = *vtx;
    vertex->x = MDKR_ORTHO_TRANSFORM->x_position;
    vertex->y = MDKR_ORTHO_TRANSFORM->y_position;
    vertex->z = MDKR_ORTHO_TRANSFORM->z_position;
    vertex->r = 255; // These don't actually do anything since vertex colours are disabled anyway.
    vertex->g = 255;
    vertex->b = 255;
    vertex->a = 255;

    gSPVertexDKR((*dList)++, OS_K0_TO_PHYSICAL(*vtx), 1, 0);
    (*vtx)++; // Can't be done in the macro?
    index = MDKR_ORTHO_ANIM_FRAME;

    gModelMatrixStackPos++;
    gCameraTransform.rotation.y_rotation = -MDKR_ORTHO_TRANSFORM->rotation.y_rotation;
    gCameraTransform.rotation.x_rotation = -MDKR_ORTHO_TRANSFORM->rotation.x_rotation;
    gCameraTransform.rotation.z_rotation =
        cam_get_active_camera()->trans.rotation.z_rotation +
        MDKR_ORTHO_TRANSFORM->rotation.z_rotation;
    gCameraTransform.x_position = 0.0f;
    gCameraTransform.y_position = 0.0f;
    gCameraTransform.z_position = 0.0f;
    if (gAdjustViewportHeight) {
        scale = MDKR_ORTHO_TRANSFORM->scale;
        mtxf_from_scale(&scaleMtxF, scale, scale, 1.0f);
        mtxf_billboard(&aspectMtxF, 0, 1.0f, gVideoAspectRatio);
        mtxf_mul(&aspectMtxF, &scaleMtxF, &gCurrentModelMatrixF);
    } else {
        scale = MDKR_ORTHO_TRANSFORM->scale;
        mtxf_from_scale(&gCurrentModelMatrixF, scale, scale, 1.0f);
    }
    mtxf_from_inverse_transform(&aspectMtxF, &gCameraTransform);
    mtxf_mul(&gCurrentModelMatrixF, &aspectMtxF, gModelMatrixF[gModelMatrixStackPos]);
    mtxf_to_mtx(gModelMatrixF[gModelMatrixStackPos], *mtx);
    gModelMatrix[gModelMatrixStackPos] = *mtx;
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_2);
    gDkrEnableBillboard((*dList)++);
    if (gSpriteAnimMode == SPRITE_ANIM_NORMALIZED) {
        index = (((u8) index) * sprite->numberOfFrames) >> 8;
    }
    material_load_simple(dList, sprite->drawFlags | flags);
    if (index >= sprite->numberOfFrames) {
        index = sprite->numberOfFrames - 1;
    }
    gSPDisplayList((*dList)++, sprite->frames[index]);
    if (--gModelMatrixStackPos == 0) {
        index = G_MTX_DKR_INDEX_0;
    } else {
        index = G_MTX_DKR_INDEX_1;
    }
    gSPSelectMatrixDKR((*dList)++, index);
    gDkrDisableBillboard((*dList)++);
}
#undef MDKR_ORTHO_TRANSFORM
#undef MDKR_ORTHO_ANIM_FRAME

#ifdef NATIVE_PORT
void render_ortho_triangle_image(Gfx **dList, Mtx **mtx, Vertex **vtx, ObjectSegment *segment,
                                 Sprite *sprite, s32 flags) {
    if (segment == NULL) {
        render_ortho_triangle_image_transform_impl(
            dList, mtx, vtx, NULL, 0, sprite, flags);
        return;
    }
    render_ortho_triangle_image_transform_impl(
        dList, mtx, vtx, &segment->trans, segment->animFrame, sprite, flags);
}

void render_ortho_triangle_image_transform(Gfx **dList, Mtx **mtx, Vertex **vtx,
                                           const ObjectTransform *transform, s16 animFrame,
                                           Sprite *sprite, s32 flags) {
    render_ortho_triangle_image_transform_impl(
        dList, mtx, vtx, transform, animFrame, sprite, flags);
}
#endif

/**
 * Generate a matrix with rotation, scaling and shearing and run it.
 * Used for wavy type effects like the shield.
 */
void mtx_shear_push(Gfx **dList, Mtx **mtx, Object *obj, Object *objBase, f32 shear) {
    UNUSED s32 pad;
    f32 cossf_x_arg2;
    f32 cossf_y_arg2;
    f32 sinsf_x_arg2;
    f32 sinsf_y_arg2;
    f32 sinsf_y_arg3;
    f32 sinsf_z_arg3;
    f32 arg2_scale;
    f32 cossf_x_arg3;
    f32 sinsf_x_arg3;
    f32 cossf_y_arg3;
    f32 cossf_z_arg3;
    f32 arg2_xPos;
    f32 arg2_yPos;
    f32 arg2_zPos;
    f32 arg3_xPos;
    f32 arg3_yPos;
    f32 arg3_zPos;
    MtxF matrix_mult;

    cossf_x_arg2 = coss_f(obj->trans.rotation.x_rotation);
    sinsf_x_arg2 = sins_f(obj->trans.rotation.x_rotation);
    cossf_y_arg2 = coss_f(obj->trans.rotation.y_rotation);
    sinsf_y_arg2 = sins_f(obj->trans.rotation.y_rotation);
    arg2_xPos = obj->trans.x_position;
    arg2_yPos = obj->trans.y_position;
    arg2_zPos = obj->trans.z_position;
    cossf_z_arg3 = coss_f(objBase->trans.rotation.z_rotation);
    sinsf_z_arg3 = sins_f(objBase->trans.rotation.z_rotation);
    cossf_x_arg3 = coss_f(objBase->trans.rotation.x_rotation);
    sinsf_x_arg3 = sins_f(objBase->trans.rotation.x_rotation);
    cossf_y_arg3 = coss_f(objBase->trans.rotation.y_rotation);
    sinsf_y_arg3 = sins_f(objBase->trans.rotation.y_rotation);
    arg3_xPos = objBase->trans.x_position;
    arg3_yPos = objBase->trans.y_position;
    arg3_zPos = objBase->trans.z_position;
    arg2_scale = obj->trans.scale;
    shear *= arg2_scale;
    matrix_mult[0][0] =
        ((((cossf_z_arg3 * cossf_y_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3))) * cossf_y_arg2) +
         (-sinsf_y_arg2 * (cossf_x_arg3 * sinsf_y_arg3))) *
        arg2_scale;
    matrix_mult[0][1] = (((sinsf_z_arg3 * cossf_x_arg3) * cossf_y_arg2) + (-sinsf_y_arg2 * -sinsf_x_arg3)) * arg2_scale;
    matrix_mult[0][2] =
        ((((-sinsf_y_arg3 * cossf_z_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3))) * cossf_y_arg2) +
         (-sinsf_y_arg2 * (cossf_x_arg3 * cossf_y_arg3))) *
        arg2_scale;
    matrix_mult[0][3] = 0.0f;
    matrix_mult[1][0] =
        ((((-sinsf_z_arg3 * cossf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3))) * cossf_x_arg2) +
         (sinsf_x_arg2 *
          ((sinsf_y_arg2 * ((cossf_z_arg3 * cossf_y_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3)))) +
           (cossf_y_arg2 * (cossf_x_arg3 * sinsf_y_arg3))))) *
        shear;
    matrix_mult[1][1] =
        (((cossf_z_arg3 * cossf_x_arg3) * cossf_x_arg2) +
         (sinsf_x_arg2 * ((sinsf_y_arg2 * (sinsf_z_arg3 * cossf_x_arg3)) + (cossf_y_arg2 * -sinsf_x_arg3)))) *
        shear;
    matrix_mult[1][2] =
        ((((-sinsf_z_arg3 * -sinsf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3))) * cossf_x_arg2) +
         (sinsf_x_arg2 *
          ((sinsf_y_arg2 * ((-sinsf_y_arg3 * cossf_z_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3)))) +
           (cossf_y_arg2 * (cossf_x_arg3 * cossf_y_arg3))))) *
        shear;
    matrix_mult[1][3] = 0.0f;
    matrix_mult[2][0] =
        ((-sinsf_x_arg2 * ((-sinsf_z_arg3 * cossf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3)))) +
         (cossf_x_arg2 *
          ((sinsf_y_arg2 * ((cossf_z_arg3 * cossf_y_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3)))) +
           (cossf_y_arg2 * (cossf_x_arg3 * sinsf_y_arg3))))) *
        arg2_scale;
    matrix_mult[2][1] =
        ((-sinsf_x_arg2 * (cossf_z_arg3 * cossf_x_arg3)) +
         (cossf_x_arg2 * ((sinsf_y_arg2 * (sinsf_z_arg3 * cossf_x_arg3)) + (cossf_y_arg2 * -sinsf_x_arg3)))) *
        arg2_scale;
    matrix_mult[2][2] =
        ((-sinsf_x_arg2 * ((-sinsf_z_arg3 * -sinsf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3)))) +
         (cossf_x_arg2 *
          ((sinsf_y_arg2 * ((-sinsf_y_arg3 * cossf_z_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3)))) +
           (cossf_y_arg2 * (cossf_x_arg3 * cossf_y_arg3))))) *
        arg2_scale;
    matrix_mult[2][3] = 0.0f;
    matrix_mult[3][0] =
        (((cossf_z_arg3 * cossf_y_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3))) * arg2_xPos) +
        (arg2_yPos * ((-sinsf_z_arg3 * cossf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * sinsf_y_arg3)))) +
        (arg2_zPos * (cossf_x_arg3 * sinsf_y_arg3)) + arg3_xPos;
    matrix_mult[3][1] = ((sinsf_z_arg3 * cossf_x_arg3) * arg2_xPos) + (arg2_yPos * (cossf_z_arg3 * cossf_x_arg3)) +
                        (arg2_zPos * -sinsf_x_arg3) + arg3_yPos;
    matrix_mult[3][2] =
        (((-sinsf_y_arg3 * cossf_z_arg3) + (sinsf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3))) * arg2_xPos) +
        (arg2_yPos * ((-sinsf_z_arg3 * -sinsf_y_arg3) + (cossf_z_arg3 * (sinsf_x_arg3 * cossf_y_arg3)))) +
        (arg2_zPos * (cossf_x_arg3 * cossf_y_arg3)) + arg3_zPos;
    matrix_mult[3][3] = 1.0f;

    mtxf_mul(&matrix_mult, &gViewProjMatrixF, &gCurrentMVPMatrixF);
    mtxf_to_mtx(&gCurrentMVPMatrixF, *mtx);
#ifdef NATIVE_PORT
    {
        MtxF identity;
        GfxPresentationMatrixOwner effectOwner;
        const GfxPresentationMatrixOwner *owner = NULL;
        uint64_t effectGeneration = 0u;
        mtxf_from_translation(&identity, 0.0f, 0.0f, 0.0f);
        if (obj != NULL && objBase != NULL &&
            presentation_snapshot_identity_ensure_generation(
                obj, &effectGeneration) &&
            mdkr_presentation_owner_root(
                &effectOwner, &objBase->trans, 1.0f, 0.0f, &identity)) {
            effectOwner.matrix_class = GFX_PRESENTATION_MATRIX_EFFECT;
            effectOwner.secondary_address = obj;
            effectOwner.secondary_generation = effectGeneration;
            effectOwner.effect_position[0] = obj->trans.x_position;
            effectOwner.effect_position[1] = obj->trans.y_position;
            effectOwner.effect_position[2] = obj->trans.z_position;
            effectOwner.effect_scale = obj->trans.scale;
            effectOwner.effect_rotation[0] =
                obj->trans.rotation.y_rotation;
            effectOwner.effect_rotation[1] =
                obj->trans.rotation.x_rotation;
            effectOwner.effect_rotation[2] =
                obj->trans.rotation.z_rotation;
            effectOwner.effect_shear = shear;
            owner = &effectOwner;
        }
        mdkr_shadow_register_matrix(
            *mtx, &matrix_mult, GFX_SHADOW_MOBILITY_DYNAMIC,
            GFX_SHADOW_SITE_SHEAR_PUSH, owner);
    }
#endif
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_1);
}

/**
 * Pushes a model matrix onto the model matrix stack, generates an MVP matrix from it, and uploads it to the RSP.
 * Also computes the camera's position relative to the new model (at the top of the stack),
 * and pushes it onto the camera-relative position stack.
 * This relative camera position can later be used for sprite positioning.
 *
 * Official Name: camPushModelMtx
 */
s32 mtx_cam_push(Gfx **dList, Mtx **mtx, ObjectTransform *trans, f32 scaleY, f32 offsetY) {
    f32 camRelX, camRelY, camRelZ;
    s32 index;
    f32 scaleFactor;
#ifdef NATIVE_PORT
    GfxPresentationMatrixOwner rootOwner;
    const GfxPresentationMatrixOwner *owner = NULL;
#endif

    // Generate model transformation matrix from input transform
    mtxf_from_transform(&gCurrentModelMatrixF, trans);

    // Apply optional Y-axis translation
    if (offsetY != 0.0f) {
        mtxf_translate_y(&gCurrentModelMatrixF, offsetY);
    }

    // Apply optional Y-axis scaling
    if (scaleY != 1.0f) {
        mtxf_scale_y(&gCurrentModelMatrixF, scaleY);
    }

    // Multiply model matrix with parent matrix (top of the model stack)
    mtxf_mul(&gCurrentModelMatrixF, gModelMatrixF[gModelMatrixStackPos], gModelMatrixF[gModelMatrixStackPos + 1]);

    // Compute the model-view-projection matrix
    mtxf_mul(gModelMatrixF[gModelMatrixStackPos + 1], &gViewProjMatrixF, &gCurrentMVPMatrixF);

    // Convert the MVP matrix to fixed-point format and upload to RSP
    mtxf_to_mtx(&gCurrentMVPMatrixF, *mtx);
#ifdef NATIVE_PORT
    if (mdkr_presentation_owner_root(
            &rootOwner, trans, scaleY, offsetY,
            gModelMatrixF[gModelMatrixStackPos])) {
        owner = &rootOwner;
    }
    mdkr_shadow_register_matrix(
        *mtx, gModelMatrixF[gModelMatrixStackPos + 1],
        GFX_SHADOW_MOBILITY_DYNAMIC, GFX_SHADOW_SITE_CAM_PUSH, owner);
#endif
    gModelMatrixStackPos++;
    gModelMatrix[gModelMatrixStackPos] = *mtx;
#ifdef NATIVE_PORT
    if (owner != NULL) {
        sPresentationOwnerStack[gModelMatrixStackPos] = *owner;
    } else {
        memset(&sPresentationOwnerStack[gModelMatrixStackPos], 0,
               sizeof(sPresentationOwnerStack[gModelMatrixStackPos]));
    }
#endif

    if (gModelMatrixStackPos > CAMERA_MODEL_STACK_SIZE) {
        stubbed_printf("cameraPushModelMtx: model stack overflow!!\n");
    }

    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_1);

    // Compute world-space position of the model's origin (0, 0, 0)
    mtxf_transform_point(*gModelMatrixF[gModelMatrixStackPos], 0.0f, 0.0f, 0.0f, &camRelX, &camRelY, &camRelZ);

    index = gActiveCameraID;
    if (gCutsceneCameraActive) {
        // Use cutscene camera if active
        index += 4;
    }

    // Compute camera position relative to the model's origin in world space
    {
        Camera *camera = camera_obstruction_camera_for_slot(index);
        camRelX = camera->trans.x_position - camRelX;
        camRelY = camera->trans.y_position - camRelY;
        camRelZ = camera->trans.z_position - camRelZ;
    }

    // Convert camera position from world space to the model's local coordinate space
    gCameraTransform.rotation.y_rotation = -trans->rotation.y_rotation;
    gCameraTransform.rotation.x_rotation = -trans->rotation.x_rotation;
    gCameraTransform.rotation.z_rotation = -trans->rotation.z_rotation;
    gCameraTransform.x_position = 0.0f;
    gCameraTransform.y_position = 0.0f;
    gCameraTransform.z_position = 0.0f;
    gCameraTransform.scale = 1.0f;

    mtxf_from_inverse_transform(&gCurrentModelMatrixF, &gCameraTransform);
    mtxf_transform_point(gCurrentModelMatrixF, camRelX, camRelY, camRelZ, &camRelX, &camRelY, &camRelZ);

    // Adjust for model scale
    scaleFactor = 1.0f / trans->scale;
    camRelX *= scaleFactor;
    camRelY *= scaleFactor;
    camRelZ *= scaleFactor;

    // Push camera position relative to the current model onto the stack
    gCameraMatrixPos++;
    gCameraRelPosStackX[gCameraMatrixPos] = camRelX;
    gCameraRelPosStackY[gCameraMatrixPos] = camRelY;
    gCameraRelPosStackZ[gCameraMatrixPos] = camRelZ;

    if (gCameraMatrixPos > CAMERA_MODEL_STACK_SIZE) {
        stubbed_printf("camPushModelMtx: bsp stack overflow!!\n");
    }

#ifdef AVOID_UB
    // Likely void in original code, but returns 0 to match waves_render.
    return 0;
#endif
}

/**
 * Calculate the rotation matrix for an actors head, then run it.
 */
void mtx_head_push(Gfx **dList, Mtx **mtx, ModelInstance *modInst, s16 headAngle) {
    f32 coss_headAngle;
    f32 sins_headAngle;
    f32 offsetX;
    f32 offsetY;
    f32 offsetZ;
    f32 coss_unk1C;
    f32 sins_unk1C;
    MtxF rotationMtxF;
    MtxF headMtxF;
#ifdef NATIVE_PORT
    MtxF headWorldMtxF;
#endif

    offsetX = (f32) modInst->offsetX;
    offsetY = (f32) modInst->offsetY;
    offsetZ = (f32) modInst->offsetZ;
    coss_unk1C = coss_f(modInst->headTilt);
    sins_unk1C = sins_f(modInst->headTilt);
    coss_headAngle = coss_f(headAngle);
    sins_headAngle = sins_f(headAngle);
    headMtxF[0][0] = (coss_headAngle * coss_unk1C);
    headMtxF[0][1] = (coss_headAngle * sins_unk1C);
    headMtxF[0][2] = -sins_headAngle;
    headMtxF[0][3] = 0.0f;
    headMtxF[1][0] = -sins_unk1C;
    headMtxF[1][1] = coss_unk1C;
    headMtxF[1][2] = 0.0f;
    headMtxF[1][3] = 0.0f;
    headMtxF[2][0] = (sins_headAngle * coss_unk1C);
    headMtxF[2][1] = (sins_headAngle * sins_unk1C);
    headMtxF[2][2] = coss_headAngle;
    headMtxF[2][3] = 0.0f;
    headMtxF[3][0] = (-offsetX * (coss_headAngle * coss_unk1C)) + (-offsetY * -sins_unk1C) +
                     (-offsetZ * (sins_headAngle * coss_unk1C)) + offsetX;
    headMtxF[3][1] = (-offsetX * (coss_headAngle * sins_unk1C)) + (-offsetY * coss_unk1C) +
                     (-offsetZ * (sins_headAngle * sins_unk1C)) + offsetY;
    headMtxF[3][2] = (-offsetX * -sins_headAngle) + (-offsetZ * coss_headAngle) + offsetZ;
    headMtxF[3][3] = 1.0f;
    mtxf_mul(&headMtxF, &gCurrentMVPMatrixF, &rotationMtxF);
    mtxf_to_mtx(&rotationMtxF, *mtx);
#ifdef NATIVE_PORT
    /*
     * The list matrix one line above composes against gCurrentMVPMatrixF --
     * whichever site wrote it last -- while this world composes against the top
     * of the MODEL stack. Those are only the same matrix if mtx_cam_push was
     * the last writer, so this reads like a seam that a mtx_shear_push or a
     * mtx_pop could open between the two.
     *
     * MEASURED: it cannot, at the only caller. objects.c calls mtx_head_push on
     * the statement immediately after mtx_cam_push, with nothing between them,
     * so the two expressions are bit-identical every time. Instrumenting both
     * and comparing them over level 40's `nav_to_time_trial_race` route at
     * MDKR_PRESENT_RATE=60 found 4,271 head pushes and 0 disagreements.
     *
     * This is recorded because the wrong registered worlds measured on this
     * path were once attributed to that seam.
     * They were dead tenants instead -- registry entries whose Mtx address had
     * been rewritten by a later, unregistered push -- and they are fixed in the
     * registry, not here. Mirroring the MVP's world into a shadow global was
     * implemented, measured to change nothing, and reverted rather than left in
     * the hottest matrix path as a 64-byte copy per object per frame.
     */
    mtxf_mul(
        &headMtxF,
        gModelMatrixF[gModelMatrixStackPos],
        &headWorldMtxF);
    {
        GfxPresentationMatrixOwner headOwner;
        const GfxPresentationMatrixOwner *owner = NULL;
        if (mdkr_presentation_owner_child(
                &headOwner,
                &sPresentationOwnerStack[gModelMatrixStackPos],
                &headMtxF)) {
            owner = &headOwner;
        }
        mdkr_shadow_register_matrix(
            *mtx, &headWorldMtxF, GFX_SHADOW_MOBILITY_DYNAMIC,
            GFX_SHADOW_SITE_HEAD_PUSH, owner);
    }
#endif
    gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL((*mtx)++), G_MTX_DKR_INDEX_2);
    gSPSelectMatrixDKR((*dList)++, G_MTX_DKR_INDEX_1);
}

/**
 * Writes the model matrix vector to the arguments.
 */
UNUSED void get_modelmatrix_vector(f32 *x, f32 *y, f32 *z) {
    *x = gCameraRelPosStackX[gCameraMatrixPos];
    *y = gCameraRelPosStackY[gCameraMatrixPos];
    *z = gCameraRelPosStackZ[gCameraMatrixPos];
}

/**
 * Run a matrix from the top of the stack and pop it.
 * If the stack pos is less than zero, set the RSP stack pos to 0.
 */
void mtx_pop(Gfx **dList) {
    s32 temp;

#ifdef NATIVE_PORT
    if (gModelMatrixStackPos >= 0 &&
        gModelMatrixStackPos <= CAMERA_MODEL_STACK_SIZE) {
        memset(&sPresentationOwnerStack[gModelMatrixStackPos], 0,
               sizeof(sPresentationOwnerStack[gModelMatrixStackPos]));
    }
#endif
    gCameraMatrixPos--;
    gModelMatrixStackPos--;

    if (gModelMatrixStackPos < 0) {
        stubbed_printf("camPopModelMtx: model stack negative overflow!!\n");
    }

    if ((temp = gCameraMatrixPos < 0)) { // temp required to match
        stubbed_printf("camPopModelMtx: bsp stack negative overflow!!\n");
    }

    if (gModelMatrixStackPos > 0) {
        gSPMatrixDKR((*dList)++, OS_K0_TO_PHYSICAL(gModelMatrix[gModelMatrixStackPos]), G_MTX_DKR_INDEX_1);
    } else {
        gSPSelectMatrixDKR((*dList)++, G_MTX_DKR_INDEX_0);
    }
}

/**
 * Move the camera with the given velocities.
 * Also recalculates which block it's in.
 */
UNUSED void cam_move(f32 x, f32 y, f32 z) {
    gCameras[gActiveCameraID].trans.x_position += x;
    gCameras[gActiveCameraID].trans.y_position += y;
    gCameras[gActiveCameraID].trans.z_position += z;
    gCameras[gActiveCameraID].cameraSegmentID = get_level_segment_index_from_position(
        gCameras[gActiveCameraID].trans.x_position, gCameras[gActiveCameraID].trans.y_position,
        gCameras[gActiveCameraID].trans.z_position);
}

/**
 * Move the camera with velocities accounting for face direction.
 * Also recalculates which block it's in.
 */
UNUSED void cam_move_dir(f32 x, UNUSED f32 y, f32 z) {
    gCameras[gActiveCameraID].trans.x_position -= x * coss_f(gCameras[gActiveCameraID].trans.rotation.y_rotation);
    gCameras[gActiveCameraID].trans.z_position -= x * sins_f(gCameras[gActiveCameraID].trans.rotation.y_rotation);
    gCameras[gActiveCameraID].trans.x_position -= z * sins_f(gCameras[gActiveCameraID].trans.rotation.y_rotation);
    gCameras[gActiveCameraID].trans.z_position += z * coss_f(gCameras[gActiveCameraID].trans.rotation.y_rotation);
    gCameras[gActiveCameraID].cameraSegmentID = get_level_segment_index_from_position(
        gCameras[gActiveCameraID].trans.x_position, gCameras[gActiveCameraID].trans.y_position,
        gCameras[gActiveCameraID].trans.z_position);
}

/**
 * Rotate the camera with the given angles.
 */
UNUSED void cam_rotate(s32 angleX, s32 angleY, s32 angleZ) {
    gCameras[gActiveCameraID].trans.rotation.y_rotation += angleX;
    gCameras[gActiveCameraID].trans.rotation.x_rotation += angleY;
    gCameras[gActiveCameraID].trans.rotation.z_rotation += angleZ;
}

/**
 * Returns the active camera, but won't apply the offset for cutscenes.
 */
Camera *cam_get_active_camera_no_cutscenes(void) {
#ifdef NATIVE_PORT
    return camera_obstruction_camera_for_slot(gActiveCameraID);
#else
    return &gCameras[gActiveCameraID];
#endif
}

/**
 * Returns the active camera.
 */
Camera *cam_get_active_camera(void) {
#ifdef NATIVE_PORT
    return camera_obstruction_camera_for_slot(
        gActiveCameraID + (gCutsceneCameraActive ? 4 : 0));
#else
    if (gCutsceneCameraActive) {
        return &gCameras[gActiveCameraID + 4];
    }
    return &gCameras[gActiveCameraID];
#endif
}

/**
 * Returns the segment data of the active cutscene camera.
 * If no cutscene is active, return player 1's camera.
 */
Camera *cam_get_cameras(void) {
#ifdef NATIVE_PORT
    return camera_obstruction_camera_for_slot(gCutsceneCameraActive ? 4 : 0);
#else
    if (gCutsceneCameraActive) {
        return &gCameras[4];
    }
    return &gCameras[0];
#endif
}

/**
 * Return the current floating point projection matrix.
 */
MtxF *get_projection_matrix_f32(void) {
    return &gInverseViewMatrixF;
}

/**
 * Return the current fixed point projection matrix.
 */
Mtx *get_projection_matrix_s16(void) {
    return &gPerspectiveMatrix;
}

/**
 * Return the current camera matrix.
 */
MtxF *get_camera_matrix(void) {
    return &gViewMatrixF;
}

/**
 * Return the screenspace distance to the camera.
 */
f32 get_distance_to_camera(f32 x, f32 y, f32 z) {
    f32 ox, oy, oz;

    mtxf_transform_point(gViewMatrixF, x, y, z, &ox, &oy, &oz);

    return oz;
}

/**
 * Apply a shake to the camera based on the distance to the source.
 */
void set_camera_shake_by_distance(f32 x, f32 y, f32 z, f32 dist, f32 magnitude) {
    f32 diffX;
    f32 distance;
    f32 diffZ;
    f32 diffY;
    s32 i;

    for (i = 0; i <= gViewportLayout; i++) {
        diffX = x - gCameras[i].trans.x_position;
        diffY = y - gCameras[i].trans.y_position;
        diffZ = z - gCameras[i].trans.z_position;
        distance = sqrtf(((diffX * diffX) + (diffY * diffY)) + (diffZ * diffZ));
        if (distance < dist) {
            gCameras[i].shakeMagnitude = ((dist - distance) * magnitude) / dist;
        }
    }
}

/**
 * Apply a shake to all active cameras.
 */
void set_camera_shake(f32 magnitude) {
    s32 i;
    for (i = 0; i <= gViewportLayout; i++) {
        gCameras[i].shakeMagnitude = magnitude;
    }
}

/**
 * Unused function that prints out the passed matrix values to the debug output.
 * This function prints in fixed point.
 */
UNUSED void debug_print_fixed_matrix_values(s16 *mtx) {
    s32 i, j;
    s32 val;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            val = mtx[i * 4 + j];
            rmonPrintf("%x.", val);
            val = mtx[((i + 4) * 4 + j)];
            rmonPrintf("%x  ", (u16) val & 0xFFFF);
        }
        rmonPrintf("\n");
        if (!val) {} // Fakematch
    }
    rmonPrintf("\n");
}

/**
 * Unused function that prints out the passed matrix values to the debug output.
 * This function prints in floating point.
 */
UNUSED void debug_print_float_matrix_values(f32 *mtx) {
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            rmonPrintf("%f  ", mtx[i * 4 + j]);
        }
        rmonPrintf("\n");
    }
    rmonPrintf("\n");
}
