// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_theme.h"
#include "ui_common.h"

#include "video_config.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// --- Status line -----------------------------------------------------------
// The panel never claims success it did not observe: every edit routes through
// mdkr_video_config_runtime_set and its verdict is shown verbatim.
std::string g_status;
ImVec4      g_statusColor;

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
            break;
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:
            std::snprintf(buf, sizeof(buf), "That value is not valid for %s.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
    }
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
struct Options { const char *const *items; int count; };

const char *kCadence[]   = {"original", "enhanced"};
const char *kFrameLimit[] = {"original", "60"};
const char *kSmoothing[] = {"off", "interpolate"};
const char *kMode[]      = {"pure", "restored", "remastered", "custom"};

bool optionsFor(MdkrVideoKey k, Options &out) {
    switch (k) {
        case MDKR_VIDEO_SIMULATION_CADENCE: out = {kCadence, 2}; return true;
        case MDKR_VIDEO_FRAME_LIMIT:        out = {kFrameLimit, 2}; return true;
        case MDKR_VIDEO_MOTION_SMOOTHING:   out = {kSmoothing, 2}; return true;
        case MDKR_VIDEO_MODE:               out = {kMode, 4}; return true;
        default: return false;
    }
}

// --- One row ---------------------------------------------------------------
bool drawKey(MdkrVideoKey k, bool compact) {
    const MdkrVideoSchema *s = mdkr_video_schema(k);
    const MdkrVideoValue  *d = desired(k);
    if (!s || !d) return false;

    const bool locked = mdkr_video_config_runtime_locked(k) != 0;
    bool changed = false;

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
        int cur = 0;
        for (int i = 0; i < opts.count; ++i) {
            if (std::strcmp(opts.items[i], d->text) == 0) { cur = i; break; }
        }
        if (ImGui::Combo("##v", &cur, opts.items, opts.count)) {
            reportResult(mdkr_video_config_runtime_set(k, opts.items[cur]), s);
            changed = true;
        }
    } else if (s->type == MDKR_VIDEO_TYPE_STRING) {
        // Open domain (aspect expressions, "authored" or a FOV number, a texture
        // pack path). Commit on Enter/blur so a half-typed value is never sent
        // to the validator and reported as invalid mid-keystroke.
        char edit[MDKR_VIDEO_STRING_MAX];
        std::snprintf(edit, sizeof(edit), "%s", d->text);
        if (ImGui::InputText("##v", edit, sizeof(edit),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            reportResult(mdkr_video_config_runtime_set(k, edit), s);
            changed = true;
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT && s->min == 0.0f && s->max == 1.0f) {
        bool on = d->number != 0.0f;
        if (ImGui::Checkbox("##v", &on)) {
            reportResult(mdkr_video_config_runtime_set(k, on ? "1" : "0"), s);
            changed = true;
        }
    } else if (s->type == MDKR_VIDEO_TYPE_INT) {
        int v = (int)d->number;
        if (ImGui::SliderInt("##v", &v, (int)s->min, (int)s->max)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", v);
            reportResult(mdkr_video_config_runtime_set(k, buf), s);
            changed = true;
        }
    } else {
        float v = d->number;
        if (ImGui::SliderFloat("##v", &v, s->min, s->max, "%.2f")) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", (double)v);
            reportResult(mdkr_video_config_runtime_set(k, buf), s);
            changed = true;
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
        ImGui::Text("Staged: %s \xE2\x86\x92 %s on the next launch (running now: %s)",
                    liveBuf, valueBuf, liveBuf);
        ImGui::PopStyleColor();
    }

    if (!compact && s->help) {
        ImGui::PushTextWrapPos(0.0f);
        ui::TextSubtle("%s", s->help);
        ImGui::PopTextWrapPos();
    }

    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

}  // namespace

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

    for (int c = 0; c < MDKR_VIDEO_CAT_COUNT; ++c) {
        const char *catName = mdkr_video_category_name((MdkrVideoCategory)c);
        if (!catName) continue;

        // Count first so an empty category never renders a bare header.
        int inCat = 0;
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
            const MdkrVideoSchema *s = mdkr_video_schema((MdkrVideoKey)i);
            if (s && (int)s->category == c) ++inCat;
        }
        if (inCat == 0) continue;

        if (ImGui::CollapsingHeader(catName, ImGuiTreeNodeFlags_DefaultOpen)) {
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
            "Some changes are staged for the next launch. They are already saved "
            "— quitting now will not lose them.");
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
