#include "thread3_main.h"

#include "asset_enums.h"
#include "asset_loading.h"
#include "audio.h"
#include "audio_spatial.h"
#include "audiomgr.h"
#include "audiosfx.h"
#include "borders.h"
#include "camera.h"
#ifdef NATIVE_PORT
#include "camera_obstruction_runtime.h"
#endif
/* Its own guarded block, deliberately: tests/test_camera_obstruction_observe.py
 * matches the three lines above exactly, to prove the matching build cannot
 * pull in the native observer. */
#ifdef NATIVE_PORT
#include "video_config.h"   /* the level apply boundary, in the load paths */
#endif
#include "common.h"
#include "f3ddkr.h"
#include "fade_transition.h"
#include "font.h"
#include "game.h"
#include "game_text.h"
#include "game_ui.h"
#include "gzip.h"
#include "joypad.h"
#include "lights.h"
#include "macros.h"
#include "main.h"
#include "math_util.h"
#include "memory.h"
#include "menu.h"
#include "network_player_authority.h"
#include "object_models.h"
#include "objects.h"
#include "particles.h"
#include "PR/os_internal.h"
#include "PRinternal/viint.h"
#include "printf.h"
#include "racer.h"
#include "rcp_dkr.h"
#include "save_data.h"
#include "save_layout.h"
#include "set_rsp_segment.h"
#include "stacks.h"
#include "structs.h"
#include "textures_sprites.h"
#include "thread0_epc.h"
#include "thread30_bgload.h"
#include "tracks.h"
#include "types.h"
#include "video.h"
#include "weather.h"
#include <PR/gu.h>
#include <PR/os_cont.h>
#include <PR/os_time.h>
#ifdef NATIVE_PORT
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net/net_roster_runtime.h"
#if MDKR_ENABLE_ONLINE_BETA
/* SEPARATED-BOOT-PATH: the online session mode + fork entry. Beta-only; a normal
 * (beta OFF) build never sees this include and the file it names is not compiled
 * (game/src/online/ is not globbed and is CMake-gated on the beta macro). */
#include "online/online_session.h"
/* mdkr_party_link_active() for the descriptor-less lobby-start fork
 * below. Dependency-free header; the TU (party_link.c) is beta-only, so the OFF
 * build never sees this include and thread3_main.o stays byte-identical. */
#include "net/party_link.h"
/* NON-BLOCKING online race pause: the deterministic retail-pause suppression
 * (mode_game) + the per-frame local overlay (main_game_loop). Beta-only TU
 * (game/src/online/, CMake-gated); the OFF build never sees this include. */
#include "online/online_race_pause.h"
#endif
#include "platform_os.h"
#include "app_overlay_hooks.h"
#include "app/engine_entry.h"
#include "modern_character_runtime.h"
#include "waves.h"
#include "fast3d/gfx_pc_dkr.h"
#include "gpu_diagnostics.h"
#include "present_sched.h"
#include "gameplay_event_trace.h"
#include "presentation_snapshot.h"
#include "rollback/rollback_game_runtime.h"
#include "taj_mod.h"
#ifndef MDKR_ADVENTURE_PARTY_OMIT
/* AP-10 shared pause + controller-disconnect authority. Behind
 * NATIVE_PORT && !OMIT with an immediate stock else at every call, so OMIT /
 * matching N64 compile the feature out. */
#include "adventure_party/adventure_party_policy.h"
#include "adventure_party/adventure_party_runtime.h"
#include "adventure_party/adventure_party_state.h"
#include "adventure_party/adventure_party_trace.h"
#include "mdkr_adventure.h" /* mdkr_test_pad_absent test injector */
#endif

extern int platform_pace_is_synthetic(void);
extern s32 gRaceStartTimer;
#endif

/************ .rodata ************/

#if VERSION >= VERSION_79
UNUSED char *sDebugRomBuildInfo[] = { "1.1634", "17/10/97 11:19", "pmountain" };
#elif VERSION == VERSION_77
UNUSED char *sDebugRomBuildInfo[] = { "1.1605", "02/10/97 16:03", "pmountain" };
#endif

const char D_800E7134[] = "BBB\n"; // Functionally unused.

/*********************************/

/************ .data ************/

#if VERSION == VERSION_80
UNUSED char gBuildString[] = "Version 8.0 27/10/97 12.30 L.Schuneman";
#elif VERSION == VERSION_79
UNUSED char gBuildString[] = "Version 7.9 14/10/97 19.40 L.Schuneman";
#elif VERSION == VERSION_77
UNUSED char gBuildString[] = "Version 7.7 29/09/97 15.00 L.Schuneman";
#endif

s8 sAntiPiracyTriggered = FALSE;
UNUSED s32 D_800DD378 = 1;
s32 gSaveDataFlags = 0; // Official Name: load_save_flags
s32 gScreenStatus = OSMESG_SWAP_BUFFER;
s32 sControllerStatus = 0;
UNUSED s32 D_800DD388 = 0;
s8 gSkipGfxTask = FALSE;
s8 gDrumstickSceneLoadTimer = 0;
s16 gLevelLoadTimer = 0;
s8 gPauseLockTimer = 0; // If this is above zero, the player cannot pause the game.
s8 gFutureFunLandLevelTarget = FALSE;
s8 gDmemInvalid = FALSE;
UNUSED s32 D_800DD3A4[] = { 0, 0, 0 };
s32 gNumF3dCmdsPerPlayer[MAXCONTROLLERS] = { 4500, 7000, 11000, 11000 };
s32 gNumHudVertsPerPlayer[MAXCONTROLLERS] = { 300, 600, 850, 900 };
s32 gNumHudMatPerPlayer[MAXCONTROLLERS] = { 300, 400, 550, 600 };
s32 gNumHudTrisPerPlayer[MAXCONTROLLERS] = { 20, 30, 40, 50 };
s8 gDrawFrameTimer = 0;
FadeTransition D_800DD3F4 = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_OUT, FADE_COLOR_BLACK, 20, 0);
UNUSED FadeTransition D_800DD3FC = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_WHITE, 20, FADE_STAY);
s32 sLogicUpdateRate = LOGIC_5FPS;
FadeTransition gDrumstickSceneTransition =
    FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_WHITE, 30, FADE_STAY);
UNUSED char *D_800DD410[3] = { "CAR", "HOV", "PLN" };
FadeTransition gLevelFadeOutTransition =
    FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_BLACK, 30, FADE_STAY);
FadeTransition D_800DD424 = FADE_TRANSITION(FADE_FULLSCREEN, FADE_FLAG_NONE, FADE_COLOR_BLACK, 260, FADE_STAY);

/*******************************/

/************ .bss ************/

#ifdef NATIVE_PORT
/* Diagnostic/rollback execution mode. It is not simulation authority: it only
 * suppresses presentation while the already-registered state is replayed. */
static s32 sRollbackResimulating;
#endif

Gfx *gDisplayLists[2];
Gfx *gCurrDisplayList;
UNUSED s32 D_801211FC;
Mtx *gMatrixHeap[2];
Mtx *gGameCurrMatrix;
Vertex *gVertexHeap[2];
Vertex *gGameCurrVertexList;
Triangle *gTriangleHeap[2];
Triangle *gGameCurrTriList;
UNUSED s32 D_80121230[8];
s8 gLevelSettings[16];
OSSched gMainSched; // 0x288 / 648 bytes
u64 gSchedStack[STACKSIZE(STACK_SCHED)];
s32 gSPTaskNum;
s32 gGameMode;
s32 gRenderMenu; // I don't think this is ever not 1
// Similar to gMapId, but is 0 if not currently playing a level (e.g. start menu).
s32 gPlayableMapId;
s32 D_801234F8;
s32 D_801234FC;
s32 gGameNumPlayers;
s32 gGameCurrentEntrance;
s32 gGameCurrentCutscene;
s32 gPrevPlayerCount;
Settings *gSettingsPtr;
static Settings *sWriteSaveSource;
s8 gIsLoading;
s8 gIsPaused;
s8 gPostRaceViewPort;
Vehicle gLevelDefaultVehicleID;
Vehicle gMenuVehicleID; // Looks to be the current level's vehicle ID.
s32 sBootDelayTimer;
s8 gLevelLoadType;
s8 gNextMap;
UNUSED s8 D_80123526; // Set to 0 then never used.
s32 gCurrNumF3dCmdsPerPlayer;
s32 gCurrNumHudMatPerPlayer;
s32 gCurrNumHudTrisPerPlayer;
s32 gCurrNumHudVertsPerPlayer;
OSScClient *gNMISched[3];
OSMesg gNMIOSMesg;
OSMesgQueue gNMIMesgQueue;
s32 gNMIMesgBuf;          // Official Name: resetPressed
UNUSED s32 D_80123568[3]; // BSS Padding

#ifdef NATIVE_PORT
#define WORKSHOP_PREVIEW_WARMUP_TICKS 120u
#define WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS 60u
#define WORKSHOP_MOTION_REVIEW_SAMPLE_TIMEOUT_TICKS 600u
_Static_assert(
    MDKR_CHARACTER_PREVIEW_CONTACTS == MDKR_MODERN_CHARACTER_CONTACTS,
    "preview and runtime contact order must remain identical");
_Static_assert(
    MDKR_CHARACTER_PREVIEW_JOINTS == MDKR_MODERN_HUMANOID_ROLE_COUNT,
    "preview and runtime humanoid role order must remain identical");
_Static_assert(
    MDKR_CHARACTER_PREVIEW_LANDMARKS ==
            MDKR_MODERN_CHARACTER_FIT_LANDMARKS &&
        MDKR_CHARACTER_PREVIEW_LANDMARK_HIPS ==
            MDKR_MODERN_CHARACTER_FIT_LANDMARK_HIPS &&
        MDKR_CHARACTER_PREVIEW_LANDMARK_CHEST ==
            MDKR_MODERN_CHARACTER_FIT_LANDMARK_CHEST &&
        MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD ==
            MDKR_MODERN_CHARACTER_FIT_LANDMARK_HEAD,
    "preview and runtime anatomy landmark order must remain identical");
static u64 sWorkshopPreviewWarmupTicks;
static s32 sWorkshopPreviewMeasurementStarted;
static s32 sWorkshopPreviewMeasurementFinished;
static u32 sWorkshopPreviewVisibilityAttempts;
static MdkrCharacterPreviewResult sWorkshopPreviewDiagnosticResult;
static MdkrCharacterMotionReviewResult sWorkshopMotionReviewDiagnosticResult;
static MdkrModernCharacterRuntimeMetrics sWorkshopPreviewCharacterBaseline;
static MdkrWorkshopPreviewVisualMetrics sWorkshopPreviewVisualBaseline;
static char sWorkshopPreviewCapturePath[1024];
static u64 sWorkshopPreviewCaptureStableFrames;
static u64 sWorkshopPreviewCaptureLastReplacementDraws;
static u64 sWorkshopPreviewCaptureLastReferenceDraws;
static u64 sWorkshopPreviewCaptureLastDonorReferenceBatches;
static s32 sWorkshopPreviewCaptureArmed;
static MdkrCharacterPreviewCaptureKind sWorkshopPreviewCaptureKind;
static u32 sWorkshopMotionReviewSample;
static u32 sWorkshopMotionReviewStageTicks;
static u32 sWorkshopMotionReviewVisibilityAttempts;
static u64 sWorkshopMotionReviewReplacementBaseline;
static s32 sWorkshopMotionReviewPoseSettled;
static s32 sWorkshopMotionReviewDiagnosticsRequested;
static s32 sWorkshopMotionReviewUiReported;
static MdkrModernCharacterRuntimeMetrics sWorkshopMotionReviewPoseBaseline;

typedef struct WorkshopMotionReviewDefinition {
    MdkrCharacterPreviewPose pose;
    const char *semantic;
    const char *label;
    u32 phase_milli;
} WorkshopMotionReviewDefinition;

static const WorkshopMotionReviewDefinition sWorkshopVehicleMotionReviewDefinitions
    [MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT] = {
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER,
         "race.steer", "Steer left", 0u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER,
         "race.steer", "Steer right", 1000u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_REVERSE,
         "race.reverse", "Reverse", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_BOOST,
         "race.boost", "Boost", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_ITEM,
         "race.item", "Use item", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_DAMAGE,
         "race.damage", "Take damage", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_SPIN,
         "race.spin", "Spin", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_AIRBORNE,
         "race.airborne", "Airborne", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_LAND,
         "race.land", "Land", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_FINISH_WIN,
         "race.finish_win", "Win finish", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_RACE_FINISH_LOSE,
         "race.finish_lose", "Lose finish", 500u},
};

static const WorkshopMotionReviewDefinition sWorkshopSelectMotionReviewDefinitions
    [MDKR_CHARACTER_MOTION_REVIEW_SELECT_SAMPLE_COUNT] = {
        {MDKR_CHARACTER_PREVIEW_POSE_SELECT_IDLE,
         "select.idle", "Idle", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_SELECT_HOVER,
         "select.hover", "Hover", 500u},
        {MDKR_CHARACTER_PREVIEW_POSE_SELECT_CONFIRM,
         "select.confirm", "Confirm", 500u},
};

_Static_assert(
    sizeof(sWorkshopVehicleMotionReviewDefinitions) /
            sizeof(sWorkshopVehicleMotionReviewDefinitions[0]) ==
        MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT,
    "semantic motion review definitions must cover every result slot");

static u32 workshop_motion_review_sample_count(
    MdkrCharacterPreviewContext context) {
    return context == MDKR_CHARACTER_PREVIEW_SELECT
        ? MDKR_CHARACTER_MOTION_REVIEW_SELECT_SAMPLE_COUNT
        : MDKR_CHARACTER_MOTION_REVIEW_VEHICLE_SAMPLE_COUNT;
}

static const WorkshopMotionReviewDefinition *workshop_motion_review_definition(
    MdkrCharacterPreviewContext context, u32 sample) {
    const u32 count = workshop_motion_review_sample_count(context);
    if (sample >= count) return NULL;
    return context == MDKR_CHARACTER_PREVIEW_SELECT
        ? &sWorkshopSelectMotionReviewDefinitions[sample]
        : &sWorkshopVehicleMotionReviewDefinitions[sample];
}

static const char *workshop_motion_review_context_label(
    MdkrCharacterPreviewContext context) {
    switch (context) {
        case MDKR_CHARACTER_PREVIEW_SELECT: return "SELECT";
        case MDKR_CHARACTER_PREVIEW_CAR: return "CAR";
        case MDKR_CHARACTER_PREVIEW_HOVERCRAFT: return "HOVERCRAFT";
        case MDKR_CHARACTER_PREVIEW_PLANE: return "PLANE";
        default: return "CHARACTER";
    }
}

static const char *workshop_motion_review_scene_label(
    MdkrCharacterPreviewScene scene) {
    switch (scene) {
        case MDKR_CHARACTER_PREVIEW_SCENE_BASELINE: return "OPEN";
        case MDKR_CHARACTER_PREVIEW_SCENE_DENSE: return "DENSE";
        case MDKR_CHARACTER_PREVIEW_SCENE_ALTERNATE: return "ALTERNATE";
        case MDKR_CHARACTER_PREVIEW_SCENE_LOW_VISIBILITY: return "DARK";
        case MDKR_CHARACTER_PREVIEW_SCENE_EFFECTS: return "EFFECTS";
        default: return "UNKNOWN";
    }
}

static void workshop_motion_review_render_status(void) {
    MdkrCharacterMotionReviewResult *review =
        g_mdkrCharacterMotionReviewResult;
    const WorkshopMotionReviewDefinition *definition;
    MdkrModernCharacterRuntimeMetrics metrics;
    u64 stableDraws = 0u;
    u32 heldDraws;
    u32 filled;
    u32 index;
    u32 courseCount;
    char progress[11];
    const char *stage;
    if (review == NULL || review->completed || review->failed_sample != 0u ||
        review->context < MDKR_CHARACTER_PREVIEW_SELECT ||
        review->context > MDKR_CHARACTER_PREVIEW_PLANE ||
        review->scene < MDKR_CHARACTER_PREVIEW_SCENE_BASELINE ||
        review->scene >= MDKR_CHARACTER_PREVIEW_SCENE_COUNT) return;
    definition = workshop_motion_review_definition(
        review->context, sWorkshopMotionReviewSample);
    if (definition == NULL) return;

    mdkr_modern_character_runtime_metrics(&metrics);
    if (sWorkshopMotionReviewPoseSettled &&
        metrics.replacement_draws >= sWorkshopMotionReviewReplacementBaseline) {
        stableDraws = metrics.replacement_draws -
            sWorkshopMotionReviewReplacementBaseline;
    }
    heldDraws = stableDraws < WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS
        ? (u32)stableDraws : WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS;
    filled = heldDraws * 10u / WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS;
    for (index = 0u; index < 10u; ++index) {
        progress[index] = index < filled ? '#' : '-';
    }
    progress[10] = '\0';
    courseCount = review->context == MDKR_CHARACTER_PREVIEW_SELECT
        ? 1u : MDKR_CHARACTER_PREVIEW_SCENE_COUNT;
    stage = !sWorkshopPreviewMeasurementStarted
        ? "PREPARING"
        : !sWorkshopMotionReviewPoseSettled
            ? "SETTLING"
            : sWorkshopMotionReviewDiagnosticsRequested
                ? "MEASURING"
                : "INSPECT";

    set_render_printf_position(8, 8);
    set_render_printf_background_colour(0, 0, 0, 184);
    set_render_printf_colour(255, 214, 76, 255);
    render_printf(
        "REVIEW %s - %s %u/%u\n",
        workshop_motion_review_context_label(review->context),
        review->context == MDKR_CHARACTER_PREVIEW_SELECT
            ? "ROOM" : workshop_motion_review_scene_label(review->scene),
        (u32)review->scene + 1u, courseCount);
    set_render_printf_colour(255, 255, 255, 255);
    render_printf(
        "%u/%u %s - %s [%s] %u/%u\n",
        sWorkshopMotionReviewSample + 1u, review->sample_count,
        definition->label, stage, progress, heldDraws,
        WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS);
    set_render_printf_colour(190, 220, 255, 255);
    /* The trailing newline commits this line's background rectangle before the
     * colour reset below. Without it, debug_text_print() reaches the reset
     * command before flushing the final line and the help text loses contrast. */
    render_printf("Esc/F1 or pad Back: pause / stop safely\n");
    set_render_printf_background_colour(0, 0, 0, 0);
    set_render_printf_colour(255, 255, 255, 255);
    if (!sWorkshopMotionReviewUiReported) {
        sWorkshopMotionReviewUiReported = TRUE;
        MDKR_TRACE(
            "character_motion_review_ui: visible=1 context=%u scene=%u "
            "course=%u/%u samples=%u stop=overlay",
            (u32)review->context, (u32)review->scene,
            (u32)review->scene + 1u, courseCount, review->sample_count);
    }
}

static s32 workshop_preview_quantize_micrometres(
    f32 value, long long *out) {
    const double scaled = (double)value * 1000000.0;
    if (out == NULL || !isfinite(scaled) || scaled < -1000000000.0 ||
        scaled > 1000000000.0) return FALSE;
    *out = (long long)(scaled + (scaled < 0.0 ? -0.5 : 0.5));
    return TRUE;
}

static s32 workshop_preview_publish_fit_diagnostics(
    MdkrCharacterPreviewResult *result) {
    MdkrModernCharacterFitDiagnostics fit;
    long long boundsMin[3];
    long long boundsMax[3];
    long long anchor[3];
    int forward[3];
    s32 context;
    s32 axis;
    if (result == NULL ||
        result->context < MDKR_CHARACTER_PREVIEW_SELECT ||
        result->context > MDKR_CHARACTER_PREVIEW_PLANE) return FALSE;
    context = (s32)result->context -
        (s32)MDKR_CHARACTER_PREVIEW_SELECT;
    if (!mdkr_modern_character_player_fit_diagnostics(
            0, (MdkrModernCharacterContext)context, &fit)) return FALSE;
    for (axis = 0; axis < 3; ++axis) {
        const double direction = (double)fit.forward[axis] * 1000.0;
        if (fit.bounds_min[axis] > fit.bounds_max[axis] ||
            !workshop_preview_quantize_micrometres(
                fit.bounds_min[axis], &boundsMin[axis]) ||
            !workshop_preview_quantize_micrometres(
                fit.bounds_max[axis], &boundsMax[axis]) ||
            !workshop_preview_quantize_micrometres(
                fit.anchor[axis], &anchor[axis]) ||
            !isfinite(direction) || direction < -1001.0 ||
            direction > 1001.0) return FALSE;
        forward[axis] = (int)(direction +
            (direction < 0.0 ? -0.5 : 0.5));
    }
    memcpy(result->fit_bounds_min_micrometres, boundsMin,
           sizeof(boundsMin));
    memcpy(result->fit_bounds_max_micrometres, boundsMax,
           sizeof(boundsMax));
    memcpy(result->fit_anchor_micrometres, anchor, sizeof(anchor));
    memcpy(result->fit_forward_milli, forward, sizeof(forward));
    if ((fit.landmark_valid_mask &
         ~((1u << MDKR_CHARACTER_PREVIEW_LANDMARKS) - 1u)) != 0u) {
        return FALSE;
    }
    for (axis = 0; axis < MDKR_CHARACTER_PREVIEW_LANDMARKS; ++axis) {
        s32 component;
        if ((fit.landmark_valid_mask & (1u << axis)) == 0u) continue;
        for (component = 0; component < 3; ++component) {
            if (!workshop_preview_quantize_micrometres(
                    fit.landmarks[axis][component],
                    &result->fit_landmark_micrometres[axis][component])) {
                return FALSE;
            }
        }
        result->fit_landmark_mask |= 1u << axis;
    }
    result->fit_diagnostics_valid = TRUE;
    return TRUE;
}

