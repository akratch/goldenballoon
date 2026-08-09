// tool_diagnostics.cpp — see tool_diagnostics.h.
//
// WHAT THIS WINDOW IS FOR. US-1: someone hits a glitch, presses F3, and copies
// ONE block of text that turns "the game glitched" into a report a contributor
// can act on. So the field list is not "everything we could show" — it is the
// answers to the questions every unactionable report is missing: which build,
// which cartridge, which renderer, which machine, which track, when, and what
// the game had just done.
//
// WHY IT READS PUBLISHED RECORDS AND NOTHING ELSE. Registering a tool is a
// claim that opening it cannot move authoritative state, and
// check_dev_tools_purity.py tests that claim over a whole race. Every number
// here comes from a record some other subsystem already publishes:
//
//   tick      the presentation scheduler's authoritative counter
//   camera    the presentation snapshot the renderer already interpolates from
//   pacing    the latched present policy plus the surface-frame counter
//   settings  the resolved video config
//   events    the [EVENTHASH] rows the gameplay event trace already writes
//
// Re-deriving any of those would give the window its own opinion, and a viewer
// that can disagree with the game is worse than no viewer — that is the same
// rule Task 6's collision overlay is held to. Reading the trace rows back out
// of the log also means the window shows exactly what a gate would compare,
// character for character.
//
// COST. The log ring is snapshotted on a timer rather than every frame. The
// purity gate compares presented-frame counts between a tools-off race and a
// tool-open one, so a window that did 128 KiB of work per frame would be
// paying for itself out of the frame loop even though it changed no state.
#include "tool_diagnostics.h"

#include "app_theme.h"
#include "app_version.h"
#include "diag_log.h"
#include "ui_common.h"

#include "mod_texture_store.h"      // installed content packs
#include "platform_os.h"            // g_romData, g_surfaceFrameCounter, backend
#include "present_sched.h"          // g_simTickCounter, latched present policy
#include "presentation_snapshot.h"  // the published camera record
#include "rom_id.h"                 // dkr_rom_identify
#include "video_config.h"

#include "imgui.h"

#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>

/*
 * The two engine reads this window makes that platform/ does not already
 * publish. Both are defined in game/src/game.c and declared in game/src/game.h,
 * which the app shell cannot include — that header pulls the decomp's N64 SDK
 * headers, whose own memmove/memcpy declarations collide with the host CRT
 * (the same collision platform/stubs_dkr.c documents around mdkr_force_track).
 * So they are re-declared here with the prototypes that header gives them; s32
 * is int32_t, and int32_t is int on every platform this port builds for.
 *
 * Both are pure reads and both fail closed. level_id() returns gMapId.
 * level_name() bounds-checks its argument against gNumberOfLevelHeaders and
 * returns NULL for anything outside it — including before any level has been
 * loaded, which is why it is safe to ask on the title screen.
 */
extern "C" {
int   level_id(void);
char *level_name(int levelId);

/*
 * The lens the renderer is actually using, from game/src/camera.c's native
 * projection handshake. Declared for the same reason and under the same
 * caveats as the two above; all three return a latched float and read nothing
 * else. This is the half of the camera that is available whatever the
 * presentation settings are — see cameraPoseLine().
 */
float cam_get_effective_horizontal_fov(void);
float cam_get_effective_vertical_fov(void);
float cam_get_effective_aspect(void);
}

