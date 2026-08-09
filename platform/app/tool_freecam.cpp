// tool_freecam.cpp — see tool_freecam.h.
//
// THE THREE THINGS THIS FILE HAS TO GET RIGHT, in the order they matter:
//
//   1. Substitute where render will see it. The finalizer relatches the
//      projection record on every fixed tick, so a substitution made before the
//      tick is overwritten before it is drawn, and one made at swap time
//      arrives after the display list has been walked. The only window is
//      inside render_scene(), which is what platformSetPresentationHook()
//      exists to reach (see platform/app_overlay_hooks.h).
//   2. Substitute a record that survives validation. cam_restore_latched_
//      effective_projection_for_viewport() re-checks the whole record —
//      identity, generation, finiteness, ranges — and refuses anything it does
//      not recognise. This file therefore READS the latched record and edits
//      the lens fields of that copy, rather than authoring a record of its own.
//      Keeping the generation is not incidental: cam_rebuild_native_projection
//      also asks camera_obstruction_projection_matches_render() whether the
//      generation is the one the finalizer published, and a fresh generation
//      would silently drop the frame back to the previous perspective matrix.
//   3. Cost nothing when attached. The hook returns on a compare, and the
//      window is not open in any run that has not asked for it.
#include "tool_freecam.h"

#include "app_theme.h"
#include "dev_tools.h"
#include "ui_common.h"

#include "app_overlay_hooks.h"  // platformSetPresentationHook
#include "display_config.h"     // MdkrCameraProjection: the substituted record
#include "platform_os.h"        // g_frameCounter
#include "present_sched.h"      // g_simTickCounter

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

/*
 * The two halves of the projection handshake, from game/src/camera.c. They are
 * re-declared here rather than reached through game/src/camera.h for the reason
 * tool_diagnostics.cpp states at length: that header pulls the decomp's N64 SDK
 * headers, whose memmove/memcpy declarations collide with the host CRT. s32 is
 * int32_t and int32_t is int on every platform this port builds for.
 *
 * The getter is a pure read. The setter is the resolver's own restore path —
 * the finalizer calls it to reinstate a held image (camera_obstruction_runtime.c
 * around the last-validated fallback), and it validates its argument before
 * accepting it, which is why an edited copy of a real record is the only shape
 * of substitution this tool can make.
 */
extern "C" {
bool cam_get_latched_effective_projection_for_viewport(
    int viewport, MdkrCameraProjection *out);
bool cam_restore_latched_effective_projection_for_viewport(
    int viewport, int cameraID, const MdkrCameraProjection *projection);

/*
 * The authored rebuild, used to CLOSE the substitution at the end of the drawn
 * frame. cam_set_fov() re-derives the authored record from authored inputs and
 * rebuilds the perspective matrix through camera.c's own path — it is not a
 * saved copy being put back, and this file keeps none. See the closing note in
 * ToolFreecam_presentationEndHook().
 *
 * gCurCamFOV is the authored level FOV (game/src/camera.c). The bounds are
 * camera.h's CAMERA_MIN_FOV / CAMERA_MAX_FOV.
 */
extern float gCurCamFOV;
void  cam_set_fov(float fov);
}