static s32 workshop_preview_publish_camera_projection(
    MdkrCharacterPreviewResult *result) {
    MdkrModernCharacterCaptureProjection projection;
    int bounds[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    u32 boundsClipFlags = 0u;
    u32 corner;
    u32 landmark;
    if (result == NULL || !result->fit_diagnostics_valid ||
        !gfx_get_modern_character_scene_projection(&projection) ||
        projection.subject_player != 0u ||
        projection.output_width != result->render_width ||
        projection.output_height != result->render_height) return FALSE;
    for (corner = 0u;
         corner < MDKR_CHARACTER_PREVIEW_PROJECTION_BOUNDS_POINTS; ++corner) {
        f32 point[3];
        int32_t pixel[2];
        int32_t depth;
        u32 clipFlags;
        u32 axis;
        for (axis = 0u; axis < 3u; ++axis) {
            const long long value = (corner & (1u << axis)) != 0u
                ? result->fit_bounds_max_micrometres[axis]
                : result->fit_bounds_min_micrometres[axis];
            point[axis] = (f32)((double)value / 1000000.0);
        }
        if (!mdkr_modern_character_capture_project_point(
                &projection, point, pixel, &depth, &clipFlags)) return FALSE;
        if (pixel[0] < bounds[0]) bounds[0] = pixel[0];
        if (pixel[1] < bounds[1]) bounds[1] = pixel[1];
        if (pixel[0] > bounds[2]) bounds[2] = pixel[0];
        if (pixel[1] > bounds[3]) bounds[3] = pixel[1];
        boundsClipFlags |= clipFlags;
    }
    if (bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) return FALSE;
    for (landmark = 0u;
         landmark < MDKR_CHARACTER_PREVIEW_LANDMARKS; ++landmark) {
        f32 point[3];
        int32_t pixel[2];
        int32_t depth;
        u32 clipFlags;
        u32 axis;
        if ((result->fit_landmark_mask & (1u << landmark)) == 0u) continue;
        for (axis = 0u; axis < 3u; ++axis) {
            point[axis] = (f32)((double)
                result->fit_landmark_micrometres[landmark][axis] /
                1000000.0);
        }
        if (!mdkr_modern_character_capture_project_point(
                &projection, point, pixel, &depth, &clipFlags)) return FALSE;
        result->camera_landmark_pixel_milli[landmark][0] = pixel[0];
        result->camera_landmark_pixel_milli[landmark][1] = pixel[1];
        result->camera_landmark_depth_millionths[landmark] = depth;
        result->camera_landmark_clip_flags[landmark] = clipFlags;
    }
    result->camera_projection_width = projection.output_width;
    result->camera_projection_height = projection.output_height;
    result->camera_projection_primitive_draws = projection.primitive_draws;
    memcpy(result->camera_projection_viewport, projection.viewport,
           sizeof(result->camera_projection_viewport));
    memcpy(result->camera_projection_scissor, projection.scissor,
           sizeof(result->camera_projection_scissor));
    memcpy(result->camera_bounds_pixel_milli, bounds, sizeof(bounds));
    result->camera_bounds_clip_flags = boundsClipFlags;
    result->camera_projection_valid = TRUE;
    return TRUE;
}

static s32 workshop_preview_publish_contact_diagnostics(
    MdkrCharacterPreviewResult *result) {
    MdkrModernCharacterContactDiagnostics contacts;
    s32 context;
    u32 contact;
    u32 axis;
    if (result == NULL ||
        result->context < MDKR_CHARACTER_PREVIEW_CAR ||
        result->context > MDKR_CHARACTER_PREVIEW_PLANE) return FALSE;
    context = (s32)result->context -
        (s32)MDKR_CHARACTER_PREVIEW_SELECT;
    if (!mdkr_modern_character_player_contact_diagnostics(
            0, (MdkrModernCharacterContext)context, &contacts) ||
        contacts.valid_mask !=
            ((1u << MDKR_MODERN_CHARACTER_CONTACTS) - 1u)) return FALSE;
    for (contact = 0u; contact < MDKR_MODERN_CHARACTER_CONTACTS; contact++) {
        const double errorMicrometres =
            (double)contacts.error[contact] * 1000000.0;
        if (!isfinite(errorMicrometres) || errorMicrometres < 0.0 ||
            errorMicrometres > 1000000000.0) return FALSE;
        for (axis = 0u; axis < 3u; axis++) {
            if (!workshop_preview_quantize_micrometres(
                    contacts.chain_root[contact][axis],
                    &result->contact_chain_root_micrometres[contact][axis]) ||
                !workshop_preview_quantize_micrometres(
                    contacts.bend[contact][axis],
                    &result->contact_bend_micrometres[contact][axis]) ||
                !workshop_preview_quantize_micrometres(
                    contacts.target[contact][axis],
                    &result->contact_target_micrometres[contact][axis]) ||
                !workshop_preview_quantize_micrometres(
                    contacts.end[contact][axis],
                    &result->contact_end_micrometres[contact][axis])) {
                return FALSE;
            }
        }
        result->contact_witness_error_micrometres[contact] =
            (u64)(errorMicrometres + 0.5);
        result->contact_witness_mask |= 1u << contact;
    }
    return TRUE;
}

static s32 workshop_preview_publish_joint_diagnostics(
    MdkrCharacterPreviewResult *result) {
    MdkrModernCharacterJointDiagnostics joints;
    unsigned measured[MDKR_CHARACTER_PREVIEW_JOINTS];
    s32 context;
    u32 role;
    if (result == NULL ||
        result->context < MDKR_CHARACTER_PREVIEW_SELECT ||
        result->context > MDKR_CHARACTER_PREVIEW_PLANE) return FALSE;
    context = (s32)result->context -
        (s32)MDKR_CHARACTER_PREVIEW_SELECT;
    if (!mdkr_modern_character_player_joint_diagnostics(
            0, (MdkrModernCharacterContext)context, &joints) ||
        (joints.valid_mask != 0u &&
         joints.valid_mask !=
            ((1u << MDKR_CHARACTER_PREVIEW_JOINTS) - 1u)) ||
        (joints.constraint_clamped_mask &
         ~((1u << MDKR_CHARACTER_PREVIEW_JOINTS) - 1u)) != 0u ||
        (joints.constraint_clamped_mask & ~joints.valid_mask) != 0u ||
        joints.secondary_chain_count > 8u ||
        joints.secondary_joint_count > 64u ||
        joints.secondary_chain_count > joints.secondary_joint_count ||
        ((joints.secondary_chain_count == 0u) !=
         (joints.secondary_joint_count == 0u)) ||
        joints.secondary_active_joint_count > joints.secondary_joint_count ||
        !isfinite(joints.secondary_max_deflection_degrees) ||
        joints.secondary_max_deflection_degrees < 0.0f ||
        joints.secondary_max_deflection_degrees > 90.0005f ||
        (joints.secondary_joint_count == 0u &&
         (joints.secondary_active_joint_count != 0u ||
          joints.secondary_max_deflection_degrees != 0.0f ||
          joints.secondary_discontinuity_resets != 0u))) return FALSE;
    memset(measured, 0, sizeof(measured));
    if (joints.valid_mask != 0u) {
        for (role = 0u; role < MDKR_CHARACTER_PREVIEW_JOINTS; ++role) {
            const double millidegrees =
                (double)joints.excursion_degrees[role] * 1000.0;
            if (!isfinite(millidegrees) || millidegrees < 0.0 ||
                millidegrees > 180000.5) return FALSE;
            measured[role] = (u32)(millidegrees + 0.5);
            if (measured[role] > 180000u) measured[role] = 180000u;
        }
    }
    memcpy(result->joint_excursion_millidegrees, measured,
           sizeof(measured));
    result->joint_excursion_mask = joints.valid_mask;
    result->constraint_clamped_mask = joints.constraint_clamped_mask;
    result->secondary_chain_count = joints.secondary_chain_count;
    result->secondary_joint_count = joints.secondary_joint_count;
    result->secondary_active_joint_count =
        joints.secondary_active_joint_count;
    result->secondary_max_deflection_millidegrees = (u32)(
        (double)joints.secondary_max_deflection_degrees * 1000.0 + 0.5);
    if (result->secondary_max_deflection_millidegrees > 90000u) {
        result->secondary_max_deflection_millidegrees = 90000u;
    }
    result->secondary_discontinuity_resets =
        joints.secondary_discontinuity_resets;
    return TRUE;
}

static s32 workshop_preview_publish_vehicle_surface_diagnostics(
    MdkrCharacterPreviewResult *result) {
    MdkrModernSurfaceIntersectionDiagnostics diagnostics;
    s32 context;
    u32 axis;
    if (result == NULL ||
        result->context < MDKR_CHARACTER_PREVIEW_CAR ||
        result->context > MDKR_CHARACTER_PREVIEW_PLANE) return FALSE;
    context = (s32)result->context -
        (s32)MDKR_CHARACTER_PREVIEW_SELECT;
    if (!mdkr_modern_character_player_surface_diagnostics(
            0, (MdkrModernCharacterContext)context, &diagnostics) ||
        diagnostics.shell_triangles_tested == 0u ||
        diagnostics.subject_triangles_tested == 0u ||
        diagnostics.shell_triangles_tested >
            diagnostics.shell_triangles_submitted ||
        diagnostics.subject_triangles_tested >
            diagnostics.subject_triangles_submitted ||
        diagnostics.crossing_subject_triangles >
            diagnostics.subject_triangles_tested ||
        diagnostics.crossing_pairs <
            diagnostics.crossing_subject_triangles) return FALSE;
    if (diagnostics.crossing_pairs == 0u) {
        for (axis = 0u; axis < 3u; ++axis) {
            if (diagnostics.first_crossing_subject_center[axis] != 0.0f) {
                return FALSE;
            }
        }
    } else {
        for (axis = 0u; axis < 3u; ++axis) {
            if (!workshop_preview_quantize_micrometres(
                    diagnostics.first_crossing_subject_center[axis],
                    &result->vehicle_surface_first_crossing_micrometres[axis])) {
                return FALSE;
            }
        }
    }
    if (diagnostics.containment_samples_tested >
            MDKR_MODERN_CHARACTER_CONTAINMENT_SAMPLE_MAX ||
        diagnostics.containment_inside_samples +
                diagnostics.containment_boundary_samples +
                diagnostics.containment_outside_samples !=
            diagnostics.containment_samples_tested) return FALSE;
    if (diagnostics.containment_qualified) {
        double maximumDepthMicrometres;
        if (diagnostics.shell_boundary_edges != 0u ||
            diagnostics.shell_nonmanifold_edges != 0u ||
            diagnostics.shell_orientation_mismatch_edges != 0u ||
            diagnostics.shell_self_intersection_pairs != 0u ||
            diagnostics.shell_triangles_submitted !=
                diagnostics.shell_triangles_tested ||
            diagnostics.containment_samples_tested == 0u) return FALSE;
        maximumDepthMicrometres =
            (double)diagnostics.containment_maximum_inside_depth * 1000000.0;
        if (!isfinite(maximumDepthMicrometres) ||
            maximumDepthMicrometres < 0.0 ||
            maximumDepthMicrometres > 1000000000.0) return FALSE;
        if (diagnostics.containment_inside_samples == 0u) {
            u32 pointAxis;
            if (diagnostics.containment_maximum_inside_depth != 0.0f) {
                return FALSE;
            }
            for (pointAxis = 0u; pointAxis < 3u; ++pointAxis) {
                if (diagnostics.containment_deepest_subject_point[pointAxis] !=
                    0.0f) return FALSE;
            }
        } else {
            u32 pointAxis;
            if (diagnostics.containment_maximum_inside_depth <= 0.0f) {
                return FALSE;
            }
            for (pointAxis = 0u; pointAxis < 3u; ++pointAxis) {
                if (!workshop_preview_quantize_micrometres(
                        diagnostics.containment_deepest_subject_point[pointAxis],
                        &result->vehicle_containment_deepest_micrometres
                            [pointAxis])) return FALSE;
            }
        }
        result->vehicle_containment_maximum_depth_micrometres =
            (u64)(maximumDepthMicrometres + 0.5);
        result->vehicle_volume_qualified = TRUE;
    } else if (diagnostics.containment_samples_tested != 0u ||
               diagnostics.containment_inside_samples != 0u ||
               diagnostics.containment_boundary_samples != 0u ||
               diagnostics.containment_outside_samples != 0u ||
               diagnostics.containment_maximum_inside_depth != 0.0f) {
        return FALSE;
    }
    result->vehicle_shell_triangles_submitted =
        diagnostics.shell_triangles_submitted;
    result->vehicle_shell_triangles_tested =
        diagnostics.shell_triangles_tested;
    result->character_surface_triangles_submitted =
        diagnostics.subject_triangles_submitted;
    result->character_surface_triangles_tested =
        diagnostics.subject_triangles_tested;
    result->vehicle_surface_crossing_triangles =
        diagnostics.crossing_subject_triangles;
    result->vehicle_surface_crossing_pairs = diagnostics.crossing_pairs;
    result->vehicle_shell_boundary_edges = diagnostics.shell_boundary_edges;
    result->vehicle_shell_nonmanifold_edges =
        diagnostics.shell_nonmanifold_edges;
    result->vehicle_shell_orientation_mismatch_edges =
        diagnostics.shell_orientation_mismatch_edges;
    result->vehicle_shell_self_intersection_pairs =
        diagnostics.shell_self_intersection_pairs;
    result->vehicle_containment_samples_tested =
        diagnostics.containment_samples_tested;
    result->vehicle_containment_inside_samples =
        diagnostics.containment_inside_samples;
    result->vehicle_containment_boundary_samples =
        diagnostics.containment_boundary_samples;
    result->vehicle_containment_outside_samples =
        diagnostics.containment_outside_samples;
    result->vehicle_surface_valid = TRUE;
    return TRUE;
}

static s32 workshop_preview_publish_opaque_visibility(
    MdkrCharacterPreviewResult *result) {
    MdkrModernCharacterVisibilityDiagnostics diagnostics;
    u32 component;
    u32 isolatedTileCount = 0u;
    u32 sceneTileCount = 0u;
    u32 occluder;
    u64 classifiedDraws;
    u64 mask;
    if (result == NULL ||
        !platform_modern_character_visibility_diagnostics(&diagnostics) ||
        diagnostics.version != MDKR_MODERN_CHARACTER_VISIBILITY_VERSION ||
        diagnostics.valid != 1u || diagnostics.qualified > 1u ||
        diagnostics.output_width == 0u ||
        diagnostics.output_height == 0u ||
        diagnostics.output_width > 16384u ||
        diagnostics.output_height > 16384u ||
        diagnostics.primitive_draws == 0u) return FALSE;
    classifiedDraws = (u64)diagnostics.opaque_draws +
        diagnostics.masked_draws + diagnostics.transparent_draws;
    mask = diagnostics.isolated_tile_mask;
    while (mask != 0u) {
        isolatedTileCount += (u32)(mask & 1u);
        mask >>= 1u;
    }
    mask = diagnostics.scene_tile_mask;
    while (mask != 0u) {
        sceneTileCount += (u32)(mask & 1u);
        mask >>= 1u;
    }
    if ((diagnostics.occluder_present_mask & ~0x7u) != 0u ||
        (diagnostics.occluder_qualified_mask &
         ~diagnostics.occluder_present_mask) != 0u) return FALSE;
    for (occluder = 0u;
         occluder < MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES;
         ++occluder) {
        u32 overlapTileCount = 0u;
        const u32 bit = 1u << occluder;
        mask = diagnostics.occluder_overlap_tile_mask[occluder];
        while (mask != 0u) {
            overlapTileCount += (u32)(mask & 1u);
            mask >>= 1u;
        }
        if (diagnostics.occluder_unqualified_draws[occluder] >
                diagnostics.occluder_draws[occluder] ||
            ((diagnostics.occluder_present_mask & bit) != 0u) !=
                (diagnostics.occluder_draws[occluder] != 0u) ||
            overlapTileCount !=
                diagnostics.occluder_overlap_tiles[occluder] ||
            diagnostics.occluder_overlap_tiles[occluder] > 64u ||
            (diagnostics.occluder_overlap_tile_mask[occluder] &
             ~diagnostics.isolated_tile_mask) != 0u ||
            ((diagnostics.occluder_qualified_mask & bit) != 0u
                 ? diagnostics.occluder_draws[occluder] == 0u ||
                       diagnostics.occluder_unqualified_draws[occluder] != 0u
                 : diagnostics.occluder_overlap_tile_mask[occluder] != 0u)) {
            return FALSE;
        }
    }
    if (classifiedDraws != diagnostics.primitive_draws ||
        diagnostics.grid_columns != 8u || diagnostics.grid_rows != 8u ||
        diagnostics.isolated_visible_tiles > 64u ||
        diagnostics.scene_visible_tiles >
            diagnostics.isolated_visible_tiles ||
        isolatedTileCount != diagnostics.isolated_visible_tiles ||
        sceneTileCount != diagnostics.scene_visible_tiles ||
        (diagnostics.scene_tile_mask &
         ~diagnostics.isolated_tile_mask) != 0u ||
        (diagnostics.qualified
             ? diagnostics.transparent_draws != 0u
             : diagnostics.transparent_draws == 0u ||
                   diagnostics.occluder_qualified_mask != 0u ||
                   diagnostics.isolated_visible_tiles != 0u ||
                   diagnostics.scene_visible_tiles != 0u ||
                   diagnostics.isolated_tile_mask != 0u ||
                   diagnostics.scene_tile_mask != 0u)) {
        return FALSE;
    }
    for (component = 0u; component < 4u; ++component) {
        const s32 limit = (component & 1u)
            ? (s32)diagnostics.output_height
            : (s32)diagnostics.output_width;
        if (diagnostics.viewport[component] < 0 ||
            diagnostics.scissor[component] < 0 ||
            diagnostics.viewport[component] > limit ||
            diagnostics.scissor[component] > limit) return FALSE;
        result->opaque_visibility_viewport[component] =
            diagnostics.viewport[component];
        result->opaque_visibility_scissor[component] =
            diagnostics.scissor[component];
    }
    if (diagnostics.viewport[2] == 0 || diagnostics.viewport[3] == 0 ||
        diagnostics.scissor[2] == 0 || diagnostics.scissor[3] == 0 ||
        diagnostics.viewport[0] + diagnostics.viewport[2] >
            (s32)diagnostics.output_width ||
        diagnostics.viewport[1] + diagnostics.viewport[3] >
            (s32)diagnostics.output_height ||
        diagnostics.scissor[0] + diagnostics.scissor[2] >
            (s32)diagnostics.output_width ||
        diagnostics.scissor[1] + diagnostics.scissor[3] >
            (s32)diagnostics.output_height) return FALSE;
    result->opaque_visibility_valid = TRUE;
    result->opaque_visibility_qualified = diagnostics.qualified != 0u;
    result->opaque_visibility_width = diagnostics.output_width;
    result->opaque_visibility_height = diagnostics.output_height;
    result->opaque_visibility_primitive_draws =
        diagnostics.primitive_draws;
    result->opaque_visibility_opaque_draws = diagnostics.opaque_draws;
    result->opaque_visibility_masked_draws = diagnostics.masked_draws;
    result->opaque_visibility_transparent_draws =
        diagnostics.transparent_draws;
    result->opaque_visibility_grid_columns = diagnostics.grid_columns;
    result->opaque_visibility_grid_rows = diagnostics.grid_rows;
    result->opaque_visibility_isolated_tiles =
        diagnostics.isolated_visible_tiles;
    result->opaque_visibility_scene_tiles = diagnostics.scene_visible_tiles;
    result->opaque_visibility_isolated_tile_mask =
        diagnostics.isolated_tile_mask;
    result->opaque_visibility_scene_tile_mask = diagnostics.scene_tile_mask;
    result->opaque_visibility_occluder_present_mask =
        diagnostics.occluder_present_mask;
    result->opaque_visibility_occluder_qualified_mask =
        diagnostics.occluder_qualified_mask;
    for (occluder = 0u;
         occluder < MDKR_MODERN_CHARACTER_OCCLUDER_CLASSES;
         ++occluder) {
        result->opaque_visibility_occluder_draws[occluder] =
            diagnostics.occluder_draws[occluder];
        result->opaque_visibility_occluder_unqualified_draws[occluder] =
            diagnostics.occluder_unqualified_draws[occluder];
        result->opaque_visibility_occluder_overlap_tiles[occluder] =
            diagnostics.occluder_overlap_tiles[occluder];
        result->opaque_visibility_occluder_overlap_tile_mask[occluder] =
            diagnostics.occluder_overlap_tile_mask[occluder];
    }
    return TRUE;
}

static void workshop_motion_review_begin_sample(void) {
    MdkrCharacterMotionReviewResult *review =
        g_mdkrCharacterMotionReviewResult;
    const WorkshopMotionReviewDefinition *definition;
    MdkrCharacterPreviewResult *sample;
    char error[192] = {0};
    if (review == NULL ||
        sWorkshopMotionReviewSample >= review->sample_count) return;
    definition = workshop_motion_review_definition(
        review->context, sWorkshopMotionReviewSample);
    if (definition == NULL) return;
    sample = &review->samples[sWorkshopMotionReviewSample];
    bzero(sample, sizeof(*sample));
    sample->version = MDKR_CHARACTER_PREVIEW_RESULT_VERSION;
    sample->started = TRUE;
    sample->warmup_complete = TRUE;
    sample->context = review->context;
    sample->players = 1;
    sample->pose = definition->pose;
    sample->pose_phase_milli = definition->phase_milli;
    sample->lighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    sample->capture_kind = MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
    sample->gpu_timing.version = MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION;
    sample->gpu_timing.status =
        MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED;
    if (!mdkr_modern_character_set_inspection_pose(
            definition->semantic,
            (f32)definition->phase_milli / 1000.0f,
            error, sizeof(error))) {
        review->failed_sample = sWorkshopMotionReviewSample + 1u;
        MDKR_TRACE(
            "character_motion_review: rejected sample=%u semantic=%s error=%s",
            sWorkshopMotionReviewSample, definition->semantic,
            error[0] != '\0' ? error : "invalid pose");
        platform_request_exit(0);
        return;
    }
    mdkr_modern_character_runtime_metrics(
        &sWorkshopMotionReviewPoseBaseline);
    sWorkshopMotionReviewReplacementBaseline =
        sWorkshopMotionReviewPoseBaseline.replacement_draws;
    sWorkshopMotionReviewPoseSettled = FALSE;
    sWorkshopMotionReviewStageTicks = 0u;
    sWorkshopMotionReviewVisibilityAttempts = 0u;
    sWorkshopMotionReviewDiagnosticsRequested = FALSE;
    MDKR_TRACE(
        "character_motion_review: settling sample=%u semantic=%s phase=%u",
        sWorkshopMotionReviewSample, definition->semantic,
        definition->phase_milli);
}

static s32 workshop_motion_review_publish_sample(void) {
    MdkrCharacterMotionReviewResult *review =
        g_mdkrCharacterMotionReviewResult;
    MdkrCharacterPreviewResult *sample;
    MdkrModernCharacterRuntimeMetrics metrics;
    u32 contact;
    u32 joint;
    u32 joint_max_millidegrees = 0u;
    if (review == NULL ||
        sWorkshopMotionReviewSample >= review->sample_count) return FALSE;
    sample = &review->samples[sWorkshopMotionReviewSample];
    mdkr_modern_character_runtime_metrics(&metrics);
    sample->output_width = gfx_output_dimensions.width;
    sample->output_height = gfx_output_dimensions.height;
    sample->render_width = gfx_current_dimensions.width;
    sample->render_height = gfx_current_dimensions.height;
    sample->replacement_draws = metrics.replacement_draws >=
            sWorkshopMotionReviewPoseBaseline.replacement_draws
        ? metrics.replacement_draws -
              sWorkshopMotionReviewPoseBaseline.replacement_draws
        : 0u;
    sample->replacement_primitives = metrics.replacement_primitives >=
            sWorkshopMotionReviewPoseBaseline.replacement_primitives
        ? metrics.replacement_primitives -
              sWorkshopMotionReviewPoseBaseline.replacement_primitives
        : 0u;
    sample->hidden_donor_batches = metrics.hidden_donor_batches >=
            sWorkshopMotionReviewPoseBaseline.hidden_donor_batches
        ? metrics.hidden_donor_batches -
              sWorkshopMotionReviewPoseBaseline.hidden_donor_batches
        : 0u;
    sample->contact_solves = metrics.contact_solves >=
            sWorkshopMotionReviewPoseBaseline.contact_solves
        ? metrics.contact_solves -
              sWorkshopMotionReviewPoseBaseline.contact_solves
        : 0u;
    for (contact = 0u; contact < MDKR_CHARACTER_PREVIEW_CONTACTS; ++contact) {
        review->contact_stability_observations
            [sWorkshopMotionReviewSample][contact] =
                metrics.contact_residual_step_observations[contact];
        review->contact_stability_max_step_micrometres
            [sWorkshopMotionReviewSample][contact] =
                metrics.contact_residual_step_max_micrometres[contact];
        if (metrics.contact_residual_step_observations[contact] >=
            MDKR_CHARACTER_CONTACT_STABILITY_MINIMUM_OBSERVATIONS) {
            review->contact_stability_mask[sWorkshopMotionReviewSample] |=
                1u << contact;
        }
    }
    sample->inspection_pose_ticks = metrics.inspection_pose_ticks >=
            sWorkshopMotionReviewPoseBaseline.inspection_pose_ticks
        ? metrics.inspection_pose_ticks -
              sWorkshopMotionReviewPoseBaseline.inspection_pose_ticks
        : 0u;
    sample->inspection_pose_fallback_ticks =
        metrics.inspection_pose_fallback_ticks >=
                sWorkshopMotionReviewPoseBaseline
                    .inspection_pose_fallback_ticks
            ? metrics.inspection_pose_fallback_ticks -
                  sWorkshopMotionReviewPoseBaseline
                      .inspection_pose_fallback_ticks
            : 0u;
    sample->transition_from_motion_source =
        (MdkrCharacterPreviewMotionSource)
            metrics.inspection_from_motion_source;
    if (!workshop_preview_publish_fit_diagnostics(sample) ||
        !workshop_preview_publish_camera_projection(sample) ||
        (review->context != MDKR_CHARACTER_PREVIEW_SELECT &&
         !workshop_preview_publish_vehicle_surface_diagnostics(sample)) ||
        !workshop_preview_publish_opaque_visibility(sample)) {
        return FALSE;
    }
    (void)workshop_preview_publish_joint_diagnostics(sample);
    for (joint = 0u; joint < MDKR_CHARACTER_PREVIEW_JOINTS; ++joint) {
        if (sample->joint_excursion_millidegrees[joint] >
            joint_max_millidegrees) {
            joint_max_millidegrees =
                sample->joint_excursion_millidegrees[joint];
        }
    }
    if (review->context != MDKR_CHARACTER_PREVIEW_SELECT &&
        !workshop_preview_publish_contact_diagnostics(sample) &&
        sample->contact_solves != 0u) return FALSE;
    if (review->context != MDKR_CHARACTER_PREVIEW_SELECT &&
        sample->contact_solves != 0u &&
        review->contact_stability_mask[sWorkshopMotionReviewSample] !=
            ((1u << MDKR_CHARACTER_PREVIEW_CONTACTS) - 1u)) return FALSE;
    for (contact = 0u; contact < MDKR_CHARACTER_PREVIEW_CONTACTS; ++contact) {
        if (sample->contact_witness_error_micrometres[contact] >
            sample->contact_error_max_micrometres) {
            sample->contact_error_max_micrometres =
                sample->contact_witness_error_micrometres[contact];
        }
    }
    review->completed_mask |= 1u << sWorkshopMotionReviewSample;
    MDKR_TRACE(
        "character_motion_review: sample=%u pose=%d phase=%u draws=%llu source=%d fallback=%llu cameraFlags=%x crossings=%u inside=%u visibility=%u/%u contactSolves=%llu contactMask=%x contactMaxUm=%llu contactStabilityMask=%x contactSteps=%llu,%llu,%llu,%llu contactStepMaxUm=%llu,%llu,%llu,%llu joints=%x jointMaxMd=%u",
        sWorkshopMotionReviewSample, (int)sample->pose,
        sample->pose_phase_milli, sample->replacement_draws,
        (int)sample->transition_from_motion_source,
        sample->inspection_pose_fallback_ticks,
        sample->camera_bounds_clip_flags,
        sample->vehicle_surface_crossing_pairs,
        sample->vehicle_containment_inside_samples,
        sample->opaque_visibility_scene_tiles,
        sample->opaque_visibility_isolated_tiles,
        sample->contact_solves,
        sample->contact_witness_mask,
        sample->contact_error_max_micrometres,
        review->contact_stability_mask[sWorkshopMotionReviewSample],
        review->contact_stability_observations
            [sWorkshopMotionReviewSample][0],
        review->contact_stability_observations
            [sWorkshopMotionReviewSample][1],
        review->contact_stability_observations
            [sWorkshopMotionReviewSample][2],
        review->contact_stability_observations
            [sWorkshopMotionReviewSample][3],
        review->contact_stability_max_step_micrometres
            [sWorkshopMotionReviewSample][0],
        review->contact_stability_max_step_micrometres
            [sWorkshopMotionReviewSample][1],
        review->contact_stability_max_step_micrometres
            [sWorkshopMotionReviewSample][2],
        review->contact_stability_max_step_micrometres
            [sWorkshopMotionReviewSample][3],
        sample->joint_excursion_mask,
        joint_max_millidegrees);
    sWorkshopMotionReviewSample++;
    if (sWorkshopMotionReviewSample == review->sample_count) {
        review->completed = TRUE;
        MDKR_TRACE(
            "character_motion_review: complete samples=%u mask=%x",
            review->sample_count,
            review->completed_mask);
        platform_request_exit(0);
    } else {
        workshop_motion_review_begin_sample();
    }
    return TRUE;
}

static void workshop_motion_review_service(void) {
    MdkrCharacterMotionReviewResult *review =
        g_mdkrCharacterMotionReviewResult;
    MdkrModernCharacterRuntimeMetrics metrics;
    MdkrModernSurfaceIntersectionDiagnostics surface;
    MdkrModernCharacterVisibilityDiagnostics visibility;
    MdkrModernCharacterContext context;
    s32 surfaceReady;
    s32 visibilityReady;
    if (review == NULL || review->completed || review->failed_sample != 0u ||
        sWorkshopMotionReviewSample >= review->sample_count) return;
    context = (MdkrModernCharacterContext)((s32)review->context -
        (s32)MDKR_CHARACTER_PREVIEW_SELECT);
    sWorkshopMotionReviewStageTicks++;
    if (sWorkshopMotionReviewStageTicks >
        WORKSHOP_MOTION_REVIEW_SAMPLE_TIMEOUT_TICKS) {
        review->failed_sample = sWorkshopMotionReviewSample + 1u;
        MDKR_TRACE(
            "character_motion_review: timeout sample=%u requested=%d visibilityPending=%d",
            sWorkshopMotionReviewSample,
            sWorkshopMotionReviewDiagnosticsRequested,
            platform_modern_character_visibility_pending());
        platform_request_exit(0);
        return;
    }
    mdkr_modern_character_runtime_metrics(&metrics);
    if (!sWorkshopMotionReviewDiagnosticsRequested) {
        if (!sWorkshopMotionReviewPoseSettled) {
            if (!mdkr_modern_character_inspection_pose_settled(0)) return;
            /* Exclude every transition draw from both the stable-frame gate and
             * the published per-state counters. */
            mdkr_modern_character_contact_metrics_reset();
            mdkr_modern_character_runtime_metrics(
                &sWorkshopMotionReviewPoseBaseline);
            sWorkshopMotionReviewReplacementBaseline =
                sWorkshopMotionReviewPoseBaseline.replacement_draws;
            sWorkshopMotionReviewPoseSettled = TRUE;
            MDKR_TRACE(
                "character_motion_review: settled sample=%u",
                sWorkshopMotionReviewSample);
            return;
        }
        if (metrics.replacement_draws <
            sWorkshopMotionReviewReplacementBaseline +
                WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS) return;
        if (review->context != MDKR_CHARACTER_PREVIEW_SELECT &&
            !mdkr_modern_character_request_surface_diagnostics(0, context)) {
            return;
        }
        if (!platform_modern_character_visibility_request_once()) return;
        sWorkshopMotionReviewVisibilityAttempts++;
        sWorkshopMotionReviewDiagnosticsRequested = TRUE;
        return;
    }
    surfaceReady = review->context == MDKR_CHARACTER_PREVIEW_SELECT ||
        mdkr_modern_character_player_surface_diagnostics(
            0, context, &surface);
    visibilityReady = platform_modern_character_visibility_diagnostics(
        &visibility);
    if (review->context != MDKR_CHARACTER_PREVIEW_SELECT && !surfaceReady &&
        !mdkr_modern_character_surface_diagnostics_requested(0, context) &&
        (sWorkshopMotionReviewStageTicks % 30u) == 0u) {
        (void)mdkr_modern_character_request_surface_diagnostics(0, context);
    }
    if (!visibilityReady &&
        !platform_modern_character_visibility_pending() &&
        sWorkshopMotionReviewVisibilityAttempts < 3u &&
        platform_modern_character_visibility_request_once()) {
        sWorkshopMotionReviewVisibilityAttempts++;
    }
    if (!surfaceReady || !visibilityReady) return;
    (void)workshop_motion_review_publish_sample();
}

static void workshop_preview_measurement_finish(void) {
    MdkrPresentPerfSnapshot present;
    MdkrModernCharacterRuntimeMetrics character;
    MdkrWorkshopPreviewVisualMetrics visual;
    MdkrCharacterPreviewResult *result = g_mdkrCharacterPreviewResult;
    const MdkrGpuInfo *gpu;
    if (result == NULL || sWorkshopPreviewMeasurementFinished) return;
    /* Stop admission and publish only readbacks already completed. This call
     * is nonblocking; pending frames remain explicit in the result instead of
     * stalling audio/input or being guessed from wall cadence. */
    gfx_finish_modern_character_gpu_timing(&result->gpu_timing);
    result->warmup_ticks = sWorkshopPreviewWarmupTicks;
    result->realtime = platform_pace_is_synthetic() ? FALSE : TRUE;
    gpu = mdkr_gpu_info_get();
    (void)snprintf(result->renderer_backend,
                   sizeof(result->renderer_backend), "%s",
                   mdkr_render_backend_name());
    if (gpu != NULL && gpu->selected >= 0 && gpu->selected < gpu->count) {
        const MdkrGpuCandidate *selected = &gpu->candidates[gpu->selected];
        (void)snprintf(result->renderer_backend,
                       sizeof(result->renderer_backend), "%s",
                       selected->backend);
        (void)snprintf(result->adapter, sizeof(result->adapter), "%s",
                       selected->adapter);
        (void)snprintf(result->driver, sizeof(result->driver), "%s",
                       selected->driver);
        result->vendor_id = selected->vendor_id;
        result->device_id = selected->device_id;
    }
    result->output_width = gfx_output_dimensions.width;
    result->output_height = gfx_output_dimensions.height;
    result->render_width = gfx_current_dimensions.width;
    result->render_height = gfx_current_dimensions.height;
    if (sWorkshopPreviewMeasurementStarted) {
        present_perf_snapshot(&present);
        mdkr_modern_character_runtime_metrics(&character);
        result->interval_samples = present.interval_samples;
        result->displayed_frames = present.displayed_frames;
        result->interval_p50_us = present.interval_p50_us;
        result->interval_p95_us = present.interval_p95_us;
        result->interval_p99_us = present.interval_p99_us;
        result->interval_mean_us = present.interval_mean_us;
        result->interval_max_us = present.interval_max_us;
        result->tickwall_samples = present.tickwall_samples;
        result->tickwall_mean_ns = present.tickwall_mean_ns;
        result->replacement_draws = character.replacement_draws >=
                sWorkshopPreviewCharacterBaseline.replacement_draws
            ? character.replacement_draws -
                  sWorkshopPreviewCharacterBaseline.replacement_draws
            : 0u;
        result->replacement_primitives = character.replacement_primitives >=
                sWorkshopPreviewCharacterBaseline.replacement_primitives
            ? character.replacement_primitives -
                  sWorkshopPreviewCharacterBaseline.replacement_primitives
            : 0u;
        result->reference_draws = character.reference_draws >=
                sWorkshopPreviewCharacterBaseline.reference_draws
            ? character.reference_draws -
                  sWorkshopPreviewCharacterBaseline.reference_draws
            : 0u;
        result->reference_primitives = character.reference_primitives >=
                sWorkshopPreviewCharacterBaseline.reference_primitives
            ? character.reference_primitives -
                  sWorkshopPreviewCharacterBaseline.reference_primitives
            : 0u;
        result->hidden_donor_batches = character.hidden_donor_batches >=
                sWorkshopPreviewCharacterBaseline.hidden_donor_batches
            ? character.hidden_donor_batches -
                  sWorkshopPreviewCharacterBaseline.hidden_donor_batches
            : 0u;
        result->contact_solves = character.contact_solves >=
                sWorkshopPreviewCharacterBaseline.contact_solves
            ? character.contact_solves -
                  sWorkshopPreviewCharacterBaseline.contact_solves
            : 0u;
        if (result->contact_solves != 0u &&
            character.contact_error_micrometres_sum >=
                sWorkshopPreviewCharacterBaseline
                    .contact_error_micrometres_sum) {
            result->contact_error_mean_micrometres =
                (character.contact_error_micrometres_sum -
                 sWorkshopPreviewCharacterBaseline
                     .contact_error_micrometres_sum) /
                result->contact_solves;
        }
        result->contact_error_max_micrometres =
            character.contact_error_micrometres_max;
        result->inspection_pose_ticks = character.inspection_pose_ticks >=
                sWorkshopPreviewCharacterBaseline.inspection_pose_ticks
            ? character.inspection_pose_ticks -
                  sWorkshopPreviewCharacterBaseline.inspection_pose_ticks
            : 0u;
        result->inspection_pose_fallback_ticks =
            character.inspection_pose_fallback_ticks >=
                    sWorkshopPreviewCharacterBaseline
                        .inspection_pose_fallback_ticks
                ? character.inspection_pose_fallback_ticks -
                      sWorkshopPreviewCharacterBaseline
                          .inspection_pose_fallback_ticks
                : 0u;
        result->inspection_transition_switches =
            character.inspection_transition_switches >=
                    sWorkshopPreviewCharacterBaseline
                        .inspection_transition_switches
                ? character.inspection_transition_switches -
                      sWorkshopPreviewCharacterBaseline
                          .inspection_transition_switches
                : 0u;
        result->inspection_transition_blending_ticks =
            character.inspection_transition_blending_ticks >=
                    sWorkshopPreviewCharacterBaseline
                        .inspection_transition_blending_ticks
                ? character.inspection_transition_blending_ticks -
                      sWorkshopPreviewCharacterBaseline
                          .inspection_transition_blending_ticks
                : 0u;
        result->inspection_transition_completions =
            character.inspection_transition_completions >=
                    sWorkshopPreviewCharacterBaseline
                        .inspection_transition_completions
                ? character.inspection_transition_completions -
                      sWorkshopPreviewCharacterBaseline
                          .inspection_transition_completions
                : 0u;
        result->transition_from_blend_milli =
            character.inspection_from_blend_milliseconds;
        result->transition_to_blend_milli =
            character.inspection_to_blend_milliseconds;
        result->transition_from_motion_source =
            (MdkrCharacterPreviewMotionSource)
                character.inspection_from_motion_source;
        result->transition_to_motion_source =
            (MdkrCharacterPreviewMotionSource)
                character.inspection_to_motion_source;
        if (result->replacement_draws != 0u) {
            if (workshop_preview_publish_fit_diagnostics(result)) {
                (void)workshop_preview_publish_camera_projection(result);
            }
            (void)workshop_preview_publish_joint_diagnostics(result);
            (void)workshop_preview_publish_contact_diagnostics(result);
            (void)workshop_preview_publish_vehicle_surface_diagnostics(
                result);
            (void)workshop_preview_publish_opaque_visibility(result);
        } else if (result->donor_reference &&
                   result->reference_draws != 0u) {
            /* Reference-only commands reached the backend without drawing.
             * Publish their exact fitted volume and camera projection so the
             * launcher can register a donor image against a custom still. */
            if (workshop_preview_publish_fit_diagnostics(result)) {
                (void)workshop_preview_publish_camera_projection(result);
            }
        }
        mdkr_workshop_preview_visual_metrics(&visual);
        result->donor_reference_batches =
            visual.donor_reference_batches >=
                    sWorkshopPreviewVisualBaseline.donor_reference_batches
                ? visual.donor_reference_batches -
                      sWorkshopPreviewVisualBaseline.donor_reference_batches
                : 0u;
        result->camera_override_ticks = visual.camera_override_ticks >=
                sWorkshopPreviewVisualBaseline.camera_override_ticks
            ? visual.camera_override_ticks -
                  sWorkshopPreviewVisualBaseline.camera_override_ticks
            : 0u;
        result->lighting_override_draws = visual.lighting_override_draws >=
                sWorkshopPreviewVisualBaseline.lighting_override_draws
            ? visual.lighting_override_draws -
                  sWorkshopPreviewVisualBaseline.lighting_override_draws
            : 0u;
    }
    sWorkshopPreviewMeasurementFinished = TRUE;
    MDKR_TRACE(
        "character_workshop_result: warmup=%d realtime=%d samples=%llu "
        "p50us=%llu p95us=%llu p99us=%llu maxus=%llu replacements=%llu "
        "reference=%llu/%llu donorReference=%d/%llu "
        "contacts=%llu contactMaxUm=%llu contactWitness=%x "
        "contactWitnessErrorUm=%llu,%llu,%llu,%llu "
        "contactLHUm=root:%lld,%lld,%lld bend:%lld,%lld,%lld "
        "target:%lld,%lld,%lld end:%lld,%lld,%lld fit=%d "
        "fitAnchorUm=%lld,%lld,%lld fitBoundsYUm=%lld,%lld "
        "fitForwardMilli=%d,%d,%d fitLandmarks=%x "
        "headUm=%lld,%lld,%lld cameraFit=%d "
        "cameraBoundsMilli=%d,%d,%d,%d/%x "
        "cameraViewport=%d,%d,%d,%d cameraHeadMilli=%d,%d,%d/%x "
        "surface=%d shell=%u/%u subject=%u/%u crossings=%u/%u "
        "crossingUm=%lld,%lld,%lld "
        "volume=%d topology=%u,%u,%u,%u containment=%u,%u,%u,%u "
        "containmentDepthUm=%llu containmentPointUm=%lld,%lld,%lld "
        "visibility=%d/%d visibilitySize=%ux%u "
        "visibilityDraws=%u,%u,%u,%u visibilityGrid=%ux%u "
        "visibilityTiles=%u/%u visibilityMask=%016llx/%016llx "
        "occluders=%x/%x occluderDraws=%u,%u,%u "
        "occluderUnqualified=%u,%u,%u occluderOverlap=%u,%u,%u "
        "occluderMask=%016llx,%016llx,%016llx "
        "pose=%d phase=%u "
        "transitionFrom=%d transitionPhase=%u transition=%llu/%llu/%llu "
        "transitionBlend=%u,%u transitionSource=%d,%d "
        "poseTicks=%llu poseFallback=%llu view=%d,%d lighting=%d "
        "cameraTicks=%llu lightingDraws=%llu capture=%d/%d/%d kind=%d "
        "captureStableFrames=%llu bytes=%llu "
        "gpu=%u/%x sceneGpuNs=%llu,%llu,%llu "
        "characterGpuNs=%llu,%llu,%llu gpuExcluded=%llu,%llu,%llu "
        "backend=%s adapter=%s driver=%s "
        "vendor=%08x device=%08x output=%ux%u render=%ux%u",
        result->warmup_complete, result->realtime,
        result->interval_samples, result->interval_p50_us,
        result->interval_p95_us, result->interval_p99_us,
        result->interval_max_us, result->replacement_draws,
        result->reference_draws, result->reference_primitives,
        result->donor_reference,
        result->donor_reference_batches,
        result->contact_solves, result->contact_error_max_micrometres,
        result->contact_witness_mask,
        result->contact_witness_error_micrometres[0],
        result->contact_witness_error_micrometres[1],
        result->contact_witness_error_micrometres[2],
        result->contact_witness_error_micrometres[3],
        result->contact_chain_root_micrometres[0][0],
        result->contact_chain_root_micrometres[0][1],
        result->contact_chain_root_micrometres[0][2],
        result->contact_bend_micrometres[0][0],
        result->contact_bend_micrometres[0][1],
        result->contact_bend_micrometres[0][2],
        result->contact_target_micrometres[0][0],
        result->contact_target_micrometres[0][1],
        result->contact_target_micrometres[0][2],
        result->contact_end_micrometres[0][0],
        result->contact_end_micrometres[0][1],
        result->contact_end_micrometres[0][2],
        result->fit_diagnostics_valid,
        result->fit_anchor_micrometres[0],
        result->fit_anchor_micrometres[1],
        result->fit_anchor_micrometres[2],
        result->fit_bounds_min_micrometres[1],
        result->fit_bounds_max_micrometres[1],
        result->fit_forward_milli[0],
        result->fit_forward_milli[1],
        result->fit_forward_milli[2],
        result->fit_landmark_mask,
        result->fit_landmark_micrometres
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD][0],
        result->fit_landmark_micrometres
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD][1],
        result->fit_landmark_micrometres
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD][2],
        result->camera_projection_valid,
        result->camera_bounds_pixel_milli[0],
        result->camera_bounds_pixel_milli[1],
        result->camera_bounds_pixel_milli[2],
        result->camera_bounds_pixel_milli[3],
        result->camera_bounds_clip_flags,
        result->camera_projection_viewport[0],
        result->camera_projection_viewport[1],
        result->camera_projection_viewport[2],
        result->camera_projection_viewport[3],
        result->camera_landmark_pixel_milli
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD][0],
        result->camera_landmark_pixel_milli
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD][1],
        result->camera_landmark_depth_millionths
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD],
        result->camera_landmark_clip_flags
            [MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD],
        result->vehicle_surface_valid,
        result->vehicle_shell_triangles_tested,
        result->vehicle_shell_triangles_submitted,
        result->character_surface_triangles_tested,
        result->character_surface_triangles_submitted,
        result->vehicle_surface_crossing_triangles,
        result->vehicle_surface_crossing_pairs,
        result->vehicle_surface_first_crossing_micrometres[0],
        result->vehicle_surface_first_crossing_micrometres[1],
        result->vehicle_surface_first_crossing_micrometres[2],
        result->vehicle_volume_qualified,
        result->vehicle_shell_boundary_edges,
        result->vehicle_shell_nonmanifold_edges,
        result->vehicle_shell_orientation_mismatch_edges,
        result->vehicle_shell_self_intersection_pairs,
        result->vehicle_containment_samples_tested,
        result->vehicle_containment_inside_samples,
        result->vehicle_containment_boundary_samples,
        result->vehicle_containment_outside_samples,
        result->vehicle_containment_maximum_depth_micrometres,
        result->vehicle_containment_deepest_micrometres[0],
        result->vehicle_containment_deepest_micrometres[1],
        result->vehicle_containment_deepest_micrometres[2],
        result->opaque_visibility_valid,
        result->opaque_visibility_qualified,
        result->opaque_visibility_width,
        result->opaque_visibility_height,
        result->opaque_visibility_primitive_draws,
        result->opaque_visibility_opaque_draws,
        result->opaque_visibility_masked_draws,
        result->opaque_visibility_transparent_draws,
        result->opaque_visibility_grid_columns,
        result->opaque_visibility_grid_rows,
        result->opaque_visibility_scene_tiles,
        result->opaque_visibility_isolated_tiles,
        result->opaque_visibility_scene_tile_mask,
        result->opaque_visibility_isolated_tile_mask,
        result->opaque_visibility_occluder_present_mask,
        result->opaque_visibility_occluder_qualified_mask,
        result->opaque_visibility_occluder_draws[0],
        result->opaque_visibility_occluder_draws[1],
        result->opaque_visibility_occluder_draws[2],
        result->opaque_visibility_occluder_unqualified_draws[0],
        result->opaque_visibility_occluder_unqualified_draws[1],
        result->opaque_visibility_occluder_unqualified_draws[2],
        result->opaque_visibility_occluder_overlap_tiles[0],
        result->opaque_visibility_occluder_overlap_tiles[1],
        result->opaque_visibility_occluder_overlap_tiles[2],
        result->opaque_visibility_occluder_overlap_tile_mask[0],
        result->opaque_visibility_occluder_overlap_tile_mask[1],
        result->opaque_visibility_occluder_overlap_tile_mask[2],
        (int)result->pose, result->pose_phase_milli,
        (int)result->transition_from_pose,
        result->transition_from_phase_milli,
        result->inspection_transition_switches,
        result->inspection_transition_blending_ticks,
        result->inspection_transition_completions,
        result->transition_from_blend_milli,
        result->transition_to_blend_milli,
        (int)result->transition_from_motion_source,
        (int)result->transition_to_motion_source,
        result->inspection_pose_ticks,
        result->inspection_pose_fallback_ticks,
        result->view_yaw_degrees, result->view_pitch_degrees,
        (int)result->lighting, result->camera_override_ticks,
        result->lighting_override_draws, result->capture_requested,
        result->capture_armed, result->capture_written,
        (int)result->capture_kind,
        result->capture_stable_frames, result->capture_png_bytes,
        (unsigned)result->gpu_timing.status,
        result->gpu_timing.supported_scopes,
        (unsigned long long)result->gpu_timing.scene_pass.samples,
        (unsigned long long)result->gpu_timing.scene_pass.p50_ns,
        (unsigned long long)result->gpu_timing.scene_pass.p95_ns,
        (unsigned long long)result->gpu_timing.character_draws.samples,
        (unsigned long long)result->gpu_timing.character_draws.p50_ns,
        (unsigned long long)result->gpu_timing.character_draws.p95_ns,
        (unsigned long long)result->gpu_timing.pending_frames,
        (unsigned long long)result->gpu_timing.ring_full_frames,
        (unsigned long long)result->gpu_timing.invalid_samples,
        result->renderer_backend,
        result->adapter[0] != '\0' ? result->adapter : "unknown",
        result->driver[0] != '\0' ? result->driver : "unknown",
        result->vendor_id, result->device_id,
        result->output_width, result->output_height,
        result->render_width, result->render_height);
}

