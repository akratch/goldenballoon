// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "ui_common.h"

#include "video_config.h"

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
bool g_frameLimitRetryRectValid = false;
ImVec2 g_frameLimitRetryRectMin;
ImVec2 g_frameLimitRetryRectMax;
bool g_smokeGamepadFocusUsed = false;

void setStatus(const char *text, const ImVec4 &color) {
    g_status = text ? text : "";
    g_statusColor = color;
}

void reportResult(MdkrVideoRuntimeResult r, const MdkrVideoSchema *s) {
    char buf[320];
    switch (r) {
        case MDKR_VIDEO_RUNTIME_LIVE:
            std::snprintf(buf, sizeof(buf), "%s applied.", s->label);
            setStatus(buf, AppTheme::good());
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
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:
            std::snprintf(buf, sizeof(buf), "That value is not valid for %s.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
    }
}

bool resultSucceeded(MdkrVideoRuntimeResult result) {
    return result == MDKR_VIDEO_RUNTIME_LIVE ||
           result == MDKR_VIDEO_RUNTIME_RESTART;
}

bool commitEdit(MdkrVideoKey key, const MdkrVideoSchema *schema,
                EditState &edit, const char *value) {
    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set(key, value);
    reportResult(result, schema);
    if (resultSucceeded(result)) {
        edit.dirty = false;
        edit.initialized = false;  // resync from the authoritative desired value
        edit.error.clear();
        return true;
    }
    edit.error = g_status;
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

void formatValue(const MdkrVideoSchema *s, const MdkrVideoValue *v, char *out, size_t cap) {
    if (!v) { std::snprintf(out, cap, "?"); return; }
    switch (s->type) {
        case MDKR_VIDEO_TYPE_STRING: std::snprintf(out, cap, "%s", v->text); break;
        case MDKR_VIDEO_TYPE_INT:    std::snprintf(out, cap, "%d", (int)v->number); break;
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
// offer a value the config layer will take. Anything with an open-ended domain
// (Aspect, GameplayFOV, TexturePack) gets a text field instead of a combo.
struct Option { const char *value; const char *label; };
struct Options { const Option *items; int count; };

constexpr const char *kRecommendedFrameLimitLabel =
    "Original (Recommended / Proven)";
constexpr const char *kExperimentalFrameLimitGroup =
    "Experimental \xE2\x80\x94 Under Construction";
constexpr const char *kExperimentalFrameLimitCaveat =
    "Non-Original choices only alter host pacing and input/event-pump "
    "opportunities. In 1.0.1+ they do not increase unique visual FPS; the "
    "supported US 1.1 game remains at its authored ~30 FPS. Any benefit may "
    "be negligible, while higher settings can use more CPU. Original is "
    "Recommended / Proven.";

const Option kCadence[] = {
    {"original", "Original (recommended)"},
    {"enhanced", "Enhanced (changes gameplay)"},
};
const Option kFrameLimit[] = {
    {"original", kRecommendedFrameLimitLabel},
    {"display",  "Match Display (Experimental \xE2\x80\x94 Under Construction)"},
    {"60",       "60 Hz (Experimental \xE2\x80\x94 Under Construction)"},
    {"120",      "120 Hz (Experimental \xE2\x80\x94 Under Construction)"},
    {"144",      "144 Hz (Experimental \xE2\x80\x94 Under Construction)"},
    {"165",      "165 Hz (Experimental \xE2\x80\x94 Under Construction)"},
    {"240",      "240 Hz (Experimental \xE2\x80\x94 Under Construction)"},
    {"uncapped", "Uncapped (Experimental \xE2\x80\x94 Under Construction)"},
};
const Option kSmoothing[] = {
    {"off", "Unavailable in 1.0.1+ (authored images only)"},
};
const Option kMode[] = {
    {"pure",       "Pure"},
    {"restored",   "Restored"},
    {"remastered", "Remastered"},
    {"custom",     "Custom"},
};
// The canonical spellings only. mdkr_video_world_shadows_canonical() also takes
// "0"/"1"/"on"/"" so the MDKR_WORLD_SHADOW diagnostic seam keeps working, but a
// combo that offered two words for the same state would be a worse control.
const Option kShadows[] = {
    {"full", "Full"}, {"soft", "Soft"}, {"off", "Off"},
};

bool optionsFor(MdkrVideoKey k, Options &out) {
    switch (k) {
        case MDKR_VIDEO_SIMULATION_CADENCE: out = {kCadence, 2}; return true;
        case MDKR_VIDEO_FRAME_LIMIT:        out = {kFrameLimit, 8}; return true;
        case MDKR_VIDEO_MOTION_SMOOTHING:   out = {kSmoothing, 1}; return true;
        case MDKR_VIDEO_MODE:               out = {kMode, 4}; return true;
        case MDKR_VIDEO_WORLD_SHADOWS:      out = {kShadows, 3}; return true;
        default: return false;
    }
}

const char *optionLabel(MdkrVideoKey key, const char *value) {
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
            return "Original preserves the game's physics timing. Enhanced "
                   "changes gameplay and is intended for compatibility use.";
        case MDKR_VIDEO_FRAME_LIMIT:
            return kExperimentalFrameLimitCaveat;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            return "Motion interpolation is disabled for 1.0.1+ while retained "
                   "render dependencies are completed. The surface updates "
                   "only when a complete authored game image is ready.";
        default:
            return schema->help;
    }
}

// --- One row ---------------------------------------------------------------
bool drawKey(MdkrVideoKey k, bool compact) {
    const MdkrVideoSchema *s = mdkr_video_schema(k);
    const MdkrVideoValue  *d = desired(k);
    if (!s || !d) return false;

    const bool locked = mdkr_video_config_runtime_locked(k) != 0;
    bool changed = false;
    EditState &editState = g_edits[static_cast<size_t>(k)];

    ImGui::PushID((int)k);
    if (locked) ImGui::BeginDisabled();

    ImGui::TextUnformatted(s->label);
    if (s->scope == MDKR_VIDEO_SCOPE_RESTART) {
        ImGui::SameLine();
        ui::RestartBadge();
    }

    ImGui::SetNextItemWidth(ui::kControlWidth());

    char valueBuf[MDKR_VIDEO_STRING_MAX];
    formatValue(s, d, valueBuf, sizeof(valueBuf));

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
                    ImGui::SeparatorText(kExperimentalFrameLimitGroup);
                }
                const bool selected = i == cur;
                if (ImGui::Selectable(opts.items[i].label, selected)) {
                    std::snprintf(editState.text, sizeof(editState.text), "%s",
                                  opts.items[i].value);
                    editState.dirty = true;
                    changed = commitEdit(k, s, editState, editState.text);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (s->type == MDKR_VIDEO_TYPE_STRING) {
        // Open domain (aspect expressions, "authored" or a FOV number, a texture
        // pack path). Commit on Enter/blur so a half-typed value is never sent
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
            changed = commitEdit(k, s, editState, editState.text);
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
            changed = commitEdit(k, s, editState, on ? "1" : "0");
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT) {
        if (!editState.initialized || (!editState.active && !editState.dirty)) {
            editState.number = d->number;
            editState.initialized = true;
        }
        int v = static_cast<int>(editState.number);
        const bool previewChanged =
            ImGui::SliderInt("##v", &v, (int)s->min, (int)s->max);
        if (previewChanged) {
            editState.number = static_cast<float>(v);
        }
        const bool commit = AppUi_deferredCommit(
            previewChanged, ImGui::IsItemDeactivatedAfterEdit(), &editState.dirty);
        editState.active = ImGui::IsItemActive();
        if (commit && editState.dirty) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", v);
            changed = commitEdit(k, s, editState, buf);
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
            changed = commitEdit(k, s, editState, buf);
        }
    }

    if (!editState.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", editState.error.c_str());
        ImGui::PopStyleColor();
        const bool retryPressed =
            editState.dirty && ImGui::SmallButton("Retry save");
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
            changed = commitEdit(k, s, editState, retry);
        }
    }

    if (locked) {
        ImGui::EndDisabled();
        ui::TextSubtle("Fixed by the %s (%s=%s). Unset it to edit here.",
                       sourceName(d->source), s->env, valueBuf);
    } else if (s->scope == MDKR_VIDEO_SCOPE_RESTART && differsFromLive(k, s)) {
        // The honest RESTART presentation: say what is running NOW and what will
        // be running next launch. Never imply the change already took effect.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Saved for next Play: %s (current: %s)",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    }

    const char *help = helpFor(k, s);
    if (!compact && help) {
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
                kRecommendedFrameLimitLabel, kExperimentalFrameLimitGroup,
                kExperimentalFrameLimitCaveat);
            tracedFrameLimit = true;
        }
    }

    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

}  // namespace

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