namespace {

// Every viewport the latched-projection array has a slot for (camera.c sizes
// sNativeProjectionByViewport[4]). Asking for a viewport that is not being
// drawn simply reports no latched record, so the loop needs no layout query.
constexpr int kViewports = 4;

constexpr float kZoomMin = 0.20f;
constexpr float kZoomMax = 4.00f;
constexpr float kNearMin = 0.25f;
constexpr float kNearMax = 8.00f;
// The record's own validation refuses a vertical or horizontal FOV outside
// (0, 180). Clamping short of the boundary keeps a slider at its extreme from
// producing a record the restore silently rejects, which would read on screen
// as the tool doing nothing.
constexpr float kFovMin = 1.0f;
constexpr float kFovMax = 175.0f;
// Spelled out rather than taken from <cmath>: M_PI is not in the C++ standard
// and MSVC hides it behind _USE_MATH_DEFINES.
constexpr double kPi = 3.14159265358979323846;
// game/src/camera.h. cam_set_fov() ignores anything outside this open interval.
constexpr float kAuthoredFovMin = 0.0f;
constexpr float kAuthoredFovMax = 90.0f;

struct Freecam {
    bool  detached = false;      // the live answer: manual OR scheduled
    bool  manual = false;        // the window's own switch
    float zoom = 1.6f;           // multiplies the authored vertical FOV
    float nearScale = 1.0f;      // multiplies the authored near plane
    // Observability. The gate needs to know the substitution actually ran, and
    // a human needs to know why nothing changed when it did not.
    unsigned long long substitutions = 0;
    unsigned long long refusals = 0;
    int   lastViewports = 0;
    bool  substitutedThisFrame = false;
    unsigned long long closes = 0;
    float lastAuthoredVfov = 0.0f;
    float lastAppliedVfov = 0.0f;
};
Freecam g_cam;

// --- the unattended schedule -------------------------------------------------
// A headless race has nobody to press F5, and the detach/re-attach gate needs
// the substitution to start and stop at known ticks. Read once, lazily: the
// hook runs inside the drawn frame and must not do work per viewport per frame
// that it can do once.
struct Schedule {
    bool resolved = false;
    long detachTick = -1;
    long reattachTick = -1;
    bool active = false;         // last evaluated state, for edge detection
};
Schedule g_schedule;

long envLong(const char *name, long fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return (end != nullptr && end != value) ? parsed : fallback;
}

void resolveSchedule() {
    if (g_schedule.resolved) return;
    g_schedule.resolved = true;
    g_schedule.detachTick = envLong("MDKR_FREECAM_DETACH", -1);
    g_schedule.reattachTick = envLong("MDKR_FREECAM_REATTACH", -1);
    const char *zoom = std::getenv("MDKR_FREECAM_ZOOM");
    if (zoom != nullptr && zoom[0] != '\0') {
        const double parsed = std::atof(zoom);
        if (std::isfinite(parsed) && parsed >= kZoomMin && parsed <= kZoomMax) {
            g_cam.zoom = static_cast<float>(parsed);
        }
    }
}

// One line per state change, on stdout, so a gate can locate the detached
// window by PRESENTED FRAME rather than having to assume frames and ticks are
// one to one. Both ordinals are printed for the same reason.
void announce(const char *event) {
    std::printf("[FREECAM] event=%s tick=%d frame=%d zoom=%.4f near=%.4f\n",
                event, g_simTickCounter, g_frameCounter,
                static_cast<double>(g_cam.zoom),
                static_cast<double>(g_cam.nearScale));
    std::fflush(stdout);
}

void setDetached(bool detached, const char *event) {
    if (g_cam.detached == detached) return;
    g_cam.detached = detached;
    announce(event);
}

// --- the substitution --------------------------------------------------------

// Edit the lens fields of a copy of the latched record. Returns false when the
// edit would be a no-op or would not survive validation, so the caller can
// leave the authored record in place rather than write an identical one back.
bool applyLens(MdkrCameraProjection *record) {
    if (!std::isfinite(record->vertical_fov) || record->vertical_fov <= 0.0f ||
        !std::isfinite(record->aspect) || record->aspect <= 0.0f ||
        !std::isfinite(record->near_plane) || record->near_plane <= 0.0f ||
        !std::isfinite(record->far_plane)) {
        return false;
    }

    float vfov = record->vertical_fov * g_cam.zoom;
    if (!(vfov > kFovMin)) vfov = kFovMin;
    if (vfov > kFovMax) vfov = kFovMax;

    // The horizontal angle is derived from the vertical one and the record's
    // own aspect, not scaled independently: the two fields are one lens, and
    // tracks.c widens its CPU cull planes from the horizontal value. Leaving
    // them inconsistent would cull geometry the frame then tries to draw.
    const double halfV = static_cast<double>(vfov) * (kPi / 360.0);
    const double halfH = std::atan(std::tan(halfV) *
                                   static_cast<double>(record->aspect));
    float hfov = static_cast<float>(halfH * (360.0 / kPi));
    if (!std::isfinite(hfov)) return false;
    if (!(hfov > kFovMin)) hfov = kFovMin;
    if (hfov > kFovMax) hfov = kFovMax;

    float nearPlane = record->near_plane * g_cam.nearScale;
    if (!std::isfinite(nearPlane) || nearPlane <= 0.0f ||
        nearPlane >= record->far_plane) {
        nearPlane = record->near_plane;
    }

    if (vfov == record->vertical_fov && hfov == record->horizontal_fov &&
        nearPlane == record->near_plane) {
        return false;
    }
    g_cam.lastAuthoredVfov = record->vertical_fov;
    g_cam.lastAppliedVfov = vfov;
    record->vertical_fov = vfov;
    record->horizontal_fov = hfov;
    record->near_plane = nearPlane;
    // Identity, generation and display_generation are deliberately untouched:
    // the restore validates them, and the render-side generation handshake
    // rejects any record the finalizer did not publish this tick.
    return true;
}

// The registrar. Static-init rather than a call from DevTools_draw() for the
// reason dev_tools.cpp gives about its own table: the engine can enter a drawn
// frame before the app shell has drawn its first ImGui frame, and a hook
// installed lazily from the UI would miss it.
struct InstallPresentationHook {
    InstallPresentationHook() {
        platformSetPresentationHook(&ToolFreecam_presentationHook);
        platformSetPresentationEndHook(&ToolFreecam_presentationEndHook);
    }
};
InstallPresentationHook g_installPresentationHook;

}  // namespace

