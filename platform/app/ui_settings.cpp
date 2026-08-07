// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_window.h"
#include "ui_common.h"

#include "controller_mapping.h"
#include "video_config.h"
#include "platform_os.h"

#include "imgui.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// --- Status line -----------------------------------------------------------
// The panel never claims success it did not observe: every edit routes through
// mdkr_video_config_runtime_set and its verdict is shown verbatim.
std::string g_status;
ImVec4      g_statusColor;

struct EditState {
    bool initialized = false;
    bool active = false;
    bool dirty = false;
    char text[MDKR_VIDEO_STRING_MAX] = {0};
    float number = 0.0f;
    std::string error;
};

std::array<EditState, MDKR_VIDEO_KEY_COUNT> g_edits;
bool g_uiScaleInitialized = false;
bool g_uiScaleDirty = false;
float g_uiScaleEdit = 1.0f;
std::string g_uiScaleError;
bool g_frameLimitRectValid = false;
ImVec2 g_frameLimitRectMin;
ImVec2 g_frameLimitRectMax;
bool g_frameLimitPopupOpen = false;
int g_frameLimitFocusedIndex = -1;
bool g_frameLimitRetryRectValid = false;
ImVec2 g_frameLimitRetryRectMin;
ImVec2 g_frameLimitRetryRectMax;
bool g_uiScaleRectValid = false;
ImVec2 g_uiScaleRectMin;
ImVec2 g_uiScaleRectMax;
bool g_smokeGamepadFocusUsed = false;
// Rendered rectangle of each presentation-pace choice, for smoke observation
// only. Indexed by MdkrPresentationPace, so slot 0 (Custom) stays unused —
// Custom is a reading of the two keys and never a control to press.
bool g_paceRectValid[3] = {false, false, false};
ImVec2 g_paceRectMin[3];
ImVec2 g_paceRectMax[3];

void setStatus(const char *text, const ImVec4 &color) {
    g_status = text ? text : "";
    g_statusColor = color;
}

bool drawSettingsSectionHeader(const char *label,
                               ImGuiTreeNodeFlags flags) {
    // These rows are independent expandable sections, not mutually exclusive
    // tabs. Keep resting and hover surfaces neutral; the chevron and a gold
    // leading rule communicate the open state without making every open
    // section look like another selected destination.
    ImGui::PushStyleColor(ImGuiCol_Header, AppTheme::hex(0x29292C));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, AppTheme::hex(0x38383C));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, AppTheme::hex(0x424247));
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    if (open) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            min, ImVec2(min.x + 3.0f * AppTheme::uiScale(), max.y),
            ImGui::GetColorU32(AppTheme::accent()),
            2.0f * AppTheme::uiScale());
    }
    return open;
}