static void workshop_preview_capture_service(void) {
    MdkrCharacterPreviewResult *result = g_mdkrCharacterPreviewResult;
    MdkrModernCharacterRuntimeMetrics character;
    MdkrWorkshopPreviewVisualMetrics visual;
    s32 ready;
    if (result == NULL || sWorkshopPreviewCapturePath[0] == '\0' ||
        sWorkshopPreviewCaptureArmed) return;
    mdkr_modern_character_runtime_metrics(&character);
    mdkr_workshop_preview_visual_metrics(&visual);
    /* The inspection camera is an exact look-at around the renderer's fitted
     * character volume, so it remains correctly composed even while an
     * authored start camera is moving. Do not wait on the HUD-owned race
     * countdown: headless and paused inspection sessions may intentionally
     * leave that state machine held forever. Instead require consecutive
     * frames in which every requested presentation layer actually rendered. */
    ready =
        (result->donor_reference
             ? visual.donor_reference_batches >
                       sWorkshopPreviewCaptureLastDonorReferenceBatches &&
                   character.reference_draws >
                       sWorkshopPreviewCaptureLastReferenceDraws &&
                   (result->pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
                    character.inspection_pose_ticks >
                        sWorkshopPreviewCharacterBaseline
                            .inspection_pose_ticks)
             : character.replacement_draws >
                       sWorkshopPreviewCaptureLastReplacementDraws &&
                   character.inspection_pose_ticks >
                       sWorkshopPreviewCharacterBaseline
                           .inspection_pose_ticks) &&
        ((result->view_yaw_degrees == 0 &&
          result->view_pitch_degrees == 0) ||
         visual.camera_override_ticks >
            sWorkshopPreviewVisualBaseline.camera_override_ticks) &&
        (result->lighting == MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
         visual.lighting_override_draws >
            sWorkshopPreviewVisualBaseline.lighting_override_draws);
    sWorkshopPreviewCaptureLastReplacementDraws =
        character.replacement_draws;
    sWorkshopPreviewCaptureLastReferenceDraws = character.reference_draws;
    sWorkshopPreviewCaptureLastDonorReferenceBatches =
        visual.donor_reference_batches;
    if (ready) {
        sWorkshopPreviewCaptureStableFrames++;
    } else {
        sWorkshopPreviewCaptureStableFrames = 0u;
    }
    result->capture_stable_frames = sWorkshopPreviewCaptureStableFrames;
    if (sWorkshopPreviewCaptureStableFrames >=
            MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES) {
        char captureError[192] = { 0 };
        const s32 captureRequested =
            sWorkshopPreviewCaptureKind ==
                    MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA
                ? platform_modern_character_capture_request_once(
                      sWorkshopPreviewCapturePath,
                      captureError, sizeof(captureError))
                : platform_frame_capture_request_once(
                      sWorkshopPreviewCapturePath,
                      captureError, sizeof(captureError));
        if (!captureRequested) {
            fprintf(stderr,
                    "[FATAL] Character Workshop capture could not be armed: %s\n",
                    captureError[0] != '\0'
                        ? captureError : "unknown capture error");
            platform_request_exit(EXIT_FAILURE);
            return;
        }
        sWorkshopPreviewCaptureArmed = TRUE;
        result->capture_armed = TRUE;
        MDKR_TRACE(
            "character_workshop_capture: armed kind=%s stableFrames=%llu countdown=%d replacements=%llu cameraTicks=%llu lightingDraws=%llu",
            sWorkshopPreviewCaptureKind ==
                    MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA
                ? "model-alpha" : "scene",
            (unsigned long long)sWorkshopPreviewCaptureStableFrames,
            result->context == MDKR_CHARACTER_PREVIEW_SELECT
                ? 0 : gRaceStartTimer,
            (unsigned long long)character.replacement_draws,
            (unsigned long long)visual.camera_override_ticks,
            (unsigned long long)visual.lighting_override_draws);
    }
}

/* Vehicle inspection is a deliberate gameplay-camera presentation. Course
 * scripts are still allowed to author their cutscene bank, but displaying that
 * bank would make a deterministic character view point at a different subject.
 * Clear only the one-frame selection latch after authored HUD/camera work; the
 * game restores its ordinary lifecycle on the next tick and after this isolated
 * engine session ends. */
static void workshop_preview_camera_bank_service(void) {
    const MdkrCharacterPreviewResult *result =
        g_mdkrCharacterPreviewResult;
    if (result != NULL && result->started &&
        result->context != MDKR_CHARACTER_PREVIEW_SELECT &&
        result->pose > MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
        result->pose < MDKR_CHARACTER_PREVIEW_POSE_COUNT) {
        disable_cutscene_camera();
    }
}