extern "C" void ToolFreecam_presentationHook(void) {
    // One gate, folded: DevTools_isOpen() already answers false when the whole
    // tool surface is off, so a player who never enabled developer tools pays
    // exactly this compare per drawn frame.
    if (!DevTools_isOpen(MDKR_TOOL_FREECAM)) {
        // Closing the window re-attaches. A detached camera that outlived its
        // window would be a substitution with no visible owner.
        setDetached(false, "reattach-closed");
        return;
    }

    resolveSchedule();
    if (g_schedule.detachTick >= 0) {
        const long tick = static_cast<long>(g_simTickCounter);
        const bool want = tick >= g_schedule.detachTick &&
                          (g_schedule.reattachTick < 0 ||
                           tick < g_schedule.reattachTick);
        if (want != g_schedule.active) {
            g_schedule.active = want;
        }
    }
    setDetached(g_cam.manual || g_schedule.active,
                g_cam.manual || g_schedule.active ? "detach" : "reattach");

    if (!g_cam.detached) return;

    int substituted = 0;
    for (int viewport = 0; viewport < kViewports; ++viewport) {
        MdkrCameraProjection record;
        if (!cam_get_latched_effective_projection_for_viewport(viewport,
                                                               &record)) {
            continue;
        }
        if (!applyLens(&record)) continue;
        if (cam_restore_latched_effective_projection_for_viewport(
                viewport, record.camera_id, &record)) {
            ++substituted;
        } else {
            // The restore refused the edited record. Counted rather than
            // silently ignored: a tool that appears to do nothing and cannot
            // say why is the failure mode this counter exists to name.
            ++g_cam.refusals;
        }
    }
    g_cam.lastViewports = substituted;
    if (substituted > 0) {
        ++g_cam.substitutions;
        g_cam.substitutedThisFrame = true;
    }
}