void reportResult(MdkrVideoRuntimeResult r, const MdkrVideoSchema *s) {
    char buf[320];
    switch (r) {
        case MDKR_VIDEO_RUNTIME_LIVE:
            /* LIVE is the setter's verdict about persistence and precedence,
             * not about WHEN the engine picks the value up — a LEVEL-scoped key
             * returns it too. Saying "applied" for one of those would claim a
             * change the player can see is not on screen yet. */
            if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) {
                std::snprintf(buf, sizeof(buf),
                              "%s is set — it takes effect the next time a "
                              "track loads.", s->label);
                setStatus(buf, AppTheme::accent());
            } else {
                std::snprintf(buf, sizeof(buf), "%s applied.", s->label);
                setStatus(buf, AppTheme::good());
            }
            break;
        case MDKR_VIDEO_RUNTIME_RESTART:
            std::snprintf(buf, sizeof(buf),
                          "%s saved — it takes effect the next time the game starts.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_LOCKED:
            std::snprintf(buf, sizeof(buf),
                          "%s is fixed by %s (or a command-line override) and cannot be "
                          "changed here.", s->label, s->env);
            setStatus(buf, AppTheme::subtle());
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "%s could NOT be saved — the settings file is not writable. "
                          "Nothing was changed.", s->label);
            setStatus(buf, AppTheme::bad());
            if (std::getenv("MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE")) {
                std::fprintf(stderr,
                             "[app-ui-test] visible settings error: %s\n", buf);
            }
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED:
            std::snprintf(buf, sizeof(buf),
                          "%s applied, but the operating system could not confirm durable "
                          "storage. It may need to be selected again after an unexpected "
                          "shutdown.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_PENDING:
            std::snprintf(buf, sizeof(buf),
                          "%s will apply at the next safe frame boundary.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_SUPERSEDED:
            std::snprintf(buf, sizeof(buf),
                          "%s will apply at the next safe frame boundary — this "
                          "replaces the choice that was still waiting.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_UNAVAILABLE:
            std::snprintf(buf, sizeof(buf),
                          "%s could not be changed right now: there is no game "
                          "window to apply it to. Try again once the game is "
                          "showing.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_APPLY_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "%s could not be applied by the operating system. "
                          "The previous setting was kept.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED:
            std::snprintf(
                buf, sizeof(buf),
                "%s could not be saved, and the operating system did not "
                "restore the previous window state. The saved preference is "
                "unchanged; use F11 or Alt+Enter, or restart the app.",
                s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:
            std::snprintf(buf, sizeof(buf), "That value is not valid for %s.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
    }
}

bool resultSucceeded(MdkrVideoRuntimeResult result) {
    return mdkr_video_runtime_result_applied(result) != 0;
}

bool commitEdit(SDL_Window *window, MdkrVideoKey key,
                const MdkrVideoSchema *schema,
                EditState &edit, const char *value) {
    const MdkrVideoRuntimeResult result = key == MDKR_WINDOW_MODE
        ? AppWindow_requestMode(window, value)
        : mdkr_video_config_runtime_set(key, value);
    reportResult(result, schema);
    /* Both spellings of "queued for the next safe frame boundary" leave the
     * widget dirty and error-free; the completion resynchronizes it later. */
    if (result == MDKR_VIDEO_RUNTIME_PENDING ||
        result == MDKR_VIDEO_RUNTIME_SUPERSEDED) {
        edit.error.clear();
        return false;
    }
    if (resultSucceeded(result)) {
        edit.dirty = false;
        edit.initialized = false;  // resync from the authoritative desired value
        edit.error.clear();
        if (key == MDKR_INPUT_RUMBLE_ENABLED ||
            key == MDKR_INPUT_RUMBLE_PROFILE) {
            platform_pad_rumble_preferences_changed();
        }
        return true;
    }
    edit.error = g_status;
    if (mdkr_video_key_is_audio(key)) {
        mdkr_audio_config_runtime_cancel_preview();
    }
    return false;
}

// --- Value access ----------------------------------------------------------
// `desired` is what the player has chosen (LIVE + staged RESTART); `current` is
// what the running engine actually has. The difference between them is exactly
// what "needs a restart" means, so the panel shows both rather than pretending.
const MdkrVideoValue *desired(MdkrVideoKey k) {
    const MdkrVideoConfig *c = mdkr_video_config_desired();
    return c ? &c->values[k] : nullptr;
}

const MdkrVideoValue *live(MdkrVideoKey k) {
    const MdkrVideoConfig *c = mdkr_video_config_current();
    return c ? &c->values[k] : nullptr;
}

bool differsFromLive(MdkrVideoKey k, const MdkrVideoSchema *s) {
    const MdkrVideoValue *d = desired(k), *l = live(k);
    if (!d || !l) return false;
    if (s->type == MDKR_VIDEO_TYPE_STRING) return std::strcmp(d->text, l->text) != 0;
    return d->number != l->number;
}

void formatValue(MdkrVideoKey key, const MdkrVideoSchema *s,
                 const MdkrVideoValue *v, char *out, size_t cap) {
    if (!v) { std::snprintf(out, cap, "?"); return; }
    switch (s->type) {
        case MDKR_VIDEO_TYPE_STRING: std::snprintf(out, cap, "%s", v->text); break;
        case MDKR_VIDEO_TYPE_INT:
            if (mdkr_video_key_is_audio(key)) {
                std::snprintf(out, cap, "%d%%", (int)v->number);
            } else {
                std::snprintf(out, cap, "%d", (int)v->number);
            }
            break;
        default:                     std::snprintf(out, cap, "%.2f", (double)v->number); break;
    }
}

// Where a value came from, in the player's words. This is the honest answer to
// "why is this greyed out?".
const char *sourceName(MdkrVideoSource src) {
    switch (src) {
        case MDKR_VIDEO_SOURCE_DEFAULT:  return "default";
        case MDKR_VIDEO_SOURCE_FILE:     return "settings file";
        case MDKR_VIDEO_SOURCE_PRESET:   return "presentation preset";
        case MDKR_VIDEO_SOURCE_LAUNCHER: return "launcher";
        case MDKR_VIDEO_SOURCE_RUNTIME:  return "you";
        case MDKR_VIDEO_SOURCE_ENV:      return "environment variable";
        case MDKR_VIDEO_SOURCE_CLI:      return "command line";
        default:                         return "unknown";
    }
}

// --- Enumerated string options --------------------------------------------
// The schema constrains these values in mdkr_video_config_set(); the tables
// here mirror exactly what those validators accept, so the UI can only ever
// offer a value the config layer will take. Implemented settings with an
// open-ended domain (Aspect and GameplayFOV) get a text field instead.
struct Option { const char *value; const char *label; };
struct Options { const Option *items; int count; };

constexpr const char *kOriginalFrameLimitLabel =
    "Original (recommended)";
constexpr const char *kModernFrameLimitGroup =
    "Higher refresh rates";
constexpr const char *kFrameLimitHelp =
    "Original presents each authored image once. Higher rates repeat authored "
    "images when smoothing is Off, or create in-between images when it is "
    "Interpolated. Gameplay speed does not change. Higher rates can use more "
    "CPU and GPU time. Rates above your display's refresh need a display "
    "connection that can drop an image it has not shown yet. Where the system "
    "does not offer one, they present at your display's refresh instead, "
    "unless Allow Tearing is on. A European 50 Hz game paces unevenly under "
    "Original: its authored image lasts 40 ms, which no whole "
    "number of 60 Hz refreshes fits, so Original holds it for two refreshes "
    "and then three and the motion ripples. Match Display with Interpolated "
    "removes that ripple without changing game speed, music pitch, or timers. "
    "Just Under Display is for a display with a variable refresh rate: it "
    "paces a few Hz below the top of that range, which keeps the display "
    "adapting to the game rather than falling back to a fixed refresh, and it "
    "re-reads the rate if you move the window to another monitor. 40 Hz is a "
    "battery-friendly choice on a handheld whose display runs at 40 or 120 Hz, "
    "where every image is held for the same length of time. "
    "Uncapped removes the native limit only when new interpolated images are "
    "available; held frames stay display-paced. A "
    "browser always maps Uncapped and Just Under Display to Match Display.";

const Option kCadence[] = {
    {"original", "Original (recommended)"},
    {"enhanced", "Enhanced (compatibility; changes gameplay)"},
};
const Option kFrameLimit[] = {
    {"original", kOriginalFrameLimitLabel},
    {"display",  "Match Display"},
    // Named for what it does rather than for the hardware feature it suits:
    // a player who has a variable-refresh display knows they have one, and a
    // player who does not is not helped by the acronym.
    {"display-margin", "Just Under Display"},
    {"30",       "30 Hz"},
    {"40",       "40 Hz (battery friendly)"},
    {"60",       "60 Hz"},
    {"90",       "90 Hz"},
    {"120",      "120 Hz"},
    {"144",      "144 Hz"},
    {"165",      "165 Hz"},
    {"240",      "240 Hz"},
    {"uncapped", "Uncapped (native)"},
};
const Option kMotionSmoothing[] = {
    {"interpolate", "Interpolated"},
    {"off",         "Off (original motion)"},
};
const Option kAllowTearing[] = {
    {"off", "Off (tear-free)"},
    {"on",  "On (lowest latency)"},
};
const Option kMode[] = {
    {"pure",       "Pure (reference presentation)"},
    {"restored",   "Restored (recommended)"},
    {"remastered", "Remastered (work in progress)"},
};
// The canonical spellings only. mdkr_video_world_shadows_canonical() also takes
// "0"/"1"/"on"/"" so the MDKR_WORLD_SHADOW diagnostic seam keeps working, but a
// combo that offered two words for the same state would be a worse control.
const Option kShadows[] = {
    {"full", "Full"}, {"soft", "Soft"}, {"off", "Off"},
};
// The two player-facing spellings only. mdkr_video_camera_obstruction_canonical()
// also takes "legacy" and "center-ray" so the MDKR_CAMERA_OBSTRUCTION diagnostic
// seam keeps working, but those are A/B arms, not states to offer a player.
// Default first, as everywhere else in this table. The corrected camera was
// the default for one wave; device acceptance sent it back to opt-in, so
// Authored leads again and neither label recommends the other.
const Option kCameraObstruction[] = {
    {"observe", "Authored (original camera)"},
    {"modern", "Keep the camera out of walls"},
};
const Option kCameraComfort[] = {
    {"authored", "Authored motion"},
    {"reduced", "Reduced motion"},
};
const Option kMenuLanguages[] = {
    {"all", "All (every language on the disc)"},
    {"authentic", "Authentic (this disc's own retail menu)"},
};
const Option kWindowMode[] = {
    {"windowed", "Windowed"},
    {"fullscreen", "Fullscreen (borderless)"},
};
const Option kRumbleProfile[] = {
    {"light", "Light (35%)"},
    {"balanced", "Balanced (65%)"},
    {"strong", "Strong (100%)"},
};
const Option kControllerAction[] = {
    {"none", "None"},
    {"a", "N64 A"},
    {"b", "N64 B"},
    {"z", "N64 Z trigger"},
    {"start", "N64 Start"},
    {"l", "N64 L"},
    {"r", "N64 R"},
    {"dpad_up", "N64 D-pad up"},
    {"dpad_down", "N64 D-pad down"},
    {"dpad_left", "N64 D-pad left"},
    {"dpad_right", "N64 D-pad right"},
    {"c_up", "N64 C-up"},
    {"c_down", "N64 C-down"},
    {"c_left", "N64 C-left"},
    {"c_right", "N64 C-right"},
};

bool optionsFor(MdkrVideoKey k, Options &out) {
    if (k >= MDKR_INPUT_CONTROLLER_A &&
        k <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT) {
        out = {kControllerAction,
               static_cast<int>(sizeof(kControllerAction) /
                                sizeof(kControllerAction[0]))};
        return true;
    }
    switch (k) {
        case MDKR_VIDEO_SIMULATION_CADENCE: out = {kCadence, 2}; return true;
        case MDKR_VIDEO_FRAME_LIMIT:
            out = {kFrameLimit,
                   static_cast<int>(std::size(kFrameLimit))};
            return true;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            out = {kMotionSmoothing, 2}; return true;
        case MDKR_VIDEO_ALLOW_TEARING:      out = {kAllowTearing, 2}; return true;
        case MDKR_VIDEO_MODE:               out = {kMode, 3}; return true;
        case MDKR_VIDEO_WORLD_SHADOWS:      out = {kShadows, 3}; return true;
        case MDKR_VIDEO_CAMERA_OBSTRUCTION:
            out = {kCameraObstruction, 2}; return true;
        case MDKR_VIDEO_CAMERA_COMFORT:
            out = {kCameraComfort, 2}; return true;
        case MDKR_VIDEO_MENU_LANGUAGES:
            out = {kMenuLanguages, 2}; return true;
        case MDKR_WINDOW_MODE:              out = {kWindowMode, 2}; return true;
        case MDKR_INPUT_RUMBLE_PROFILE:     out = {kRumbleProfile, 3}; return true;
        default: return false;
    }
}

const char *optionLabel(MdkrVideoKey key, const char *value) {
    if (key == MDKR_VIDEO_MODE && std::strcmp(value, "custom") == 0) {
        return "Custom (Individual Settings)";
    }
    Options options;
    if (!optionsFor(key, options)) return value;
    for (int i = 0; i < options.count; ++i) {
        if (std::strcmp(options.items[i].value, value) == 0) {
            return options.items[i].label;
        }
    }
    return value;
}

const char *helpFor(MdkrVideoKey key, const MdkrVideoSchema *schema) {
    switch (key) {
        case MDKR_VIDEO_SIMULATION_CADENCE:
            return "Original preserves retail physics, AI, timers, and input "
                   "timing. Enhanced is a compatibility mode for older port "
                   "configurations and changes gameplay speed. It is not an "
                   "FPS setting.";
        case MDKR_VIDEO_FRAME_LIMIT:
            return kFrameLimitHelp;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            return "Interpolated blends adjacent authored presentation states "
                   "at the display's exact fractional time. Simulation, input, "
                   "audio, timers, and saves still advance only on Original "
                   "gameplay ticks. Off shows authored images only.";
        case MDKR_VIDEO_MODE:
            // The section introduction directly above this control explains the
            // three modes; repeating the schema paragraph creates a text wall.
            return nullptr;
        default:
            return schema->help;
    }
}

// --- One row ---------------------------------------------------------------
bool drawKey(SDL_Window *window, MdkrVideoKey k, bool compact) {
    const MdkrVideoSchema *s = mdkr_video_schema(k);
    const MdkrVideoValue  *d = desired(k);
    if (!s || !d) return false;

    const bool locked = mdkr_video_config_runtime_locked(k) != 0;
    const MdkrVideoValue *rumbleEnabled =
        desired(MDKR_INPUT_RUMBLE_ENABLED);
    const bool rumbleProfileUnavailable =
        k == MDKR_INPUT_RUMBLE_PROFILE && rumbleEnabled != nullptr &&
        rumbleEnabled->number == 0.0f;
    bool changed = false;
    EditState &editState = g_edits[static_cast<size_t>(k)];

    ImGui::PushID((int)k);
    if (locked || rumbleProfileUnavailable) ImGui::BeginDisabled();

    ImGui::TextUnformatted(s->label);
    if (s->scope == MDKR_VIDEO_SCOPE_RESTART) {
        ImGui::SameLine();
        ui::RestartBadge();
    } else if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) {
        ui::LiveBadge("next track");
    } else if (mdkr_video_key_apply_domain(k) != MDKR_VIDEO_APPLY_NONE) {
        /* Only the keys that USED to demand a restart say so. Every other LIVE
         * key has always applied immediately and has never carried a badge;
         * giving them one now would add noise to twenty rows to make a point
         * about four. */
        ui::LiveBadge("applies immediately");
    }

    ImGui::SetNextItemWidth(ui::kControlWidth());

    char valueBuf[MDKR_VIDEO_STRING_MAX];
    formatValue(k, s, d, valueBuf, sizeof(valueBuf));

    Options opts;
    if (optionsFor(k, opts)) {
        if (!editState.initialized ||
            (!editState.active && !editState.dirty &&
             std::strcmp(editState.text, d->text) != 0)) {
            std::snprintf(editState.text, sizeof(editState.text), "%s", d->text);
            editState.initialized = true;
            editState.error.clear();
        }
        if (k == MDKR_VIDEO_FRAME_LIMIT && !g_smokeGamepadFocusUsed &&
            AppUi_smokeInputMode() == AppUiSmokeInputMode::Gamepad) {
            // Deterministic initial nav anchor only. Opening, movement, and
            // activation below still arrive exclusively from SDL's virtual
            // controller through the production ImGui platform backend.
            ImGui::SetKeyboardFocusHere();
            g_smokeGamepadFocusUsed = true;
        }
        int cur = -1;
        for (int i = 0; i < opts.count; ++i) {
            if (std::strcmp(opts.items[i].value, editState.text) == 0) {
                cur = i;
                break;
            }
        }
        const char *preview = cur >= 0 ? opts.items[cur].label : editState.text;
        const bool comboOpen = ImGui::BeginCombo("##v", preview);
        editState.active = comboOpen;
        if (k == MDKR_VIDEO_FRAME_LIMIT) {
            g_frameLimitPopupOpen = comboOpen;
            g_frameLimitRectMin = ImGui::GetItemRectMin();
            g_frameLimitRectMax = ImGui::GetItemRectMax();
            g_frameLimitRectValid = true;
            if (std::getenv("MDKR_APP_UI_INPUT_TRACE")) {
                static int smokeFrame = 0;
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                std::fprintf(stderr,
                             "[app-ui-test] combo frame=%d open=%d hovered=%d "
                             "active=%d focused=%d rect=%.0f,%.0f..%.0f,%.0f "
                             "mouse=%.0f,%.0f\n",
                             smokeFrame++, comboOpen ? 1 : 0,
                             ImGui::IsItemHovered() ? 1 : 0,
                             ImGui::IsItemActive() ? 1 : 0,
                             ImGui::IsItemFocused() ? 1 : 0,
                             g_frameLimitRectMin.x, g_frameLimitRectMin.y,
                             g_frameLimitRectMax.x, g_frameLimitRectMax.y,
                             mouse.x, mouse.y);
            }
        }
        if (comboOpen) {
            for (int i = 0; i < opts.count; ++i) {
                if (k == MDKR_VIDEO_FRAME_LIMIT && i == 1) {
                    ImGui::SeparatorText(kModernFrameLimitGroup);
                }
                const bool selected = i == cur;
                if (ImGui::Selectable(
                        opts.items[i].label, selected, 0,
                        ImVec2(0.0f, ui::kTouchRowHeight()))) {
                    if (k == MDKR_VIDEO_FRAME_LIMIT &&
                        std::getenv("MDKR_APP_UI_INPUT_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[app-ui-test] frame-limit activated "
                                     "index=%d value=%s\n",
                                     i, opts.items[i].value);
                    }
                    std::snprintf(editState.text, sizeof(editState.text), "%s",
                                  opts.items[i].value);
                    editState.dirty = true;
                    changed = commitEdit(
                        window, k, s, editState, editState.text);
                }
                if (k == MDKR_VIDEO_FRAME_LIMIT && ImGui::IsItemFocused()) {
                    g_frameLimitFocusedIndex = i;
                    if (std::getenv("MDKR_APP_UI_INPUT_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[app-ui-test] frame-limit focused "
                                     "index=%d value=%s\n",
                                     i, opts.items[i].value);
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (s->type == MDKR_VIDEO_TYPE_STRING) {
        // Open domain (aspect expressions, or "authored" / a FOV number).
        // Commit on Enter/blur so a half-typed value is never sent
        // to the validator and reported as invalid mid-keystroke.
        if (!editState.initialized ||
            (!editState.active && !editState.dirty &&
             std::strcmp(editState.text, d->text) != 0)) {
            std::snprintf(editState.text, sizeof(editState.text), "%s", d->text);
            editState.initialized = true;
            editState.error.clear();
        }
        const bool entered = ImGui::InputText(
            "##v", editState.text, sizeof(editState.text),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemEdited()) editState.dirty = true;
        const bool commit = entered || ImGui::IsItemDeactivatedAfterEdit();
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            changed = commitEdit(window, k, s, editState, editState.text);
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT && s->min == 0.0f && s->max == 1.0f) {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        bool on = editState.number != 0.0f;
        if (ImGui::Checkbox("##v", &on)) {
            editState.number = on ? 1.0f : 0.0f;
            editState.dirty = true;
            changed = commitEdit(window, k, s, editState, on ? "1" : "0");
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT) {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        int v = static_cast<int>(editState.number);
        const bool previewChanged = ImGui::SliderInt(
            "##v", &v, (int)s->min, (int)s->max,
            mdkr_video_key_is_audio(k) ? "%d%%" : "%d");
        if (previewChanged) {
            editState.number = static_cast<float>(v);
            if (mdkr_video_key_is_audio(k)) {
                (void)mdkr_audio_config_runtime_preview(k, v);
            }
        }
        const bool commit = AppUi_deferredCommit(
            previewChanged, ImGui::IsItemDeactivatedAfterEdit(), &editState.dirty);
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", v);
            changed = commitEdit(window, k, s, editState, buf);
        }
    } else {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        const bool previewChanged = ImGui::SliderFloat(
            "##v", &editState.number, s->min, s->max, "%.2f");
        const bool commit = AppUi_deferredCommit(
            previewChanged, ImGui::IsItemDeactivatedAfterEdit(), &editState.dirty);
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f",
                          static_cast<double>(editState.number));
            changed = commitEdit(window, k, s, editState, buf);
        }
    }

    if (!editState.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", editState.error.c_str());
        ImGui::PopStyleColor();
        const bool retryPressed = editState.dirty && ImGui::Button(
            "Retry Save", ImVec2(0.0f, ui::kBtnSecondary().y));
        if (editState.dirty && k == MDKR_VIDEO_FRAME_LIMIT) {
            g_frameLimitRetryRectMin = ImGui::GetItemRectMin();
            g_frameLimitRetryRectMax = ImGui::GetItemRectMax();
            g_frameLimitRetryRectValid = true;
        }
        if (retryPressed) {
            if (k == MDKR_VIDEO_FRAME_LIMIT) {
                std::fprintf(stderr,
                             "[app-ui-test] Retry save widget activated\n");
            }
            char retry[MDKR_VIDEO_STRING_MAX];
            if (s->type == MDKR_VIDEO_TYPE_STRING) {
                std::snprintf(retry, sizeof(retry), "%s", editState.text);
            } else if (s->type == MDKR_VIDEO_TYPE_INT) {
                std::snprintf(retry, sizeof(retry), "%d",
                              static_cast<int>(editState.number));
            } else {
                std::snprintf(retry, sizeof(retry), "%.2f",
                              static_cast<double>(editState.number));
            }
            changed = commitEdit(window, k, s, editState, retry);
        }
    }

    if (locked || rumbleProfileUnavailable) {
        ImGui::EndDisabled();
        if (rumbleProfileUnavailable && !locked) {
            ui::TextSubtle("Enable rumble to choose a strength.");
        } else if (mdkr_video_config_is_readonly()) {
            ui::TextSubtle("Locked for this Pure session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_CLI) {
            ui::TextSubtle("Fixed by a command-line option for this session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_ENV) {
            ui::TextSubtle("Fixed by %s for this session.", s->env);
        } else {
            ui::TextSubtle("Fixed by the %s for this session.",
                           sourceName(d->source));
        }
    } else if (s->scope == MDKR_VIDEO_SCOPE_RESTART && differsFromLive(k, s)) {
        // The honest RESTART presentation: say what is running NOW and what will
        // be running next launch. Never imply the change already took effect.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Saved for next Play: %s (current: %s)",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    } else if (s->scope == MDKR_VIDEO_SCOPE_LEVEL && differsFromLive(k, s)) {
        // Same honesty, one boundary earlier. The desired/live split is exactly
        // as real here as it is for a RESTART key — video_config_runtime.c
        // deliberately does not copy a staged LEVEL value into the live config
        // until the level applier runs — so the panel can name both without
        // guessing, and "the next time a track loads" is a promise the engine
        // keeps rather than a hope.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Set to %s — takes effect the next time a track loads "
                    "(now: %s)",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    }

    const char *help = helpFor(k, s);
    const bool controllerBinding =
        k >= MDKR_INPUT_CONTROLLER_A &&
        k <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT;
    if (!compact && help && !controllerBinding) {
        ImGui::PushTextWrapPos(0.0f);
        ui::TextSubtle("%s", help);
        ImGui::PopTextWrapPos();
    }
    if (k == MDKR_VIDEO_FRAME_LIMIT &&
        std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedFrameLimit = false;
        if (!tracedFrameLimit) {
            std::fprintf(stderr,
                         "[app-ui] frame-limit value=%s label=%s restartPending=%d\n",
                         d->text, optionLabel(k, d->text),
                         Settings_restartPending() ? 1 : 0);
            std::fprintf(
                stderr,
                "[app-ui] frame-limit-contract recommended=\"%s\" "
                "group=\"%s\" caveat=\"%s\"\n",
                kOriginalFrameLimitLabel, kModernFrameLimitGroup,
                kFrameLimitHelp);
            tracedFrameLimit = true;
        }
    }

    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

// --- Presentation pace: one control over Frame limit + Motion smoothing ----
//
// WHY THIS IS SUGAR AND NOT A SETTING. The two keys below it stay the source of
// truth: this writes them and reads them back, and holds no state of its own
// between frames. So a player who sets them individually sees whichever quick
// choice their pair spells, a config file written by hand behaves identically,
// and every gate that drives Video.FrameLimit keeps testing the same thing.
// The alternative -- a third persisted key that the other two had to be kept in
// sync with -- has a wrong answer available at every layer, and this one does
// not.
//
// WHY RADIO BUTTONS AND NOT A COMBO. Two named choices plus a state the control
// cannot produce. A combo would have to either hide Custom, which makes the
// panel claim a pace the player is not on, or offer it, which invites selecting
// a value that means nothing to write. Radios show all three honestly: two
// pressable, and Custom as a plain statement when neither is filled.
struct PaceChoice { MdkrPresentationPace pace; const char *label; };
const PaceChoice kPaceChoices[] = {
    {MDKR_PRESENTATION_PACE_ORIGINAL, "Original (authored pace)"},
    {MDKR_PRESENTATION_PACE_SMOOTH,   "Smooth (match display)"},
};

bool drawPresentationPace(bool compact) {
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    const MdkrVideoSchema *frameSchema =
        mdkr_video_schema(MDKR_VIDEO_FRAME_LIMIT);
    if (config == nullptr || frameSchema == nullptr) return false;

    // Locked if EITHER underlying key is pinned above RUNTIME rank: the choice
    // writes both, so it can only be offered when both can be written.
    const bool locked =
        mdkr_video_config_runtime_locked(MDKR_VIDEO_FRAME_LIMIT) != 0 ||
        mdkr_video_config_runtime_locked(MDKR_VIDEO_MOTION_SMOOTHING) != 0;
    const MdkrPresentationPace current = mdkr_video_presentation_pace(config);
    bool changed = false;

    ImGui::PushID("presentation-pace");
    if (locked) ImGui::BeginDisabled();
    ImGui::TextUnformatted("Presentation pace");
    ui::LiveBadge("applies immediately");

    for (int i = 0; i < static_cast<int>(std::size(kPaceChoices)); ++i) {
        const PaceChoice &choice = kPaceChoices[i];
        if (i > 0) ImGui::SameLine(0.0f, ui::kGapM);
        const bool pressed =
            ImGui::RadioButton(choice.label, current == choice.pace);
        const int slot = static_cast<int>(choice.pace);
        g_paceRectMin[slot] = ImGui::GetItemRectMin();
        g_paceRectMax[slot] = ImGui::GetItemRectMax();
        g_paceRectValid[slot] = true;
        if (!pressed) continue;
        const MdkrVideoRuntimeResult result =
            mdkr_video_config_runtime_set_presentation_pace(choice.pace);
        // One verdict for the pair, reported against Frame limit's label: the
        // transaction succeeded or failed as a whole, and two status lines for
        // one press would be two claims about one thing.
        reportResult(result, frameSchema);
        if (resultSucceeded(result)) {
            // Both underlying combos must resynchronize from the authoritative
            // desired config rather than from their own stale edit buffers.
            g_edits[static_cast<size_t>(MDKR_VIDEO_FRAME_LIMIT)] = EditState{};
            g_edits[static_cast<size_t>(MDKR_VIDEO_MOTION_SMOOTHING)] =
                EditState{};
            changed = true;
        }
    }

    if (current == MDKR_PRESENTATION_PACE_CUSTOM) {
        ui::TextSubtleWrapped(
            "Custom — Frame limit and Motion smoothing are set individually "
            "below.");
    }
    if (locked) {
        ImGui::EndDisabled();
        ui::TextSubtle("Fixed for this session.");
    } else if (!compact) {
        ui::TextSubtleWrapped(
            "Original presents each picture the game makes, once — the "
            "motion the game was drawn with. Smooth presents on your "
            "display's own schedule and draws in-between pictures, so motion "
            "reads as more continuous; the in-between pictures are invented, "
            "and fast-moving parts of the screen can show artifacts in them. "
            "Either way the game runs at its original speed — racing, timers, "
            "sound and saves are identical. This changes Frame limit and "
            "Motion smoothing together, and takes effect straight away; you "
            "can change it while you play.");
    }
    // One row per DISTINCT reading, not per frame: the panel redraws sixty
    // times a second and a per-frame row would bury the transition a gate is
    // looking for in thousands of identical ones.
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        const MdkrVideoConfig *now = mdkr_video_config_desired();
        static std::string traced;
        char row[256];
        std::snprintf(row, sizeof(row),
                      "[app-ui] presentation-pace value=%s frameLimit=%s "
                      "motionSmoothing=%s locked=%d",
                      mdkr_video_presentation_pace_name(
                          mdkr_video_presentation_pace(now)),
                      now->values[MDKR_VIDEO_FRAME_LIMIT].text,
                      now->values[MDKR_VIDEO_MOTION_SMOOTHING].text,
                      locked ? 1 : 0);
        if (traced != row) {
            traced = row;
            std::fprintf(stderr, "%s\n", row);
        }
    }
    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

bool restoreControllerDefaults() {
    constexpr size_t kInputCount =
        static_cast<size_t>(MDKR_INPUT_LAST_KEY - MDKR_INPUT_FIRST_KEY + 1);
    MdkrVideoConfig defaults;
    std::array<std::string, kInputCount> values;
    std::array<MdkrVideoRuntimeChange, kInputCount> changes;

    mdkr_video_config_defaults(&defaults);
    for (size_t i = 0; i < kInputCount; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(
            static_cast<int>(MDKR_INPUT_FIRST_KEY) + static_cast<int>(i));
        const MdkrVideoSchema *schema = mdkr_video_schema(key);
        if (schema == nullptr) return false;
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            values[i] = defaults.values[key].text;
        } else {
            values[i] = std::to_string(
                static_cast<int>(defaults.values[key].number));
        }
        changes[i] = {key, values[i].c_str()};
    }

    const MdkrVideoRuntimeResult result = mdkr_video_config_runtime_set_many(
        changes.data(), static_cast<int>(changes.size()));
    if (!resultSucceeded(result)) {
        reportResult(result, mdkr_video_schema(MDKR_INPUT_RUMBLE_ENABLED));
        return false;
    }
    for (int key = MDKR_INPUT_FIRST_KEY; key <= MDKR_INPUT_LAST_KEY; ++key) {
        EditState &edit = g_edits[static_cast<size_t>(key)];
        edit.initialized = false;
        edit.active = false;
        edit.dirty = false;
        edit.error.clear();
    }
    platform_pad_rumble_preferences_changed();
    setStatus("Controller mappings and rumble defaults restored.",
              AppTheme::good());
    return true;
}

}  // namespace

void Settings_cancelAudioPreview() {
    mdkr_audio_config_runtime_cancel_preview();
    for (MdkrVideoKey key : {MDKR_AUDIO_MASTER_VOLUME,
                             MDKR_AUDIO_MUSIC_VOLUME,
                             MDKR_AUDIO_EFFECTS_VOLUME}) {
        EditState &edit = g_edits[static_cast<size_t>(key)];
        edit.initialized = false;
        edit.active = false;
        edit.dirty = false;
        edit.error.clear();
    }
}

void Settings_loadUiScalePreference() {
    const std::string stored = AppConfig::get("ui_scale", "1.0");
    float parsed = 1.0f;
    const float scale = AppUi_parseScale(stored.c_str(), &parsed) ? parsed : 1.0f;
    AppTheme::setUiScale(scale);
    g_uiScaleEdit = scale;
    g_uiScaleInitialized = true;
    g_uiScaleDirty = false;
    g_uiScaleError.clear();
    g_smokeGamepadFocusUsed = false;
    if (std::getenv("MDKR_APP_UI_TRACE")) {
        std::fprintf(stderr, "[app-ui] ui-scale loaded=%.2f\n",
                     static_cast<double>(scale));
    }
}

bool Settings_smokeFrameLimitCenter(int *x, int *y) {
    if (!x || !y || !g_frameLimitRectValid) return false;
    *x = static_cast<int>((g_frameLimitRectMin.x + g_frameLimitRectMax.x) * 0.5f);
    *y = static_cast<int>((g_frameLimitRectMin.y + g_frameLimitRectMax.y) * 0.5f);
    return true;
}

bool Settings_smokeFrameLimitPopup(int *focusedIndex) {
    if (!focusedIndex || !g_frameLimitPopupOpen) return false;
    *focusedIndex = g_frameLimitFocusedIndex;
    return true;
}

int Settings_smokeFrameLimitDownSteps(const char *from, const char *to) {
    if (!from || !to) return -1;
    int fromIndex = -1;
    int toIndex = -1;
    for (int i = 0; i < static_cast<int>(std::size(kFrameLimit)); ++i) {
        if (std::strcmp(kFrameLimit[i].value, from) == 0) fromIndex = i;
        if (std::strcmp(kFrameLimit[i].value, to) == 0) toIndex = i;
    }
    return fromIndex >= 0 && toIndex >= fromIndex ? toIndex - fromIndex : -1;
}

bool Settings_smokeFrameLimitRetryCenter(int *x, int *y) {
    if (!x || !y || !g_frameLimitRetryRectValid) return false;
    *x = static_cast<int>(
        (g_frameLimitRetryRectMin.x + g_frameLimitRetryRectMax.x) * 0.5f);
    *y = static_cast<int>(
        (g_frameLimitRetryRectMin.y + g_frameLimitRetryRectMax.y) * 0.5f);
    return true;
}

bool Settings_smokePresentationPaceCenter(const char *pace, int *x, int *y) {
    if (!pace || !x || !y) return false;
    const int resolved = mdkr_video_presentation_pace_from_name(pace);
    if (resolved <= 0 ||
        resolved >= static_cast<int>(std::size(g_paceRectValid)) ||
        !g_paceRectValid[resolved]) {
        return false;
    }
    *x = static_cast<int>(
        (g_paceRectMin[resolved].x + g_paceRectMax[resolved].x) * 0.5f);
    *y = static_cast<int>(
        (g_paceRectMin[resolved].y + g_paceRectMax[resolved].y) * 0.5f);
    return true;
}

bool Settings_smokeUiScaleRect(int *minX, int *minY, int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY || !g_uiScaleRectValid) return false;
    *minX = static_cast<int>(g_uiScaleRectMin.x);
    *minY = static_cast<int>(g_uiScaleRectMin.y);
    *maxX = static_cast<int>(g_uiScaleRectMax.x);
    *maxY = static_cast<int>(g_uiScaleRectMax.y);
    return true;
}

void Settings_dumpSchemaContract() {
    std::printf(
        "[app] frame-limit UI contract: recommended=\"%s\" group=\"%s\" "
        "caveat=\"%s\"\n",
        kOriginalFrameLimitLabel, kModernFrameLimitGroup,
        kFrameLimitHelp);
}

bool Settings_restartPending() {
    /* Keep the launcher, overlay, and original in-game settings on one
     * predicate. In particular, Video.Mode is a preset label rather than a
     * staged engine override; the runtime helper correctly evaluates the
     * individual values expanded by that preset. */
    return mdkr_video_config_restart_pending() != 0;
}

int Settings_collectStagedOverrides(const char **out, int cap) {
    // Static storage: the caller (the launcher's boot config) holds these
    // pointers until mdkr64_engine_boot copies them, which happens on the same
    // stack, so a per-key static buffer is both sufficient and lifetime-safe.
    static char s_buf[MDKR_VIDEO_KEY_COUNT][MDKR_VIDEO_NAME_MAX + MDKR_VIDEO_STRING_MAX + 2];
    int n = 0;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT && n < cap; ++i) {
        MdkrVideoKey k = (MdkrVideoKey)i;
        const MdkrVideoSchema *s = mdkr_video_schema(k);
        if (!s || s->scope != MDKR_VIDEO_SCOPE_RESTART) continue;
        if (!differsFromLive(k, s)) continue;
        // Video.Mode is a preset label, not a single engine override. Passing it
        // here would re-expand the preset over the individual keys the player
        // just staged; the engine reads the already-persisted resolved config.
        if (k == MDKR_VIDEO_MODE) continue;
        char v[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, desired(k), v, sizeof(v));
        std::snprintf(s_buf[n], sizeof(s_buf[n]), "%s=%s", s->name, v);
        out[n] = s_buf[n];
        ++n;
    }
    return n;
}

bool Settings_draw(SDL_Window *window, bool compact) {
    bool changed = false;
    g_frameLimitPopupOpen = false;
    g_frameLimitFocusedIndex = -1;
    g_frameLimitRetryRectValid = false;
    MdkrVideoRuntimeResult windowResult = MDKR_VIDEO_RUNTIME_INVALID;
    bool windowResultFresh = false;
    if (AppWindow_consumeCompleted(&windowResult, &windowResultFresh)) {
        const MdkrVideoSchema *schema = mdkr_video_schema(MDKR_WINDOW_MODE);
        EditState &edit = g_edits[static_cast<size_t>(MDKR_WINDOW_MODE)];
        // A stale completion still resynchronizes the widget from the
        // authoritative desired value; only its status line is suppressed.
        if (windowResultFresh) reportResult(windowResult, schema);
        if (resultSucceeded(windowResult)) {
            edit.dirty = false;
            edit.initialized = false;
            edit.error.clear();
            changed = true;
        } else if (windowResultFresh) {
            edit.error = g_status;
        }
    }
    static const MdkrVideoCategory categoryOrder[] = {
        MDKR_VIDEO_CAT_PRESENTATION,
        MDKR_VIDEO_CAT_PACING,
        MDKR_VIDEO_CAT_AUDIO,
        MDKR_VIDEO_CAT_INPUT,
        MDKR_VIDEO_CAT_FIDELITY,
    };
    const bool controllerSettingsSmoke =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_SETTINGS") != nullptr;
    const bool smokeControllerRestore =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_RESTORE") != nullptr;
    /* Both scripted pacing gates need the same thing: Frame Rate & Motion in
     * view without scrolling, so a queued click lands on the widget rather
     * than on whatever the panel happened to have scrolled under it. */
    const bool selectingFrameLimit =
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") != nullptr ||
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE") != nullptr;
    int controllerMappingWidgets = 0;
    int controllerRumbleWidgets = 0;
    bool controllerRestoreAvailable = false;
    bool controllerRestoreSucceeded = false;

    if (mdkr_video_config_is_readonly()) {
        // Explicit --pure locks this session's resolved values. Timing and
        // smoothing are intentionally independent of art-direction presets,
        // so never describe a retained enhanced choice as "original".
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Pure locks this session's resolved rendering settings. Frame Limit, "
            "Motion smoothing, and Simulation cadence keep the values selected "
            "before launch; review them below for timing comparisons. Audio, "
            "window, controller, and rumble preferences remain adjustable for "
            "comfort.");
        ImGui::PopStyleColor();
        ui::Gap(ui::kGapM);
    }

    const ImGuiTreeNodeFlags interfaceFlags =
        (controllerSettingsSmoke || selectingFrameLimit)
        ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;
    if (drawSettingsSectionHeader("Interface", interfaceFlags)) {
        if (!g_uiScaleInitialized) {
            g_uiScaleEdit = AppTheme::uiScale();
            g_uiScaleInitialized = true;
        }
        ui::Gap(ui::kGapS);
        changed |= drawKey(window, MDKR_WINDOW_MODE, compact);
        ImGui::TextUnformatted("UI scale");
        ImGui::SetNextItemWidth(ui::kControlWidth());
        const bool scalePreviewChanged = ImGui::SliderFloat(
            "##ui-scale", &g_uiScaleEdit, 0.75f, 2.0f, "%.2fx",
            ImGuiSliderFlags_AlwaysClamp);
        g_uiScaleRectMin = ImGui::GetItemRectMin();
        g_uiScaleRectMax = ImGui::GetItemRectMax();
        g_uiScaleRectValid = true;
        if (AppUi_deferredCommit(scalePreviewChanged,
                                 ImGui::IsItemDeactivatedAfterEdit(),
                                 &g_uiScaleDirty)) {
            // Applying while held changes every widget's geometry underneath
            // the pointer. That feedback loop made the slider oscillate and
            // the whole launcher flash. Commit once, on release, and let the
            // host apply the new metrics at the next safe frame boundary.
            AppTheme::requestUiScale(g_uiScaleEdit);
            char value[32];
            std::snprintf(value, sizeof(value), "%.2f",
                          static_cast<double>(g_uiScaleEdit));
            const AppConfig::PersistResult persist =
                AppConfig::setAndSave("ui_scale", value);
            if (AppConfig::persistResultApplied(persist)) {
                g_uiScaleDirty = false;
                g_uiScaleError.clear();
                setStatus(
                    persist == AppConfig::PersistResult::DurabilityUnconfirmed
                        ? "UI scale applied, but durable storage was not confirmed. "
                          "It may need to be selected again after an unexpected shutdown."
                        : "UI scale saved.",
                    persist == AppConfig::PersistResult::DurabilityUnconfirmed
                        ? AppTheme::accent() : AppTheme::good());
                changed = true;
            } else {
                g_uiScaleError =
                    "UI scale could not be saved. It remains active for this session; "
                    "retry after restoring write access.";
                setStatus(g_uiScaleError.c_str(), AppTheme::bad());
            }
        }
        if (!g_uiScaleError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextWrapped("%s", g_uiScaleError.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button(
                    "Retry UI Scale Save",
                    ImVec2(0.0f, ui::kBtnSecondary().y))) {
                char value[32];
                std::snprintf(value, sizeof(value), "%.2f",
                              static_cast<double>(g_uiScaleEdit));
                const AppConfig::PersistResult persist =
                    AppConfig::setAndSave("ui_scale", value);
                if (AppConfig::persistResultApplied(persist)) {
                    g_uiScaleDirty = false;
                    g_uiScaleError.clear();
                    setStatus(
                        persist == AppConfig::PersistResult::DurabilityUnconfirmed
                            ? "UI scale applied, but durable storage was not confirmed. "
                              "It may need to be selected again after an unexpected shutdown."
                            : "UI scale saved.",
                        persist == AppConfig::PersistResult::DurabilityUnconfirmed
                            ? AppTheme::accent() : AppTheme::good());
                    changed = true;
                }
            }
        }
        if (!compact) {
            ui::TextSubtleWrapped(
                "Scales text and controls together after you release the slider. "
                "Supported range: 0.75x to 2.00x. For touch, use 1.25x or larger.");
        }
        ui::Gap(ui::kGapS);
    }

    const bool webGpuRenderer =
        mdkr_render_backend() == MDKR_BACKEND_WEBGPU;
    /* Video.Widescreen is normally hidden because no preset ever selects its
     * 0 branch (see AppUi_videoSettingVisible). A config that already resolved
     * to 0 is the exception: the player is looking at the pre-widescreen
     * stretch and needs a control to leave it. */
    const bool legacyStretchActive =
        mdkr_video_config_current()
            ->values[MDKR_VIDEO_WIDESCREEN].number == 0.0f;
    for (MdkrVideoCategory category : categoryOrder) {
        const int c = static_cast<int>(category);
        const char *catName = category == MDKR_VIDEO_CAT_PACING
            ? "Frame Rate & Motion"
            : category == MDKR_VIDEO_CAT_FIDELITY
            ? "Advanced Graphics"
            : mdkr_video_category_name(category);
        if (!catName) continue;

        // Count first so an empty category never renders a bare header.
        int inCat = 0;
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
            const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
            if (!AppUi_videoSettingVisible(key, webGpuRenderer,
                                          legacyStretchActive)) continue;
            const MdkrVideoSchema *s = mdkr_video_schema(key);
            if (s && (int)s->category == c) ++inCat;
        }
        if (inCat == 0) continue;

        /* Lead with everyday comfort controls while keeping the proven pacing
         * choice discoverable. The scripted Frame Limit gate collapses Audio
         * so it still exercises the real widget at small window sizes. */
        const ImGuiTreeNodeFlags flags =
            (category == MDKR_VIDEO_CAT_INPUT && controllerSettingsSmoke) ||
            (!controllerSettingsSmoke &&
             ((category == MDKR_VIDEO_CAT_PRESENTATION && !selectingFrameLimit) ||
              category == MDKR_VIDEO_CAT_PACING))
            ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        if (drawSettingsSectionHeader(catName, flags)) {
            if (category == MDKR_VIDEO_CAT_PRESENTATION) {
                ui::Gap(ui::kGapS);
                ui::TextSubtleWrapped(
                    "Restored is the recommended default: widescreen and sharper "
                    "output while preserving the original art direction. Pure uses "
                    "the original 4:3 framing. Remastered previews additional "
                    "lighting, shadows, and finishing effects.");
                ui::Gap(ui::kGapS);
                changed |= drawKey(window, MDKR_VIDEO_MODE, compact);
                if (webGpuRenderer) {
                    ui::TextSubtle("Graphics backend: WebGPU (recommended)");
                } else if (!compact) {
                    if (ui::CardBegin("##renderer-warning", AppTheme::accent(), 0.0f)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                        ImGui::TextUnformatted("OpenGL diagnostic renderer active");
                        ImGui::PopStyleColor();
                        ui::TextSubtleWrapped(
                            "For the qualified visual path, use WebGPU with Restored.");
                    }
                    ui::CardEnd();
                }
            }
            if (category == MDKR_VIDEO_CAT_AUDIO && !compact) {
                ui::Gap(ui::kGapS);
                ui::TextSubtleWrapped(
                    "Master controls everything. Music and Sound effects preserve "
                    "DKR's original mix buses. Changes apply immediately and are "
                    "remembered for next time.");
            }
            if (category == MDKR_VIDEO_CAT_INPUT) {
                ui::Gap(ui::kGapS);
                if (!compact) {
                    ui::TextSubtleWrapped(
                        "SDL first normalizes each physical gamepad. Map those "
                        "named controls to DKR's N64 buttons here; the left stick "
                        "always remains analog steering, and View/Back remains the "
                        "in-game overlay shortcut.");
                }
                const bool restorePressed = ImGui::Button(
                    "Restore Controller Defaults", ui::kBtnWide());
                controllerRestoreAvailable = true;
                static bool smokeRestoreAttempted = false;
                const bool runSmokeRestore = controllerSettingsSmoke &&
                    smokeControllerRestore && !smokeRestoreAttempted;
                if (restorePressed || runSmokeRestore) {
                    if (runSmokeRestore) smokeRestoreAttempted = true;
                    const bool restored = restoreControllerDefaults();
                    controllerRestoreSucceeded |= restored;
                    changed |= restored;
                }
            }
            if (category == MDKR_VIDEO_CAT_PACING && !compact) {
                ui::Gap(ui::kGapS);
                ui::TextSubtleWrapped(
                    "Frame Limit controls how often the app presents an image. "
                    "Motion smoothing can create unique in-between images. "
                    "Allow Tearing shows a finished frame without waiting for "
                    "the display. None of these make gameplay run faster.");
            }
            ui::Gap(ui::kGapS);
            ImGui::Indent(ui::kGapM);
            for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
                const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
                if (!AppUi_videoSettingVisible(key, webGpuRenderer,
                                              legacyStretchActive)) continue;
                const MdkrVideoSchema *s = mdkr_video_schema(key);
                if (!s || (int)s->category != c) continue;
                if (key == MDKR_VIDEO_MODE) continue;
                if (category == MDKR_VIDEO_CAT_PACING) {
                    continue;
                }
                changed |= drawKey(window, key, compact);
                if (category == MDKR_VIDEO_CAT_INPUT) {
                    if (key >= MDKR_INPUT_CONTROLLER_A &&
                        key <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT) {
                        controllerMappingWidgets++;
                    } else if (key == MDKR_INPUT_RUMBLE_ENABLED ||
                               key == MDKR_INPUT_RUMBLE_PROFILE) {
                        controllerRumbleWidgets++;
                    }
                }
            }
            if (category == MDKR_VIDEO_CAT_PACING) {
                // Above the two rows it writes, because it is the answer for
                // most players and the individual controls are the way to
                // leave it, not the way to reach it.
                if (AppUi_videoSettingVisible(
                        MDKR_VIDEO_FRAME_LIMIT, webGpuRenderer,
                        legacyStretchActive) &&
                    AppUi_videoSettingVisible(
                        MDKR_VIDEO_MOTION_SMOOTHING, webGpuRenderer,
                        legacyStretchActive)) {
                    changed |= drawPresentationPace(compact);
                    ImGui::Separator();
                    ui::Gap(ui::kGapS);
                }
                if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
                    static bool tracedFrameRateControls = false;
                    if (!tracedFrameRateControls) {
                        std::fprintf(
                            stderr,
                            "[app-ui] frame-rate-controls visible=1 "
                            "gameplay-accuracy-separated=1\n");
                        tracedFrameRateControls = true;
                    }
                }
                if (AppUi_videoSettingVisible(
                        MDKR_VIDEO_FRAME_LIMIT, webGpuRenderer,
                        legacyStretchActive)) {
                    changed |= drawKey(
                        window, MDKR_VIDEO_FRAME_LIMIT, compact);
                }
                if (AppUi_videoSettingVisible(
                        MDKR_VIDEO_MOTION_SMOOTHING, webGpuRenderer,
                        legacyStretchActive)) {
                    changed |= drawKey(
                        window, MDKR_VIDEO_MOTION_SMOOTHING, compact);
                }
                if (AppUi_videoSettingVisible(
                        MDKR_VIDEO_ALLOW_TEARING, webGpuRenderer,
                        legacyStretchActive)) {
                    changed |= drawKey(
                        window, MDKR_VIDEO_ALLOW_TEARING, compact);
                }
                ui::Gap(ui::kGapS);
                ImGui::SeparatorText("Gameplay Accuracy");
                changed |= drawKey(
                    window, MDKR_VIDEO_SIMULATION_CADENCE, compact);
            }
            ImGui::Unindent(ui::kGapM);
        }
    }

    if (controllerSettingsSmoke) {
        static bool tracedControllerSettings = false;
        if (!tracedControllerSettings) {
            std::fprintf(
                stderr,
                "[app-ui] controller settings rendered mappings=%d rumble=%d "
                "restoreAvailable=%d restoreSucceeded=%d\n",
                controllerMappingWidgets, controllerRumbleWidgets,
                controllerRestoreAvailable ? 1 : 0,
                controllerRestoreSucceeded ? 1 : 0);
            tracedControllerSettings = true;
        }
    }

    if (Settings_restartPending()) {
        ui::Gap(ui::kGapS);
        ImGui::Separator();
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(compact
            ? "Changes are saved. Use Restart & Apply below to activate them now."
            : "Changes are saved. They will activate when you next press Play.");
        ImGui::PopStyleColor();
    }

    if (!g_status.empty()) {
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, g_statusColor);
        ImGui::TextWrapped("%s", g_status.c_str());
        ImGui::PopStyleColor();
    }

    return changed;
}