static void workshop_preview_measurement_service(s32 overlayPaused) {
    MdkrCharacterPreviewResult *result = g_mdkrCharacterPreviewResult;
    if (result == NULL || !result->started) {
        return;
    }
    if (g_mdkrCharacterMotionReviewResult != NULL &&
        sWorkshopPreviewMeasurementStarted) {
        workshop_motion_review_service();
        return;
    }
    /* Evidence capture is allowed to wait longer than the bounded performance
     * sample. Race-start cameras and transitions can legitimately outlive that
     * sample, and arming early would preserve a transient rather than the
     * authored inspection view. */
    if (sWorkshopPreviewMeasurementFinished) {
        if (!overlayPaused) workshop_preview_capture_service();
        return;
    }
    if (!sWorkshopPreviewMeasurementStarted) {
        sWorkshopPreviewWarmupTicks++;
        result->warmup_ticks = sWorkshopPreviewWarmupTicks;
        /* Collect the expensive exact surface witness during warm-up only.
         * Retry a consumed/failed request while there is still a 20-tick
         * settling margin, but never let this authoring diagnostic pollute the
         * measured performance interval. */
        if (!result->donor_reference &&
            result->context >= MDKR_CHARACTER_PREVIEW_CAR &&
            result->context <= MDKR_CHARACTER_PREVIEW_PLANE &&
            sWorkshopPreviewWarmupTicks <=
                WORKSHOP_PREVIEW_WARMUP_TICKS - 20u) {
            const MdkrModernCharacterContext context =
                (MdkrModernCharacterContext)(
                    (s32)result->context -
                    (s32)MDKR_CHARACTER_PREVIEW_SELECT);
            MdkrModernSurfaceIntersectionDiagnostics diagnostics;
            if (!mdkr_modern_character_player_surface_diagnostics(
                    0, context, &diagnostics) &&
                !mdkr_modern_character_surface_diagnostics_requested(
                    0, context)) {
                (void)mdkr_modern_character_request_surface_diagnostics(
                    0, context);
            }
        }
        if (sWorkshopPreviewWarmupTicks <=
                WORKSHOP_PREVIEW_WARMUP_TICKS - 20u) {
            MdkrModernCharacterVisibilityDiagnostics diagnostics;
            MdkrModernCharacterRuntimeMetrics character;
            mdkr_modern_character_runtime_metrics(&character);
            if (character.replacement_draws != 0u &&
                !platform_modern_character_visibility_diagnostics(
                    &diagnostics) &&
                !platform_modern_character_visibility_pending() &&
                sWorkshopPreviewVisibilityAttempts < 3u &&
                platform_modern_character_visibility_request_once()) {
                sWorkshopPreviewVisibilityAttempts++;
            }
        }
        /* Map completion is queue-ordered after the diagnostic replay. Do not
         * begin the clean timing interval while that work is still in flight:
         * on a slow/high-poly device the nominal 20-tick request margin is a
         * convenience, not a proof that the GPU has drained. The launcher's
         * bounded stalled-session recovery remains the escape hatch for a
         * lost callback or device hang. */
        if (sWorkshopPreviewWarmupTicks >= WORKSHOP_PREVIEW_WARMUP_TICKS &&
            !platform_modern_character_visibility_pending()) {
            present_perf_measurement_reset();
            mdkr_modern_character_contact_metrics_reset();
            gfx_begin_modern_character_gpu_timing();
            mdkr_modern_character_runtime_metrics(
                &sWorkshopPreviewCharacterBaseline);
            mdkr_workshop_preview_visual_metrics(
                &sWorkshopPreviewVisualBaseline);
            sWorkshopPreviewCaptureLastReplacementDraws =
                sWorkshopPreviewCharacterBaseline.replacement_draws;
            sWorkshopPreviewCaptureLastReferenceDraws =
                sWorkshopPreviewCharacterBaseline.reference_draws;
            sWorkshopPreviewCaptureLastDonorReferenceBatches =
                sWorkshopPreviewVisualBaseline.donor_reference_batches;
            sWorkshopPreviewMeasurementStarted = TRUE;
            result->warmup_complete = TRUE;
            if (g_mdkrCharacterMotionReviewResult != NULL) {
                g_mdkrCharacterMotionReviewResult->started = TRUE;
                workshop_motion_review_begin_sample();
            }
            MDKR_TRACE(
                "character_workshop_measurement: started warmupTicks=%llu",
                (unsigned long long)sWorkshopPreviewWarmupTicks);
        }
        if (overlayPaused) workshop_preview_measurement_finish();
        return;
    }
    if (!overlayPaused) workshop_preview_capture_service();
    if (overlayPaused) workshop_preview_measurement_finish();
}
#endif

/******************************/

/**
 * Main looping function for the main thread.
 * Official Name: mainThread
 */
void thread3_main(UNUSED void *unused) {
    /* These values live in the original cartridge's .data, whose initializer
     * ran once per process. A persistent launcher can invoke the engine more
     * than once, so restore the boot contract explicitly before init_game(). */
    gSaveDataFlags = 0;
    gScreenStatus = OSMESG_SWAP_BUFFER;
    sControllerStatus = 0;
    gSkipGfxTask = FALSE;
    gDrumstickSceneLoadTimer = 0;
    gLevelLoadTimer = 0;
    gPauseLockTimer = 0;
    gFutureFunLandLevelTarget = FALSE;
    gDmemInvalid = FALSE;
    gDrawFrameTimer = 0;
    sLogicUpdateRate = LOGIC_5FPS;
    bzero(gLevelSettings, sizeof(gLevelSettings));
    gSPTaskNum = 0;
    gGameMode = 0;
    gRenderMenu = 0;
    gPlayableMapId = 0;
    gGameNumPlayers = 0;
    gGameCurrentEntrance = 0;
    gGameCurrentCutscene = 0;
    gPrevPlayerCount = 0;
    gSettingsPtr = NULL;
    sWriteSaveSource = NULL;
    gIsLoading = FALSE;
    gIsPaused = FALSE;
    gPostRaceViewPort = 0;
    gLevelDefaultVehicleID = VEHICLE_CAR;
    gMenuVehicleID = VEHICLE_CAR;
    sBootDelayTimer = 0;
    gLevelLoadType = 0;
    gNextMap = 0;
    gCurrNumF3dCmdsPerPlayer = 0;
    gCurrNumHudMatPerPlayer = 0;
    gCurrNumHudTrisPerPlayer = 0;
    gCurrNumHudVertsPerPlayer = 0;
    bzero(gNMISched, sizeof(gNMISched));
    bzero(&gNMIMesgQueue, sizeof(gNMIMesgQueue));
    gNMIOSMesg = NULL;
    gNMIMesgBuf = 0;
#ifdef NATIVE_PORT
    sWorkshopPreviewWarmupTicks = 0u;
    sWorkshopPreviewMeasurementStarted = FALSE;
    sWorkshopPreviewMeasurementFinished = FALSE;
    sWorkshopPreviewVisibilityAttempts = 0u;
    bzero(&sWorkshopPreviewCharacterBaseline,
          sizeof(sWorkshopPreviewCharacterBaseline));
    bzero(&sWorkshopPreviewVisualBaseline,
          sizeof(sWorkshopPreviewVisualBaseline));
    sWorkshopPreviewCapturePath[0] = '\0';
    sWorkshopPreviewCaptureStableFrames = 0u;
    sWorkshopPreviewCaptureLastReplacementDraws = 0u;
    sWorkshopPreviewCaptureLastReferenceDraws = 0u;
    sWorkshopPreviewCaptureLastDonorReferenceBatches = 0u;
    sWorkshopPreviewCaptureArmed = FALSE;
    sWorkshopPreviewCaptureKind =
        MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
    sWorkshopMotionReviewSample = 0u;
    sWorkshopMotionReviewStageTicks = 0u;
    sWorkshopMotionReviewVisibilityAttempts = 0u;
    sWorkshopMotionReviewReplacementBaseline = 0u;
    sWorkshopMotionReviewPoseSettled = FALSE;
    sWorkshopMotionReviewDiagnosticsRequested = FALSE;
    sWorkshopMotionReviewUiReported = FALSE;
    bzero(&sWorkshopMotionReviewPoseBaseline,
          sizeof(sWorkshopMotionReviewPoseBaseline));
    mdkr_workshop_preview_visual_clear();
    mdkr_workshop_preview_visual_metrics_reset();
#endif
    init_game();
    gSaveDataFlags = input_update(gSaveDataFlags, 0);
    sBootDelayTimer = 0;
    gGameMode = GAMEMODE_INTRO;
    while (
#ifdef NATIVE_PORT
        !platform_exit_requested()
#else
        1
#endif
    ) {
        if (is_reset_pressed()) {
            rumble_kill();
            audioStopThread();
            bgload_kill();
            __osSpSetStatus(SP_SET_HALT | SP_CLR_INTR_BREAK | SP_CLR_YIELD | SP_CLR_YIELDED | SP_CLR_TASKDONE |
                            SP_CLR_RSPSIGNAL | SP_CLR_CPUSIGNAL | SP_CLR_SIG5 | SP_CLR_SIG6 | SP_CLR_SIG7);
            osDpSetStatus(DPC_SET_XBUS_DMEM_DMA | DPC_CLR_FREEZE | DPC_CLR_FLUSH | DPC_CLR_TMEM_CTR | DPC_CLR_PIPE_CTR |
                          DPC_CLR_CMD_CTR | DPC_CLR_CMD_CTR);
            while (1) {
                ; // Infinite loop
            }
        }
        main_game_loop();
#ifdef NATIVE_PORT
        if (platform_exit_requested()) {
            break;
        }
#endif
        thread3_verify_stack();
    }
#ifdef NATIVE_PORT
    workshop_preview_measurement_finish();
    mdkr_workshop_preview_visual_clear();
#endif
}

/**
 * Setup all of the necessary pieces required for the game to function.
 * This includes the memory pool. controllers, video, audio, core assets and more.
 * Official Name: mainInitGame
 */
void init_game(void) {
#ifdef NATIVE_PORT
    /* The host normally reports NTSC, but a malformed/unknown platform value
     * must still select a valid scheduler mode instead of reading stack data. */
    s32 viMode = OS_VI_NTSC_LPN1;
#else
    s32 viMode;
#endif

    stubbed_printf(sDebugRomBuildInfo);
    mempool_init_main();
    gzip_init();
#ifdef ANTI_TAMPER
    sAntiPiracyTriggered = TRUE;
    if (drm_validate_imem()) {
        sAntiPiracyTriggered = FALSE;
    }
#endif
    gIsLoading = FALSE;
    gLevelDefaultVehicleID = VEHICLE_CAR;

    if (osTvType == OS_TV_TYPE_PAL) {
        viMode = OS_VI_PAL_LPN1;
    } else if (osTvType == OS_TV_TYPE_NTSC) {
        viMode = OS_VI_NTSC_LPN1;
    } else if (osTvType == OS_TV_TYPE_MPAL) {
        viMode = OS_VI_MPAL_LPN1;
    }
    osCreateScheduler(&gMainSched, &gSchedStack[STACKSIZE(STACK_SCHED)], /*priority*/ 13, viMode, 1);
#ifdef ANTI_TAMPER
    // Antipiracy measure.
    gDmemInvalid = FALSE;
    if (drm_validate_dmem() == FALSE) {
        gDmemInvalid = TRUE;
    }
#endif
    video_init(VIDEO_MODE_LOWRES_LPN, &gMainSched);
    pi_init();
    gfxtask_init(&gMainSched);
    audio_init(&gMainSched);
    audspat_init();
    sControllerStatus = input_init();
    tex_init_textures();
    allocate_object_model_pools();
    allocate_object_pools();
    debug_text_init();
    allocate_ghost_data();
    init_particle_assets();
    weather_init();
    calc_and_alloc_heap_for_settings();
    default_alloc_displaylist_heap();
    load_fonts();
    init_controller_paks();
    init_save_data();
    bgload_init();
    osCreateMesgQueue(&gNMIMesgQueue, &gNMIOSMesg, 1);
    osScAddClient(&gMainSched, (OSScClient *) gNMISched, &gNMIMesgQueue, OS_SC_ID_PRENMI);
    gNMIMesgBuf = 0;
    gGameCurrentEntrance = 0;
    gGameCurrentCutscene = 0;
    gSPTaskNum = 0;

    gCurrDisplayList = gDisplayLists[gSPTaskNum];
    gDPFullSync(gCurrDisplayList++);
    gSPEndDisplayList(gCurrDisplayList++);

    osSetTime(0);
}

/**
 * The main gameplay loop.
 * Contains all game logic, audio and graphics processing.
 */
void main_game_loop(void) {
    s32 debugLoopCounter;
    s32 framebufferSize;
    s32 tempLogicUpdateRate, tempLogicUpdateRateMax;
    s32 logicUpdateRate = sLogicUpdateRate;
#ifdef NATIVE_PORT
    s32 elideCatchupRender;
#endif

    osSetTime(0);

#ifdef NATIVE_PORT
    logicUpdateRate = video_logic_update_rate(sLogicUpdateRate);
    present_sched_note_game_update((unsigned)sLogicUpdateRate,
                                   (unsigned)logicUpdateRate);
    elideCatchupRender = present_sched_should_elide_render();
    present_sched_set_render_elided(elideCatchupRender != 0);
#endif

    if (gScreenStatus == MESG_SKIP_BUFFER_SWAP) {
        gCurrDisplayList = gDisplayLists[gSPTaskNum];
        rsp_segment(&gCurrDisplayList, SEGMENT_MAIN, 0x00000000);
        rsp_segment(&gCurrDisplayList, SEGMENT_FRAMEBUFFER,
                    (s32) DKR_TOK(gVideoCurrFramebuffer));
        rsp_segment(&gCurrDisplayList, SEGMENT_ZBUFFER,
                    (s32) DKR_TOK(gVideoLastDepthBuffer));
        rsp_segment(&gCurrDisplayList, SEGMENT_FRAMEBUFFER_OFFSET,
                    (s32) ((u32) DKR_TOK(gVideoCurrFramebuffer) -
                           VI_OFFSET)); // Unused
    }
    if (gDrawFrameTimer == 0
#ifdef NATIVE_PORT
        && !elideCatchupRender
#endif
    ) {
        gfxtask_run_xbus(gDisplayLists[gSPTaskNum], gCurrDisplayList, 0);
        gSPTaskNum += 1;
        gSPTaskNum &= 1;
    }
    if (gDrawFrameTimer) {
        gDrawFrameTimer--;
    }

    gCurrDisplayList = gDisplayLists[gSPTaskNum];
    gGameCurrMatrix = gMatrixHeap[gSPTaskNum];
    gGameCurrVertexList = gVertexHeap[gSPTaskNum];
    gGameCurrTriList = gTriangleHeap[gSPTaskNum];
#ifdef NATIVE_PORT
    /* Stamp the main list exactly once. Nested rsp_init() calls (notably HUD)
     * reset RSP state inside this list and must not replace task ownership. */
    presentation_task_authoring_begin(gCurrDisplayList);
#endif

    rsp_segment(&gCurrDisplayList, SEGMENT_MAIN, 0x00000000);
    rsp_segment(&gCurrDisplayList, SEGMENT_FRAMEBUFFER,
                (s32) DKR_TOK(gVideoLastFramebuffer));
    rsp_segment(&gCurrDisplayList, SEGMENT_ZBUFFER,
                (s32) DKR_TOK(gVideoLastDepthBuffer));
    rsp_segment(&gCurrDisplayList, SEGMENT_FRAMEBUFFER_OFFSET,
                (s32) ((u32) DKR_TOK(gVideoLastFramebuffer) -
                       VI_OFFSET)); // Unused
    rsp_init(&gCurrDisplayList);
    rdp_init(&gCurrDisplayList);
    bgdraw_render(&gCurrDisplayList, &gGameCurrMatrix, TRUE);
    gSaveDataFlags = input_update(gSaveDataFlags, logicUpdateRate);
#ifdef NATIVE_PORT
    /* The application overlay is a real pause boundary, not merely input
     * capture. Keep polling input and rendering the held scene, but advance
     * every time-based game/menu/audio/transition consumer with a zero rate.
     * is_game_paused() below exposes the same state to subsystems with their own
     * pause gates. Input capture is a separate query: online Party chrome may
     * consume navigation without stopping this endpoint's authored clock.
     * Without registered hooks both queries are constant-zero. */
    {
        const s32 overlayPaused = platformOverlayWantsPause();
        workshop_preview_measurement_service(overlayPaused);
        /* A cutscene camera is a one-frame pulse. The app overlay opens from
         * presentation, so unlike the authored START pause it reaches this
         * input boundary one tick after that pulse was cleared. Restore the
         * preceding frame's bank without advancing its camera object; the
         * existing paused clear gate below then holds the authored pose. */
        if (overlayPaused
#if MDKR_ENABLE_ONLINE_BETA
            /* Online the overlay is non-pausing chrome (below), so the paused
             * camera-bank restore must not fire over a still-running sim. */
            && !mdkr_online_race_pause_suppressed()
#endif
        ) {
            cutscene_camera_pause_restore();
        }
        /* The scripted overlay gate needs the exact simulation boundary, not
         * the presentation ordinal printed by ImGui. Keep this diagnostic
         * dormant outside that explicit test contract. */
        static s32 previousOverlayPaused = -1;
        if (getenv("MDKR_TEST_OVERLAY_OPEN_FRAME") != NULL ||
            getenv("MDKR_TEST_OVERLAY_ESCAPE_OPEN_FRAME") != NULL) {
            if (previousOverlayPaused >= 0 &&
                previousOverlayPaused != overlayPaused) {
                printf("[overlay-test] simulation %s at tick %d rate=%d\n",
                       overlayPaused ? "paused" : "resumed",
                       g_simTickCounter, logicUpdateRate);
            }
            previousOverlayPaused = overlayPaused;
        }
        if (overlayPaused
#if MDKR_ENABLE_ONLINE_BETA
            /* In a LIVE online rollback race the app overlay must NOT become a
             * sim pause boundary: a zero-rate authored tick is exactly what a
             * later correction replay refuses (rollback_game_runtime.c's
             * sim-refusal arm), so zeroing the clock here turned "player opened
             * the overlay" into the recoverable clean-LEFT unwind -- ejected
             * from the race by a menu. Online the overlay is presentation-only
             * chrome over the running sim, the same non-blocking contract as
             * the START overlay (online_race_pause.h). While it owns the
             * keyboard the platform input pump publishes one NEUTRAL pad
             * sample (overlay_capture_sync), so the local seat coasts neutral
             * and that neutral input seals canonically like any other frame --
             * deterministic on both endpoints, live and in resim. Keyed on the
             * same constant-per-race gate as the retail-pause suppression.
             * Beta-gated: the OFF build sees the original condition and its
             * thread3_main.c.o anchor stays byte-identical. */
            && !mdkr_online_race_pause_suppressed()
#endif
        ) {
            logicUpdateRate = 0;
        }
    }
#endif
#ifdef NATIVE_PORT
    if (!mdkr_rollback_game_runtime_prepare_tick(
            (unsigned)logicUpdateRate)) {
#if MDKR_ENABLE_ONLINE_BETA
        /* P0 CRASH FIX, mid-race sibling (beta only): a live online race whose
         * peer/input for THIS authored tick vanished -- a peer/console that
         * dropped cleanly mid-race, so the launcher input provider could not
         * supply the tick (or a rewound correction's peer input is gone). That is
         * a RECOVERABLE peer loss, NOT rollback invariant corruption: route it to
         * the SAME clean return-to-room as the race-start path (note session-end
         * LEFT + clean rollback teardown + platform_request_exit(0)) instead of
         * crashing. Genuine invariant violations (tick-counter exhaustion,
         * prepared-twice, snapshot restore, correction-replay allocation/coverage)
         * leave the recoverable flag CLEAR and still hit the abort() below,
         * byte-for-byte as before. Wrapped in MDKR_ENABLE_ONLINE_BETA so the OFF
         * build's thread3_main.c.o (anchor 20ed811d) is byte-identical. */
        if (mdkr_rollback_game_runtime_online_input_recoverable()) {
            mdkr_online_session_return_to_room_on_peer_loss();
            return;
        }
#endif
        fprintf(stderr,
                "[FATAL] rollback lab could not prepare canonical input\n");
        abort();
    }
#endif
    if (get_lockup_status()) {
        render_epc_lock_up_display();
        gGameMode = GAMEMODE_LOCKUP;
    }
    if (gDmemInvalid) {
        debugLoopCounter = 0;
        while (debugLoopCounter != 10000000) {
            debugLoopCounter++;
        }
        if (debugLoopCounter > 20000000) { // This shouldn't ever be true?
            render_printf(D_800E7134 /* "BBB\n" */);
        }
    }

    switch (gGameMode) {
        case GAMEMODE_INTRO: // Pre-boot screen
            mode_intro();
            break;
        case GAMEMODE_MENU: // In a menu
            mode_menu(logicUpdateRate);
            break;
        case GAMEMODE_INGAME: // In game (Controlling a character)
            mode_game(logicUpdateRate);
            break;
#if MDKR_ENABLE_ONLINE_BETA
        // Separated online boot path (beta). GAMEMODE_ONLINE_SESSION aliases the
        // dead offline slot GAMEMODE_UNUSED_2, which offline code never produces,
        // so this case is unreachable in a normal (beta OFF) build and compiles
        // out entirely there.
        case GAMEMODE_ONLINE_SESSION:
            mdkr_online_session_tick(logicUpdateRate);
            break;
#endif
        case GAMEMODE_LOCKUP: // EPC (lockup display)
            mode_lockup(logicUpdateRate);
            break;
    }

#if MDKR_ENABLE_ONLINE_BETA
    /* NON-BLOCKING online race pause overlay (beta only): serviced AFTER the
     * mode dispatch so it reads this frame's canonical local-seat edges and
     * draws over the completed race frame -- and never during resimulation
     * (resim re-enters mode_game directly, not this loop), so overlay state is
     * pure local presentation the rollback authority never sees. Inert unless
     * a live online rollback race is running. The OFF build strips this block
     * and thread3_main.c.o stays byte-identical. */
    mdkr_online_race_overlay_frame(logicUpdateRate);
#endif

    // This is a good spot to place custom text if you want it to overlay it over ALL the
    // menus & gameplay.

    sound_update_queue(logicUpdateRate);
#ifdef NATIVE_PORT
    workshop_motion_review_render_status();
#endif
    debug_text_print(&gCurrDisplayList);
#ifdef NATIVE_PORT
    /* Confine the widescreen HUD's expanded draw space to the HUD: the dialogue
     * boxes (including Taj's) render next and must keep their authored centered
     * 4:3 placement (#50).  The expanded WIDE_HUD ortho is only ever emitted
     * from mode_game -- hud_render_general runs there, and hud_render_player's
     * HUD body is gated out of the menu -- so confine the restore to
     * GAMEMODE_INGAME.  Outside it there is nothing to undo, and firing it would
     * force an unnecessary SAFE_2D onto the shared dialogue-box path used by the
     * intro/menu/lockup dialogue. */
    if (gGameMode == GAMEMODE_INGAME) {
        hud_widescreen_restore_screen_ortho(&gCurrDisplayList, &gGameCurrMatrix);
    }
#endif
    render_dialogue_boxes(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList);
    dialogue_close(4);
    dialogue_clear(4);
    // transition_update will perform the logic of transitions and return the transition ID.
    if (transition_update(logicUpdateRate)) {
        transition_render(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList);
    }
    if (sBootDelayTimer >= 8 && is_controller_missing()) {
        menu_missing_controller(&gCurrDisplayList, logicUpdateRate);
    }
#ifdef NATIVE_PORT
    /* The rollback boundary is the completed authored pass, not merely the
     * end of mode_game(): transition_update above owns gameplay-readable fade
     * timers and must be included in the same tick snapshot. Presentation has
     * finished authoring and fb_update has not issued the next ticket yet. */
    if (!mdkr_rollback_game_runtime_validate_boundary(
            (unsigned)logicUpdateRate)) {
#if MDKR_ENABLE_ONLINE_BETA
        /* P0 CRASH FIX (beta only): a live online race whose peer/bootstrap input
         * vanished at race start -- the peer LOST (ICE failed) and the launcher's
         * race-start barrier aborted the tick-1 drain -- starves this boundary.
         * That is a RECOVERABLE peer loss, NOT rollback invariant corruption:
         * route it to a clean return-to-room (note session-end LEFT + clean
         * rollback teardown + platform_request_exit(0)) and break out of the tick
         * loop, instead of crashing BOTH machines. Genuine invariant violations
         * (authority allocation lifetime/coverage, snapshot capture, side-effect
         * journal, tick-counter exhaustion) leave the recoverable flag CLEAR and
         * still hit the abort() below, byte-for-byte as before. Wrapped in
         * MDKR_ENABLE_ONLINE_BETA so the OFF build's thread3_main.c.o (anchor
         * 20ed811d) is untouched: the preprocessor strips this block entirely and
         * the offline / OFF abort() path is byte-identical. */
        if (mdkr_rollback_game_runtime_online_input_recoverable()) {
            mdkr_online_session_return_to_room_on_peer_loss();
            return;
        }
#endif
        fprintf(stderr,
                "[FATAL] rollback lab lost a registered authority allocation\n");
        abort();
    }
#endif

    gDPFullSync(gCurrDisplayList++);
    gSPEndDisplayList(gCurrDisplayList++);

    copy_viewports_to_stack();
    if (gDrawFrameTimer != 1) {
        if (gSkipGfxTask == FALSE
#ifdef NATIVE_PORT
            && !elideCatchupRender
#endif
        ) {
            gScreenStatus = gfxtask_wait();
        }
    } else {
        gDrawFrameTimer = 0;
    }
    gSkipGfxTask = FALSE;
    mempool_free_queue_clear();
#ifdef NATIVE_PORT
    /* Capture the authored bank before its ordinary end-of-frame clear. This
     * is reset by cam_init(), so a level generation cannot lend its camera to
     * the next one. */
    cutscene_camera_pause_snapshot();
#endif
    if (!is_game_paused()) {
        disable_cutscene_camera();
    }
    if (gDrawFrameTimer == 2) {
        framebufferSize = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
        if (osTvType == OS_TV_TYPE_PAL) {
            framebufferSize = (s32) ((SCREEN_WIDTH * SCREEN_HEIGHT * 2) * 1.1f);
        }
        dmacopy_doubleword(gVideoLastFramebuffer, gVideoCurrFramebuffer,
#ifdef NATIVE_PORT
                           (u32) framebufferSize);
#else
                           (u32) gVideoCurrFramebuffer + (u32) framebufferSize);
#endif
    }
    // tempLogicUpdateRate will be set to a value 2 or higher, based on the framerate.
    // the mul factor is hardcapped at 6, which happens at 10FPS. The mul factor
    // affects frameskipping, to maintain consistent game speed, through the (many)
    // dropped frames in DKR.