extern "C" void ToolFreecam_presentationEndHook(void) {
    if (!g_cam.substitutedThisFrame) return;
    g_cam.substitutedThisFrame = false;

    /*
     * CLOSING THE SCOPE, AND WHY IT IS NOT "RESTORING A SAVED POSE".
     *
     * Nothing is saved anywhere in this file. cam_set_fov() re-derives the
     * authored projection from the authored inputs that are still sitting in
     * gCurCamFOV and the display config, relatches it, and rebuilds
     * gPerspectiveMatrixF through camera.c's own path — the same path the
     * un-detached run took. Two calls are needed only because cam_set_fov()
     * early-outs when the value it is handed already equals gCurCamFOV; the
     * nudge is discarded by the second call, which writes the identical float
     * back, so gCurCamFOV ends bit-identical to what it was.
     *
     * WHY THIS IS NECESSARY AT ALL. The record is presentation-scoped; the
     * globals cam_rebuild_native_projection() derives from it are not. The next
     * fixed tick's obj_visibility_tick rebuilds its cull planes from
     * gPerspectiveMatrixF and gEffectiveCamHFOV without refreshing them, and
     * that visibility answer gates AI RNG — so a lens left substituted past the
     * end of the frame moves authoritative state. Measured, not theorised: it
     * diverged the v3 [SIMHASH] stream 232 ticks after detaching. The frame
     * still shows the substituted lens because the display list was already
     * authored from it; only the leftovers are put back.
     *
     * The projection's `generation` is a hash of the record's own contents
     * (platform/display_config.c), not a counter, so re-deriving the authored
     * record reproduces the authored generation exactly and the render-side
     * handshake is not perturbed either.
     */
    const float authored = gCurCamFOV;
    float nudge = authored * 0.99f;
    if (!(nudge > kAuthoredFovMin) || !(nudge < kAuthoredFovMax) ||
        nudge == authored || !(authored > kAuthoredFovMin) ||
        !(authored < kAuthoredFovMax)) {
        return;
    }
    cam_set_fov(nudge);
    cam_set_fov(authored);
    ++g_cam.closes;
}

void ToolFreecam_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(460.0f * AppTheme::uiScale(),
                                    340.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Free Camera", open)) {
        ImGui::End();
        return;
    }

    ui::TextSubtleWrapped(
        "Detach the presentation lens from the authored one and fly it. This "
        "substitutes the projection record the camera finalizer latched, at "
        "the same depth the obstruction resolver corrects a camera, so it "
        "cannot change the race.");
    ui::Gap(ui::kGapS);

    // The hook owns the live detached state so the schedule and this switch
    // cannot disagree; it reconciles on the next drawn frame.
    (void)ImGui::Checkbox("Detached", &g_cam.manual);
    ImGui::SameLine();
    if (g_cam.detached) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("substituting");
        ImGui::PopStyleColor();
    } else {
        ui::TextSubtle("authored lens");
    }

    ImGui::SliderFloat("Zoom", &g_cam.zoom, kZoomMin, kZoomMax, "%.2fx");
    ImGui::SliderFloat("Near plane", &g_cam.nearScale, kNearMin, kNearMax,
                       "%.2fx");
    ui::TextSubtleWrapped(
        "Zoom multiplies the authored vertical field of view; the horizontal "
        "angle is re-derived from it so the CPU cull planes stay in step with "
        "the lens. The near plane slices the scene open in front of the "
        "camera.");

    ui::Gap(ui::kGapS);
    if (ImGui::Button("Re-attach", ui::kBtnWide())) {
        g_cam.manual = false;
    }
    ui::TextSubtleWrapped(
        "Re-attaching restores nothing: it stops substituting. The finalizer "
        "relatches the authored record every fixed tick, so the next frame is "
        "built from bytes this window never touched.");

    ui::Gap(ui::kGapS);
    if (ImGui::BeginTable("##freecam", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg)) {
        const auto row = [](const char *label, const char *value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextSubtle("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        };
        char buffer[128];
        std::snprintf(buffer, sizeof buffer, "%d", g_simTickCounter);
        row("Tick", buffer);
        std::snprintf(buffer, sizeof buffer, "%d this frame", g_cam.lastViewports);
        row("Viewports", buffer);
        std::snprintf(buffer, sizeof buffer, "%.2f deg authored, %.2f deg drawn",
                      static_cast<double>(g_cam.lastAuthoredVfov),
                      static_cast<double>(g_cam.lastAppliedVfov));
        row("Vertical FOV", buffer);
        std::snprintf(buffer, sizeof buffer, "%llu frames", g_cam.substitutions);
        row("Substituted", buffer);
        std::snprintf(buffer, sizeof buffer, "%llu", g_cam.refusals);
        row("Records refused", buffer);
        ImGui::EndTable();
    }
    if (g_cam.refusals > 0) {
        ui::TextSubtleWrapped(
            "A refused record means the projection handshake rejected the "
            "edit, and the authored lens was drawn instead. That is the "
            "contract working, not a lost frame.");
    }

    ImGui::End();
}
