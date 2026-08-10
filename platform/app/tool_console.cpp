// tool_console.cpp — see tool_console.h.
//
// THREE RULES THIS FILE IS BUILT AROUND.
//
//  1. IT DOES NOT PARSE. Every line goes through mdkr_dev_command_parse(),
//     which resolves `set`/`get` keys through mdkr_video_key_from_name() — the
//     same lookup the ini loader and --video-set use — and refuses anything the
//     schema does not name. A second parser here would be a second, untested
//     definition of what the console accepts, and the one that mattered would
//     be the untested one.
//
//  2. IT DOES NOT WRITE SETTINGS ITSELF. `set` goes through
//     mdkr_video_config_runtime_set() (and AppWindow_requestMode for the one
//     key that needs a window transition), which is exactly what
//     ui_settings.cpp's commitEdit does. Validation, precedence, persistence
//     and the deferred-apply boundary are all already correct there; a direct
//     write from here would bypass every one of them.
//
//  3. IT SAYS EVERYTHING TWICE. Output lands in the scrollback AND on stdout,
//     which platform/app/diag_log.cpp tees into mdkr64.log. A console session
//     that only existed on screen would be missing from the very log the
//     Diagnostics window tells people to attach.
//
// THE ONE VERB THAT IS NOT AN OBSERVATION. `warp` changes authoritative state
// by design, which is the opposite of the claim every registered tool makes.
// See warpRefusalReason() below for how it is held.
#include "tool_console.h"

#include "app_theme.h"
#include "app_window.h"        // AppWindow_requestMode — the Window.Mode path
#include "tool_diagnostics.h"  // the shared view of the trace rows in the log
#include "ui_common.h"

#include "dev_command.h"
#include "platform_os.h"       // platformGetSdlWindow
#include "video_config.h"

#include "imgui.h"

#include <SDL.h>

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int    kScrollbackMax = 512;
constexpr int    kHistoryMax = 64;
constexpr int    kEventRows = 32;

/*
 * THE PURITY MARKER.
 *
 * tests/check_dev_tools_purity.py forces a tool open in an unattended race by
 * setting MDKR_TOOLS_OPEN=<tool name>; that is the only variable it sets that
 * means "this run exists to prove the tool surface changed nothing". Keying the
 * warp refusal off that exact variable is what makes the guard testable by the
 * gate it protects, rather than off some second flag that could be set in one
 * place and checked in another.
 */
const char *const kPurityMarker = "MDKR_TOOLS_OPEN";

struct ConsoleState {
    std::vector<std::string> scrollback;
    std::vector<std::string> history;
    int    historyCursor = -1;         // -1 == editing a fresh line
    char   input[MDKR_DEV_COMMAND_LINE_MAX + 1] = {0};
    bool   scrollToBottom = false;
    bool   reclaimFocus = false;
    bool   greeted = false;
};

ConsoleState g_console;

// Scrollback + the diagnostic log, in that order, for every line the console
// produces. Rule 3 in the file header.
void emit(const char *text) {
    if (text == nullptr) return;
    g_console.scrollback.emplace_back(text);
    if (static_cast<int>(g_console.scrollback.size()) > kScrollbackMax) {
        g_console.scrollback.erase(g_console.scrollback.begin());
    }
    g_console.scrollToBottom = true;
    std::printf("[console] %s\n", text);
    std::fflush(stdout);
}

void emitf(const char *format, ...) IM_FMTARGS(1);

void emitf(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    emit(buffer);
}

// --- reporting helpers -------------------------------------------------------