bool Settings_smokeFrameLimitRetryCenter(int *x, int *y) {
    if (!x || !y || !g_frameLimitRetryRectValid) return false;
    *x = static_cast<int>(
        (g_frameLimitRetryRectMin.x + g_frameLimitRetryRectMax.x) * 0.5f);
    *y = static_cast<int>(
        (g_frameLimitRetryRectMin.y + g_frameLimitRetryRectMax.y) * 0.5f);
    return true;
}

void Settings_dumpSchemaContract() {
    std::printf(
        "[app] frame-limit UI contract: recommended=\"%s\" group=\"%s\" "
        "caveat=\"%s\"\n",
        kRecommendedFrameLimitLabel, kExperimentalFrameLimitGroup,
        kExperimentalFrameLimitCaveat);
}

bool Settings_restartPending() {
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey)i);
        if (s && s->scope == MDKR_VIDEO_SCOPE_RESTART &&
            differsFromLive((MdkrVideoKey)i, s)) {
            return true;
        }
    }
    return false;
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
        // Video.Mode is a preset, not a single value: passing it as --video-set
        // would re-expand the preset over the individual keys the player just
        // staged. The launcher passes the mode as its own flag instead.
        if (k == MDKR_VIDEO_MODE) continue;
        char v[MDKR_VIDEO_STRING_MAX];
        formatValue(s, desired(k), v, sizeof(v));
        std::snprintf(s_buf[n], sizeof(s_buf[n]), "%s=%s", s->name, v);
        out[n] = s_buf[n];
        ++n;
    }
    return n;
}