#ifdef NATIVE_PORT
    gameplay_event_trace_observe_context(gGameMode, level_id());
#endif
#ifdef NATIVE_PORT
    tempLogicUpdateRate = fb_update(
        elideCatchupRender ? MESG_SKIP_BUFFER_SWAP : gScreenStatus);
#else
    tempLogicUpdateRate = fb_update(gScreenStatus);
#endif
    sLogicUpdateRate = tempLogicUpdateRate;
    tempLogicUpdateRateMax = LOGIC_10FPS;
    if (tempLogicUpdateRate > tempLogicUpdateRateMax) {
        sLogicUpdateRate = tempLogicUpdateRateMax;
    }
#if REGION == REGION_JP
    func_800C78E0_C84E0();
#endif
}

/**
 * Loads a level for gameplay based on what the next track ID is, with the option to override.
 */
void load_next_ingame_level(s32 numPlayers, s32 trackID, Vehicle vehicle) {
    gGameNumPlayers = numPlayers - 1;
    if (trackID == -1) {
        gPlayableMapId = get_track_id_to_load();
    } else {
        gPlayableMapId = trackID; // Unused, because arg1 is always -1.
    }
    load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, vehicle);
}

/**
 * Calls level_load() with the same arguments except for the cutsceneId,
 * which is the value at gGameCurrentCutscene. Also does some other stuff.
 * Used when ingame.
 */
void load_level_game(s32 levelId, s32 numberOfPlayers, s32 entranceId, Vehicle vehicleId) {
#ifdef NATIVE_PORT
    /* THE LEVEL APPLY BOUNDARY (video_config.h). A staged Camera.Obstruction
     * change takes effect here, so the reset immediately below is the one that
     * retires the outgoing policy's held poses -- the same reset this path has
     * always run, doing the same job for one more reason. */
    (void)mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LEVEL);
    camera_obstruction_runtime_reset();
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_LEVEL, levelId, numberOfPlayers, entranceId, vehicleId);
#endif
    alloc_displaylist_heap(numberOfPlayers);
    mempool_free_timer(0);
    cam_init();
    load_game_text_table();
    level_load(levelId, numberOfPlayers, entranceId, vehicleId, gGameCurrentCutscene);
    hud_init(cam_get_viewport_layout());
    init_particle_buffers(8, 16, 150, 100, 50, 0);
    ainode_update();
    osSetTime(0);
    mempool_free_timer(2);
    rumble_init(TRUE);
#ifdef NATIVE_PORT
    if (!mdkr_rollback_game_runtime_level_ready()) {
        fprintf(stderr,
                "[ROLLBACK] engine startup rejected before authored tick one\n");
        /* The level itself is completely initialized and can follow the normal
         * owner-last host teardown. A startup/admission failure must never turn
         * an unsupported online room into an abort or crash loop. */
        platform_request_exit(EXIT_FAILURE);
        return;
    }
#endif
}

#ifdef NATIVE_PORT
/**
 * Retire the presentation history whose backing memory is about to be freed or
 * reissued.
 *
 * Presentation replay publishes a private authored task, but its ownership
 * token and matrix/object snapshots still belong to one level generation. The
 * only operation that retires that complete history is
 * gfx_dkr_replay_invalidate().
 *
 * game.c's level_load() calls gfx_dkr_resource_generation_begin(), which
 * invalidates -- but a quit-to-title never reaches level_load(): it unloads and
 * then loads a MENU with SPECIAL_MAP_ID_NO_LEVEL (:788/:844/:1151/:1190 below),
 * and load_menu_with_level_background skips the level path entirely for that
 * id. The DL heap and level geometry are freed there without a following
 * resource-generation begin. A retained task must not remain eligible after
 * that boundary even though its private bytes are safe: its owner generations
 * and next-state pair describe the retired scene.
 *
 * So the invalidation belongs at the FREE, not at the load: every teardown that
 * can release what the replay points into calls this. The snapshot store is
 * reset alongside it for the same reason it is reset at level load -- the
 * object pool is about to be torn down and every recycled address after that is
 * a coincidence. Both calls are cheap no-ops when the features are unarmed.
 */
static void presentation_history_retire(void) {
    gfx_dkr_replay_invalidate();
    presentation_snapshot_stage_reset();
}
#endif

/**
 * Call numerous functions to clear data in RAM.
 * Then call to free particles, HUD and text.
 * Waits for a GFX task before unloading.
 */
void unload_level_game(void) {
    mempool_free_timer(0);
#ifdef NATIVE_PORT
    mdkr_rollback_game_runtime_level_end();
#endif
    if (gSkipGfxTask == FALSE) {
        if (gDrawFrameTimer != 1) {
            gfxtask_wait();
        }
        gSkipGfxTask = TRUE;
    }
#ifdef NATIVE_PORT
    /* Before level_free(): the held display list's commands reference the
     * level's segments and the object models it is about to release. */
    camera_obstruction_runtime_reset();
    presentation_history_retire();
#endif
    level_free();
    transition_begin(&D_800DD3F4);
    reset_particles();
    hud_free();
    free_game_text_table();
    gCurrDisplayList = gDisplayLists[gSPTaskNum];
    gDPFullSync(gCurrDisplayList++);
    gSPEndDisplayList(gCurrDisplayList++);
    mempool_free_timer(2);
}

/**
 * The main behaviour function involving all of the ingame stuff.
 * Involves the updating of all objects and setting up the render scene.
 */
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
/* Controller-disconnect shared pause (AP-10), lobby-scoped this task. Pad
 * presence is the platform API, overridable by the MDKR_AP_DROP_PAD test injector
 * so a headless route can simulate a mid-session drop (the input-script presence
 * mask is whole-route and cannot). Split into a GUARDED force-open and a
 * block-unpause so a drop can never bypass the retail pause guards. */

/* 1 if a bound seat's controller is absent during the lobby -> the shared pause
 * must be held. Fills *out_seat (when non-NULL) with the first missing bound
 * seat (host seat as fallback). Reads pad presence, then the pure policy
 * decision. */
static int adventure_party_disconnect_hold(int *out_seat) {
    AdventurePartySession *s = adventure_party_runtime_session();
    int seat, n, present_mask = 0;
    if (!adventure_party_runtime_is_active() ||
        s->state != ADVENTURE_PARTY_STATE_ACTIVE_LOBBY) {
        return 0;
    }
    n = adventure_party_participant_count(s);
    for (seat = 0; seat < n; seat++) {
        /* Hub binding is identity (seat i -> controller port i, Task 7). */
        if (platform_pad_present(seat) && !mdkr_test_pad_absent(seat)) {
            present_mask |= (1 << seat);
        }
    }
    if (!adventure_party_disconnect_should_pause(s->roster.seat_mask,
                                                 (uint8_t) present_mask)) {
        return 0;
    }
    if (out_seat) {
        *out_seat = adventure_party_host_seat(s);
        for (seat = 0; seat < n; seat++) {
            if (!(present_mask & (1 << seat))) { *out_seat = seat; break; }
        }
    }
    return 1;
}

/* Part A -- force the shared pause OPEN on a bound-pad drop, but ONLY under the
 * exact safety guards the retail Start open uses: never mid-fade during a door
 * transition (racer_enter_door raises gPauseLockTimer every tick precisely to
 * forbid pausing then, which would otherwise inject a QUIT-capable menu while a
 * func_8006D968 load is still pending), never mid-level-load, out of INGAME, in
 * a post-race screen, a scene push, or a cutscene. The hold persists, so a drop
 * inside the door window simply opens the pause on the next safe frame (the
 * destination lobby). Called at the retail open site, so gPauseLockTimer is the
 * un-decremented value set by this frame's obj_update. Emits the drop diagnostic
 * only when the pause actually opens. */
static void adventure_party_disconnect_open_if_safe(void) {
    int dropped;
    if (is_game_paused() || !adventure_party_disconnect_hold(&dropped)) {
        return;
    }
    if (level_properties_get() != 0 || gDrumstickSceneLoadTimer != 0 ||
        gGameMode != GAMEMODE_INGAME || gPostRaceViewPort != FALSE ||
        gLevelLoadTimer != 0 || gPauseLockTimer != 0) {
        return; /* not a safe frame -- the hold persists, retry next frame */
    }
    gIsPaused = TRUE;
    menu_pause_init();
    adventure_party_trace_emit_interaction(
        (uint8_t) dropped, ADVENTURE_PARTY_ACTION_PAUSE_DECISION,
        ADVENTURE_PARTY_ARBITRATE_REJECTED_SEAT);
}

/* Part B -- BLOCK unpause: while a bound pad is missing, re-assert a pause that
 * was already open before the menu ran (never opens a fresh one; that is Part A's
 * guarded job). Emits the resume diagnostic when every bound pad returns. */
static void adventure_party_disconnect_block_unpause(s8 was_paused) {
    static int sPrevHold;
    int hold = adventure_party_disconnect_hold(NULL);
    if (hold && was_paused) {
        gIsPaused = TRUE;
    } else if (!hold && sPrevHold) {
        adventure_party_trace_emit_interaction(
            (uint8_t) adventure_party_host_seat(adventure_party_runtime_session()),
            ADVENTURE_PARTY_ACTION_PAUSE_DECISION,
            ADVENTURE_PARTY_ARBITRATE_LATCHED);
    }
    sPrevHold = hold;
}
#endif

void mode_game(s32 updateRate) {
    s32 buttonPressedInputs, buttonHeldInputs, i, loadContext, sp3C;

    loadContext = LEVEL_CONTEXT_NONE;
    buttonHeldInputs = 0;
    buttonPressedInputs = 0;

    // Get input data for all 4 players.
    for (i = 0; i < mdkr_authoritative_player_count(
                        get_active_player_count()); i++) {
        buttonHeldInputs |= input_held(i);
        buttonPressedInputs |= input_pressed(i);
    }
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
    /* Any party seat may REQUEST the one shared pause: fold in each participant
     * seat's START so a non-host press opens it too (host authority over the menu
     * itself is applied in menu_pause_init). Only START is added, so no other
     * seat's input leaks into the retail load/quit paths. */
    if (adventure_party_runtime_is_active()) {
        AdventurePartySession *apPauseSession = adventure_party_runtime_session();
        /* AP-17: during a host-solo special challenge/boss (SOLO_ACTIVITY) or its
         * restore, the host plays alone — fold in NO non-host START, so the
         * activity's pause is host-only exactly as retail 1P. Every other state
         * (lobby, race) keeps the any-seat pause REQUEST (AP-10). */
        if (apPauseSession->state != ADVENTURE_PARTY_STATE_SOLO_ACTIVITY &&
            apPauseSession->state != ADVENTURE_PARTY_STATE_RESTORING_PARTY) {
            int apSeat, apCount = adventure_party_participant_count(apPauseSession);
            for (apSeat = 0; apSeat < apCount; apSeat++) {
                buttonPressedInputs |= (input_pressed(apSeat) & START_BUTTON);
            }
        }
    }
#endif
#ifdef ANTI_TAMPER
    // Spam the start button, making the game unplayable because it's constantly paused.
    if (sAntiPiracyTriggered) {
        buttonPressedInputs |= START_BUTTON;
    }
#endif
    // Update all objects
    if (!is_game_paused()) {
        obj_update(updateRate);
        if (check_if_showing_cutscene_camera() == 0 || get_race_countdown()) {
            if (buttonPressedInputs & START_BUTTON && level_properties_get() == 0 && gDrumstickSceneLoadTimer == 0 &&
                gGameMode == GAMEMODE_INGAME && gPostRaceViewPort == FALSE && gLevelLoadTimer == 0 &&
                gPauseLockTimer == 0) {
#if MDKR_ENABLE_ONLINE_BETA
                /* ONLINE PAUSE CRASH FIX (beta only): in an online rollback race
                 * the retail pause NEVER engages -- this branch consumes the
                 * CANONICAL input of every seat, so a START press (local or the
                 * remote player's, live or replayed by a correction) would pause
                 * the networked sim itself, and a paused sim makes every later
                 * correction replay refuse (mdkr_game_resimulate_tick admission/
                 * completion) -- the two-machine beta abort. The suppression is
                 * keyed only on the online input runtime (constant for the race,
                 * identical on both machines and in resim), so both sims skip the
                 * engage deterministically; the LOCAL non-blocking overlay in
                 * main_game_loop owns the local START instead. Wrapped in
                 * MDKR_ENABLE_ONLINE_BETA so the OFF build's thread3_main.c.o
                 * (anchor) strips this block and offline pause is byte-identical. */
                if (mdkr_online_race_pause_suppressed()) {
                    if (!sRollbackResimulating) {
                        fprintf(stderr,
                                "[online-pause] retail pause suppressed "
                                "(online race; overlay owns START)\n");
                    }
                } else
#endif
                {
                    buttonPressedInputs = 0;
                    gIsPaused = TRUE;
                    menu_pause_init();
                }
            }
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
            /* A dropped bound pad forces the shared pause here, under the very
             * same guards as the Start open above (this frame's un-decremented
             * gPauseLockTimer), so a drop mid-door-transition waits for a safe
             * frame instead of injecting a QUIT-capable menu mid-fade. */
            adventure_party_disconnect_open_if_safe();
#endif
        }
    } else {
        set_anti_aliasing(TRUE);
    }
    gPauseLockTimer -= updateRate;
    if (gPauseLockTimer < 0) {
        gPauseLockTimer = 0;
    }
    if (gPostRaceViewPort) {
        gIsPaused = FALSE;
    }
    gParticlePtrList_flush();
    ainode_update();
#ifdef NATIVE_PORT
    /*
     * TICK ORDERING CONTRACT. hud_tick runs FIRST.
     *
     * hud_player_tick consumes this frame's C-button edges and writes
     * gHudToggleSettings[] (game_ui.c). Three later readers key off that value:
     *   - scene_build_last_viewport_basis (tracks.c) selects the 3P TT camera
     *     with `hud_setting() == 0`, and that basis is what obj_sort_tick's
     *     distances are measured from;
     *   - obj_visibility_tick's fourth TT pass uses the identical predicate;
     *   - render_scene itself re-reads it later in the same frame.
     * If hud_tick ran LAST those three would read the PREVIOUS frame's toggle,
     * so an R-C press would move the sort basis and the visibility frusta one
     * tick later than it moved the drawn viewport -- the tick and the draw
     * disagreeing about how many viewports exist.
     *
     * Running it first is safe, not merely earlier: hud_tick consumes input
     * EDGES (input_pressed, sampled at the tick boundary by input_update before
     * mode_game is entered), it consumes no RNG at all, and its writes
     * (gHudToggleSettings, gMinimapOpacity/gMinimapFade, gRaceStartShowHudStep,
     * the HUD slide/bounce state) are disjoint from every other tick's reads and
     * writes -- none of waves/sort/visibility/animate/fog/weather/presentation
     * touches HUD state. It also restores gActiveCameraID (game_ui.c), so it
     * leaves no camera selection behind for the ticks after it.
     *
     * RAW rate, not pause-gated: this group runs while paused today.
     */
    hud_tick(updateRate);
#ifdef NATIVE_PORT
    workshop_preview_camera_bank_service();
#endif
    /* The three-player TT spectator camera used to advance from render_scene.
     * It feeds this tick's final sort/LOD/visibility basis, so advance it once
     * from fixed-tick authority before any of those consumers. */
    scene_tt_camera_tick(updateRate);
#ifdef NATIVE_PORT
    /* Camera obstruction finalizer. It may publish a native presentation
     * sidecar, but never writes Camera/gCameras or logical consumers. */
    camera_obstruction_tick(updateRate);
#endif
    /* The authoritative half of waves_update, hoisted out of
     * render_scene (tracks.c:410) into the tick. Same guard and same pause gate
     * render_scene applied, evaluated at the same point in the frame -- gIsPaused
     * is decided above (thread3_main.c:481-501) and nothing between here and
     * render_scene touches it, so this observes the identical value. */
    if (gWaveBlockCount) {
        waves_tick(is_game_paused() ? 0 : updateRate);
    }
    /* The authoritative object ORDER. Render used to
     * partition and distance-sort gObjPtrList once per viewport; that array is what
     * obj_update / process_object_interactions / checkpoint_update_all iterate, so
     * drawing chose the next tick's collision and RNG order. Placed before
     * obj_animate_tick, mirroring render's own order (the sort ran before the object
     * draw loop that animates). */
    obj_sort_tick();
    /* Commit the final viewport's collision-visible racer LOD and the visual
     * light phase once per fixed tick; render uses local read-only results. */
    obj_lod_tick();
    /* Racer visibility -- the "was drawn" timer that
     * gates the AI's steering and its RNG -- evaluated from the logical frusta of
     * every viewport rather than from render admission. */
    obj_visibility_tick();
    /* Object animation and obj->curVertData -- the
     * vertex buffer SPHERE COLLISION reads -- hoisted out of render_3d_model
     * (objects.c:4772-4802). Ordered AFTER waves_tick, matching the order
     * render_scene ran them in. */
    obj_animate_tick();
    /* The fog integrator, plus rain_fog ahead of it in the exact
     * order render_scene ran the pair. gFogData is simulation-read --
     * obj_loop_fogchanger latches on gFogData[].fogChanger, and
     * object_functions.c's TT/Taj cutscene objects call get_fog_settings() to
     * save the current fog before slowly_change_fog() fades it -- so the fade
     * has to advance once per TICK, not once per draw. Same pause gate
     * render_scene applied. */
    fog_tick(is_game_paused() ? 0 : updateRate);
    /*
     * The presentation accumulators -- colour cycles, pulsating lights, the
     * skydome scroll and the particle texture scroll. Same pause gate.
     *
     * ORDER: preserve render_scene's authored sky-before-object sequence.
     * Texture/HUD dice keep the authored RNG stream in original cadence and use
     * the presentation stream in enhanced cadence.
     */
    scene_presentation_tick(is_game_paused() ? 0 : updateRate);
    /* Commit opacity, ordinary-object texture animation, weather and HUD RNG in
     * the exact object -> weather -> HUD order the canonical draw owned. */
    scene_authoritative_render_tick(updateRate);
    /* Hash authoritative state around the render traversal, so a render-side
     * mutation of it is detectable (no-op unless MDKR_RENDER_CENSUS=1). */
    if (!sRollbackResimulating) {
        extern void mdkr_render_census_pre(void);
        extern void mdkr_render_census_post(void);
        mdkr_render_census_pre();
        render_scene(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList,
                     &gGameCurrTriList, updateRate);
        mdkr_render_census_post();
    }
#else
    render_scene(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList, &gGameCurrTriList, updateRate);
#endif
#ifdef NATIVE_PORT
    obj_animation_cadence_tick();
#endif
    if (gGameMode == GAMEMODE_INGAME) {
        // Ignore the user's L/R/Z buttons.
        buttonHeldInputs &= ~(L_TRIG | R_TRIG | Z_TRIG);
    }
    if (gPostRaceViewPort) {
        i = menu_postrace(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList, updateRate);
        switch (i) {
            case POSTRACE_OPT_2:
                buttonHeldInputs |= (L_TRIG | Z_TRIG);
                break;
            case POSTRACE_OPT_1:
                gPostRaceViewPort = FALSE;
                func_8006D8F0(-1);
                break;
            case POSTRACE_OPT_4:
                level_properties_reset();
                gDrumstickSceneLoadTimer = 0;
                buttonHeldInputs |= (L_TRIG | R_TRIG);
                break;
            case POSTRACE_OPT_5:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_TRACK_SELECT;
                break;
            case POSTRACE_OPT_8:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_RESULTS;
                break;
            case POSTRACE_OPT_9:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_TROPHY_ROUND;
                break;
            case POSTRACE_OPT_10:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_TROPHY_RESULTS;
                break;
            case POSTRACE_OPT_11:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_UNUSED;
                break;
            case POSTRACE_OPT_12:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_CHARACTER_SELECT;
                break;
            case POSTRACE_OPT_13:
                buttonHeldInputs |= L_TRIG, loadContext = LEVEL_CONTEXT_UNK7;
                break;
        }
    }
    process_onscreen_textbox(updateRate);
    i = textbox_visible();
    if (i != 0) {
        if (i == 2) {
            gIsPaused = TRUE;
        }
        if (textbox_visible() != 2) {
            gIsPaused = FALSE;
            menu_close_dialogue();
        }
    }
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
    /* Whether the shared pause was open BEFORE the menu runs, so the disconnect
     * block-unpause below re-asserts only a pause that already existed. */
    s8 apWasPaused = gIsPaused;
#endif
    if (gIsPaused) {
        i = menu_pause_loop(&gCurrDisplayList, updateRate);
        switch (i) {
            case PAUSE_CONTINUE:
                gIsPaused = FALSE;
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
                /* The host owns the shared pause decision; record the confirm.
                 * If a bound pad is still missing the disconnect tick below
                 * re-asserts the pause, so this unpause does not actually take. */
                if (adventure_party_runtime_is_active()) {
                    adventure_party_trace_emit_interaction(
                        (uint8_t) adventure_party_host_seat(
                            adventure_party_runtime_session()),
                        ADVENTURE_PARTY_ACTION_PAUSE_DECISION,
                        ADVENTURE_PARTY_ARBITRATE_LATCHED);
                }
#endif
                break;
            case PAUSE_RESET:
                sound_clear_delayed();
                reset_delayed_text();
                if (func_80023568() != 0 && is_in_two_player_adventure()) {
                    swap_lead_player();
                }
                buttonHeldInputs |= (L_TRIG | Z_TRIG);
                break;
            case PAUSE_QUIT_LOBBY:
                sound_clear_delayed();
                reset_delayed_text();
                if (func_80023568() != 0 && is_in_two_player_adventure()) {
                    swap_lead_player();
                }
                buttonHeldInputs |= L_TRIG;
                break;
            case PAUSE_QUIT_TRACKS:
                loadContext = LEVEL_CONTEXT_TRACK_SELECT;
                reset_delayed_text();
                buttonHeldInputs |= L_TRIG;
                break;
            case PAUSE_QUIT_CHARSELECT:
                loadContext = LEVEL_CONTEXT_CHARACTER_SELECT;
                reset_delayed_text();
                buttonHeldInputs |= L_TRIG;
                break;
            case PAUSE_OPT_6:
                gIsPaused = FALSE;
                break;
            case PAUSE_QUIT_CHALLENGE:
                mode_end_taj_race(CHALLENGE_END_QUIT);
                gIsPaused = FALSE;
                break;
            case PAUSE_OPT_4:
                gDrumstickSceneLoadTimer = 0;
                sound_clear_delayed();
                reset_delayed_text();
                level_properties_reset();
                buttonHeldInputs |= (L_TRIG | R_TRIG);
                break;
        }
    }
#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
    /* Block unpause: after the pause menu has run, re-assert the shared pause if a
     * bound controller is still missing (overrides a host CONTINUE). Only ever
     * re-asserts a pause that was already open -- Part A owns the guarded open. */
    adventure_party_disconnect_block_unpause(apWasPaused);
#endif
    if (!sRollbackResimulating
#ifdef NATIVE_PORT
        && (!mdkr_net_roster_runtime_active() ||
            mdkr_net_roster_runtime_viewport_count(1u) > 0u)
#endif
    ) {
        rdp_init(&gCurrDisplayList);
        divider_draw(&gCurrDisplayList);
        hud_render_general(
            &gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList,
            updateRate);
        divider_clear_coverage(&gCurrDisplayList);
    }
#if MDKR_ENABLE_ONLINE_BETA
    else if (!sRollbackResimulating) {
        /* Zero-viewport endpoint: skip the HUD draw, not the HUD audio
         * authority. hud_audio_update() advances the crowd/delayed-voice
         * lifecycle and restores the post-fade active-sound limit
         * (sndp_set_active_sound_limit), and the sound-state pool it shapes
         * decides when Object_Racer::soundMask releases -- a gate
         * play_random_character_voice() consults before drawing gameplay RNG.
         * Rendering endpoints reach this same call through
         * hud_render_general() at this exact point in the tick, so a
         * verifier endpoint must run it too or its RNG stream forks from the
         * rendering peers'. Wrapped in MDKR_ENABLE_ONLINE_BETA so the OFF
         * build's thread3_main object stays byte-for-byte (a roster can only
         * be active in an online epoch). */
        hud_audio_update(updateRate);
    }
#endif
    if (gFutureFunLandLevelTarget) {
        if (func_800214C4() != 0) {
            gPlayableMapId = ASSET_LEVEL_FUTUREFUNLANDHUB;
            D_801234F8 = TRUE;
            gGameCurrentEntrance = 0;
            gFutureFunLandLevelTarget = FALSE;
        }
    }
    sp3C = FALSE;
    if (gDrumstickSceneLoadTimer) {
        gDrumstickSceneLoadTimer -= updateRate;
        if (gDrumstickSceneLoadTimer <= 0) {
            gDrumstickSceneLoadTimer = 0;
            level_properties_push(ASSET_LEVEL_CENTRALAREAHUB, 0, VEHICLE_CAR, CUTSCENE_ID_NONE);
            level_properties_push(ASSET_LEVEL_WIZPIGAMULETSEQUENCE, 0, -1, CUTSCENE_ID_UNK_A);
            sp3C = TRUE;
        }
    }
    if (gLevelLoadTimer > 0) {
        gLevelLoadTimer -= updateRate;
        if (gLevelLoadTimer <= 0) {
            buttonHeldInputs = L_TRIG;
            sp3C = TRUE;
            switch (gLevelLoadType) {
                case LEVEL_LOAD_UNK1:
                    buttonHeldInputs = (L_TRIG | Z_TRIG);
                    break;
                case LEVEL_LOAD_TROPHY_RACE:
                    loadContext = LEVEL_CONTEXT_TROPHY_ROUND;
                    trophyround_adventure();
                    D_801234FC = 2;
                    break;
                case LEVEL_LOAD_LIGHTHOUSE_CUTSCENE:
                    gFutureFunLandLevelTarget = TRUE;
                    // fall-through
                case LEVEL_LOAD_FUTURE_FUN_LAND:
                    D_801234F8 = TRUE;
                    gPlayableMapId = gNextMap;
                    gGameCurrentEntrance = 0;
                    gGameCurrentCutscene = 0;
                    buttonHeldInputs = 0;
                    break;
            }
            gLevelLoadType = LEVEL_LOAD_NORMAL;
            gLevelLoadTimer = 0;
        }
    }
    if (sp3C) {
        if (level_properties_get() != 0) {
            level_properties_pop(&gPlayableMapId, &gGameCurrentEntrance, &i, &gGameCurrentCutscene);
            set_frame_blackout_timer();
            if (gPlayableMapId < 0) {
                if (gPlayableMapId == SPECIAL_MAP_ID_NO_LEVEL || gPlayableMapId == SPECIAL_MAP_ID_UNK_NEG10) {
                    if (gPlayableMapId == SPECIAL_MAP_ID_UNK_NEG10 && is_in_two_player_adventure()) {
                        swap_lead_player();
                    }
                    buttonHeldInputs |= L_TRIG;
                    D_801234FC = 2;
                } else {
                    buttonHeldInputs = 0;
                    D_801234FC = 1;
                    loadContext = LEVEL_CONTEXT_CREDITS;
                }
            } else {
                D_801234FC = 0;
                D_801234F8 = TRUE;
                buttonHeldInputs = 0;
            }
        }
    } else {
        sp3C = func_8006C300();
        if (level_properties_get()) {
            if (gLevelLoadTimer == 0) {
                i = func_800214C4();
                if ((i != 0) || ((buttonPressedInputs & A_BUTTON) && (sp3C != 0))) {
                    if (sp3C != 0) {
                        music_change_on();
                    }
                    set_frame_blackout_timer();
                    level_properties_pop(&gPlayableMapId, &gGameCurrentEntrance, &i, &gGameCurrentCutscene);
                    if (gPlayableMapId < 0) {
                        if (gPlayableMapId == -1 || gPlayableMapId == -10) {
                            if (gPlayableMapId == -10 && is_in_two_player_adventure()) {
                                swap_lead_player();
                            }
                            buttonHeldInputs |= L_TRIG;
                            D_801234FC = 2;
                        } else {
                            buttonHeldInputs = 0;
                            D_801234FC = 1;
                            loadContext = LEVEL_CONTEXT_CREDITS;
                        }
                    } else {
                        D_801234F8 = TRUE;
                    }
                }
            }
        }
    }
    if ((buttonHeldInputs & L_TRIG && gGameMode == GAMEMODE_INGAME) || D_801234FC != 0) {
        gIsPaused = FALSE;
        gLevelLoadTimer = 0;
        gPostRaceViewPort = FALSE;
        unload_level_game();
        safe_mark_write_save_file(get_save_file_index());
        if (loadContext) {
            gIsLoading = FALSE;
            switch (loadContext) {
                case LEVEL_CONTEXT_TRACK_SELECT:
                    // Go to track select menu from "Select Track" option in tracks menu.
                    load_menu_with_level_background(MENU_TRACK_SELECT, SPECIAL_MAP_ID_NO_LEVEL, 1);
                    break;
                case LEVEL_CONTEXT_RESULTS:
                    load_menu_with_level_background(MENU_RESULTS, ASSET_LEVEL_TROPHYRACE, 0);
                    break;
                case LEVEL_CONTEXT_TROPHY_ROUND:
                    load_menu_with_level_background(MENU_TROPHY_RACE_ROUND, ASSET_LEVEL_TROPHYRACE, 0);
                    break;
                case LEVEL_CONTEXT_TROPHY_RESULTS:
                    load_menu_with_level_background(MENU_TROPHY_RACE_RANKINGS, ASSET_LEVEL_TROPHYRACE, 0);
                    break;
                case LEVEL_CONTEXT_UNUSED:
                    // Trophy race related?
                    load_menu_with_level_background(MENU_UNUSED_22, ASSET_LEVEL_TROPHYRACE, 0);
                    break;
                case LEVEL_CONTEXT_CHARACTER_SELECT:
                    // Go to character select menu from "Select Character" option in tracks menu.
                    i = 0;
                    if (is_drumstick_unlocked()) {
                        i ^= 1;
                    }
                    if (is_tt_unlocked()) {
                        i ^= 3;
                    }
                    charselect_prev(1, 0);
                    load_menu_with_level_background(MENU_CHARACTER_SELECT, ASSET_LEVEL_CHARACTERSELECT, i);
                    break;
                case LEVEL_CONTEXT_UNK7:
                    gIsLoading = TRUE;
                    load_menu_with_level_background(MENU_NEWGAME_CINEMATIC, SPECIAL_MAP_ID_NO_LEVEL, 0);
                    gIsLoading = FALSE;
                    break;
                case LEVEL_CONTEXT_CREDITS:
                    load_menu_with_level_background(MENU_CREDITS, SPECIAL_MAP_ID_NO_LEVEL, 0);
                    break;
            }
        } else if (D_801234FC == 1) {
            if (gLevelSettings[2] == -1) {
                load_menu_with_level_background(MENU_UNUSED_8, SPECIAL_MAP_ID_NO_LEVEL, 0);
            } else {
                gIsLoading = TRUE;
                load_menu_with_level_background(MENU_TRACK_SELECT_ADVENTURE, SPECIAL_MAP_ID_NO_LEVEL, -1);
            }
        } else if (!(buttonHeldInputs & R_TRIG)) {
            if (!(buttonHeldInputs & Z_TRIG)) {
                gPlayableMapId = gLevelSettings[0];
                gGameCurrentEntrance = gLevelSettings[15];
                gGameCurrentCutscene = gLevelSettings[gLevelSettings[1] + 8];
                gLevelDefaultVehicleID = leveltable_vehicle_default(gPlayableMapId);
                if (gGameCurrentCutscene < 0) {
                    gGameCurrentCutscene = CUTSCENE_ID_UNK_64;
                }
            }
            load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
        } else {
            safe_mark_write_save_file(get_save_file_index());
            load_menu_with_level_background(MENU_TITLE, SPECIAL_MAP_ID_NO_LEVEL, 0);
        }
        D_801234FC = 0;
    }
    if (D_801234F8) {
        gPostRaceViewPort = FALSE;
        unload_level_game();
        load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
        safe_mark_write_save_file(get_save_file_index());
        D_801234F8 = FALSE;
    }
#ifdef NATIVE_PORT
    /* Preserve the retail three-player TT-camera side effect without making
     * every paused layout inherit it. The general per-frame clear remains in
     * main_game_loop and deliberately does not fire while paused: when the app
     * overlay opens during presentation, that retained latch is what keeps the
     * just-drawn cutscene bank selected until simulation resumes. */
    if (cam_get_viewport_layout() == VIEWPORT_LAYOUT_3_PLAYERS &&
        level_type() != RACETYPE_CHALLENGE_EGGS &&
        level_type() != RACETYPE_CHALLENGE_BATTLE &&
        level_type() != RACETYPE_CHALLENGE_BANANAS && hud_setting() == 0) {
        disable_cutscene_camera();
    }
#endif
}