const char *runtimeResultText(MdkrVideoRuntimeResult result) {
    switch (result) {
        case MDKR_VIDEO_RUNTIME_LIVE:     return "applied now";
        case MDKR_VIDEO_RUNTIME_RESTART:  return "saved; takes effect next launch";
        case MDKR_VIDEO_RUNTIME_LOCKED:
            return "refused: the environment or the command line pins this key";
        case MDKR_VIDEO_RUNTIME_SAVE_FAILED:
            return "refused: the settings file could not be written";
        case MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED:
            return "applied, but the settings file may not survive a power loss";
        case MDKR_VIDEO_RUNTIME_PENDING:
            return "queued; it applies with the next picture";
        case MDKR_VIDEO_RUNTIME_APPLY_FAILED:
            return "refused: the system would not make that change";
        case MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED:
            return "failed, and the previous state was not restored";
        case MDKR_VIDEO_RUNTIME_UNAVAILABLE:
            return "not applied: there is no window to apply it to";
        case MDKR_VIDEO_RUNTIME_SUPERSEDED:
            return "replaced by a newer request already queued";
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:                          return "refused: not a valid value";
    }
}

const char *scopeText(MdkrVideoScope scope) {
    switch (scope) {
        case MDKR_VIDEO_SCOPE_LIVE:  return "live";
        case MDKR_VIDEO_SCOPE_LEVEL: return "next level";
        default:                     return "next launch";
    }
}

const char *sourceText(MdkrVideoSource source) {
    switch (source) {
        case MDKR_VIDEO_SOURCE_DEFAULT:  return "default";
        case MDKR_VIDEO_SOURCE_FILE:     return "settings file";
        case MDKR_VIDEO_SOURCE_PRESET:   return "preset";
        case MDKR_VIDEO_SOURCE_LAUNCHER: return "launcher";
        case MDKR_VIDEO_SOURCE_RUNTIME:  return "you";
        case MDKR_VIDEO_SOURCE_ENV:      return "environment";
        default:                         return "command line";
    }
}

void formatValue(const MdkrVideoSchema *schema, const MdkrVideoValue *value,
                 char *out, size_t capacity) {
    if (schema == nullptr || value == nullptr) {
        std::snprintf(out, capacity, "?");
        return;
    }
    switch (schema->type) {
        case MDKR_VIDEO_TYPE_STRING:
            std::snprintf(out, capacity, "%s", value->text);
            break;
        case MDKR_VIDEO_TYPE_INT:
            std::snprintf(out, capacity, "%d", static_cast<int>(value->number));
            break;
        default:
            std::snprintf(out, capacity, "%.3f",
                          static_cast<double>(value->number));
            break;
    }
}

// --- verbs -------------------------------------------------------------------

void runHelp() {
    emit("help                  this list");
    emit("get <key>             read one setting, and say where its value came from");
    emit("set <key> <value>     change one setting through the same path the "
         "settings panel uses");
    emit("hash                  the most recent authoritative-state row");
    emit("dump events           the last gameplay-event rows the trace wrote");
    emit("warp <track>          race another track; this changes the race, so "
         "it is refused while a tool is being proved inert");
    emit("Keys are the names in the settings schema, for example "
         "Video.RenderScale. Tab completes a command; Up and Down walk what you "
         "have already typed.");
}

void runGet(const MdkrDevCommandResult &command) {
    const MdkrVideoSchema *schema = mdkr_video_schema(command.key);
    const MdkrVideoConfig *desired = mdkr_video_config_desired();
    const MdkrVideoConfig *live = mdkr_video_config_current();
    if (schema == nullptr || desired == nullptr || live == nullptr) {
        emit("get: the settings system is not initialised yet");
        return;
    }
    char chosen[MDKR_VIDEO_STRING_MAX];
    char running[MDKR_VIDEO_STRING_MAX];
    formatValue(schema, &desired->values[command.key], chosen, sizeof chosen);
    formatValue(schema, &live->values[command.key], running, sizeof running);
    emitf("%s = %s (from %s, applies %s)", schema->name, chosen,
          sourceText(desired->values[command.key].source),
          scopeText(schema->scope));
    if (std::strcmp(chosen, running) != 0) {
        emitf("  the running engine still has %s", running);
    }
    if (mdkr_video_config_runtime_locked(command.key) != 0) {
        emit("  locked: a higher-precedence source owns this key, so set will "
             "be refused");
    }
}

