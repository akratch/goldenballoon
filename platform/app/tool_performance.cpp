// tool_performance.cpp — see tool_performance.h.
//
// TWO SOURCES, AND WHY EACH IS THE ONE IT IS.
//
//   Scalars come from the exported accessors — present_sched_present_rate(),
//   present_sched_present_policy_name(), g_frameCounter, g_surfaceFrameCounter,
//   g_simTickCounter. Cheap, always available, exactly what the F10 readout and
//   the diagnostics window already show, so the three never disagree.
//
//   Per-phase timings come from the [PRESENTPERF] / [PRESENTPERF-HIST] /
//   [PRESENTPERF-LATENCY] rows read back out of the diagnostic log ring. Those
//   counters are file-static in present_sched.c with no accessor, and the
//   alternative was to export them — which would mean this window shows numbers
//   assembled a second way from the same statics, and a rounding or windowing
//   difference between the two would be indistinguishable from a real pacing
//   change. Reading the rows means what is on screen is character-for-character
//   what tests/check_pacing_quality.py parses.
//
// COST. The log ring is snapshotted on the same quarter-second cadence the
// diagnostics window uses, through the same helper, so both windows open at
// once still take one snapshot.
#include "tool_performance.h"

#include "app_theme.h"
#include "tool_diagnostics.h"  // ToolDiagnostics_recentLogLines: the log ring
#include "ui_common.h"

#include "platform_os.h"    // g_frameCounter, g_surfaceFrameCounter, backend
#include "present_sched.h"  // g_simTickCounter, policy/rate accessors

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int    kRows = 16;
constexpr double kRefreshSeconds = 0.25;

/*
 * platform/platform_sdl_min.c. Re-declared rather than included from
 * platform_os.h's neighbourhood for the same reason enh_draw_distance.c
 * extern's its one symbol: this is one predicate.
 *
 * It answers "is the pacer synthetic", which is the state MDKR_SYNTH_FIELDS
 * puts a headless run into. Both are consulted below, because they are not the
 * same question: the variable is what a person set, the predicate is what the
 * pacer resolved (MDKR_PACE_REALTIME can override it).
 */
extern "C" int platform_pace_is_synthetic(void);

struct RowCache {
    double                   at = -1.0;
    std::vector<std::string> rows;
};

// One cache per marker; refreshed together on the shared cadence.
const std::vector<std::string> &markerRows(const char *marker, RowCache &cache) {
    const double now = ImGui::GetTime();
    if (cache.at < 0.0 || now - cache.at >= kRefreshSeconds) {
        cache.rows = ToolDiagnostics_recentLogLines(marker, kRows);
        cache.at = now;
    }
    return cache.rows;
}

void rowBlock(const char *title, const char *marker, RowCache &cache,
              const char *emptyNote) {
    ImGui::TextUnformatted(title);
    const std::vector<std::string> &rows = markerRows(marker, cache);
    if (rows.empty()) {
        ui::TextSubtleWrapped("%s", emptyNote);
        return;
    }
    ImGui::PushFont(AppTheme::fonts().small);
    for (const std::string &row : rows) {
        // Not a format string: a census row legitimately contains '%'.
        ImGui::TextUnformatted(row.c_str());
    }
    ImGui::PopFont();
}

}  // namespace

void ToolPerformance_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(640.0f * AppTheme::uiScale(),
                                    500.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Performance", open)) {
        ImGui::End();
        return;
    }

    // --- the caveat, first ----------------------------------------------------
    // Above the numbers on purpose. A caveat below a histogram is a caveat
    // nobody reads, and this one has already cost this project measurement work.
    const char *synthFields = std::getenv("MDKR_SYNTH_FIELDS");
    const bool synthVariable = synthFields != nullptr && synthFields[0] != '\0';
    const bool synthPacer = platform_pace_is_synthetic() != 0;
    if (synthVariable || synthPacer) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextUnformatted(
            synthVariable ? "MDKR_SYNTH_FIELDS is set — the presentation and "
                            "GPU numbers below are not measurements."
                          : "The pacer is synthetic — the presentation and GPU "
                            "numbers below are not measurements.");
        ImGui::PopStyleColor();
        ui::TextSubtleWrapped(
            "Under synthetic fields the pacer hands the game a fixed field "
            "count per frame and the loop runs as fast as the machine allows, "
            "so present intervals, displayed intervals, alpha phase and queue "
            "depth describe the harness rather than the game. Re-measure with "
            "MDKR_PACE_REALTIME=1 before drawing any conclusion from them. The "
            "simulation tick count and the phase totals below remain "
            "meaningful.");
        ui::Gap(ui::kGapS);
    }

    ui::TextSubtleWrapped(
        "Sourced from the counters the pacing gate already reads. This window "
        "adds no instrumentation of its own.");
    ui::Gap(ui::kGapS);

    if (ImGui::BeginTable("##now", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg)) {
        const auto row = [](const char *label, const char *value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextSubtle("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        };
        char buffer[192];

        std::snprintf(buffer, sizeof buffer, "%d", g_simTickCounter);
        row("Authoritative ticks", buffer);
        std::snprintf(buffer, sizeof buffer, "%d", g_frameCounter);
        row("Present opportunities", buffer);
        std::snprintf(buffer, sizeof buffer, "%llu",
                      static_cast<unsigned long long>(g_surfaceFrameCounter));
        row("Images committed", buffer);

        const char *policy = present_sched_present_policy_name();
        const unsigned rate = present_sched_present_rate();
        if (rate > 0u) {
            std::snprintf(buffer, sizeof buffer, "%s, capped at %u/s",
                          policy != nullptr ? policy : "unknown", rate);
        } else {
            std::snprintf(buffer, sizeof buffer, "%s, no native cap",
                          policy != nullptr ? policy : "unknown");
        }
        row("Present policy", buffer);
        row("Renderer", mdkr_render_backend_name());
        row("Pacing", synthPacer ? "synthetic (deterministic fields per frame)"
                                 : "real time");
        row("Frame smoothing",
            present_sched_smoothing_enabled() ? "on" : "off");
        ImGui::EndTable();
    }

    ui::Gap(ui::kGapS);

    if (!present_perf_enabled()) {
        ui::TextSubtleWrapped(
            "The per-phase census is switched off. Set MDKR_PRESENT_PERF=1 "
            "before launching to have the scheduler emit its section totals, "
            "interval histograms and queue-depth latency — the same rows "
            "check_pacing_quality reads. It is off by default because the "
            "counters cost a clock read per phase per frame.");
    }

    if (ImGui::BeginChild("##census", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        static RowCache sections;
        static RowCache histograms;
        static RowCache latency;
        static RowCache summary;

        rowBlock("Per-phase totals", "[PRESENTPERF] ", sections,
                 "No section rows yet. They are emitted at exit and on the "
                 "scheduler's own cadence; a run that has not reached one has "
                 "none.");
        ui::Gap(ui::kGapS);
        rowBlock("Interval histograms", "[PRESENTPERF-HIST]", histograms,
                 "No histogram rows yet.");
        ui::Gap(ui::kGapS);
        rowBlock("Queue-depth latency", "[PRESENTPERF-LATENCY]", latency,
                 "No latency row yet.");
        ui::Gap(ui::kGapS);
        rowBlock("Scheduler summary", "[PRESENTSCHED-SUMMARY]", summary,
                 "No scheduler summary yet; it is written once, at exit.");

        ui::TouchScrollCurrentWindow();
    }
    ImGui::EndChild();

    ImGui::End();
}
