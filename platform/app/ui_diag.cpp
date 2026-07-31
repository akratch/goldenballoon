// ui_diag.cpp — the launcher's Diagnostics panel: the log, and a copy of it.
//
// DiagLog tees the engine's stdout/stderr into a ring buffer and a rotating
// file, so the thing a player is asked to attach to a bug report is one button
// away instead of "run it from a terminal and paste what you see".
#include "ui_launcher.h"
#include "app_theme.h"
#include "diag_log.h"
#include "ui_common.h"

#include "platform_os.h"   // mdkr_render_backend_name

#include "imgui.h"

#include <SDL.h>

#include <cstdio>
#include <vector>

void DiagPanel_draw(LauncherState &s, LauncherAction &out) {
    (void)s;
    (void)out;

    ui::SectionHeader("Diagnostics",
                      "Engine output from this session. Attach this to a bug report.");

    // Environment summary: the handful of facts that explain most reports.
    SDL_version linked;
    SDL_GetVersion(&linked);
    ui::TextSubtle("Renderer: %s   \xE2\x80\xA2   SDL %d.%d.%d   \xE2\x80\xA2   %s",
                   mdkr_render_backend_name(),
                   linked.major, linked.minor, linked.patch,
                   SDL_GetPlatform());
    const char *logPath = DiagLog_path();
    if (logPath && logPath[0]) {
        ui::TextSubtle("Log file: %s", logPath);
    } else {
        ui::TextSubtle("Log file: (not installed on this platform)");
    }

    ui::Gap(ui::kGapM);

    // The ring buffer. Sized generously; DiagLog_snapshot NUL-terminates and
    // reports the byte count, so a short read is not mistaken for an empty log.
    static std::vector<char> buf(64 * 1024);
    int n = DiagLog_snapshot(buf.data(), (int)buf.size());

    if (ImGui::Button("Copy Log to Clipboard", ui::kBtnWide())) {
        ImGui::SetClipboardText(n > 0 ? buf.data() : "(log is empty)");
    }

    ui::Gap(ui::kGapS);
    ImGui::BeginChild("##log", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (n > 0) {
        ImGui::PushFont(AppTheme::fonts().small);
        // TextUnformatted with an explicit end pointer: the log is not a
        // format string and may legitimately contain '%'.
        ImGui::TextUnformatted(buf.data(), buf.data() + n);
        ImGui::PopFont();
    } else {
        ui::TextSubtle("Nothing logged yet this session.");
    }
    ImGui::EndChild();
}