namespace {

constexpr int kEventRows = 32;      // the last N gameplay-event rows to show
constexpr int kLogTailBytes = 128 * 1024;
constexpr double kLogRefreshSeconds = 0.25;

// --- the diagnostic log tail ------------------------------------------------
// One snapshot shared by every reader in this file and by the console, taken
// at most four times a second. See the cost note in the file header.
struct LogTail {
    double            fetchedAt = -1.0;
    std::vector<char> bytes;
    int               length = 0;
};
LogTail g_logTail;

const char *logTail(int *outLength) {
    const double now = ImGui::GetTime();
    if (g_logTail.fetchedAt < 0.0 ||
        now - g_logTail.fetchedAt >= kLogRefreshSeconds) {
        if (g_logTail.bytes.empty()) {
            g_logTail.bytes.resize(static_cast<size_t>(kLogTailBytes));
        }
        g_logTail.length =
            DiagLog_snapshot(g_logTail.bytes.data(), kLogTailBytes);
        g_logTail.fetchedAt = now;
    }
    *outLength = g_logTail.length;
    return g_logTail.bytes.empty() ? "" : g_logTail.bytes.data();
}

// --- measured present rate ---------------------------------------------------
// g_surfaceFrameCounter counts images actually committed to the window, which
// is the number a report means by "how many frames was it really drawing" —
// deliberately not g_frameCounter, which counts pacing opportunities. Same
// half-second window the F10 readout uses, so the two never disagree on screen.
struct RateMeter {
    uint64_t lastCounter = 0;
    uint64_t lastFrame = 0;
    double   rate = 0.0;
};
RateMeter g_presentRate;

double measuredPresentRate() {
    const uint64_t now = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (g_presentRate.lastCounter == 0) {
        g_presentRate.lastCounter = now;
        g_presentRate.lastFrame = g_surfaceFrameCounter;
    } else if (frequency > 0 &&
               now >= g_presentRate.lastCounter + frequency / 2) {
        const double seconds =
            static_cast<double>(now - g_presentRate.lastCounter) /
            static_cast<double>(frequency);
        g_presentRate.rate =
            static_cast<double>(g_surfaceFrameCounter - g_presentRate.lastFrame) /
            seconds;
        g_presentRate.lastCounter = now;
        g_presentRate.lastFrame = g_surfaceFrameCounter;
    }
    return g_presentRate.rate;
}

// --- state readers -----------------------------------------------------------

std::string versionLine() {
    std::string text = AppVersion();
    const char *stamp = AppBuildStamp();
    if (stamp != nullptr && stamp[0] != '\0') {
        text += " (";
        text += stamp;
        text += ")";
    }
    return text;
}

std::string platformLine() {
    SDL_version linked;
    SDL_GetVersion(&linked);
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, "%s, SDL %d.%d.%d",
                  SDL_GetPlatform(), linked.major, linked.minor, linked.patch);
    return buffer;
}

// Identified from the image the engine actually loaded, not from whatever the
// launcher validated: a report has to name the bytes that were running.
std::string romLine() {
    if (g_romData == nullptr || g_romSize < 0x40u) {
        return "not loaded";
    }
    DkrRomId id;
    dkr_rom_identify(g_romData, &id);
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, "%s%s%s%s crc %08X/%08X",
                  id.revisionName != nullptr ? id.revisionName : "unrecognised",
                  id.decompBuild != nullptr ? " [" : "",
                  id.decompBuild != nullptr ? id.decompBuild : "",
                  id.decompBuild != nullptr ? "]" : "",
                  id.crc1, id.crc2);
    return buffer;
}

std::string contentPacksLine() {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    const bool chosen =
        config == nullptr ||
        config->values[MDKR_CONTENT_PACKS_ENABLED].number != 0.0f;
    if (!mdkr_mod_texture_store_active()) {
        return chosen ? "none installed" : "none installed, and switched off";
    }
    const bool live = mdkr_mod_texture_enabled();
    // An installed pack is the single most common reason a screenshot does not
    // match anyone else's, so this line says both halves out loud.
    return live ? "installed and applied — report may not match a stock install"
                : "installed, currently switched off";
}

int currentTrackId() {
    // Before the first authoritative tick the game has not finished init_game()
    // and gMapId means nothing, so do not ask.
    return g_simTickCounter > 0 ? level_id() : -1;
}

std::string trackLine() {
    const int id = currentTrackId();
    if (id < 0) {
        return "not in a level yet";
    }
    const char *name = level_name(id);
    char buffer[128];
    if (name != nullptr && name[0] != '\0') {
        std::snprintf(buffer, sizeof buffer, "%d \"%s\"", id, name);
    } else {
        std::snprintf(buffer, sizeof buffer, "%d (unnamed)", id);
    }
    return buffer;
}

// The camera record the renderer itself presents from. It exists only while the
// presentation snapshot seam is live (Motion smoothing, or MDKR_PRESENT_SNAPSHOT),
// and saying so is better than inventing a second camera opinion for the window.
const PresentationCameraEntry *publishedCamera() {
    const PresentationSnapshot *snapshot = presentation_snapshot_current();
    if (snapshot == nullptr || !snapshot->valid || snapshot->camera_count == 0) {
        return nullptr;
    }
    return &snapshot->cameras[0];
}

