// dev_tools.cpp — see dev_tools.h.
//
// WHY THE TABLE IS FULL BEFORE ANY TOOL EXISTS. All six ids are present from
// the first commit with draw == NULL. That is what lets check_dev_tools_purity
// enumerate six rows today and prove every one of them inert, so a tool added
// later registers into a slot the gate is ALREADY testing instead of arriving
// with its own bespoke arm that someone has to remember to write.
//
// WHAT "INERT" HAS TO MEAN HERE. The gate compares two whole runs, so the tool
// surface is not allowed to move authoritative state OR the frame loop's shape.
// Two consequences show up in this file:
//
//   * Default off is a hard early-out, not a hidden window. With Tools.Enabled
//     at 0 nothing in here runs, so a player pays nothing for a surface they
//     never asked for and no arm of the gate has anything to see.
//   * Hotkey dispatch reads ImGui's key state, it does not consume events. The
//     game reads the pad straight from SDL; if opening a window could swallow a
//     key, opening a window would change the race.
#include "dev_tools.h"

#include "video_config.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A registry row. `tool` is the published record; `name` and `key` are the
// host's own business -- the stable machine name the gate keys its rows on and
// the ImGui code behind the human-readable hotkey string.
struct ToolSlot {
    MdkrDevTool tool;
    const char *name;
    ImGuiKey    key;
};

// F1 (overlay) and F10 (FPS readout) are already spoken for by ui_overlay.cpp,
// which reads both from the app prefs. F3..F8 is the first unclaimed run.
ToolSlot g_tools[MDKR_TOOL_COUNT] = {
    {{MDKR_TOOL_DIAGNOSTICS, "Diagnostics", "F3", nullptr}, "diagnostics", ImGuiKey_F3},
    {{MDKR_TOOL_CONSOLE,     "Console",     "F4", nullptr}, "console",     ImGuiKey_F4},
    {{MDKR_TOOL_FREECAM,     "Free Camera", "F5", nullptr}, "freecam",     ImGuiKey_F5},
    {{MDKR_TOOL_COLLISION,   "Collision",   "F6", nullptr}, "collision",   ImGuiKey_F6},
    {{MDKR_TOOL_OBJECTS,     "Objects",     "F7", nullptr}, "objects",     ImGuiKey_F7},
    {{MDKR_TOOL_PERFORMANCE, "Performance", "F8", nullptr}, "performance", ImGuiKey_F8},
};

bool g_open[MDKR_TOOL_COUNT];

// Observability. These exist for the purity gate, which otherwise cannot tell
// "the tool was open and changed nothing" from "the tool was never open" --
// the single most likely way for a gate whose every subject is inert to pass
// while proving nothing.
unsigned long long g_draws = 0;        // frames DevTools_draw() ran with tools on
unsigned long long g_dispatched = 0;   // callbacks actually invoked
bool g_envApplied = false;
bool g_forced = false;
bool g_armedReported = false;

bool validId(MdkrDevToolId id) {
    return id >= 0 && id < MDKR_TOOL_COUNT;
}

bool toolsEnabled() {
    const MdkrVideoConfig *config = mdkr_video_config_current();
    return config != nullptr &&
           config->values[MDKR_TOOLS_ENABLED].number != 0.0f;
}

// MDKR_TOOLS_OPEN=name[,name...] or `all`. Test-facing: the purity gate needs
// a tool open from the first frame of an unattended run, and there is no other
// way to press F5 in a headless race.
//
// Applied lazily rather than at startup because this file has no init hook and
// deliberately wants none -- the resolved config it consults does not exist
// until mdkr_video_config_init() has run.
void ensureEnvOverrides() {
    if (g_envApplied) return;
    g_envApplied = true;

    const char *spec = std::getenv("MDKR_TOOLS_OPEN");
    if (spec == nullptr || spec[0] == '\0') return;

    // Non-destructive scan: getenv's buffer is not ours to write through.
    const char *cursor = spec;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ' ') ++cursor;
        const char *begin = cursor;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ' ') ++cursor;
        const size_t length = static_cast<size_t>(cursor - begin);
        if (length == 0) continue;

        if (length == 3 && std::strncmp(begin, "all", 3) == 0) {
            for (int i = 0; i < MDKR_TOOL_COUNT; ++i) {
                g_open[i] = true;
                g_forced = true;
                std::printf("[TOOLS-OPEN] name=%s id=%d forced=1\n",
                            g_tools[i].name, i);
            }
            continue;
        }

        bool matched = false;
        for (int i = 0; i < MDKR_TOOL_COUNT && !matched; ++i) {
            if (std::strlen(g_tools[i].name) != length ||
                std::strncmp(begin, g_tools[i].name, length) != 0) {
                continue;
            }
            g_open[i] = true;
            g_forced = true;
            matched = true;
            std::printf("[TOOLS-OPEN] name=%s id=%d forced=1\n",
                        g_tools[i].name, i);
        }
        if (!matched) {
            // Loud and machine-readable. A gate that silently forced nothing
            // would compare a run against itself and call it purity.
            std::printf("[TOOLS-OPEN] unknown=%.*s forced=0\n",
                        static_cast<int>(length), begin);
            std::fprintf(stderr,
                         "[dev-tools] MDKR_TOOLS_OPEN names an unknown tool: "
                         "%.*s\n",
                         static_cast<int>(length), begin);
        }
    }
    std::fflush(stdout);
}