#ifdef NATIVE_PORT
s32 mdkr_game_resimulate_tick(
    s32 updateRate, const MdkrInputSample input[MDKR_INPUT_PORTS]) {
    s32 ok;
    if (sRollbackResimulating || input == NULL || updateRate <= 0 ||
        gGameMode != GAMEMODE_INGAME || gIsPaused ||
        D_801234FC != 0 || D_801234F8 || gLevelLoadTimer != 0 ||
        textbox_visible() != 0) {
        fprintf(stderr,
                "[ROLLBACK] game-tick admission rejected active=%d input=%d "
                "rate=%d mode=%d paused=%d postrace=%d loadA=%d loadB=%d "
                "levelTimer=%d textbox=%d\n",
                sRollbackResimulating, input != NULL, updateRate, gGameMode,
                gIsPaused, gPostRaceViewPort, D_801234FC, D_801234F8,
                gLevelLoadTimer, textbox_visible());
        return FALSE;
    }
    input_rollback_apply(input);
    sRollbackResimulating = TRUE;
    mode_game(updateRate);
    if (gGameMode == GAMEMODE_INGAME) {
        (void)transition_update(updateRate);
    }
    ok = gGameMode == GAMEMODE_INGAME && !gIsPaused &&
         D_801234FC == 0 && !D_801234F8;
    if (!ok) {
        fprintf(stderr,
                "[ROLLBACK] game-tick completion rejected mode=%d paused=%d "
                "postrace=%d loadA=%d loadB=%d\n",
                gGameMode, gIsPaused, gPostRaceViewPort,
                D_801234FC, D_801234F8);
    }
    sRollbackResimulating = FALSE;
    return ok;
}
#endif

/**
 * Reset dialogue and set the transition effect for the cutscene showing an unlocked Drumstick.
 */
void set_drumstick_unlock_transition(void) {
    gDrumstickSceneLoadTimer = 44;
    gIsPaused = 0;
    menu_close_dialogue();
    transition_begin(&gDrumstickSceneTransition);
}

/**
 * Set the postrace viewport var to match the finish state.
 * The game never actually uses this beyond checking it's nonzero.
 */
void race_postrace_type(s32 finishState) {
    gPostRaceViewPort = finishState + 1;
}

void func_8006D8F0(UNUSED s32 arg0) {
    s32 temp;
    if (gGameMode != GAMEMODE_UNUSED_4) {
        gPlayableMapId = gLevelSettings[0];
        gGameCurrentEntrance = 0;
        gGameCurrentCutscene = CUTSCENE_ID_UNK_64;
        temp = gLevelSettings[1];
        if (gLevelSettings[15] >= 0) {
            gGameCurrentEntrance = gLevelSettings[15];
        }
        if (gLevelSettings[temp + 8] >= 0) {
            gGameCurrentCutscene = gLevelSettings[temp + 8];
        }
        D_801234F8 = TRUE;
    }
}

void func_8006D968(s8 *arg0) {
    // Is arg0 LevelObjectEntry_Exit?
    s32 i;
    if (gGameMode != GAMEMODE_UNUSED_4) {
        gLevelSettings[0] = gPlayableMapId;
        for (i = 0; i < 2; i++) {
            gLevelSettings[i + 2] = arg0[i + 8];   // 0x8-0x9 - destinationMapId
            gLevelSettings[i + 4] = arg0[i + 10];  // 0xA-0xB - overworldSpawnIndex
            gLevelSettings[i + 6] = arg0[i + 12];  // 0xC-0xD - ?
            gLevelSettings[i + 8] = arg0[i + 14];  // 0xE-0xF - ?
            gLevelSettings[i + 10] = arg0[i + 18]; // 0x12-0x13 - ?
            gLevelSettings[i + 12] = arg0[i + 20]; // 0x14-0x15 - ?
        }
        gLevelSettings[14] = arg0[22]; // 0x16 - ?
        gLevelSettings[15] = arg0[23]; // 0x17 returnSpawnIndex
        D_801234FC = 1;
    }
}

/**
 * Returns the current game mode.
 */
GameMode get_game_mode(void) {
    return gGameMode;
}

/**
 *  Sets the current game mode.
 *  Official Name: mainSetMode?
 */
UNUSED void set_game_mode(s32 changeTo) {
    gGameMode = changeTo;
}

/**
 * Sets up and loads a level to be used in the background of the menu that's about to be set up.
 * Used for every kind of menu that's not ingame.
 */
void load_menu_with_level_background(s32 menuId, s32 levelId, s32 cutsceneId) {
    alloc_displaylist_heap(PLAYER_ONE);
    gGameMode = GAMEMODE_MENU;
    gRenderMenu = TRUE;
    sndp_set_group_volume(0, AL_SNDP_GROUP_VOLUME_MAX);
    sndp_set_group_volume(1, AL_SNDP_GROUP_VOLUME_MAX);
    sndp_set_group_volume(2, AL_SNDP_GROUP_VOLUME_MAX);
    cam_init();

    if (!gIsLoading) {
        gIsLoading = FALSE;
        if (levelId < 0) {
            gIsLoading = TRUE;
        } else {
            load_level_menu(levelId, -1, 0, VEHICLE_PLANE, cutsceneId);
        }
    }
    if (menuId == MENU_UNUSED_2 || menuId == MENU_LOGOS || menuId == MENU_TITLE) {
        reset_title_logo_scale();
    }
    menu_init(menuId);
    gGameCurrentEntrance = 0;
}

/**
 * Set the default vehicle option from the current loaded level.
 */
void set_level_default_vehicle(Vehicle vehicleID) {
    gLevelDefaultVehicleID = vehicleID;
}

/**
 * Sets the vehicle option that the next level loaded for a menu may use.
 */
void set_vehicle_id_for_menu(Vehicle vehicleId) {
    stubbed_printf("Swapping\n");
    gMenuVehicleID = vehicleId;
}

/**
 * Get the default vehicle option, set by a loaded level.
 */
Vehicle get_level_default_vehicle(void) {
    return gLevelDefaultVehicleID;
}

/**
 * Calls level_load() with the same arguments, but also does some other stuff.
 * Used for menus.
 */
void load_level_menu(s32 levelId, s32 numberOfPlayers, s32 entranceId, Vehicle vehicleId, s32 cutsceneId) {
#ifdef NATIVE_PORT
    /* The level apply boundary, for the same reason as load_level_game's. A
     * menu background is a track load like any other, and servicing it here is
     * what lets a change made in the pause overlay land at the transition the
     * player is already watching rather than at the one after it. */
    (void)mdkr_video_config_apply_pending(MDKR_VIDEO_SCOPE_LEVEL);
    camera_obstruction_runtime_reset();
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_LEVEL, levelId, numberOfPlayers, entranceId,
        (s32)(((u32)(u16)vehicleId << 16) |
              ((u32)cutsceneId & 0xFFFFu)));
#endif
    mempool_free_timer(0);
    cam_init();
    load_game_text_table();
    level_load(levelId, numberOfPlayers, entranceId, vehicleId, cutsceneId);
    hud_init(cam_get_viewport_layout());
    init_particle_buffers(4, 4, 110, 48, 32, 0);
    ainode_update();
    osSetTime(0);
    mempool_free_timer(2);
}

/**
 * Call numerous functions to clear data in RAM.
 * Then call to free particles, HUD and text.
 */
void unload_level_menu(void) {
    if (!gIsLoading) {
        gIsLoading = TRUE;
        mempool_free_timer(0);
#ifdef NATIVE_PORT
        /* Same reason as unload_level_game's call; see
         * presentation_history_retire. */
        camera_obstruction_runtime_reset();
        presentation_history_retire();
#endif
        level_free();
        transition_begin(&D_800DD3F4);
        reset_particles();
        hud_free();
        free_game_text_table();
        mempool_free_timer(2);
    }
    gIsLoading = FALSE;
}

/**
 * Used in menus, update objects and draw the game.
 * In the tracks menu, this only runs if there's a track actively loaded.
 */
void update_menu_scene(s32 updateRate) {
    if (bgload_active() == FALSE) {
        obj_update(updateRate);
        gParticlePtrList_flush();
        ainode_update();
    #ifdef NATIVE_PORT
    /* The HUD's authoritative half. Runs FIRST -- see the tick ordering contract
     * on the twin call in mode_game. */
    hud_tick(updateRate);
#ifdef NATIVE_PORT
    workshop_preview_camera_bank_service();
#endif
    /* Fixed-tick ownership of the three-player TT spectator camera; see the
     * twin call and tracks.c's scene_tt_camera_tick contract. */
    scene_tt_camera_tick(updateRate);
#ifdef NATIVE_PORT
    /* Camera obstruction finalizer; twin of mode_game's sidecar-only call. */
    camera_obstruction_tick(updateRate);
#endif
    /* Authoritative wave phase, hoisted out of render_scene.
     * See the twin call in mode_game. */
    if (gWaveBlockCount) {
        waves_tick(is_game_paused() ? 0 : updateRate);
    }
    /* The authoritative object ORDER. Render used to
     * partition and distance-sort gObjPtrList once per viewport; that array is what
     * obj_update / process_object_interactions / checkpoint_update_all iterate, so
     * drawing chose the next tick's collision and RNG order. Placed before
     * obj_animate_tick, mirroring render's own order (the sort ran before the object
     * draw loop that animates). */
    obj_sort_tick();
    obj_lod_tick();
    /* Racer visibility -- the "was drawn" timer that
     * gates the AI's steering and its RNG -- evaluated from the logical frusta of
     * every viewport rather than from render admission. */
    obj_visibility_tick();
    /* Object animation and obj->curVertData -- the
     * vertex buffer SPHERE COLLISION reads -- hoisted out of render_3d_model
     * (objects.c:4772-4802). Ordered AFTER waves_tick, matching the order
     * render_scene ran them in. */
    obj_animate_tick();
    /* The fog integrator, plus rain_fog ahead of it in the exact
     * order render_scene ran the pair. gFogData is simulation-read --
     * obj_loop_fogchanger latches on gFogData[].fogChanger, and
     * object_functions.c's TT/Taj cutscene objects call get_fog_settings() to
     * save the current fog before slowly_change_fog() fades it -- so the fade
     * has to advance once per TICK, not once per draw. Same pause gate
     * render_scene applied. */
    fog_tick(is_game_paused() ? 0 : updateRate);
    /* The presentation accumulators, same pause gate. Must precede the
     * authoritative object/weather/HUD traversal -- see the ORDER note on the
     * twin call in mode_game. */
    scene_presentation_tick(is_game_paused() ? 0 : updateRate);
    scene_authoritative_render_tick(updateRate);
    /* Hash authoritative state around the render traversal, so a render-side
     * mutation of it is detectable (no-op unless MDKR_RENDER_CENSUS=1). */
    {
        extern void mdkr_render_census_pre(void);
        extern void mdkr_render_census_post(void);
        mdkr_render_census_pre();
        render_scene(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList,
                     &gGameCurrTriList, updateRate);
        mdkr_render_census_post();
    }
#else
    render_scene(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList, &gGameCurrTriList, updateRate);
#endif
#ifdef NATIVE_PORT
    obj_animation_cadence_tick();
#endif
        process_onscreen_textbox(updateRate);
        rdp_init(&gCurrDisplayList);
        divider_draw(&gCurrDisplayList);
        divider_clear_coverage(&gCurrDisplayList);
    }
}

/**
 * Main function for handling behaviour in menus.
 * Runs the menu code, with a simplified object update and scene rendering system.
 */
void mode_menu(s32 updateRate) {
    s32 menuLoopResult;
    s32 temp;
    s32 playerVehicle;
    s32 temp5;

    gIsPaused = FALSE;
    gPostRaceViewPort = 0;
    if (!gIsLoading && gRenderMenu) {
        update_menu_scene(updateRate);
    }
    menuLoopResult =
        menu_loop(&gCurrDisplayList, &gGameCurrMatrix, &gGameCurrVertexList, &gGameCurrTriList, updateRate);
    gRenderMenu = TRUE;
    if (menuLoopResult == -2) {
        gRenderMenu = FALSE;
        return;
    }
    if (menuLoopResult != -1 && menuLoopResult & MENU_RESULT_FLAGS_200) {
        unload_level_menu();
        gCurrDisplayList = gDisplayLists[gSPTaskNum];
        gDPFullSync(gCurrDisplayList++);
        gSPEndDisplayList(gCurrDisplayList++);
        gPlayableMapId = menuLoopResult & 0x7F;
        gLevelDefaultVehicleID = leveltable_vehicle_default(gPlayableMapId);
        gGameCurrentEntrance = 0;
        gGameCurrentCutscene = CUTSCENE_ID_UNK_64;
        gGameMode = GAMEMODE_INGAME;
        gIsPaused = FALSE;
        gPostRaceViewPort = 0;
        load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
        safe_mark_write_save_file(get_save_file_index());
        return;
    }
    if (menuLoopResult != -1 && menuLoopResult & MENU_RESULT_FLAGS_100) {
        unload_level_game();
        gIsPaused = FALSE;
        gPostRaceViewPort = 0;
        switch (menuLoopResult & 0x7F) {
            case MENU_RESULT_TRACKS_MODE:
                load_menu_with_level_background(MENU_TRACK_SELECT, SPECIAL_MAP_ID_NO_LEVEL, 1);
                break;
            case MENU_RESULT_UNK14:
                gPlayableMapId = ASSET_LEVEL_CENTRALAREAHUB;
                gGameCurrentEntrance = 0;
                gGameCurrentCutscene = CUTSCENE_ID_UNK_64;
                gGameMode = GAMEMODE_INGAME;
                load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
                safe_mark_write_save_file(get_save_file_index());
                break;
            case MENU_RESULT_RETURN_TO_GAME:
                gGameCurrentEntrance = 0;
                gPlayableMapId = gLevelSettings[0];
                gGameCurrentCutscene = CUTSCENE_ID_UNK_64;
                gGameMode = GAMEMODE_INGAME;
                temp5 = gLevelSettings[1];
                if (gLevelSettings[15] >= 0) {
                    gGameCurrentEntrance = gLevelSettings[15];
                }
                temp = gLevelSettings[temp5 + 8];
                if (temp >= 0) {
                    gGameCurrentCutscene = temp;
                }
                load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
                safe_mark_write_save_file(get_save_file_index());
                break;
            case MENU_RESULT_UNK2:
                gGameMode = GAMEMODE_INGAME;
                load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
                break;
            case MENU_RESULT_UNK3:
                gGameMode = GAMEMODE_INGAME;
                gPlayableMapId = gLevelSettings[0];
                gGameCurrentEntrance = gLevelSettings[15];
                gGameCurrentCutscene = gLevelSettings[gLevelSettings[1] + 8];
                gLevelDefaultVehicleID = leveltable_vehicle_default(gPlayableMapId);
                load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, gLevelDefaultVehicleID);
                break;
            default:
                load_menu_with_level_background(MENU_TITLE, SPECIAL_MAP_ID_NO_LEVEL, 0);
                break;
        }
        return;
    }
    if (menuLoopResult & MENU_RESULT_FLAGS_80 && menuLoopResult != -1) {
        unload_level_menu();
        gCurrDisplayList = gDisplayLists[gSPTaskNum];
        gDPFullSync(gCurrDisplayList++);
        gSPEndDisplayList(gCurrDisplayList++);

        menuLoopResult &= 0x7f;
        gLevelSettings[1] = menuLoopResult;
        gLevelSettings[0] = gPlayableMapId;

        gPlayableMapId = gLevelSettings[menuLoopResult + 2];
        gGameCurrentEntrance = gLevelSettings[menuLoopResult + 4];
        gGameMode = GAMEMODE_INGAME;
        gGameCurrentCutscene = gLevelSettings[menuLoopResult + 12];
        playerVehicle = get_player_selected_vehicle(PLAYER_ONE);
        gGameNumPlayers = gSettingsPtr->gNumRacers - 1;
        load_level_game(gPlayableMapId, gGameNumPlayers, gGameCurrentEntrance, playerVehicle);
        D_801234FC = 0;
        gLevelDefaultVehicleID = gMenuVehicleID;
        return;
    }
    if (menuLoopResult > 0) {
        unload_level_menu();
        gCurrDisplayList = gDisplayLists[gSPTaskNum];
        gDPFullSync(gCurrDisplayList++);
        gSPEndDisplayList(gCurrDisplayList++);
        gGameMode = GAMEMODE_INGAME;
        load_next_ingame_level(menuLoopResult, -1, gLevelDefaultVehicleID);
        if (gSettingsPtr->newGame && !is_in_tracks_mode()) {
            music_change_on();
            gSettingsPtr->newGame = FALSE;
        }
    }
}

/**
 * Loads a level, intended to be used in a menu.
 * Skips loading many things, otherwise used in gameplay.
 */
void load_level_for_menu(s32 levelId, s32 numberOfPlayers, s32 cutsceneId) {
    Vehicle vehicleId;

    if (!gIsLoading) {
        unload_level_menu();
        if (bgload_active() == FALSE) {
            gCurrDisplayList = gDisplayLists[gSPTaskNum];
            gDPFullSync(gCurrDisplayList++);
            gSPEndDisplayList(gCurrDisplayList++);
        }
    }
    if (levelId != (s32) SPECIAL_MAP_ID_NO_LEVEL) {
#ifdef NATIVE_PORT
        extern int mdkr_menu_vehicle_legacy(void);
        vehicleId = mdkr_menu_vehicle_legacy()
                        ? VEHICLE_PLANE
                        : leveltable_vehicle_default(levelId);
#else
        //!@bug: Forcing the plane here makes all AI use plane paths.
        //! This can be seen most evidently in the Ancient Lake demo.
        vehicleId = VEHICLE_PLANE;
#endif
        load_level_menu(levelId, numberOfPlayers, 0, vehicleId, cutsceneId);
        gIsLoading = FALSE;
        return;
    }
    gIsLoading = TRUE;
}

/**
 * Initialise global game settings data.
 * Allocate space to accomodate it then set the start points for each data point.
 */