/*
 * The eye position exists only while the presentation snapshot is publishing,
 * which is whenever the renderer is interpolating between ticks (Motion
 * smoothing, or MDKR_PRESENT_SNAPSHOT). On Original presentation there is no
 * published camera to read, and the alternative -- re-deriving an eye from the
 * view matrix, or re-declaring the decomp's Camera layout in the app shell --
 * would give this window its own opinion of where the camera is. A field that
 * says it does not know is worth more in a bug report than one that is
 * confidently reconstructed.
 */
std::string cameraPositionLine() {
    const PresentationCameraEntry *camera = publishedCamera();
    if (camera == nullptr) {
        return "not published (turn on Motion smoothing to record the camera)";
    }
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, "%.2f, %.2f, %.2f",
                  static_cast<double>(camera->position[0]),
                  static_cast<double>(camera->position[1]),
                  static_cast<double>(camera->position[2]));
    return buffer;
}

std::string cameraPoseLine() {
    // The lens half is always available, so the report is never silent about
    // the camera even when the pose half is not being published.
    char lens[96];
    std::snprintf(lens, sizeof lens, "fov %.1f wide x %.1f tall, aspect %.3f",
                  static_cast<double>(cam_get_effective_horizontal_fov()),
                  static_cast<double>(cam_get_effective_vertical_fov()),
                  static_cast<double>(cam_get_effective_aspect()));

    const PresentationCameraEntry *camera = publishedCamera();
    if (camera == nullptr) {
        return lens;
    }
    // DKR fixed angles: 0x10000 is one full turn. Raw and degrees, because a
    // report is read by a human and reproduced by a machine.
    char buffer[256];
    std::snprintf(buffer, sizeof buffer,
                  "yaw %d (%.1f deg), pitch %d, roll %d, extra pitch %d, %s",
                  camera->rotation_y,
                  static_cast<double>(camera->rotation_y) * 360.0 / 65536.0,
                  camera->rotation_x, camera->rotation_z, camera->pitch, lens);
    return buffer;
}

std::string frameLimitLine() {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    const char *chosen =
        config != nullptr ? config->values[MDKR_VIDEO_FRAME_LIMIT].text : "";
    const char *resolved = present_sched_present_policy_name();
    // A rate of zero is not "no frames": `original` and `display` have no
    // native numeric cap, and printing 0/s beside them reads like a fault.
    const unsigned rate = present_sched_present_rate();
    char buffer[160];
    if (rate > 0u) {
        std::snprintf(buffer, sizeof buffer, "%s (running as %s, capped at %u/s)",
                      chosen != nullptr && chosen[0] != '\0' ? chosen : "default",
                      resolved != nullptr ? resolved : "unknown", rate);
    } else {
        std::snprintf(buffer, sizeof buffer, "%s (running as %s, no native cap)",
                      chosen != nullptr && chosen[0] != '\0' ? chosen : "default",
                      resolved != nullptr ? resolved : "unknown");
    }
    return buffer;
}

std::string logPathLine() {
    const char *path = DiagLog_path();
    return (path != nullptr && path[0] != '\0')
               ? path
               : std::string("not written on this platform");
}

// Cached so the window's per-frame cost is a pointer copy. Refreshed on the
// same cadence as the log snapshot it is derived from.
struct EventCache {
    double                   at = -1.0;
    std::vector<std::string> rows;
};
EventCache g_events;

const std::vector<std::string> &eventRows() {
    const double now = ImGui::GetTime();
    if (g_events.at < 0.0 || now - g_events.at >= kLogRefreshSeconds) {
        g_events.rows = ToolDiagnostics_recentLogLines("[EVENTHASH]", kEventRows);
        g_events.at = now;
    }
    return g_events.rows;
}

void appendField(std::string &out, const char *label, const std::string &value) {
    char padded[16];
    std::snprintf(padded, sizeof padded, "%-10s", label);
    out += padded;
    out += value;
    out += '\n';
}

}  // namespace