void runSet(const MdkrDevCommandResult &command) {
    const MdkrVideoSchema *schema = mdkr_video_schema(command.key);
    if (schema == nullptr) {
        emit("set: the settings system is not initialised yet");
        return;
    }
    const char *value = command.args[1].text;
    /* The same two-way split ui_settings.cpp's commitEdit() makes, and for the
     * same reason: Window.Mode is an SDL transition that has to be applied and
     * rolled back around its own persistence, which AppWindow_requestMode owns.
     * Everything else is an ordinary validated config transaction. */
    const MdkrVideoRuntimeResult result =
        command.key == MDKR_WINDOW_MODE
            ? AppWindow_requestMode(
                  static_cast<SDL_Window *>(platformGetSdlWindow()), value)
            : mdkr_video_config_runtime_set(command.key, value);
    emitf("%s = %s: %s", schema->name, value, runtimeResultText(result));
    if (mdkr_video_runtime_result_applied(result) &&
        (command.key == MDKR_INPUT_RUMBLE_ENABLED ||
         command.key == MDKR_INPUT_RUMBLE_PROFILE)) {
        platform_pad_rumble_preferences_changed();
    }
}

void runHash() {
    const std::vector<std::string> rows =
        ToolDiagnostics_recentLogLines("[SIMHASH]", 1);
    if (rows.empty()) {
        emit("hash: no rows yet. The authoritative-state stream is written only "
             "when MDKR_STATE_HASH is set.");
        return;
    }
    emit(rows.front().c_str());
}

void runDump(const MdkrDevCommandResult &command) {
    const char *target = command.args[0].text;
    if (std::strcmp(target, "events") != 0) {
        emitf("dump: '%s' is not something to dump. The only target is 'events'.",
              target);
        return;
    }
    const std::vector<std::string> rows =
        ToolDiagnostics_recentLogLines("[EVENTHASH]", kEventRows);
    if (rows.empty()) {
        emit("dump events: nothing recorded. The gameplay event trace writes "
             "its rows only when MDKR_EVENT_HASH is set.");
        return;
    }
    for (const std::string &row : rows) {
        emit(row.c_str());
    }
}

/*
 * Non-NULL when warp must be refused, and the text is the reason.
 *
 * Every other verb in this file is an observation. A warp is not: it tears down
 * the loaded level and builds another one, which is authoritative state moving
 * because a tool asked it to. That is exactly what the registry's claim forbids
 * and what check_dev_tools_purity.py compares two whole races to catch, so the
 * verb is held shut for the duration of any run that gate is driving.
 */
const char *warpRefusalReason() {
    const char *marker = std::getenv(kPurityMarker);
    if (marker != nullptr && marker[0] != '\0') {
        return "refused: this run was started with MDKR_TOOLS_OPEN set, which "
               "means a tool was opened to prove it changes nothing. A warp "
               "would change the race and make that proof meaningless.";
    }
    return nullptr;
}

void runWarp(const MdkrDevCommandResult &command) {
    const long long track = command.args[0].int_value;
    const char *refusal = warpRefusalReason();
    if (refusal != nullptr) {
        emitf("warp %lld: %s", track, refusal);
        return;
    }
    // The warning comes first and unconditionally: it is true of the command,
    // not of whether the transition below happens to succeed.
    emit("warp: this changes the race, so a warped run can no longer be "
         "compared with a clean one.");
    emitf("warp %lld: not applied. Swapping the loaded level tears down the "
          "level heap, and there is no boundary an observer tool reaches where "
          "that is safe. Relaunch with MDKR_LOAD_TRACK=%lld to race it.",
          track, track);
}

// --- one submitted line ------------------------------------------------------

// The first whitespace-delimited token, for the "did you mean" suggestion. The
// parser reports an unknown verb but does not hand back the token it rejected.
std::string firstToken(const std::string &line) {
    size_t begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::string();
    const size_t end = line.find_first_of(" \t", begin);
    return line.substr(begin,
                       end == std::string::npos ? std::string::npos : end - begin);
}