void calc_and_alloc_heap_for_settings(void) {
    s32 dataSize;
    u32 sizes[15];
    s32 numWorlds, numLevels;

    level_global_init();
    reset_character_id_slots();
    level_count(&numLevels, &numWorlds);
    sizes[0] = sizeof(Settings);
    sizes[1] = sizes[0] + (numLevels * 4); // balloonsPtr
    sizes[2] = sizes[1] + (numWorlds * 2); // flapInitialsPtr[0]
    dataSize = (numLevels * 2);
    sizes[3] = sizes[2] + dataSize;   // flapInitialsPtr[1]
    sizes[4] = sizes[3] + dataSize;   // flapInitialsPtr[2]
    sizes[5] = sizes[4] + dataSize;   // flapTimesPtr[0]
    sizes[6] = sizes[5] + dataSize;   // flapTimesPtr[1]
    sizes[7] = sizes[6] + dataSize;   // flapTimesPtr[2]
    sizes[8] = sizes[7] + dataSize;   // courseInitialsPtr[0]
    sizes[9] = sizes[8] + dataSize;   // courseInitialsPtr[1]
    sizes[10] = sizes[9] + dataSize;  // courseInitialsPtr[2]
    sizes[11] = sizes[10] + dataSize; // courseTimesPtr[0]
    sizes[12] = sizes[11] + dataSize; // courseTimesPtr[1]
    sizes[13] = sizes[12] + dataSize; // courseTimesPtr[2]
    sizes[14] = sizes[13] + dataSize; // total size

    gSettingsPtr = mempool_alloc_safe(sizes[14], COLOUR_TAG_WHITE);
    gSettingsPtr->courseFlagsPtr = (s32 *) ((u8 *) gSettingsPtr + sizes[0]);
    gSettingsPtr->balloonsPtr = (s16 *) ((u8 *) gSettingsPtr + sizes[1]);
    gSettingsPtr->tajFlags = 0;
    gSettingsPtr->flapInitialsPtr[0] = (u16 *) ((u8 *) gSettingsPtr + sizes[2]);
    gSettingsPtr->flapInitialsPtr[1] = (u16 *) ((u8 *) gSettingsPtr + sizes[3]);
    gSettingsPtr->flapInitialsPtr[2] = (u16 *) ((u8 *) gSettingsPtr + sizes[4]);
    gSettingsPtr->flapTimesPtr[0] = (u16 *) ((u8 *) gSettingsPtr + sizes[5]);
    gSettingsPtr->flapTimesPtr[1] = (u16 *) ((u8 *) gSettingsPtr + sizes[6]);
    gSettingsPtr->flapTimesPtr[2] = (u16 *) ((u8 *) gSettingsPtr + sizes[7]);
    gSettingsPtr->courseInitialsPtr[0] = (u16 *) ((u8 *) gSettingsPtr + sizes[8]);
    gSettingsPtr->courseInitialsPtr[1] = (u16 *) ((u8 *) gSettingsPtr + sizes[9]);
    gSettingsPtr->courseInitialsPtr[2] = (u16 *) ((u8 *) gSettingsPtr + sizes[10]);
    gSettingsPtr->courseTimesPtr[0] = (u16 *) ((u8 *) gSettingsPtr + sizes[11]);
    gSettingsPtr->courseTimesPtr[1] = (u16 *) ((u8 *) gSettingsPtr + sizes[12]);
    gSettingsPtr->courseTimesPtr[2] = (u16 *) ((u8 *) gSettingsPtr + sizes[13]);
    gSettingsPtr->unk4C = (Settings4C *) &gLevelSettings;
    gSaveDataFlags = // Set bits 0/1/2/8 and wipe out all others
        SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES | SAVE_DATA_FLAG_READ_SAVE_DATA |
        SAVE_DATA_FLAG_READ_EEPROM_SETTINGS;
#ifdef NATIVE_PORT
    mdkr_save_inventory_selftest();
#endif
}

/**
 * Set the init values for each racer based on which character they are and which player they are.
 * Then reset race status.
 */
void init_racer_headers(void) {
    s32 i, j;
    gSettingsPtr->gNumRacers = mdkr_authoritative_player_count(
        get_number_of_active_players());
    for (i = 0; i < 8; i++) {
        gSettingsPtr->racers[i].best_times = 0;
        gSettingsPtr->racers[i].character = get_character_id_from_slot(i);
        if (gSettingsPtr->gNumRacers >= 2) {
            gSettingsPtr->racers[i].starting_position = i;
        } else if (is_in_two_player_adventure()) {
            gSettingsPtr->racers[i].starting_position = 5 - i;
        } else {
            gSettingsPtr->racers[i].starting_position = 7 - i;
        }
        gSettingsPtr->racers[i].unk7 = 0;
        for (j = 0; j < 4; j++) {
            gSettingsPtr->racers[i].placements[j] = 0;
        }
        gSettingsPtr->racers[i].course_time = 0;
        for (j = 0; j < 3; j++) {
            gSettingsPtr->racers[i].lap_times[j] = 0;
        }
    }
#ifdef NATIVE_PORT
    {
        const MdkrMatchLaunchDescriptorV1 *launch =
            mdkr_net_roster_runtime_launch_descriptor();
        if (launch != NULL) {
            fprintf(stderr,
                    "[NET-SELECTIONS] epoch=%u racers="
                    "0:%u/%u,1:%u/%u,2:%u/%u,3:%u/%u "
                    "source=launch-descriptor\n",
                    launch->manifest.match_epoch,
                    (unsigned)gSettingsPtr->racers[0].character,
                    (unsigned)get_player_selected_vehicle(0),
                    (unsigned)gSettingsPtr->racers[1].character,
                    (unsigned)get_player_selected_vehicle(1),
                    (unsigned)gSettingsPtr->racers[2].character,
                    (unsigned)get_player_selected_vehicle(2),
                    (unsigned)gSettingsPtr->racers[3].character,
                    (unsigned)get_player_selected_vehicle(3));
        }
    }
#endif
    gSettingsPtr->timeTrialRacer = 0;
    gSettingsPtr->unk115[0] = 0;
    gSettingsPtr->unk115[1] = 0;
    gSettingsPtr->display_times = 0;
    gSettingsPtr->worldId = 0;
    gSettingsPtr->courseId = 0;
}

/**
 * Depending on flags, clear fastest lap times and/or overall course times.
 */
void clear_lap_records(Settings *settings, s32 flags) {
    s32 i, j;
    s32 numWorlds, numLevels;
    s32 index;
    u16 *temp_v0;
#ifdef NATIVE_PORT
    s32 retailLevels, retailWorlds;
    level_count_for_save(&retailLevels, &retailWorlds);
#endif

    level_count(&numLevels, &numWorlds);
    temp_v0 = (u16 *) get_misc_asset(ASSET_MISC_23);
    for (i = 0; i < NUMBER_OF_SAVE_FILES; i++) {
        for (j = 0; j < numLevels; j++) {
#ifdef NATIVE_PORT
            /* ASSET_MISC_23 contains retail defaults only. Zero denotes no
             * record in the finish-time comparison; custom progress is restored
             * separately after the retail codec has initialized settings. */
            if (j >= retailLevels) {
                if (flags & 1) {
                    settings->flapInitialsPtr[i][j] = 0;
                    settings->flapTimesPtr[i][j] = 0;
                }
                if (flags & 2) {
                    settings->courseInitialsPtr[i][j] = 0;
                    settings->courseTimesPtr[i][j] = 0;
                }
                continue;
            }
#endif
            index = (j * 12) + (i * 4);
            if (flags & 1) {
                settings->flapInitialsPtr[i][j] = temp_v0[index + 3];
                settings->flapTimesPtr[i][j] = temp_v0[index + 2];
            }
            if (flags & 2) {
                settings->courseInitialsPtr[i][j] = temp_v0[index + 1];
                settings->courseTimesPtr[i][j] = temp_v0[index];
            }
        }
    }
}

/**
 * Set all game progression values to their default, as if it were a new game.
 */
void clear_game_progress(Settings *settings) {
    s32 i;
    s32 worldCount;
    s32 levelCount;

    level_count(&levelCount, &worldCount);
    settings->newGame = TRUE;

    for (i = 0; i < worldCount; i++) {
        settings->balloonsPtr[i] = 0;
    }
    for (i = 0; i < levelCount; i++) {
        settings->courseFlagsPtr[i] = RACE_UNATTEMPTED;
    }

    settings->keys = 0;
    settings->unkA = 0;
    settings->bosses = 0;
    settings->trophies = 0;
    settings->cutsceneFlags = CUTSCENE_NONE;
    settings->tajFlags = 0;
    settings->ttAmulet = 0;
    settings->wizpigAmulet = 0;
}

/**
 * Call functions to set all game save data to the default.
 */
UNUSED void reset_save_data(void) {
    clear_lap_records(gSettingsPtr, 3);
    clear_game_progress(gSettingsPtr);
}

/**
 * Return the global game settings.
 * This is where global game records and perferences are stored.
 */
Settings *get_settings(void) {
    return gSettingsPtr;
}

/**
 * Returns the value in gIsPaused.
 */
s8 is_game_paused(void) {
#ifdef NATIVE_PORT
    if (platformOverlayWantsPause()
#if MDKR_ENABLE_ONLINE_BETA
        /* Online the app overlay never pauses the sim (main_game_loop keeps
         * the clock running under it), so it must be invisible to the sim's
         * pause query too: this reads LIVE overlay state, and a TRUE here
         * while the online sim runs would gate obj_update differently in the
         * authored pass vs. a correction replay (resim cannot see the live
         * overlay) -- a divergence, and the resimulate admission gate rejects
         * a paused game outright. gIsPaused stays authoritative (it is
         * registered rollback authority and never engages online). */
        && !mdkr_online_race_pause_suppressed()
#endif
    ) {
        return TRUE;
    }
#endif
    return gIsPaused;
}

/**
 * Returns the status of the post-race shrunken viewport.
 */
s8 is_postrace_viewport_active(void) {
    return gPostRaceViewPort;
}

/**
 * Sets and returns (nonzero) the message set when pressing the reset button.
 * Official name: mainResetPressed
 */
s32 is_reset_pressed(void) {
    if (gNMIMesgBuf == 0) {
        gNMIMesgBuf = (s32) ((osRecvMesg(&gNMIMesgQueue, NULL, OS_MESG_NOBLOCK) + 1) != 0);
    }
    return gNMIMesgBuf;
}

/**
 * Returns the current map ID if ingame, since this var is only set ingame.
 */
s32 get_ingame_map_id(void) {
    return gPlayableMapId;
}

/**
 * Marks a flag to read flap times from the eeprom
 */
UNUSED void mark_to_read_flap_times(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_READ_FLAP_TIMES;
}

/**
 * Marks a flag to read course times from the eeprom
 */
UNUSED void mark_to_read_course_times(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_READ_COURSE_TIMES;
}

/**
 * Marks a flag to read both flap times and course times from the eeprom
 */
void mark_to_read_flap_and_course_times(void) {
    gSaveDataFlags |= (SAVE_DATA_FLAG_READ_FLAP_TIMES | SAVE_DATA_FLAG_READ_COURSE_TIMES);
}

/**
 * Marks a flag to read the save file from the passed index from flash.
 */
void mark_read_save_file(s32 saveFileIndex) {
    // Wipe out bits 8 and 9
    gSaveDataFlags &= ~(SAVE_DATA_FLAG_READ_EEPROM_SETTINGS | SAVE_DATA_FLAG_WRITE_EEPROM_SETTINGS);
    // Place saveFileIndex at bits 8 and 9 and set bit 2
    gSaveDataFlags |= (SAVE_DATA_FLAG_READ_SAVE_DATA | ((saveFileIndex & SAVE_DATA_FLAG_INDEX_VALUE) << 8));
}

/**
 * Marks a flag to read all save file data from flash.
 */
void mark_read_all_save_files(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_READ_ALL_SAVE_DATA; // Set bit 3
}

/**
 * Marks a flag to write flap times to the eeprom
 */
void mark_to_write_flap_times(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_WRITE_FLAP_TIMES;
}

/**
 * Marks a flag to write course times to the eeprom
 */
void mark_to_write_course_times(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_WRITE_COURSE_TIMES;
}

/**
 * Marks a flag to write both flap times and course times to the eeprom
 */
void mark_to_write_flap_and_course_times(void) {
    gSaveDataFlags |= (SAVE_DATA_FLAG_WRITE_FLAP_TIMES | SAVE_DATA_FLAG_WRITE_COURSE_TIMES);
}

/**
 * Forcefully marks a flag to write a save file to flash.
 * Official Name: mainSaveGame
 */
void force_mark_write_save_file(s32 saveFileIndex, Settings *source) {
    if (source == NULL) {
        return;
    }
#ifdef NATIVE_PORT
    GAMEPLAY_EVENT_TRACE(
        GAMEPLAY_EVENT_SAVE, saveFileIndex, 2, 0, 0);
#endif
    sWriteSaveSource = source;
    gSaveDataFlags &= ~SAVE_DATA_FLAG_WRITE_SAVE_FILE_NUMBER_BITS; // Wipe out bits 10 and 11
    gSaveDataFlags |= (SAVE_DATA_FLAG_WRITE_SAVE_DATA |
                       ((saveFileIndex & 3) << 10)); // Set bit 6 and place saveFileIndex into bits 10 and 11
}

/**
 * Marks a flag to write a save file to flash as long as we're not in tracks mode, and we're in the draw game render
 * context. This should prevent save data from being overwritten outside of Adventure Mode.
 * Official Name: mainSaveGame2
 */
void safe_mark_write_save_file(s32 saveFileIndex) {
    if (gGameMode == GAMEMODE_INGAME && !is_in_tracks_mode()) {
#ifdef NATIVE_PORT
        GAMEPLAY_EVENT_TRACE(
            GAMEPLAY_EVENT_SAVE, saveFileIndex, 3, 0, 0);
#endif
        sWriteSaveSource = gSettingsPtr;
        gSaveDataFlags &= ~SAVE_DATA_FLAG_WRITE_SAVE_FILE_NUMBER_BITS; // Wipe out bits 10 and 11
        gSaveDataFlags |= (SAVE_DATA_FLAG_WRITE_SAVE_DATA |
                           ((saveFileIndex & 3) << 10)); // Set bit 6 and place saveFileIndex into bits 10 and 11
    }
}

/*
 * A write request owns its source explicitly until the SI boundary consumes
 * it. In particular, Tracks-mode aggregate records live in gSettingsPtr but
 * forced file copies use gSavefileData[]/the Pak scratch slot instead, so an
 * aggregate display can never become persisted progress through call ordering.
 */
Settings *take_write_save_file_source(void) {
    Settings *source = sWriteSaveSource;
    sWriteSaveSource = NULL;
    return source;
}

/**
 * Marks a flag to erase a save file from flash later
 */
void mark_save_file_to_erase(s32 saveFileIndex) {
    // Set bit 7 and and place saveFileIndex into bits 10 and 11 while wiping everything else
    gSaveDataFlags = SAVE_DATA_FLAG_ERASE_SAVE_DATA | ((saveFileIndex & 3) << 10);
}

/**
 * Marks a flag to read eeprom settings from flash later
 * @bug: Because this is the same bit used for reading save files,
 *       it will change the save file number to read from
 */
UNUSED void mark_read_eeprom_settings(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_READ_EEPROM_SETTINGS; // Set bit 8
}

/**
 * Marks a flag to write eeprom settings to flash later
 * @bug: Because this is the same bit used for reading save files,
 *       it will change the save file number to read from
 */
void mark_write_eeprom_settings(void) {
    gSaveDataFlags |= SAVE_DATA_FLAG_WRITE_EEPROM_SETTINGS; // Set bit 9
}

/**
 * Allocates an amount of memory for the number of players passed in.
 */
void alloc_displaylist_heap(s32 numberOfPlayers) {
    s32 num;
    s32 totalSize;

#if defined(NATIVE_PORT) && !defined(MDKR_ADVENTURE_PARTY_OMIT)
    /* A party renders up to participant_count viewports in levels retail
     * loads as 1P (the central hub / world lobbies load with
     * numberOfPlayers=0, AP-08 re-forms N viewports after the load). Sizing
     * these per-frame heaps by the retail index then under-provisions them:
     * a 4-viewport party hub authors ~7900-8800 Gfx against the 1P budget of
     * 4500, so gCurrDisplayList overflows into gMatrixHeap[] in the same
     * allocation and the submitted task list ends in matrix/vertex bytes.
     * Both DL walkers then parse those bytes as commands until one decodes
     * as a G_DL whose segment-resolved target lies outside the arena --
     * the AP-12 4P party-hub SIGSEGV (ASAN: heap-buffer-overflow past
     * g_dkrArenaBase; see task-12b-asan-report.md). Size by the stable 2-4
     * roster count instead while a session exists: the roster is the upper
     * bound of viewports a party ever forms, and the retail 4P table row is
     * the budget retail itself uses for 4 viewports. Stock (no session) and
     * OMIT arms are byte-unchanged. */
    {
        int apCount = adventure_party_participant_count(
            adventure_party_runtime_session());
        /* MDKR_TEST_UNDERSIZED_DL_HEAP=1 keeps the retail 1P index so the
         * misauthored stream above is authored on purpose. It is the fault
         * injector the fast3d display-list hardening gate walks
         * (tests/check_fast3d_dl_hardening.py); nothing sets it in
         * production. */
        const char *undersize = getenv("MDKR_TEST_UNDERSIZED_DL_HEAP");
        if (undersize != NULL && undersize[0] == '1') {
            apCount = 0;
        }
        if (apCount > 0 && numberOfPlayers < apCount - 1) {
            numberOfPlayers = apCount - 1;
        }
    }
#endif

    if (numberOfPlayers != gPrevPlayerCount) {
        gPrevPlayerCount = numberOfPlayers;
        num = numberOfPlayers;
        mempool_free_timer(0);
#ifdef NATIVE_PORT
        /* The DL buffers themselves are about to be freed and reissued at a
         * different size. dkr_last_walked_dl points INTO gDisplayLists[]; see
         * presentation_history_retire. This is the innermost of the three
         * teardown seams -- a player-count change reaches it without any level
         * unload at all. */
        presentation_history_retire();
#endif
        mempool_free(gDisplayLists[0]);
        mempool_free(gDisplayLists[1]);
        totalSize = ((gNumF3dCmdsPerPlayer[num] * sizeof(Gfx))) + ((gNumHudMatPerPlayer[num] * sizeof(Mtx))) +
                    ((gNumHudVertsPerPlayer[num] * sizeof(Vertex))) + ((gNumHudTrisPerPlayer[num] * sizeof(Triangle)));
        gDisplayLists[0] = (Gfx *) mempool_alloc_fixed(totalSize, (u8 *) gDisplayLists[0], COLOUR_TAG_RED);
        gDisplayLists[1] = (Gfx *) mempool_alloc_fixed(totalSize, (u8 *) gDisplayLists[1], COLOUR_TAG_YELLOW);
        if ((gDisplayLists[0] == NULL) || gDisplayLists[1] == NULL) {
            if (gDisplayLists[0] != NULL) {
                mempool_free(gDisplayLists[0]);
                gDisplayLists[0] = NULL;
            }
            if (gDisplayLists[1] != NULL) {
                mempool_free(gDisplayLists[1]);
                gDisplayLists[1] = NULL;
            }
            default_alloc_displaylist_heap();
        }
        gMatrixHeap[0] = (Mtx *) ((u8 *) gDisplayLists[0] + ((gNumF3dCmdsPerPlayer[num] * sizeof(Gfx))));
        gTriangleHeap[0] = (Triangle *) ((u8 *) gMatrixHeap[0] + ((gNumHudMatPerPlayer[num] * sizeof(Mtx))));
        gVertexHeap[0] = (Vertex *) ((u8 *) gTriangleHeap[0] + ((gNumHudTrisPerPlayer[num] * sizeof(Triangle))));
        gMatrixHeap[1] = (Mtx *) ((u8 *) gDisplayLists[1] + ((gNumF3dCmdsPerPlayer[num] * sizeof(Gfx))));
        gTriangleHeap[1] = (Triangle *) ((u8 *) gMatrixHeap[1] + ((gNumHudMatPerPlayer[num] * sizeof(Mtx))));
        gVertexHeap[1] = (Vertex *) ((u8 *) gTriangleHeap[1] + ((gNumHudTrisPerPlayer[num] * sizeof(Triangle))));
        gCurrNumF3dCmdsPerPlayer = gNumF3dCmdsPerPlayer[num];
        gCurrNumHudMatPerPlayer = gNumHudMatPerPlayer[num];
        gCurrNumHudTrisPerPlayer = gNumHudTrisPerPlayer[num];
        gCurrNumHudVertsPerPlayer = gNumHudVertsPerPlayer[num];
        mempool_free_timer(2);
    }
    gCurrDisplayList = gDisplayLists[gSPTaskNum];
    gGameCurrMatrix = gMatrixHeap[gSPTaskNum];
    gGameCurrTriList = gTriangleHeap[gSPTaskNum];
    gGameCurrVertexList = gVertexHeap[gSPTaskNum];

    gDPFullSync(gCurrDisplayList++);
    gSPEndDisplayList(gCurrDisplayList++);
}

#ifdef ANTI_TAMPER
/**
 * Returns FALSE if dmem doesn't begin with a -1. This is checked on every main game loop iteration.
 */
s32 drm_validate_dmem(void) {
    if (IO_READ(SP_DMEM_START) != -1U) {
        return FALSE;
    }
    return TRUE;
}
#endif

/**
 * Defaults allocations for 4 players
 */
void default_alloc_displaylist_heap(void) {
    s32 numberOfPlayers;
    s32 totalSize;

    numberOfPlayers = FOUR_PLAYERS;
    gPrevPlayerCount = numberOfPlayers;
    totalSize = (gNumF3dCmdsPerPlayer[numberOfPlayers] * sizeof(Gfx)) +
                (gNumHudMatPerPlayer[numberOfPlayers] * sizeof(Mtx)) +
                (gNumHudVertsPerPlayer[numberOfPlayers] * sizeof(Vertex)) +
                (gNumHudTrisPerPlayer[numberOfPlayers] * sizeof(Triangle));

    gDisplayLists[0] = (Gfx *) mempool_alloc_safe(totalSize, COLOUR_TAG_RED);
    gMatrixHeap[0] = (Mtx *) ((u8 *) gDisplayLists[0] + (gNumF3dCmdsPerPlayer[numberOfPlayers] * sizeof(Gfx)));
    gVertexHeap[0] = (Vertex *) ((u8 *) gMatrixHeap[0] + (gNumHudMatPerPlayer[numberOfPlayers] * sizeof(Mtx)));
    gTriangleHeap[0] = (Triangle *) ((u8 *) gVertexHeap[0] + (gNumHudVertsPerPlayer[numberOfPlayers] * sizeof(Vertex)));

    gDisplayLists[1] = (Gfx *) mempool_alloc_safe(totalSize, COLOUR_TAG_YELLOW);
    gMatrixHeap[1] = (Mtx *) ((u8 *) gDisplayLists[1] + (gNumF3dCmdsPerPlayer[numberOfPlayers] * sizeof(Gfx)));
    gVertexHeap[1] = (Vertex *) ((u8 *) gMatrixHeap[1] + (gNumHudMatPerPlayer[numberOfPlayers] * sizeof(Mtx)));
    gTriangleHeap[1] = (Triangle *) ((u8 *) gVertexHeap[1] + (gNumHudVertsPerPlayer[numberOfPlayers] * sizeof(Vertex)));

    gCurrNumF3dCmdsPerPlayer = gNumF3dCmdsPerPlayer[numberOfPlayers];
    gCurrNumHudMatPerPlayer = gNumHudMatPerPlayer[numberOfPlayers];
    gCurrNumHudTrisPerPlayer = gNumHudTrisPerPlayer[numberOfPlayers];
    gCurrNumHudVertsPerPlayer = gNumHudVertsPerPlayer[numberOfPlayers];
}

/**
 * Set a delayed level trigger and a transition.
 * Once the timer hits zero, the level will change.
 */
void level_transition_begin(s32 type) {
    if (gLevelLoadTimer == 0) {
#ifdef NATIVE_PORT
        GAMEPLAY_EVENT_TRACE(
            GAMEPLAY_EVENT_LEVEL, type, LEVEL_LOAD_NORMAL, 1, 0);
#endif
        gLevelLoadTimer = 40;
        gLevelLoadType = LEVEL_LOAD_NORMAL;
        D_80123526 = 0;
        if (type == 1) { // FADE_BARNDOOR_HORIZONTAL?
            transition_begin(&gLevelFadeOutTransition);
        }
        if (type == 3) { // FADE_CIRCLE?
            gLevelLoadTimer = 282;
            transition_begin(&D_800DD424);
        }
        if (type == 4) { // FADE_WAVES?
            gLevelLoadTimer = 360;
            transition_begin(&D_800DD424);
        }
        if (type == 0) { // FADE_FULLSCREEN?
            gLevelLoadTimer = 2;
        }
    }
}

UNUSED void func_8006F20C(void) {
    if (gLevelLoadTimer == 0) {
        transition_begin(&gLevelFadeOutTransition);
        gLevelLoadTimer = 40;
        gLevelLoadType = LEVEL_LOAD_UNK1;
    }
}

/**
 * Begins a fade transition, then signals to the level loading that it wants to be a trophy race.
 */
void begin_trophy_race_teleport(void) {
    if (gLevelLoadTimer == 0) {
        transition_begin(&gLevelFadeOutTransition);
        gLevelLoadTimer = 40;
        gLevelLoadType = LEVEL_LOAD_TROPHY_RACE;
    }
}

/**
 * Check if all available trophy races and Wizpig 1 has been beaten, and if the cutscene has not yet played.
 */