// Printed at exit rather than per frame so the count is one line instead of a
// race's worth. Only speaks when the surface was actually used, which is what
// keeps a default-off run byte-identical in its OUTPUT too, not just its state.
struct SummaryOnExit {
    ~SummaryOnExit() {
        if (g_draws == 0 && !g_forced) return;
        std::printf("[TOOLS-SUMMARY] draws=%llu dispatched=%llu forced=%d\n",
                    g_draws, g_dispatched, g_forced ? 1 : 0);
        std::fflush(stdout);
    }
};
SummaryOnExit g_summaryOnExit;

}  // namespace

void DevTools_register(MdkrDevToolId id, MdkrDevToolDraw draw) {
    if (!validId(id)) return;
    g_tools[id].tool.draw = draw;
}

bool DevTools_wantsFrame(void) {
    return toolsEnabled();
}

bool DevTools_isOpen(MdkrDevToolId id) {
    if (!validId(id)) return false;
    // The enable key folds into the answer on purpose: with the surface off a
    // tool is not "open but hidden", it is not open. Every consumer -- the
    // render predicate here, the engine-side substitutions Tasks 5-7 will
    // add -- then rides one gate instead of each remembering to check two.
    if (!toolsEnabled()) return false;
    ensureEnvOverrides();
    return g_open[id];
}

void DevTools_setOpen(MdkrDevToolId id, bool open) {
    if (!validId(id)) return;
    ensureEnvOverrides();
    g_open[id] = open;
}

void DevTools_draw(void) {
    // Default off: the whole surface costs one comparison per frame.
    if (!toolsEnabled()) return;
    ensureEnvOverrides();

    ++g_draws;
    if (!g_armedReported) {
        g_armedReported = true;
        std::printf("[TOOLS-ARMED] enabled=1 tools=%d\n", MDKR_TOOL_COUNT);
        std::fflush(stdout);
    }

    for (int i = 0; i < MDKR_TOOL_COUNT; ++i) {
        ToolSlot &slot = g_tools[i];
        // Reads ImGui's key state; it does not claim the key. See the file
        // header: a tool that could swallow input would change the race.
        if (slot.key != ImGuiKey_None &&
            ImGui::IsKeyPressed(slot.key, /*repeat=*/false)) {
            g_open[i] = !g_open[i];
        }
        if (!g_open[i] || slot.tool.draw == nullptr) continue;
        ++g_dispatched;
        slot.tool.draw(&g_open[i]);
    }
}

void DevTools_dumpTable(void) {
    // Deliberately reads nothing: no config, no ImGui, no window. The gate runs
    // this before it has a run to inspect, and a dump that needed the engine up
    // would be a dump the gate could not trust to enumerate the table itself.
    for (int i = 0; i < MDKR_TOOL_COUNT; ++i) {
        const ToolSlot &slot = g_tools[i];
        std::printf("[TOOLTABLE] id=%d name=%s title=\"%s\" hotkey=%s "
                    "registered=%d\n",
                    static_cast<int>(slot.tool.id), slot.name,
                    slot.tool.title, slot.tool.hotkey,
                    slot.tool.draw != nullptr ? 1 : 0);
    }
    std::printf("[TOOLTABLE-SUMMARY] tools=%d\n", MDKR_TOOL_COUNT);
    std::fflush(stdout);
}