std::vector<std::string> ToolDiagnostics_recentLogLines(const char *marker,
                                                        int max) {
    std::vector<std::string> rows;
    if (marker == nullptr || marker[0] == '\0' || max <= 0) {
        return rows;
    }
    int length = 0;
    const char *text = logTail(&length);
    if (length <= 0) {
        return rows;
    }
    const std::string haystack(text, static_cast<size_t>(length));
    size_t position = 0;
    // Searched inside the line rather than anchored at its start: the tee
    // interleaves stdout and stderr into one pipe, and mdkr_trace() prefixes
    // its own "[TRACE] " to some rows — check_dev_tools_purity.py parses the
    // [PACE] stream the same way for the same reason.
    while ((position = haystack.find(marker, position)) != std::string::npos) {
        const size_t end = haystack.find('\n', position);
        rows.push_back(end == std::string::npos
                           ? haystack.substr(position)
                           : haystack.substr(position, end - position));
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (static_cast<int>(rows.size()) > max) {
        rows.erase(rows.begin(), rows.end() - max);
    }
    return rows;
}

std::string ToolDiagnostics_copyBlock() {
    std::string out;
    out.reserve(4096);
    out += "mdkr64 diagnostics\n";
    out += "------------------\n";
    appendField(out, "app", versionLine());
    appendField(out, "renderer", mdkr_render_backend_name());
    appendField(out, "system", platformLine());
    appendField(out, "rom", romLine());
    appendField(out, "packs", contentPacksLine());
    out += '\n';
    appendField(out, "tick", std::to_string(g_simTickCounter));
    appendField(out, "track", trackLine());
    appendField(out, "camera", cameraPositionLine());
    appendField(out, "pose", cameraPoseLine());
    appendField(out, "frames", frameLimitLine());
    {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.1f per second",
                      measuredPresentRate());
        appendField(out, "measured", buffer);
    }
    appendField(out, "log", logPathLine());

    out += "\nlast gameplay events (oldest first)\n";
    const std::vector<std::string> &rows = eventRows();
    if (rows.empty()) {
        out += "  none recorded — the gameplay event trace was not running\n";
    } else {
        for (const std::string &row : rows) {
            out += "  ";
            out += row;
            out += '\n';
        }
    }
    return out;
}

void ToolDiagnostics_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(560.0f * AppTheme::uiScale(),
                                    460.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Diagnostics", open)) {
        ImGui::End();
        return;
    }

    ui::TextSubtleWrapped(
        "What this build is, what it is running, and what just happened. "
        "Copy it into a bug report.");
    ui::Gap(ui::kGapS);

    if (ImGui::BeginTable("##state", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg)) {
        const auto row = [](const char *label, const std::string &value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextSubtle("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        row("Tick", std::to_string(g_simTickCounter));
        row("Track", trackLine());
        row("Camera", cameraPositionLine());
        row("Pose", cameraPoseLine());
        row("Frame limit", frameLimitLine());
        {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%.1f per second",
                          measuredPresentRate());
            row("Measured", buffer);
        }
        row("Renderer", mdkr_render_backend_name());
        row("ROM", romLine());
        row("Content packs", contentPacksLine());
        ImGui::EndTable();
    }

    ui::Gap(ui::kGapS);

    // Transient confirmation, for the reason ui_diag.cpp states: a latched
    // "Copied" minutes later would be a claim about a block that has moved on.
    static double copiedAt = -1.0;
    if (ImGui::Button("Copy Report to Clipboard", ui::kBtnWide())) {
        ImGui::SetClipboardText(ToolDiagnostics_copyBlock().c_str());
        copiedAt = ImGui::GetTime();
    }
    if (copiedAt >= 0.0 && ImGui::GetTime() - copiedAt < 3.0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::good());
        ImGui::TextUnformatted("Copied");
        ImGui::PopStyleColor();
    }

    ui::Gap(ui::kGapS);
    ImGui::TextUnformatted("Recent gameplay events");
    ImGui::BeginChild("##events", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<std::string> &rows = eventRows();
    if (rows.empty()) {
        ui::TextSubtleWrapped(
            "Nothing recorded. The gameplay event trace writes its rows only "
            "when MDKR_EVENT_HASH is set, so an ordinary session has none.");
    } else {
        ImGui::PushFont(AppTheme::fonts().small);
        for (const std::string &line : rows) {
            // Not a format string: a trace row may legitimately contain '%'.
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::PopFont();
    }
    ui::TouchScrollCurrentWindow();
    ImGui::EndChild();

    ImGui::End();
}