bool Settings_draw(bool compact) {
    bool changed = false;
    g_frameLimitRetryRectValid = false;
    static const MdkrVideoCategory categoryOrder[] = {
        MDKR_VIDEO_CAT_PACING,
        MDKR_VIDEO_CAT_PRESENTATION,
        MDKR_VIDEO_CAT_FIDELITY,
    };

    if (mdkr_video_config_is_readonly()) {
        // A Pure session never rewrites the ini (video_config.h), so editing
        // here would silently do nothing. Say so instead of offering dead
        // controls.
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Pure mode is read-only: it presents the original game exactly as "
            "authored and never rewrites your settings file. Switch to Restored "
            "or Remastered to change these.");
        ImGui::PopStyleColor();
        ui::Gap(ui::kGapM);
    }

    if (ImGui::CollapsingHeader("Interface", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!g_uiScaleInitialized) {
            g_uiScaleEdit = AppTheme::uiScale();
            g_uiScaleInitialized = true;
        }
        ui::Gap(ui::kGapS);
        ImGui::TextUnformatted("UI scale");
        ImGui::SetNextItemWidth(ui::kControlWidth());
        const bool scalePreviewChanged = ImGui::SliderFloat(
            "##ui-scale", &g_uiScaleEdit, 0.75f, 2.0f, "%.2fx",
            ImGuiSliderFlags_AlwaysClamp);
        if (scalePreviewChanged) {
            // Apply at the next pre-NewFrame safe point, never midway through
            // the current widget tree.
            AppTheme::requestUiScale(g_uiScaleEdit);
        }
        if (AppUi_deferredCommit(scalePreviewChanged,
                                 ImGui::IsItemDeactivatedAfterEdit(),
                                 &g_uiScaleDirty)) {
            char value[32];
            std::snprintf(value, sizeof(value), "%.2f",
                          static_cast<double>(g_uiScaleEdit));
            if (AppConfig::setAndSave("ui_scale", value)) {
                g_uiScaleDirty = false;
                g_uiScaleError.clear();
                setStatus("UI scale saved.", AppTheme::good());
                changed = true;
            } else {
                g_uiScaleError =
                    "UI scale could not be saved. The preview remains available; "
                    "retry after restoring write access.";
                setStatus(g_uiScaleError.c_str(), AppTheme::bad());
            }
        }
        if (!g_uiScaleError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::TextWrapped("%s", g_uiScaleError.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Retry UI scale save")) {
                char value[32];
                std::snprintf(value, sizeof(value), "%.2f",
                              static_cast<double>(g_uiScaleEdit));
                if (AppConfig::setAndSave("ui_scale", value)) {
                    g_uiScaleDirty = false;
                    g_uiScaleError.clear();
                    setStatus("UI scale saved.", AppTheme::good());
                    changed = true;
                }
            }
        }
        if (!compact) {
            ui::TextSubtleWrapped(
                "Scales text and controls together. Supported range: 0.75x to 2.00x.");
        }
        ui::Gap(ui::kGapS);
    }

    for (MdkrVideoCategory category : categoryOrder) {
        const int c = static_cast<int>(category);
        const char *catName = category == MDKR_VIDEO_CAT_PACING
            ? "Frame rate & timing"
            : mdkr_video_category_name(category);
        if (!catName) continue;

        // Count first so an empty category never renders a bare header.
        int inCat = 0;
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
            const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey)i);
            if (s && (int)s->category == c) ++inCat;
        }
        if (inCat == 0) continue;

        const ImGuiTreeNodeFlags flags = category == MDKR_VIDEO_CAT_PACING
            ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        if (ImGui::CollapsingHeader(catName, flags)) {
            if (category == MDKR_VIDEO_CAT_PACING && !compact) {
                ui::Gap(ui::kGapS);
                ImGui::PushTextWrapPos(0.0f);
                ui::TextSubtle(
                    "Original is Recommended / Proven. Every other frame-limit "
                    "choice is Experimental \xE2\x80\x94 Under Construction and does not "
                    "increase unique visual FPS in 1.0.1+.");
                ImGui::PopTextWrapPos();
            }
            ui::Gap(ui::kGapS);
            ImGui::Indent(ui::kGapM);
            for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
                const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey)i);
                if (!s || (int)s->category != c) continue;
                changed |= drawKey((MdkrVideoKey)i, compact);
            }
            ImGui::Unindent(ui::kGapM);
        }
    }

    if (Settings_restartPending()) {
        ui::Gap(ui::kGapS);
        ImGui::Separator();
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped(
            "Changes are saved. Restart the game to apply them.");
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