void submit(const std::string &line) {
    emitf("> %s", line.c_str());

    MdkrDevCommandResult command;
    mdkr_dev_command_parse(line.c_str(), line.size(), &command);
    if (!command.ok) {
        emit(command.error);
        if (command.verb == MDKR_DEV_CMD_COUNT) {
            const std::string token = firstToken(line);
            const char *nearest = mdkr_dev_command_suggest(token.c_str());
            if (nearest != nullptr && token != nearest) {
                emitf("did you mean '%s'?", nearest);
            }
        }
        return;
    }

    switch (command.verb) {
        case MDKR_DEV_CMD_HELP: runHelp(); break;
        case MDKR_DEV_CMD_GET:  runGet(command); break;
        case MDKR_DEV_CMD_SET:  runSet(command); break;
        case MDKR_DEV_CMD_HASH: runHash(); break;
        case MDKR_DEV_CMD_DUMP: runDump(command); break;
        case MDKR_DEV_CMD_WARP: runWarp(command); break;
        default:                emit("that command is not implemented"); break;
    }
}

// --- input line --------------------------------------------------------------

void replaceInput(const std::string &text, ImGuiInputTextCallbackData *data) {
    data->DeleteChars(0, data->BufTextLen);
    if (!text.empty()) {
        data->InsertChars(0, text.c_str());
    }
}

int inputCallback(ImGuiInputTextCallbackData *data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Verb completion only. Values are schema-owned and `get`/`set` already
        // name the key they rejected, which is the better teacher for those.
        const std::string typed(data->Buf, static_cast<size_t>(data->BufTextLen));
        const std::string token = firstToken(typed);
        if (token.empty() || token.size() != typed.size()) {
            return 0;
        }
        const char *nearest = mdkr_dev_command_suggest(token.c_str());
        if (nearest != nullptr) {
            replaceInput(std::string(nearest) + " ", data);
        }
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (g_console.history.empty()) return 0;
        const int last = static_cast<int>(g_console.history.size()) - 1;
        if (data->EventKey == ImGuiKey_UpArrow) {
            g_console.historyCursor =
                g_console.historyCursor < 0 ? last
                                            : (g_console.historyCursor > 0
                                                   ? g_console.historyCursor - 1
                                                   : 0);
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (g_console.historyCursor < 0) return 0;
            if (g_console.historyCursor >= last) {
                g_console.historyCursor = -1;
                replaceInput(std::string(), data);
                return 0;
            }
            g_console.historyCursor++;
        } else {
            return 0;
        }
        replaceInput(
            g_console.history[static_cast<size_t>(g_console.historyCursor)],
            data);
    }
    return 0;
}

}  // namespace

void ToolConsole_draw(bool *open) {
    ImGui::SetNextWindowSize(ImVec2(620.0f * AppTheme::uiScale(),
                                    380.0f * AppTheme::uiScale()),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console", open)) {
        ImGui::End();
        return;
    }

    if (!g_console.greeted) {
        g_console.greeted = true;
        emit("Type help for the list of commands.");
    }

    // Scrollback first so the input line sits at the bottom, where a console
    // puts it. Reserve exactly one line plus the separator for the input.
    const float inputHeight = ImGui::GetFrameHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##scrollback", ImVec2(0, -inputHeight), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(AppTheme::fonts().small);
    for (const std::string &line : g_console.scrollback) {
        // Not a format string: output may legitimately contain '%'.
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopFont();
    if (g_console.scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        g_console.scrollToBottom = false;
    }
    ui::TouchScrollCurrentWindow();
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackCompletion |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputText("##command", g_console.input, sizeof g_console.input,
                         flags, &inputCallback)) {
        const std::string line = g_console.input;
        g_console.input[0] = '\0';
        g_console.historyCursor = -1;
        if (!firstToken(line).empty()) {
            g_console.history.push_back(line);
            if (static_cast<int>(g_console.history.size()) > kHistoryMax) {
                g_console.history.erase(g_console.history.begin());
            }
            submit(line);
        }
        g_console.reclaimFocus = true;
    }

    ImGui::SetItemDefaultFocus();
    if (g_console.reclaimFocus) {
        g_console.reclaimFocus = false;
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
}