void begin_lighthouse_rocket_cutscene(void) {
    if (gLevelLoadTimer == 0) {
        if ((gSettingsPtr->trophies & 0xFF) == 0xFF && !(gSettingsPtr->cutsceneFlags & CUTSCENE_LIGHTHOUSE_ROCKET) &&
            gSettingsPtr->bosses & 1) {
            gSettingsPtr->cutsceneFlags |= CUTSCENE_LIGHTHOUSE_ROCKET;
            transition_begin(&gLevelFadeOutTransition);
            gLevelLoadTimer = 40;
            gNextMap = ASSET_LEVEL_ROCKETSEQUENCE;
            gLevelLoadType = LEVEL_LOAD_LIGHTHOUSE_CUTSCENE;
        }
    }
}

/**
 * Begin a transition, then set the next level to the passed argument.
 * This is used only to warp to Future Fun Land from the hub area.
 */
void begin_level_teleport(s32 levelID) {
    if (gLevelLoadTimer == 0) {
        gNextMap = levelID;
        transition_begin(&gLevelFadeOutTransition);
        gLevelLoadTimer = 40;
        gLevelLoadType = LEVEL_LOAD_FUTURE_FUN_LAND;
    }
}

/**
 * Set the number of frames to disallow pausing for.
 */
void set_pause_lockout_timer(u8 time) {
    gPauseLockTimer = time;
}

/**
 * Switch the data around for player 1 and 2 for two player adventure,
 * effectively passing the lead over to the other player.
 */
void swap_lead_player(void) {
    s32 i;
    u8 temp;
    u8 *first_racer_data;
    u8 *second_racer_data;

    input_swap_id();
    toggle_lead_player_index();

    first_racer_data = (u8 *) (gSettingsPtr->racers);
    second_racer_data = (u8 *) (gSettingsPtr->racers + 1);

    for (i = 0; i < (s32) sizeof(Racer); i++) {
        temp = first_racer_data[i];
        first_racer_data[i] = second_racer_data[i];
        second_racer_data[i] = temp;
    }
#ifdef NATIVE_PORT
    /* The settings rows and Taj's virtual identity are one transaction. Live
     * racer bindings deliberately remain unchanged until the next race load. */
    taj_mod_swap_player_selections(PLAYER_ONE, PLAYER_TWO);
#endif
}

/**
 * Sets the timer to delay drawing new frames.
 * When set to 2, the game will copy the previous framebuffer over to the next.
 */
void set_frame_blackout_timer(void) {
    gDrawFrameTimer = 2;
}

#ifdef NATIVE_PORT
static MdkrCharacterPreviewPose workshop_preview_pose_from_semantic(
    const char *semantic) {
    static const struct {
        const char *semantic;
        MdkrCharacterPreviewPose pose;
    } poses[] = {
#define MDKR_WORKSHOP_PREVIEW_POSE_ROW(suffix, value, label) \
        { value, MDKR_CHARACTER_PREVIEW_POSE_##suffix },
        MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
            MDKR_WORKSHOP_PREVIEW_POSE_ROW)
#undef MDKR_WORKSHOP_PREVIEW_POSE_ROW
    };
    size_t index;
    if (semantic == NULL) return MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    for (index = 0u; index < sizeof(poses) / sizeof(poses[0]); index++) {
        if (strcmp(semantic, poses[index].semantic) == 0) {
            return poses[index].pose;
        }
    }
    return MDKR_CHARACTER_PREVIEW_POSE_COUNT;
}

static MdkrWorkshopPreviewLighting workshop_preview_lighting_from_name(
    const char *name) {
    if (name == NULL) return MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT;
    if (strcmp(name, "neutral") == 0) {
        return MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    }
    if (strcmp(name, "bright") == 0) {
        return MDKR_WORKSHOP_PREVIEW_LIGHTING_BRIGHT;
    }
    if (strcmp(name, "low-key") == 0) {
        return MDKR_WORKSHOP_PREVIEW_LIGHTING_LOW_KEY;
    }
    if (strcmp(name, "backlit") == 0) {
        return MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT;
    }
    return MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT;
}

/* Exercise each vehicle in a course that exposes its ordinary presentation
 * pressures. Ancient Lake is a clean car baseline, Whale Bay contains the
 * water/shore transitions a hovercraft appearance must survive, and Windmill
 * Plains provides the open elevation and camera angles needed to judge a
 * plane rider. These routes are part of the evidence contract: changing one
 * must also invalidate the launcher's presentation signature. */
static s32 workshop_preview_level_for_vehicle(
    s32 vehicle, MdkrCharacterPreviewScene scene) {
    static const s32 levels[3][MDKR_CHARACTER_PREVIEW_SCENE_COUNT] = {
        {ASSET_LEVEL_ANCIENTLAKE, ASSET_LEVEL_GREENWOODVILLAGE,
         ASSET_LEVEL_SNOWBALLVALLEY, ASSET_LEVEL_HAUNTEDWOODS,
         ASSET_LEVEL_JUNGLEFALLS},
        {ASSET_LEVEL_WHALEBAY, ASSET_LEVEL_CRESCENTISLAND,
         ASSET_LEVEL_HOTTOPVOLCANO, ASSET_LEVEL_TREASURECAVES,
         ASSET_LEVEL_PIRATELAGOON},
        {ASSET_LEVEL_WINDMILLPLAINS, ASSET_LEVEL_SPACEPORTALPHA,
         ASSET_LEVEL_EVERFROSTPEAK, ASSET_LEVEL_DARKMOONCAVERNS,
         ASSET_LEVEL_SPACEDUSTALLEY},
    };
    if (vehicle < VEHICLE_CAR || vehicle > VEHICLE_PLANE ||
        scene < MDKR_CHARACTER_PREVIEW_SCENE_BASELINE ||
        scene >= MDKR_CHARACTER_PREVIEW_SCENE_COUNT) return -1;
    return levels[vehicle][scene];
}

static MdkrCharacterPreviewScene workshop_preview_scene_from_name(
    const char *name) {
    if (name == NULL || strcmp(name, "baseline") == 0) {
        return MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
    }
    if (strcmp(name, "dense") == 0) {
        return MDKR_CHARACTER_PREVIEW_SCENE_DENSE;
    }
    if (strcmp(name, "alternate") == 0) {
        return MDKR_CHARACTER_PREVIEW_SCENE_ALTERNATE;
    }
    if (strcmp(name, "low-visibility") == 0) {
        return MDKR_CHARACTER_PREVIEW_SCENE_LOW_VISIBILITY;
    }
    if (strcmp(name, "effects") == 0) {
        return MDKR_CHARACTER_PREVIEW_SCENE_EFFECTS;
    }
    return MDKR_CHARACTER_PREVIEW_SCENE_COUNT;
}

static s32 workshop_preview_start(void) {
    const char *context = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW");
    const char *playersText;
    const char *poseText;
    const char *phaseText;
    const char *transitionFromPoseText;
    const char *transitionFromPhaseText;
    const char *yawText;
    const char *pitchText;
    const char *lightingText;
    const char *captureText;
    const char *captureKindText;
    const char *motionReviewText;
    const char *sceneText;
    const char *donorReferenceText;
    MdkrCharacterPreviewPose pose = MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned posePhaseMilli = 0u;
    MdkrCharacterPreviewPose transitionFromPose =
        MDKR_CHARACTER_PREVIEW_POSE_LIVE;
    unsigned transitionFromPhaseMilli = 0u;
    int viewYawDegrees = 0;
    int viewPitchDegrees = 0;
    MdkrWorkshopPreviewLighting lighting =
        MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    MdkrCharacterPreviewScene scene =
        MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
    s32 players = 1;
    s32 vehicle = -1;
    s32 motionReview = FALSE;
    s32 donorReference = FALSE;
    if (context == NULL || context[0] == '\0') return FALSE;
    playersText = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW_PLAYERS");
    if (playersText != NULL && playersText[0] != '\0') {
        char *end = NULL;
        long parsed = strtol(playersText, &end, 10);
        if (end == playersText || *end != '\0' || parsed < 1 || parsed > 4) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop player count: %s\n",
                    playersText);
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        players = (s32)parsed;
    }
    poseText = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE");
    phaseText = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW_POSE_PHASE");
    if ((poseText != NULL && poseText[0] != '\0') ||
        (phaseText != NULL && phaseText[0] != '\0')) {
        char *end = NULL;
        long parsed;
        char poseError[192] = { 0 };
        if (poseText == NULL || poseText[0] == '\0' ||
            phaseText == NULL || phaseText[0] == '\0') {
            fprintf(stderr,
                    "[FATAL] Character Workshop pose and phase must be "
                    "provided together\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        pose = workshop_preview_pose_from_semantic(poseText);
        parsed = strtol(phaseText, &end, 10);
        if (pose == MDKR_CHARACTER_PREVIEW_POSE_COUNT ||
            end == phaseText || *end != '\0' || parsed < 0 ||
            parsed > 1000 ||
            !mdkr_modern_character_set_inspection_pose(
                poseText, (float)parsed / 1000.0f,
                poseError, sizeof(poseError))) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop pose request: "
                    "%s at %s (%s)\n",
                    poseText, phaseText,
                    poseError[0] != '\0' ? poseError : "invalid contract");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        posePhaseMilli = (unsigned)parsed;
    } else {
        mdkr_modern_character_clear_inspection_pose();
    }
    transitionFromPoseText = getenv(
        "MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_POSE");
    transitionFromPhaseText = getenv(
        "MDKR_CHARACTER_WORKSHOP_PREVIEW_TRANSITION_FROM_PHASE");
    if ((transitionFromPoseText != NULL &&
         transitionFromPoseText[0] != '\0') ||
        (transitionFromPhaseText != NULL &&
         transitionFromPhaseText[0] != '\0')) {
        char *end = NULL;
        long parsed;
        char transitionError[192] = { 0 };
        if (pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
            transitionFromPoseText == NULL ||
            transitionFromPoseText[0] == '\0' ||
            transitionFromPhaseText == NULL ||
            transitionFromPhaseText[0] == '\0') {
            fprintf(stderr,
                    "[FATAL] Character Workshop transition source and phase must be provided together with a destination pose\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        transitionFromPose = workshop_preview_pose_from_semantic(
            transitionFromPoseText);
        parsed = strtol(transitionFromPhaseText, &end, 10);
        if (transitionFromPose == MDKR_CHARACTER_PREVIEW_POSE_COUNT ||
            transitionFromPose == pose || end == transitionFromPhaseText ||
            *end != '\0' || parsed < 0 || parsed > 1000 ||
            !mdkr_modern_character_set_inspection_transition(
                transitionFromPoseText, (float)parsed / 1000.0f,
                poseText, (float)posePhaseMilli / 1000.0f,
                transitionError, sizeof(transitionError))) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop transition request: %s at %s to %s at %u (%s)\n",
                    transitionFromPoseText, transitionFromPhaseText,
                    poseText != NULL ? poseText : "none", posePhaseMilli,
                    transitionError[0] != '\0'
                        ? transitionError : "invalid contract");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        transitionFromPhaseMilli = (unsigned)parsed;
    }
    yawText = getenv("MDKR_CHARACTER_WORKSHOP_VIEW_YAW_DEGREES");
    pitchText = getenv("MDKR_CHARACTER_WORKSHOP_VIEW_PITCH_DEGREES");
    lightingText = getenv("MDKR_CHARACTER_WORKSHOP_LIGHTING");
    captureText = getenv("MDKR_CHARACTER_WORKSHOP_CAPTURE_PNG");
    captureKindText = getenv("MDKR_CHARACTER_WORKSHOP_CAPTURE_KIND");
    donorReferenceText = getenv(
        "MDKR_CHARACTER_WORKSHOP_DONOR_REFERENCE");
    if (donorReferenceText != NULL && donorReferenceText[0] != '\0') {
        if (strcmp(donorReferenceText, "1") != 0) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop donor-reference request\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        donorReference = TRUE;
    }
    if ((yawText != NULL && yawText[0] != '\0') ||
        (pitchText != NULL && pitchText[0] != '\0') ||
        (lightingText != NULL && lightingText[0] != '\0')) {
        char *yawEnd = NULL;
        char *pitchEnd = NULL;
        long parsedYaw;
        long parsedPitch;
        char visualError[192] = { 0 };
        if (pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
            yawText == NULL || yawText[0] == '\0' ||
            pitchText == NULL || pitchText[0] == '\0' ||
            lightingText == NULL || lightingText[0] == '\0') {
            fprintf(stderr,
                    "[FATAL] Character Workshop view and lighting fields must be provided together for pose inspection\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        parsedYaw = strtol(yawText, &yawEnd, 10);
        parsedPitch = strtol(pitchText, &pitchEnd, 10);
        lighting = workshop_preview_lighting_from_name(lightingText);
        if (yawEnd == yawText || *yawEnd != '\0' ||
            pitchEnd == pitchText || *pitchEnd != '\0' ||
            parsedYaw < -180 || parsedYaw > 180 ||
            parsedPitch < MDKR_WORKSHOP_PREVIEW_PITCH_MIN_DEGREES ||
            parsedPitch > MDKR_WORKSHOP_PREVIEW_PITCH_MAX_DEGREES ||
            (strcmp(context, "select") == 0 &&
             (parsedYaw != 0 || parsedPitch != 0)) ||
            lighting == MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT ||
            !mdkr_workshop_preview_visual_set(
                (int)parsedYaw, (int)parsedPitch, lighting,
                visualError, sizeof(visualError))) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop view request: %s,%s %s (%s)\n",
                    yawText, pitchText, lightingText,
                    visualError[0] != '\0'
                        ? visualError : "invalid contract");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        viewYawDegrees = (int)parsedYaw;
        viewPitchDegrees = (int)parsedPitch;
    } else {
        mdkr_workshop_preview_visual_clear();
    }
    if ((captureText != NULL && captureText[0] != '\0') !=
        (captureKindText != NULL && captureKindText[0] != '\0')) {
        fprintf(stderr,
                "[FATAL] Character Workshop capture path and kind must be provided together\n");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (captureText != NULL && captureText[0] != '\0') {
        const size_t captureLength = strlen(captureText);
        if ((!donorReference &&
             pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE) ||
            transitionFromPose != MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
            captureLength >= sizeof(sWorkshopPreviewCapturePath) ||
            captureLength < 4u ||
            strcmp(captureText + captureLength - 4u, ".png") != 0) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop capture request\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        if (strcmp(captureKindText, "scene") == 0) {
            sWorkshopPreviewCaptureKind =
                MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
        } else if (strcmp(captureKindText, "model-alpha") == 0) {
            sWorkshopPreviewCaptureKind =
                MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA;
        } else {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop capture kind: %s\n",
                    captureKindText);
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        (void)snprintf(sWorkshopPreviewCapturePath,
                       sizeof(sWorkshopPreviewCapturePath), "%s", captureText);
    } else {
        sWorkshopPreviewCapturePath[0] = '\0';
        sWorkshopPreviewCaptureKind =
            MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
    }
    if (donorReference &&
        (players != 1 ||
         !((pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE &&
            posePhaseMilli == 0u) ||
           (posePhaseMilli == 500u &&
            ((strcmp(context, "select") == 0 &&
              pose == MDKR_CHARACTER_PREVIEW_POSE_SELECT_IDLE) ||
             (strcmp(context, "select") != 0 &&
              pose == MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER)))) ||
         transitionFromPose != MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
         ((pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
           strcmp(context, "select") == 0) &&
          (viewYawDegrees != 0 || viewPitchDegrees != 0)) ||
         lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
         sWorkshopPreviewCapturePath[0] == '\0' ||
         sWorkshopPreviewCaptureKind !=
             MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE)) {
        fprintf(stderr,
                "[FATAL] donor reference requires one player, a live or neutral held context pose, neutral presentation, and a composed capture\n");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    mdkr_workshop_preview_reference_set(donorReference);
    motionReviewText = getenv("MDKR_CHARACTER_WORKSHOP_MOTION_REVIEW");
    if (motionReviewText != NULL && motionReviewText[0] != '\0') {
        if (strcmp(motionReviewText, "1") != 0) {
            fprintf(stderr,
                    "[FATAL] invalid Character Workshop motion review request\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        motionReview = TRUE;
    }
    sceneText = getenv("MDKR_CHARACTER_WORKSHOP_PREVIEW_SCENE");
    scene = workshop_preview_scene_from_name(sceneText);
    if (scene == MDKR_CHARACTER_PREVIEW_SCENE_COUNT) {
        fprintf(stderr,
                "[FATAL] invalid Character Workshop scene: %s\n",
                sceneText != NULL ? sceneText : "(null)");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (strcmp(context, "car") == 0) {
        vehicle = VEHICLE_CAR;
    } else if (strcmp(context, "hovercraft") == 0) {
        vehicle = VEHICLE_HOVERCRAFT;
    } else if (strcmp(context, "plane") == 0) {
        vehicle = VEHICLE_PLANE;
    } else if (strcmp(context, "select") != 0) {
        fprintf(stderr,
                "[FATAL] invalid Character Workshop context: %s\n",
                context);
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (vehicle < 0 && scene != MDKR_CHARACTER_PREVIEW_SCENE_BASELINE) {
        fprintf(stderr,
                "[FATAL] Character select admits only the baseline scene\n");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (motionReview &&
        (players != 1 ||
         (vehicle < 0
              ? pose != MDKR_CHARACTER_PREVIEW_POSE_SELECT_IDLE ||
                    posePhaseMilli != 500u
              : vehicle > VEHICLE_PLANE ||
                    pose != MDKR_CHARACTER_PREVIEW_POSE_RACE_STEER ||
                    posePhaseMilli != 0u) ||
         transitionFromPose != MDKR_CHARACTER_PREVIEW_POSE_LIVE ||
         viewYawDegrees != 0 || viewPitchDegrees != 0 ||
         lighting != MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL ||
         sWorkshopPreviewCapturePath[0] != '\0')) {
        fprintf(stderr,
                "[FATAL] semantic motion review requires one player, the context's neutral start pose, and no capture\n");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (!mdkr_workshop_preview_prepare(players, vehicle)) {
        fprintf(stderr,
                "[FATAL] Character Workshop package assignment is unavailable\n");
        platform_request_exit(EXIT_FAILURE);
        return TRUE;
    }
    if (g_mdkrCharacterPreviewResult == NULL) {
        bzero(&sWorkshopPreviewDiagnosticResult,
              sizeof(sWorkshopPreviewDiagnosticResult));
        sWorkshopPreviewDiagnosticResult.version =
            MDKR_CHARACTER_PREVIEW_RESULT_VERSION;
        sWorkshopPreviewDiagnosticResult.gpu_timing.version =
            MDKR_MODERN_CHARACTER_GPU_TIMING_VERSION;
        sWorkshopPreviewDiagnosticResult.gpu_timing.status =
            MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED;
        sWorkshopPreviewDiagnosticResult.context = vehicle < 0
            ? MDKR_CHARACTER_PREVIEW_SELECT
            : (MdkrCharacterPreviewContext)(vehicle +
                  MDKR_CHARACTER_PREVIEW_CAR);
        sWorkshopPreviewDiagnosticResult.players = players;
        g_mdkrCharacterPreviewResult = &sWorkshopPreviewDiagnosticResult;
    }
    if (g_mdkrCharacterPreviewResult != NULL) {
        g_mdkrCharacterPreviewResult->started = TRUE;
        g_mdkrCharacterPreviewResult->pose = pose;
        g_mdkrCharacterPreviewResult->pose_phase_milli = posePhaseMilli;
        g_mdkrCharacterPreviewResult->transition_from_pose =
            transitionFromPose;
        g_mdkrCharacterPreviewResult->transition_from_phase_milli =
            transitionFromPhaseMilli;
        g_mdkrCharacterPreviewResult->view_yaw_degrees = viewYawDegrees;
        g_mdkrCharacterPreviewResult->view_pitch_degrees = viewPitchDegrees;
        g_mdkrCharacterPreviewResult->lighting = lighting;
        g_mdkrCharacterPreviewResult->capture_requested =
            sWorkshopPreviewCapturePath[0] != '\0';
        g_mdkrCharacterPreviewResult->capture_kind =
            sWorkshopPreviewCaptureKind;
        g_mdkrCharacterPreviewResult->donor_reference = donorReference;
    }
    if (motionReview) {
        if (g_mdkrCharacterMotionReviewResult == NULL) {
            bzero(&sWorkshopMotionReviewDiagnosticResult,
                  sizeof(sWorkshopMotionReviewDiagnosticResult));
            g_mdkrCharacterMotionReviewResult =
                &sWorkshopMotionReviewDiagnosticResult;
        }
        g_mdkrCharacterMotionReviewResult->version =
            MDKR_CHARACTER_MOTION_REVIEW_RESULT_VERSION;
        g_mdkrCharacterMotionReviewResult->context =
            vehicle < 0 ? MDKR_CHARACTER_PREVIEW_SELECT
                        : (MdkrCharacterPreviewContext)(vehicle +
                              MDKR_CHARACTER_PREVIEW_CAR);
        g_mdkrCharacterMotionReviewResult->scene = scene;
        g_mdkrCharacterMotionReviewResult->sample_count =
            workshop_motion_review_sample_count(
                g_mdkrCharacterMotionReviewResult->context);
        g_mdkrCharacterMotionReviewResult->started = TRUE;
    }
    if (vehicle < 0) {
        charselect_prev(1, NULL);
        load_menu_with_level_background(
            MENU_CHARACTER_SELECT, ASSET_LEVEL_CHARACTERSELECT, 0);
    } else {
        set_time_trial_enabled(FALSE);
        init_racer_headers();
        gPlayableMapId = workshop_preview_level_for_vehicle(vehicle, scene);
        if (gPlayableMapId < 0) {
            fprintf(stderr,
                    "[FATAL] Character Workshop scene has no qualified course\n");
            platform_request_exit(EXIT_FAILURE);
            return TRUE;
        }
        gGameNumPlayers = players - 1;
        gGameCurrentEntrance = 0;
        gGameCurrentCutscene = CUTSCENE_NONE;
        gLevelDefaultVehicleID = (Vehicle)vehicle;
        gGameMode = GAMEMODE_INGAME;
        gIsPaused = FALSE;
        gPostRaceViewPort = FALSE;
        load_level_game(gPlayableMapId, gGameNumPlayers,
                        gGameCurrentEntrance, gLevelDefaultVehicleID);
    }
    MDKR_TRACE(
        "character_workshop_preview: started context=%s scene=%d players=%d "
        "vehicle=%d level=%d pose=%s phase=%u view=%d,%d lighting=%d capture=%d kind=%s donorReference=%d",
        context, (int)scene, players, vehicle,
        vehicle < 0 ? ASSET_LEVEL_CHARACTERSELECT : gPlayableMapId,
        pose == MDKR_CHARACTER_PREVIEW_POSE_LIVE ? "live" : poseText,
        posePhaseMilli, viewYawDegrees, viewPitchDegrees, (int)lighting,
        sWorkshopPreviewCapturePath[0] != '\0',
        sWorkshopPreviewCaptureKind ==
                MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA
            ? "model-alpha" : "scene",
        donorReference);
    return TRUE;
}
#endif

/**
 * Give the player 8 frames to enter the CPak menu with start, then load the intro sequence.
 */
void mode_intro(void) {
    s32 i;
    s32 buttonInputs = 0;

#ifdef NATIVE_PORT
    if (workshop_preview_start()) return;
#endif
    for (i = 0; i < MAXCONTROLLERS; i++) {
        buttonInputs |= input_held(i);
    }
    if (buttonInputs & START_BUTTON) {
        gShowControllerPakMenu = TRUE;
    }
    sBootDelayTimer++;
    if (sBootDelayTimer >= 8) {
#if MDKR_ENABLE_ONLINE_BETA
        /* A validated online roster + launch descriptor is installed only by the
         * beta online-wiring layer, and only for a real online match. When one
         * is present, skip the front-end entirely and load the manifest race. */
        {
            const MdkrMatchLaunchDescriptorV1 *launch =
                mdkr_net_roster_runtime_launch_descriptor();
            if (launch != NULL && mdkr_net_roster_runtime_active()) {
                /* SEPARATED-BOOT-PATH: fork into the online session mode instead
                 * of booting the race here. The session (a fully separate boot
                 * path that never runs the offline menu state machine) stashes
                 * the descriptor, waits in LOBBY_WAIT, then hands off to the same
                 * race boot. Offline is provably unimpacted. */
                mdkr_online_session_begin(launch);
                return;
            }
            /* DESCRIPTOR-LESS lobby-start fork (SECOND condition; the
             * descriptor-first condition above stays FIRST + unchanged). When the
             * party_link bridge is installed but no descriptor/roster is present
             * yet, the launcher wants the NATIVE online screens to own race 1: the
             * session begins descriptor-less, fronts CHARSELECT -> TRACKSELECT,
             * and the race-1 readiness gate holds the boot until the real
             * descriptor + roster + match-input land. mdkr_party_link_active() is
             * false for every non-lobby-start path (the bridge is installed only
             * by a resident/lobby-start boot before the engine runs, and a
             * resident boot ALSO installs a descriptor -> takes the first fork),
             * so no existing lane reaches here. */
            if (mdkr_party_link_active()) {
                mdkr_online_session_begin(NULL);
                return;
            }
        }
#endif
        load_menu_with_level_background(MENU_BOOT, ASSET_LEVEL_OPTIONSBACKGROUND, 2);
    }
}

/**
 * Returns TRUE if the game doesn't detect any controllers.
 * Official name: mainDemoOnly
 */
s32 is_controller_missing(void) {
    if (sControllerStatus == CONTROLLER_MISSING) {
        return TRUE;
    } else {
        return FALSE;
    }
}

#ifdef ANTI_TAMPER
/**
 * Ran on boot, will make sure the CIC chip (CIC6103) is to spec. Will return true if it's all good, otherwise it
 * returns false. The intention of this function, is an attempt to check that the cartridge is a legitimate copy. A
 * false read, meaning you're caught running an illegitimate copy, will force the game to pause when you enter the
 * world.
 */
s32 drm_validate_imem(void) {
    if (IO_READ(SP_IMEM_START) != CIC_ID) {
        return FALSE;
    }
    return TRUE;
}
#endif
