// ui_settings.cpp — see ui_settings.h.
#include "ui_settings.h"
#include "app_config.h"
#include "app_theme.h"
#include "app_ui_policy.h"
#include "app_window.h"
#include "ui_common.h"

#include "controller_mapping.h"
#include "enhancement_registry.h"
#include "mod_registry.h"
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
/*
 * Widget rectangles the scripted gates drive, and one flag each.
 *
 * The flag answers ONE question: is the panel showing this widget right now?
 * Not "was it ever submitted" -- a widget scrolled past the bottom of the
 * panel, or sitting in a collapsed section, is still submitted by ImGui, and a
 * flag that only records submission hands a queued click coordinates the panel
 * is currently clipping. The gate then fails on whatever it was really
 * measuring (a saved value, an applied scale) and says nothing about layout.
 * That mis-attribution cost hours once; see docs/open-items/misc.md.
 *
 * So each flag is assigned from ImGui::IsItemVisible() where the rect is read,
 * and every flag is cleared at the top of Settings_draw() for the frames the
 * widget is not submitted at all. Both halves are required: IsItemVisible()
 * cannot speak for a widget whose section never drew it.
 */
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
bool g_controllerSectionRequested = false;
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

/*
 * One settings group: a heading a player can collapse, one line saying what
 * the group is for, and an indented body.
 *
 * The heading is drawn in the section font rather than the body font. Three
 * levels of heading — page, group, setting — were previously spelled with two
 * sizes, so a group name and a setting name looked identical and the page read
 * as one flat list of thirty controls.
 *
 * A caller that gets `true` back owes an ImGui::Unindent(ui::kGapM); the indent
 * is what gives the gameplay rule in SettingLabel somewhere to live.
 */
// Reset each frame by Settings_draw. The leading gap below separates one group
// from the PREVIOUS one, so the first group on the page has nothing to be
// separated from -- and spending it there simply stacked on the page title's
// own trailing gap, leaving a band of empty panel above the first header that
// read as a rendering fault rather than as breathing room.
int g_sectionsDrawn = 0;

bool drawSettingsSectionHeader(const char *label, const char *subtitle,
                               ImGuiTreeNodeFlags flags, bool compact) {
    // Groups need air between them, and the gap belongs to the header rather
    // than to each body's exit path: a body that forgets it leaves one seam on
    // the page tighter than every other, which is exactly how a settings list
    // starts looking like a debug dump.
    if (g_sectionsDrawn++ > 0) ui::Gap(ui::kGapM);
    // These rows are independent expandable sections, not mutually exclusive
    // tabs. Keep resting and hover surfaces neutral; the chevron and a gold
    // leading rule communicate the open state without making every open
    // section look like another selected destination.
    ImGui::PushStyleColor(ImGuiCol_Header, AppTheme::groupHeader());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, AppTheme::groupHeaderHover());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, AppTheme::groupHeaderActive());
    // The scripted accessibility walk cannot reach a row inside a collapsed
    // section, because a collapsed section does not draw one.
    if (AppUi_a11yWalkArmed()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushFont(AppTheme::fonts().section);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopFont();
    // Landing on a header IS moving between sections; this is the one place
    // the settings panel says so. Only one header can hold focus, and
    // ui::SpeakSection drops the repeat on every subsequent frame.
    if (ImGui::IsItemFocused()) ui::SpeakSection(label);
    ImGui::PopStyleColor(3);
    if (!open) return false;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        min, ImVec2(min.x + 3.0f * AppTheme::uiScale(), max.y),
        ImGui::GetColorU32(AppTheme::accent()),
        2.0f * AppTheme::uiScale());
    ImGui::Indent(ui::kGapM);
    if (!compact && subtitle != nullptr && subtitle[0] != '\0') {
        ui::Gap(ui::kGapXS);
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtleUnformattedWrapped(subtitle);
        ImGui::PopFont();
    }
    ui::Gap(ui::kGapS);
    return true;
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
                              "%s is set — it starts at the next race.",
                              s->label);
                setStatus(buf, AppTheme::accent());
            } else {
                std::snprintf(buf, sizeof(buf), "%s applied.", s->label);
                setStatus(buf, AppTheme::good());
            }
            break;
        case MDKR_VIDEO_RUNTIME_RESTART:
            std::snprintf(buf, sizeof(buf),
                          "%s saved — it starts the next time you press Play.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_LOCKED:
            std::snprintf(buf, sizeof(buf),
                          "%s is set by %s or a command-line option, so it "
                          "cannot be changed here.", s->label, s->env);
            setStatus(buf, AppTheme::subtle());
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "%s was not saved — the settings file is not "
                          "writable. Nothing changed.", s->label);
            setStatus(buf, AppTheme::bad());
            if (std::getenv("MDKR_APP_SMOKE_EXPECT_SAVE_FAILURE")) {
                std::fprintf(stderr,
                             "[app-ui-test] visible settings error: %s\n", buf);
            }
            break;
        case MDKR_VIDEO_RUNTIME_SAVE_UNCONFIRMED:
            std::snprintf(buf, sizeof(buf),
                          "%s applied, but the system could not confirm it "
                          "reached the disk. Set it again after an unexpected "
                          "shutdown.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_PENDING:
            std::snprintf(buf, sizeof(buf), "%s applies on the next frame.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_SUPERSEDED:
            std::snprintf(buf, sizeof(buf),
                          "%s applies on the next frame, replacing the choice "
                          "that was still waiting.",
                          s->label);
            setStatus(buf, AppTheme::accent());
            break;
        case MDKR_VIDEO_RUNTIME_UNAVAILABLE:
            std::snprintf(buf, sizeof(buf),
                          "%s needs a game window. Try again once the game is "
                          "showing.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_APPLY_FAILED:
            std::snprintf(buf, sizeof(buf),
                          "The system refused %s. The previous setting was "
                          "kept.", s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_ROLLBACK_FAILED:
            std::snprintf(
                buf, sizeof(buf),
                "%s could not be saved, and the window did not go back to "
                "how it was. Your saved preference is unchanged — use F11 or "
                "Alt+Enter, or restart the app.",
                s->label);
            setStatus(buf, AppTheme::bad());
            break;
        case MDKR_VIDEO_RUNTIME_INVALID:
        default:
            std::snprintf(buf, sizeof(buf), "That is not a valid %s.", s->label);
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
    /* Both spellings of "queued for the next frame" leave the
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
/*
 * The one long tooltip on the page, and it earns its length: Frame limit is the
 * only setting whose right answer depends on hardware the app cannot see.
 *
 * It used to be twelve sentences rendered inline under the control, which is
 * where "reads like AI slop" came from. It is now five, behind a marker, and it
 * still carries every fact a player has actually needed: that gameplay speed is
 * unaffected, what smoothing changes about the higher rates, who Just Under
 * Display and 40 Hz are for, and what a browser does with the two it cannot
 * honour.
 *
 * Three files quote this string byte-for-byte -- CMakeLists.txt's app_schema
 * PASS_REGULAR_EXPRESSION, macos/Scripts/verify_unsigned_release.sh, and
 * tests/ci_contract_manifest.py. tests/check_ci_contract.py regenerates the
 * first two from this definition and fails when they drift, so editing this
 * text means editing those with it. Keep it free of parentheses and regex
 * metacharacters: check_ci_contract.py rejects the first outright, and the
 * others would silently widen the CTest pin.
 */
constexpr const char *kFrameLimitHelp =
    "How often the app draws to your screen. It never changes how fast the "
    "game runs. Original draws each picture the game makes, once; higher rates "
    "repeat it, or draw new in-between pictures when Motion smoothing is "
    "Interpolated. Just Under Display suits a variable-refresh display, and "
    "40 Hz suits a handheld screen that runs at 40 or 120 Hz. Rates above your "
    "display's refresh only apply where the system can drop a picture it has "
    "not shown yet, and a browser maps Uncapped and Just Under Display to "
    "Match Display.";

const Option kCadence[] = {
    {"original", "Original — 30 Hz, as authored"},
    {"enhanced", "Enhanced — 60 Hz, breaks some boss races"},
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
    {"off",         "Off — the game's own pictures only"},
};
const Option kAllowTearing[] = {
    {"off", "Off"},
    {"on",  "On — lowest delay, visible seam"},
};
/*
 * "Original" rather than "Pure". Pure is what the config file, the CLI flag and
 * every gate call this value and none of that changes; it was never a word a
 * player could act on. The three modes are Original, Restored and Remastered
 * everywhere a player reads about them -- the release notes, the site, the
 * device acceptance sheets -- and the settings panel was the last place still
 * spelling one of them differently.
 */
const Option kMode[] = {
    {"pure",       "Original — pixel-exact N64 picture"},
    {"restored",   "Restored — recommended"},
    {"remastered", "Remastered — in progress"},
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
    {"observe", "Authored — the original camera"},
    {"modern", "Keep it out of walls"},
};
const Option kCameraComfort[] = {
    {"authored", "Authored"},
    {"reduced", "Reduced"},
};
const Option kMenuLanguages[] = {
    {"all", "All on the cartridge"},
    {"authentic", "Only your region's"},
};
const Option kWindowMode[] = {
    {"windowed", "Windowed"},
    {"fullscreen", "Fullscreen"},
};
const Option kRumbleProfile[] = {
    {"light", "Light — 35%"},
    {"balanced", "Balanced — 65%"},
    {"strong", "Strong — 100%"},
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

// --- Player-facing copy ----------------------------------------------------
//
// WHY THIS TABLE EXISTS RATHER THAN THE SCHEMA'S OWN label/help. The schema
// strings are written for the person editing the ini and reading --video-list:
// they name values in their config spelling ("observe", "interpolate", "1
// engages the display policy") and explain WHY a key has the scope it has.
// Both are the right thing there and the wrong thing in front of a player, and
// trying to serve both audiences from one string is how they ended up long.
//
// The rules this table follows, and the ones the old copy broke:
//   * a label a player could say out loud;
//   * one line of description, present tense, no hedging, no "seamlessly";
//   * a tooltip only where a fact genuinely will not fit in that line;
//   * every setting that changes how the game PLAYS carries `gameplay`, and
//     nothing else does -- a marker on a row that only changes the picture is
//     what taught players to ignore the marker.
//
// A key with no entry falls back to the schema, so the table can never hide a
// setting by omission.
struct Copy {
    const char *label;
    const char *description;
    const char *tooltip;
    bool gameplay;
    bool experimental;
};

const Copy *copyFor(MdkrVideoKey key) {
    static const Copy kMode = {
        "Presentation",
        "Original is the N64 picture, pixel for pixel. Restored is that same "
        "art in widescreen at modern resolution. Remastered adds new lighting "
        "and shadows.",
        nullptr, false, false};
    static const Copy kCadenceCopy = {
        "Gameplay tick rate",
        "DKR's physics and AI were written for 30 updates a second. Enhanced "
        "runs them at 60.",
        "Enhanced is experimental. It gives you 60 FPS gameplay rather than "
        "60 FPS presentation, but parts of the authored physics are written "
        "around the 30 Hz tick, so boss races still run measurably off pace. "
        "Original is the accurate setting. Frame limit and Motion smoothing "
        "raise the frame rate without touching any of this.",
        true, true};
    static const Copy kFrameLimitCopy = {
        "Frame limit", "How often the app draws to your screen.",
        kFrameLimitHelp, false, false};
    static const Copy kSmoothingCopy = {
        "Motion smoothing",
        "Draws extra pictures between the game's own so motion reads as more "
        "continuous.",
        "The in-between pictures are invented, so fast-moving edges can show "
        "artefacts in them. Nothing else changes: physics, input, timers, "
        "audio and saves still advance only on the game's own ticks.",
        false, true};
    static const Copy kTearingCopy = {
        "Allow tearing",
        "Shows a finished frame without waiting for the display. Lowest input "
        "delay, and a visible seam while things move.",
        "Leave this off on a variable-refresh display. That display already "
        "adapts to the game, and this gives that up for a seam you do not "
        "need. Use Frame limit = Just Under Display instead.",
        false, false};
    static const Copy kCameraCopy = {
        "Camera",
        "The original camera passes through walls. The alternative pulls it in "
        "front of them.",
        "Only the view moves. Handling, results, ghosts and saves are "
        "identical either way. Authored is the default and is what the game "
        "shipped with.",
        false, true};
    static const Copy kComfortCopy = {
        "Camera shake",
        "Reduced smooths the vertical shake from bumps, landings and "
        "explosions.",
        "It also eases the camera back out more gently after it squeezes past "
        "a wall, so it has nothing to soften if you are using the authored "
        "camera.",
        false, false};
    static const Copy kLanguagesCopy = {
        "Menu languages",
        "Every cartridge carries every translation, whatever region it was "
        "sold in. Authentic shows only the ones your region's menu listed.",
        nullptr, false, false};
    static const Copy kFovCopy = {
        "Field of view",
        "authored keeps each track's original lens. Or type a number from 20 "
        "to 140.",
        nullptr, false, false};
    static const Copy kAspectCopy = {
        "Aspect ratio",
        "auto fills the window. 4:3 pillarboxes the original framing.",
        nullptr, false, false};
    static const Copy kWidescreenCopy = {
        "Widescreen",
        "Off is the old stretch-to-fill path, which distorts the picture. Turn "
        "it on.",
        nullptr, false, false};
    static const Copy kRenderScaleCopy = {
        "Render scale",
        "Renders above your window size and shrinks the result. This is the "
        "anti-aliasing control.",
        nullptr, false, false};
    static const Copy kMsaaCopy = {
        "MSAA", "Redundant with render scale. Off by default.",
        nullptr, false, false};
    static const Copy kAnisoCopy = {
        "Anisotropic filtering",
        "1 keeps the N64's own 3-point filter. Higher sharpens surfaces seen "
        "at a shallow angle.",
        nullptr, false, false};
    static const Copy kMipmapsCopy = {
        "Mipmaps", "Removes shimmer on distant track surfaces.",
        nullptr, false, false};
    static const Copy kHiresTextCopy = {
        "Sharper lettering",
        "Redraws the game's plain text from outline fonts. Same layout, "
        "sharper letters. Ignored in Original.",
        nullptr, false, false};
    static const Copy kShadowsCopy = {
        "World shadows",
        "Full is the shipped Remastered look. Soft is lighter. Off restores "
        "the original blob shadows under karts.",
        "DKR's artwork already paints its own shading in, so the shadow pass "
        "always darkens twice to some degree. This is the control for how "
        "much. It does nothing outside Remastered.",
        false, false};
    static const Copy kRemasterFxCopy = {
        "Remaster effects",
        "The master switch for Remastered's look-changing effects.",
        nullptr, false, false};
    static const Copy kWindowCopy = {
        "Window", "F11 or Alt+Enter does the same thing.",
        nullptr, false, false};
    static const Copy kRumbleCopy = {
        "Rumble",
        "Your controller's motors. The in-game Rumble Pak stays connected "
        "either way.",
        nullptr, false, false};
    static const Copy kRumbleProfileCopy = {
        "Rumble strength", nullptr, nullptr, false, false};
    static const Copy kMasterCopy = {"Master", nullptr, nullptr, false, false};
    static const Copy kMusicCopy = {"Music", nullptr, nullptr, false, false};
    static const Copy kEffectsCopy = {
        "Sound effects", nullptr, nullptr, false, false};

    switch (key) {
        case MDKR_VIDEO_MODE:               return &kMode;
        case MDKR_VIDEO_SIMULATION_CADENCE: return &kCadenceCopy;
        case MDKR_VIDEO_FRAME_LIMIT:        return &kFrameLimitCopy;
        case MDKR_VIDEO_MOTION_SMOOTHING:   return &kSmoothingCopy;
        case MDKR_VIDEO_ALLOW_TEARING:      return &kTearingCopy;
        case MDKR_VIDEO_CAMERA_OBSTRUCTION: return &kCameraCopy;
        case MDKR_VIDEO_CAMERA_COMFORT:     return &kComfortCopy;
        case MDKR_VIDEO_MENU_LANGUAGES:     return &kLanguagesCopy;
        case MDKR_VIDEO_GAMEPLAY_FOV:       return &kFovCopy;
        case MDKR_VIDEO_ASPECT:             return &kAspectCopy;
        case MDKR_VIDEO_WIDESCREEN:         return &kWidescreenCopy;
        case MDKR_VIDEO_RENDER_SCALE:       return &kRenderScaleCopy;
        case MDKR_VIDEO_MSAA:               return &kMsaaCopy;
        case MDKR_VIDEO_ANISOTROPY:         return &kAnisoCopy;
        case MDKR_VIDEO_MIPMAPS:            return &kMipmapsCopy;
        case MDKR_VIDEO_HIRES_TEXT:         return &kHiresTextCopy;
        case MDKR_VIDEO_WORLD_SHADOWS:      return &kShadowsCopy;
        case MDKR_VIDEO_REMASTER_FX:        return &kRemasterFxCopy;
        case MDKR_WINDOW_MODE:              return &kWindowCopy;
        case MDKR_INPUT_RUMBLE_ENABLED:     return &kRumbleCopy;
        case MDKR_INPUT_RUMBLE_PROFILE:     return &kRumbleProfileCopy;
        case MDKR_AUDIO_MASTER_VOLUME:      return &kMasterCopy;
        case MDKR_AUDIO_MUSIC_VOLUME:       return &kMusicCopy;
        case MDKR_AUDIO_EFFECTS_VOLUME:     return &kEffectsCopy;
        default:                            return nullptr;
    }
}

/*
 * The value in the player's own words, for anything that has to SAY it rather
 * than draw it. A voice reading "Frame limit, original" or "Rumble, 1" is
 * reading the config file aloud; the control on screen says "Original
 * (recommended)" and shows a tick, and the two must not disagree.
 *
 * Settings_dumpSchemaContract() and the row announcement both call this, so
 * the gate's expectation and the utterance are produced by one function.
 */
void displayValue(MdkrVideoKey key, const MdkrVideoSchema *s,
                  const MdkrVideoValue *v, char *out, size_t cap) {
    char raw[MDKR_VIDEO_STRING_MAX];
    formatValue(key, s, v, raw, sizeof(raw));
    Options options;
    if (optionsFor(key, options) || key == MDKR_VIDEO_MODE) {
        std::snprintf(out, cap, "%s", optionLabel(key, raw));
        return;
    }
    // A checkbox row. "on"/"off" is what the player sees and what a voice can
    // act on; "1" is neither.
    if (s->type == MDKR_VIDEO_TYPE_INT && s->min == 0.0f && s->max == 1.0f) {
        std::snprintf(out, cap, "%s",
                      v != nullptr && v->number != 0.0f ? "on" : "off");
        return;
    }
    std::snprintf(out, cap, "%s", raw);
}

// --- Enhancement help ------------------------------------------------------
// Every enhancement row ends with its AUTHORITY CLASS in the player's words.
// The class is asserted and gated in platform/enhancement_registry.c, so that
// is where this reads it from — never from the enum name, which says nothing to
// a player, and never from a second list kept here.
constexpr const char *kEnhancementGameplayNote = "Changes how the game plays.";
constexpr const char *kEnhancementLooksNote =
    "Changes only how the game looks.";

// Composed once per key; drawn every frame.
std::array<std::string, MDKR_VIDEO_KEY_COUNT> g_enhancementHelp;

/*
 * WHEN a setting takes effect, said on every row that has room to say it.
 *
 * The page had three timings and only spelled two of them. A RESTART key gets
 * the gold "Next launch" chip, a LEVEL key gets the grey "Next race" chip, and
 * a LIVE key got NOTHING -- so "this applies immediately" was communicated by
 * the absence of a marker, which is not communication. Eighteen of the visible
 * rows were in that silent majority.
 *
 * The two scoped cases keep their chip and are left alone here: a chip on the
 * label line plus a sentence under it would be the same fact twice. Only LIVE
 * gains a sentence, and it is generated from the schema scope rather than
 * written per row, so a key added tomorrow cannot be the one that forgets.
 */
std::array<std::string, MDKR_VIDEO_KEY_COUNT> g_rowDescription;

const char *describeRow(MdkrVideoKey key, const MdkrVideoSchema *schema,
                        const char *base) {
    if (schema->scope == MDKR_VIDEO_SCOPE_RESTART ||
        schema->scope == MDKR_VIDEO_SCOPE_LEVEL) {
        return base;   // the chip on the label line already said it
    }
    std::string &text = g_rowDescription[static_cast<size_t>(key)];
    if (!text.empty()) return text.c_str();
    text = base != nullptr ? base : "";
    if (!text.empty()) text += ' ';
    text += "Applies straight away.";
    return text.c_str();
}

const char *enhancementHelp(MdkrVideoKey key,
                            const MdkrEnhancement *enhancement,
                            const MdkrVideoSchema *schema) {
    std::string &text = g_enhancementHelp[static_cast<size_t>(key)];
    if (!text.empty()) return text.c_str();
    const char *note = enhancement->authority == MDKR_ENH_GAMEPLAY
        ? kEnhancementGameplayNote : kEnhancementLooksNote;
    // The schema paragraph, because it is the one that explains what the
    // values mean; the registry's own sentence is the shorter summary the
    // enhancement table carries.
    text = schema->help != nullptr ? schema->help : enhancement->help;
    // Appended only when the paragraph does not already end with it. Most were
    // written with the sentence in place, and printing it twice reads as a
    // stutter rather than as emphasis — while a new row whose author forgot it
    // still gets the class stated, which is the part that must not be optional.
    const size_t noteLength = std::strlen(note);
    if (text.size() < noteLength ||
        text.compare(text.size() - noteLength, noteLength, note) != 0) {
        if (!text.empty()) text += ' ';
        text += note;
    }
    return text.c_str();
}

const char *helpFor(MdkrVideoKey key, const MdkrVideoSchema *schema) {
    const MdkrEnhancement *enhancement = mdkr_enhancement_for_key(key);
    if (enhancement != nullptr) {
        return enhancementHelp(key, enhancement, schema);
    }
    switch (key) {
        case MDKR_VIDEO_SIMULATION_CADENCE:
            return "Original preserves retail physics, AI, timers, and input "
                   "timing. Enhanced runs the game's logic at 60 Hz and "
                   "changes gameplay speed. It is not an FPS setting.";
        case MDKR_VIDEO_FRAME_LIMIT:
            return kFrameLimitHelp;
        case MDKR_VIDEO_MOTION_SMOOTHING:
            return "Interpolated blends the game's own adjacent pictures at "
                   "the display's exact fractional time. Simulation, input, "
                   "audio, timers, and saves still advance only on Original "
                   "gameplay ticks. Off shows the game's own pictures only.";
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
    // While an open combo is being browsed, "the last item" is the option under
    // the cursor rather than this row. Announcing the row's CURRENT value then
    // would tell the player the opposite of what they are about to choose.
    bool comboBrowsing = false;
    EditState &editState = g_edits[static_cast<size_t>(k)];

    ImGui::PushID((int)k);
    if (locked || rumbleProfileUnavailable) ImGui::BeginDisabled();

    const Copy *copy = copyFor(k);
    const bool controllerBinding =
        k >= MDKR_INPUT_CONTROLLER_A &&
        k <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT;
    ui::RowStyle rowStyle;
    rowStyle.gameplay = copy != nullptr && copy->gameplay;
    rowStyle.experimental = copy != nullptr && copy->experimental;
    rowStyle.restart = s->scope == MDKR_VIDEO_SCOPE_RESTART;
    // LEVEL is not a weaker RESTART, and saying "restart" for it would be a
    // lie a player can disprove. It has its own boundary and its own word.
    if (s->scope == MDKR_VIDEO_SCOPE_LEVEL) rowStyle.badge = "Next race";
    if (copy != nullptr && !compact) rowStyle.tooltip = copy->tooltip;
    // A binding row is one of eighteen in a grid; a description under each one
    // would be a wall, and the label already says which control it is.
    //
    // A key with no Copy entry -- every row the sprint work added -- falls back
    // to helpFor(), which carries the enhancement authority-class note and the
    // update-check "not active yet" paragraph. A bare label here would silently
    // break the Enhancements header's promise that every row states whether it
    // changes gameplay.
    const char *rowLabel = copy != nullptr ? copy->label : s->label;
    const char *description =
        copy != nullptr ? copy->description : helpFor(k, s);
    const bool describable = !compact && !controllerBinding;
    ui::SettingLabel(rowLabel,
                     describable ? describeRow(k, s, description) : nullptr,
                     rowStyle);

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
            /* Sample only while the popup is closed. BeginCombo() returning
             * true means it has ALREADY begun the popup window, and Begin()
             * reassigns ImGui's last-item data to that window's title bar --
             * so on open frames GetItemRectMin/Max describe the popup (a
             * zero-height rect wherever it was placed) and IsItemVisible()
             * answers for the popup too. Reading them there overwrote the
             * combo's rect with popup geometry and made the off-screen
             * diagnostic below fire on a perfectly visible combo. */
            if (!comboOpen) {
                g_frameLimitRectMin = ImGui::GetItemRectMin();
                g_frameLimitRectMax = ImGui::GetItemRectMax();
                /* The one source of truth for "the panel is showing this
                 * widget right now": ImGui clears the visible bit for an item
                 * whose rectangle misses the current clip rect, whether it was
                 * scrolled past the bottom or squeezed off any other edge. The
                 * scripted pacing gates click these coordinates on the next
                 * frame, and Settings_smokeFrameLimitCenter() refuses to hand
                 * them over unless this says the widget is on screen. */
                g_frameLimitRectValid = ImGui::IsItemVisible();
            }
            /* Same fact, said out loud, because a gate that simply cannot find
             * its target reports "was not rendered" and leaves the reader to
             * guess which of the two it is. Read from the flag above so the
             * diagnostic and the refusal can never disagree. Armed only for
             * the gates: a player scrolling past this row is the same
             * observation and means nothing. */
            static const bool pacingGateArmed =
                std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") != nullptr ||
                std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE") != nullptr;
            static bool offscreenReported = false;
            if (pacingGateArmed && !offscreenReported && !comboOpen &&
                !g_frameLimitRectValid) {
                offscreenReported = true;
                std::fprintf(stderr,
                             "[app-ui-test] frame-limit combo is scrolled out "
                             "of the panel (rect y=%.0f..%.0f, panel height "
                             "%.0f): something drawn above it -- an open "
                             "section, or a larger UI scale -- pushed it past "
                             "the bottom\n",
                             g_frameLimitRectMin.y, g_frameLimitRectMax.y,
                             ImGui::GetIO().DisplaySize.y);
            }
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
            comboBrowsing = true;
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

    /*
     * THE announcement. Every settings row in the product passes through here
     * -- the launcher panel, the in-game overlay, the Enhancements and Content
     * sections all call drawKey() -- so a row added later speaks without
     * anybody remembering to make it, and a row can only be silent by being
     * hand-rolled somewhere else. It sits immediately after the widget on
     * purpose: ui::SpeakFocusedItem reads ImGui's last-submitted item, and the
     * error text, badges and help paragraph below would take that place.
     */
    if (!comboBrowsing) {
        char spoken[MDKR_VIDEO_STRING_MAX];
        displayValue(k, s, d, spoken, sizeof(spoken));
        // The drawn label and the drawn help, not the schema's: the voice
        // and the screen must not disagree (see displayValue's contract).
        const char *spokenHelp = copy != nullptr
            ? (copy->tooltip != nullptr ? copy->tooltip : copy->description)
            : helpFor(k, s);
        ui::SpeakFocusedItem(rowLabel, spoken, spokenHelp);
    }

    if (!editState.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", editState.error.c_str());
        ImGui::PopStyleColor();
        const bool retryPressed = editState.dirty && ImGui::Button(
            "Try again", ImVec2(0.0f, ui::kBtnSecondary().y));
        if (editState.dirty && k == MDKR_VIDEO_FRAME_LIMIT) {
            g_frameLimitRetryRectMin = ImGui::GetItemRectMin();
            g_frameLimitRetryRectMax = ImGui::GetItemRectMax();
            // Submitted is not the same as on screen; the same-process Retry
            // gate clicks these coordinates. See the flag block up top.
            g_frameLimitRetryRectValid = ImGui::IsItemVisible();
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
            ui::TextSubtle("Turn rumble on to choose a strength.");
        } else if (mdkr_video_config_is_readonly()) {
            ui::TextSubtle("Locked for this Original session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_CLI) {
            ui::TextSubtle("Set on the command line for this session.");
        } else if (d->source == MDKR_VIDEO_SOURCE_ENV) {
            ui::TextSubtle("Set by %s for this session.", s->env);
        } else {
            ui::TextSubtle("Set by the %s for this session.",
                           sourceName(d->source));
        }
    } else if (s->scope == MDKR_VIDEO_SCOPE_RESTART && differsFromLive(k, s)) {
        // The honest RESTART presentation: say what is running NOW and what will
        // be running next launch. Never imply the change already took effect.
        char liveBuf[MDKR_VIDEO_STRING_MAX];
        formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::Text("Next Play: %s. Running now: %s.",
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
        ImGui::Text("Next race: %s. Running now: %s.",
                    optionLabel(k, valueBuf), optionLabel(k, liveBuf));
        ImGui::PopStyleColor();
    }

    /* The description and any tooltip were drawn ABOVE the control, with the
     * label. A paragraph underneath is read after the choice has already been
     * made, which is exactly backwards, and it was the single largest source of
     * text on the page. */

    ui::Gap(ui::kGapS);
    ImGui::PopID();
    return changed;
}

// --- Frame rate: ONE control over Frame limit + Motion smoothing + tearing --
//
// WHY THIS IS ONE CONTROL NOW. The page used to offer the same setting twice:
// a "Presentation pace" quick choice with two radios, and, a few rows below it
// behind a "Set the rate yourself" disclosure, the two combos that quick choice
// writes. Both were always on screen, both always live, and a player who
// changed one watched the other move on its own with nothing saying why.
//
// So the presets and the individual rows became one control. Off Custom the
// individual rows are NOT DRAWN -- the page offers one way to set the frame
// rate, not two -- and Custom is a real, pressable third choice that reveals
// them. A line under the choices names the two values the selected preset
// writes, so a preset is never a black box.
//
// WHY THIS IS STILL SUGAR AND NOT A SETTING. The two keys below stay the source
// of truth: this writes them and reads them back and holds no persisted state
// of its own. A player who edits the config file by hand behaves identically,
// and every gate that drives Video.FrameLimit keeps testing the same thing. A
// third persisted key that the other two had to be kept in sync with has a
// wrong answer available at every layer, and this does not.
//
// WHY Custom IS STICKY. It is revealed state, not a value: pressing it writes
// nothing, because there is nothing honest to write. Without the latch, a
// player who entered Custom and then tuned their way into a pair that happens
// to spell "Smooth" would have the controls they were using vanish from under
// their hands. The latch clears only when they press Original or Smooth.
struct PaceChoice { MdkrPresentationPace pace; const char *label; };
const PaceChoice kPaceChoices[] = {
    {MDKR_PRESENTATION_PACE_ORIGINAL, "Original"},
    {MDKR_PRESENTATION_PACE_SMOOTH,   "Smooth"},
};

bool g_frameRateCustomRevealed = false;

// What the selected choice actually does, named in the player's words rather
// than left for them to discover by watching two other controls move.
const char *paceSummary(MdkrPresentationPace pace) {
    switch (pace) {
        case MDKR_PRESENTATION_PACE_ORIGINAL:
            return "Original draws each picture the game makes, once, and "
                   "nothing in between. Frame limit: Original. Motion "
                   "smoothing: Off.";
        case MDKR_PRESENTATION_PACE_SMOOTH:
            return "Smooth follows your display and fills in the pictures "
                   "between the game's own. Frame limit: Match Display. "
                   "Motion smoothing: Interpolated.";
        default:
            return "Set Frame limit and Motion smoothing yourself below.";
    }
}

bool drawFrameRate(SDL_Window *window, bool compact, bool selectingFrameLimit) {
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
    // A pair no preset names, or a scripted gate about to click the individual
    // combo, both mean the same thing: the individual rows have to be reachable.
    if (current == MDKR_PRESENTATION_PACE_CUSTOM || selectingFrameLimit) {
        g_frameRateCustomRevealed = true;
    }
    bool changed = false;

    ImGui::PushID("presentation-pace");
    if (locked) ImGui::BeginDisabled();
    ui::RowStyle paceStyle;
    paceStyle.tooltip = compact ? nullptr :
        "The in-between pictures Smooth draws are invented, so fast-moving "
        "edges can show artefacts in them. Race speed, timers, music and saves "
        "are identical either way.";
    ui::SettingLabel(
        "Frame rate",
        compact ? nullptr
                : "How often the app draws to your screen. It never changes "
                  "how fast the game runs. Applies straight away.",
        paceStyle);

    for (int i = 0; i < static_cast<int>(std::size(kPaceChoices)); ++i) {
        const PaceChoice &choice = kPaceChoices[i];
        if (i > 0) ImGui::SameLine(0.0f, ui::kGapM);
        const bool selected =
            !g_frameRateCustomRevealed && current == choice.pace;
        const bool pressed = ImGui::RadioButton(choice.label, selected);
        const int slot = static_cast<int>(choice.pace);
        g_paceRectMin[slot] = ImGui::GetItemRectMin();
        g_paceRectMax[slot] = ImGui::GetItemRectMax();
        // The quick-choice gate presses one of these radios at last frame's
        // coordinates, so the flag has to mean "on screen", not "submitted".
        g_paceRectValid[slot] = ImGui::IsItemVisible();
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
            g_frameRateCustomRevealed = false;
            changed = true;
        }
    }
    ImGui::SameLine(0.0f, ui::kGapM);
    if (ImGui::RadioButton("Custom", g_frameRateCustomRevealed)) {
        // Reveal only. There is no honest pair to write for "Custom", and
        // writing one would silently move the player off the values they came
        // here to keep.
        g_frameRateCustomRevealed = true;
    }

    if (!compact) {
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtleUnformattedWrapped(paceSummary(
            g_frameRateCustomRevealed ? MDKR_PRESENTATION_PACE_CUSTOM
                                      : current));
        ImGui::PopFont();
    }
    if (locked) {
        ImGui::EndDisabled();
        ui::TextSubtle("Fixed for this session.");
    }
    // One row per DISTINCT reading, not per frame: the panel redraws sixty
    // times a second and a per-frame row would bury the transition a gate is
    // looking for in thousands of identical ones.
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        const MdkrVideoConfig *now = mdkr_video_config_desired();
        static std::string traced;
        char row[256];
        std::snprintf(row, sizeof(row),
                      "[app-ui] presentation-pace value=%.31s frameLimit=%.31s "
                      "motionSmoothing=%.31s locked=%d",
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

    // The individual rows, drawn ONLY on Custom. This is what makes the merge
    // real rather than cosmetic: off Custom they are not on the page at all, so
    // there is exactly one control for the frame rate.
    if (g_frameRateCustomRevealed) {
        ImGui::Indent(ui::kGapM);
        changed |= drawKey(window, MDKR_VIDEO_FRAME_LIMIT, compact);
        changed |= drawKey(window, MDKR_VIDEO_MOTION_SMOOTHING, compact);
        changed |= drawKey(window, MDKR_VIDEO_ALLOW_TEARING, compact);
        ImGui::Unindent(ui::kGapM);
    }
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
    setStatus("Controller mapping and rumble reset to defaults.",
              AppTheme::good());
    return true;
}

// --- Enhancements ----------------------------------------------------------
// The rows are enumerated from platform/enhancement_registry.c rather than
// listed here, for the reason that table exists: a panel carrying its own copy
// of the list keeps looking complete after somebody adds a row and forgets it.
// Grouping is the registry's category, and a category with no rows draws no
// header — COSMETIC is declared and still empty.
struct EnhancementGroup {
    MdkrEnhCategory category;
    const char *label;
};
const EnhancementGroup kEnhancementGroups[] = {
    {MDKR_ENH_CAT_DISPLAY,    "On screen"},
    {MDKR_ENH_CAT_DIFFICULTY, "Difficulty"},
    {MDKR_ENH_CAT_COSMETIC,   "Appearance"},
};

// Restores the enhancement keys, and only those, to their schema defaults.
//
// The scoping is the action. "Reset" next to a list of extras must not be a
// trap that also throws away the frame limit, the volume levels, and a
// remapped controller, so the key set comes from AppUi_enhancementResetIncludes
// and one transaction writes exactly it — the same shape as the controller
// restore above, so a partial write is not a state either can land in.
bool resetEnhancements() {
    MdkrVideoConfig defaults;
    std::array<std::string, MDKR_VIDEO_KEY_COUNT> values;
    std::array<MdkrVideoRuntimeChange, MDKR_VIDEO_KEY_COUNT> changes;
    int count = 0;

    mdkr_video_config_defaults(&defaults);
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (!AppUi_enhancementResetIncludes(key)) continue;
        const MdkrVideoSchema *schema = mdkr_video_schema(key);
        if (schema == nullptr) return false;
        if (schema->type == MDKR_VIDEO_TYPE_STRING) {
            values[static_cast<size_t>(count)] = defaults.values[key].text;
        } else if (schema->type == MDKR_VIDEO_TYPE_INT) {
            values[static_cast<size_t>(count)] = std::to_string(
                static_cast<int>(defaults.values[key].number));
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f",
                          static_cast<double>(defaults.values[key].number));
            values[static_cast<size_t>(count)] = buf;
        }
        // `values` is a fixed array, so no element ever moves and the pointer
        // handed to the transaction stays valid until it returns.
        changes[static_cast<size_t>(count)] =
            {key, values[static_cast<size_t>(count)].c_str()};
        ++count;
    }
    if (count == 0) return false;

    const MdkrVideoRuntimeResult result =
        mdkr_video_config_runtime_set_many(changes.data(), count);
    if (!resultSucceeded(result)) {
        // One pinned row refuses the whole transaction, so name the row that
        // refused it rather than whichever one happened to be written first:
        // "Speedometer is fixed by ..." is not an answer for a player whose
        // Opponent skill is the pinned one.
        MdkrVideoKey blamed = changes[0].key;
        for (int i = 0; i < count; ++i) {
            const MdkrVideoKey key = changes[static_cast<size_t>(i)].key;
            if (mdkr_video_config_runtime_locked(key) != 0) {
                blamed = key;
                break;
            }
        }
        reportResult(result, mdkr_video_schema(blamed));
        return false;
    }
    for (int i = 0; i < count; ++i) {
        g_edits[static_cast<size_t>(changes[static_cast<size_t>(i)].key)] =
            EditState{};
    }
    // Scope is stated under the button, where it can be read BEFORE the press.
    // Repeating it here made one sentence appear twice on one page.
    setStatus("Extras are back to the way the game shipped.", AppTheme::good());
    return true;
}

int enhancementsInGroup(MdkrEnhCategory category, bool webGpuRenderer,
                        bool legacyStretchActive) {
    int rows = 0;
    for (int i = 0; i < mdkr_enhancement_count(); ++i) {
        const MdkrEnhancement *enhancement = mdkr_enhancement_at(i);
        if (enhancement == nullptr || enhancement->category != category) {
            continue;
        }
        if (!AppUi_videoSettingVisible(enhancement->key, webGpuRenderer,
                                       legacyStretchActive)) {
            continue;
        }
        ++rows;
    }
    return rows;
}

bool drawEnhancementsSection(SDL_Window *window, bool compact,
                             bool webGpuRenderer, bool legacyStretchActive) {
    bool changed = false;
    ui::Gap(ui::kGapS);
    if (!compact) {
        ui::TextSubtleWrapped(
            "Extras that stay off until you switch them on. Each one says "
            "whether it changes how the game plays or only how it looks.");
    }
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    for (const EnhancementGroup &group : kEnhancementGroups) {
        if (enhancementsInGroup(group.category, webGpuRenderer,
                                legacyStretchActive) == 0) {
            continue;
        }
        ImGui::SeparatorText(group.label);
        ui::Gap(ui::kGapXS);
        for (int i = 0; i < mdkr_enhancement_count(); ++i) {
            const MdkrEnhancement *enhancement = mdkr_enhancement_at(i);
            if (enhancement == nullptr ||
                enhancement->category != group.category) {
                continue;
            }
            if (!AppUi_videoSettingVisible(enhancement->key, webGpuRenderer,
                                           legacyStretchActive)) {
                continue;
            }
            changed |= drawKey(window, enhancement->key, compact);
        }
    }
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Reset extras", ui::kBtnWide())) {
        changed |= resetEnhancements();
    }
    if (!compact) {
        ui::TextSubtleWrapped(
            "Puts every extra above back to the way the game shipped. Your "
            "picture, sound, controller and content-pack settings are not "
            "touched.");
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

// --- UI scale --------------------------------------------------------------
//
// The one settings control with no schema key: it is a launcher preference, so
// nothing in MdkrVideoKey can route it. Which section draws it is therefore a
// policy question (AppUi_shellPreferenceSection), asked at both candidate
// sections so that exactly one of them ever answers yes. Hand-placing the
// widget instead is how a control ends up drawn twice, with two independent
// copies of the commit path underneath it.
bool drawUiScale(bool compact) {
    bool changed = false;
    if (!g_uiScaleInitialized) {
        g_uiScaleEdit = AppTheme::uiScale();
        g_uiScaleInitialized = true;
    }
    ui::RowStyle scaleStyle;
    ui::SettingLabel(
        "Interface scale",
        compact ? nullptr
                : "Text and controls together, 0.75x to 2x. Use 1.25x or "
                  "more for touch.",
        scaleStyle);
    ImGui::SetNextItemWidth(ui::kControlWidth());
    const bool scalePreviewChanged = ImGui::SliderFloat(
        "##ui-scale", &g_uiScaleEdit, 0.75f, 2.0f, "%.2fx",
        ImGuiSliderFlags_AlwaysClamp);
    g_uiScaleRectMin = ImGui::GetItemRectMin();
    g_uiScaleRectMax = ImGui::GetItemRectMax();
    /* Same rule as the frame-limit combo: the drag gate grabs this slider at
     * last frame's coordinates, so the flag has to mean "the panel is showing
     * it", not "it was submitted". A slider is its own last item -- there is no
     * popup to displace it -- so IsItemVisible() reads directly here. */
    g_uiScaleRectValid = ImGui::IsItemVisible();
    /* Say which failure this is, from the flag above rather than a second read,
     * so the diagnostic cannot contradict the refusal. */
    {
        static const bool dragGateArmed =
            std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") != nullptr;
        static bool offscreenReported = false;
        if (dragGateArmed && !offscreenReported && !g_uiScaleRectValid) {
            offscreenReported = true;
            std::fprintf(stderr,
                         "[app-ui-test] UI-scale slider is scrolled out of the "
                         "panel (rect y=%.0f..%.0f, panel height %.0f): "
                         "something drawn above it -- an open section, or the "
                         "scale this very slider just applied -- pushed it "
                         "past the bottom\n",
                         g_uiScaleRectMin.y, g_uiScaleRectMax.y,
                         ImGui::GetIO().DisplaySize.y);
        }
        if (dragGateArmed) {
            /* Deterministic starting viewport, and nothing more. The scripted
             * drag presses a real SDL pointer at a fixed coordinate, so the
             * slider has to be on screen before the first press. Two frames:
             * the first scrolls, the second confirms the position is already
             * correct, so the rectangle the gate captures from frame 1 onward
             * never moves. */
            static int scrolledFrames = 0;
            if (scrolledFrames < 2) {
                ImGui::SetScrollHereY(0.5f);
                ++scrolledFrames;
            }
        }
    }
    // Spoken here for the same reason drawKey() speaks its rows: this widget
    // is hand-rolled, so the shared helper cannot reach it, and a silent text
    // size control is the one a player who cannot read the panel needs most.
    {
        char value[32];
        std::snprintf(value, sizeof(value), "%.2fx",
                      static_cast<double>(g_uiScaleEdit));
        ui::SpeakFocusedItem(
            "Interface scale", value,
            "Scales text and controls together after you release the slider.");
    }
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
                    ? "UI scale applied, but the system could not confirm it "
                      "reached the disk. Set it again after an unexpected "
                      "shutdown."
                    : "Interface scale saved.",
                persist == AppConfig::PersistResult::DurabilityUnconfirmed
                    ? AppTheme::accent() : AppTheme::good());
            changed = true;
        } else {
            g_uiScaleError =
                "Interface scale could not be saved. It stays active for this "
                "session; try again once the settings file is writable.";
            setStatus(g_uiScaleError.c_str(), AppTheme::bad());
        }
    }
    if (!g_uiScaleError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", g_uiScaleError.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button(
                "Try again",
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
                        ? "UI scale applied, but the system could not confirm it "
                          "reached the disk. Set it again after an unexpected "
                          "shutdown."
                        : "Interface scale saved.",
                    persist == AppConfig::PersistResult::DurabilityUnconfirmed
                        ? AppTheme::accent() : AppTheme::good());
                changed = true;
            }
        }
    }
    ui::Gap(ui::kGapS);
    return changed;
}

// --- Accessibility ---------------------------------------------------------
//
// One place for every option a player might need before they can use the rest
// of the panel. The rows are ENUMERATED from AppUi_settingsSection rather than
// listed here, so an accessibility option added later appears the moment it is
// routed -- and so this section and the sections the keys came from cannot
// disagree about which of them draws a row.
bool drawAccessibilitySection(SDL_Window *window, bool compact,
                              bool webGpuRenderer, bool legacyStretchActive) {
    bool changed = false;
    /* No introduction here either: the group header says it, and said it
     * better. */
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    if (AppUi_shellPreferenceSection(AppUiShellPreference::UiScale) ==
        AppUiSettingsSection::Accessibility) {
        changed |= drawUiScale(compact);
    }
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (AppUi_settingsSection(key) !=
            AppUiSettingsSection::Accessibility) continue;
        if (!AppUi_videoSettingVisible(key, webGpuRenderer,
                                       legacyStretchActive)) continue;
        changed |= drawKey(window, key, compact);
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
}

// --- Content packs ---------------------------------------------------------
// The read-only list below exists for one failure: a pack that is quietly
// ignored looks to the player exactly like a pack that loaded and did nothing.
// Every directory the scan saw is accounted for here — installed, or skipped
// with a reason — which is the same accounting the startup log prints and the
// single commonest question a pack author is asked.

// Only `name` is mandatory in a pack.ini, so the optional parts are appended
// rather than formatted in: a pack that declared neither must not read as
// "Name  by  (priority 100)".
std::string packSummary(const MdkrModEntry *entry) {
    std::string text = entry->manifest.name;
    if (entry->manifest.version[0] != '\0') {
        text += "  ";
        text += entry->manifest.version;
    }
    if (entry->manifest.author[0] != '\0') {
        text += "  by ";
        text += entry->manifest.author;
    }
    char priority[48];
    std::snprintf(priority, sizeof(priority), "  (priority %d)",
                  entry->manifest.priority);
    text += priority;
    return text;
}

// Why a disabled entry is disabled. The registry records the state but not the
// cause, so the cause is re-derived from the list the scan itself read, through
// the scan's own matcher rather than a second one that could disagree with it.
const char *packSkipReason(const MdkrModEntry *entry,
                           const char *disabledList) {
    return platform_content_pack_name_disabled(disabledList,
                                               entry->manifest.name)
        ? "you listed it under Skipped packs"
        : "its own pack.ini switches it off";
}

// One row per pack, once per process, so a gate can read what the panel drew
// without depending on whether the section happens to be expanded.
void traceContentPacks(const MdkrModRegistry *packs, const char *disabledList) {
    if (std::getenv("MDKR_APP_UI_TRACE") == nullptr) return;
    static bool traced = false;
    if (traced) return;
    traced = true;
    const int count = mdkr_mod_registry_count(packs);
    const int skipped = mdkr_mod_registry_skipped(packs);
    std::fprintf(stderr, "[app-ui] content-list found=%d unreadable=%d\n",
                 count, skipped);
    for (int i = 0; i < count; ++i) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
        if (entry == nullptr) continue;
        std::fprintf(
            stderr,
            "[app-ui] content-pack name=\"%s\" version=\"%s\" author=\"%s\" "
            "priority=%d state=%s reason=\"%s\"\n",
            entry->manifest.name, entry->manifest.version,
            entry->manifest.author, entry->manifest.priority,
            entry->manifest.enabled ? "installed" : "skipped",
            entry->manifest.enabled ? "" : packSkipReason(entry, disabledList));
    }
    for (int i = 0; i < skipped; ++i) {
        const char *reason = mdkr_mod_registry_skip_reason(packs, i);
        std::fprintf(stderr,
                     "[app-ui] content-pack name=\"%s\" state=skipped "
                     "reason=\"%s\"\n",
                     packs->skip_name[i], reason != nullptr ? reason : "");
    }
}

bool drawContentSection(SDL_Window *window, bool compact,
                        const MdkrModRegistry *packs,
                        const char *disabledList) {
    bool changed = false;
    const int count = mdkr_mod_registry_count(packs);
    const int unreadable = mdkr_mod_registry_skipped(packs);

    ui::Gap(ui::kGapS);
    if (!compact) {
        ui::TextSubtleWrapped(
            "Put a pack's folder in the mods folder beside your saves. "
            "Everything found there is listed below, installed or skipped "
            "with the reason.");
    }
    ui::Gap(ui::kGapS);
    ImGui::Indent(ui::kGapM);
    changed |= drawKey(window, MDKR_CONTENT_PACKS_ENABLED, compact);
    changed |= drawKey(window, MDKR_CONTENT_PACK_DISABLED, compact);

    ImGui::SeparatorText("Installed");
    ui::Gap(ui::kGapXS);
    int installed = 0;
    for (int i = 0; i < count; ++i) {
        const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
        if (entry == nullptr || !entry->manifest.enabled) continue;
        ImGui::BulletText("%s", packSummary(entry).c_str());
        ++installed;
    }
    if (installed == 0) {
        ui::TextSubtleWrapped(
            count == 0 && unreadable == 0
                ? "Nothing yet. A pack is a folder with a pack.ini file in it."
                : "None of the packs found are in use. Every one of them is "
                  "listed below with the reason.");
    } else if (!compact) {
        ui::TextSubtleWrapped(
            "Where two packs supply the same artwork, the higher priority "
            "wins.");
    }

    if (count - installed + unreadable > 0) {
        ImGui::SeparatorText("Skipped");
        ui::Gap(ui::kGapXS);
        for (int i = 0; i < count; ++i) {
            const MdkrModEntry *entry = mdkr_mod_registry_entry(packs, i);
            if (entry == nullptr || entry->manifest.enabled) continue;
            ImGui::BulletText("%s — %s", entry->manifest.name,
                              packSkipReason(entry, disabledList));
        }
        for (int i = 0; i < unreadable; ++i) {
            const char *reason = mdkr_mod_registry_skip_reason(packs, i);
            ImGui::BulletText("%s — %s", packs->skip_name[i],
                              reason != nullptr && reason[0] != '\0'
                                  ? reason : "it could not be read");
        }
    }
    ImGui::Unindent(ui::kGapM);
    ui::Gap(ui::kGapS);
    return changed;
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

    /*
     * The control inventory, one row per schema key: the name a voice would
     * say, the value it would say, and whether the product offers the setting
     * at all.
     *
     * tests/check_a11y_shell.py grades the walk against THIS, rather than
     * against a list kept in the test, which is the whole point: a setting
     * added tomorrow gets a row here the moment it has a schema entry, and the
     * gate then demands an utterance for it without anybody remembering to
     * extend the test. The label and value come from the same displayValue()
     * the announcement uses, so the expectation and the utterance cannot drift
     * apart while both still look right.
     *
     * Visibility is reported for the qualified WebGPU renderer with widescreen
     * engaged -- the shipped configuration. A diagnostic OpenGL session offers
     * one setting MORE (MSAA), which can only ever add an utterance the gate
     * did not require, never remove one it did.
     */
    const MdkrVideoConfig *config = mdkr_video_config_desired();
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey     key = static_cast<MdkrVideoKey>(i);
        const MdkrVideoSchema *s   = mdkr_video_schema(key);
        if (s == nullptr) continue;
        char value[MDKR_VIDEO_STRING_MAX];
        displayValue(key, s, config != nullptr ? &config->values[i] : nullptr,
                     value, sizeof(value));
        /* The DRAWN label -- the Copy table's where one exists -- for the same
         * reason the value comes from displayValue(): the gate's expectation
         * and the on-screen text must be one string, and the announcement
         * speaks the drawn label. */
        const Copy *copy = copyFor(key);
        std::printf(
            "[app-a11y] control key=%s visible=%d label=\"%s\" value=\"%s\"\n",
            s->name,
            AppUi_videoSettingVisible(key, /*webGpuRenderer=*/true,
                                      /*legacyStretchActive=*/false) ? 1 : 0,
            copy != nullptr ? copy->label : s->label, value);
    }
}

bool Settings_restartPending() {
    /* Keep the launcher, overlay, and original in-game settings on one
     * predicate. In particular, Video.Mode is a preset label rather than a
     * staged engine override; the runtime helper correctly evaluates the
     * individual values expanded by that preset. */
    return mdkr_video_config_restart_pending() != 0;
}

void Settings_requestControllerSection() {
    g_controllerSectionRequested = true;
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
    /*
     * Every widget-rect flag starts each frame false, and only the widget's own
     * submission can raise it again. This is the half IsItemVisible() cannot
     * cover: a collapsed section returns from here without ever reaching the
     * widget, so nothing would run to lower a flag left true by an earlier
     * frame -- which is precisely the stale latch this panel used to have. The
     * whole set is cleared in one place so a rect added later is either listed
     * here or is not per-frame, rather than being quietly sticky.
     *
     * Limit worth knowing: this is per Settings_draw() call, so it says nothing
     * about frames where the panel is not drawn at all (another launcher tab,
     * or the overlay with settings hidden). Every scripted gate pins
     * MDKR_APP_PANEL=Settings, so for them "drawn" and "this frame" coincide.
     */
    g_frameLimitRectValid = false;
    g_frameLimitRetryRectValid = false;
    g_uiScaleRectValid = false;
    g_sectionsDrawn = 0;
    for (bool &paceValid : g_paceRectValid) paceValid = false;
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
    const bool controllerSettingsSmoke =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_SETTINGS") != nullptr;
    const bool smokeControllerRestore =
        std::getenv("MDKR_APP_SMOKE_CONTROLLER_RESTORE") != nullptr;
    /* Both scripted pacing gates need the same thing: the frame-rate controls
     * in view without scrolling, so a queued click lands on the widget rather
     * than on whatever the panel happened to have scrolled under it. */
    const bool selectingFrameLimit =
        std::getenv("MDKR_APP_SMOKE_SELECT_FRAME_LIMIT") != nullptr ||
        std::getenv("MDKR_APP_SMOKE_SELECT_PRESENTATION_PACE") != nullptr;
    /* Same need, one group further down: the scripted interface-scale drag
     * holds a real pointer on the slider, so the slider has to be on screen
     * without a scroll the script has no way to perform. */
    const bool draggingUiScale =
        std::getenv("MDKR_APP_SMOKE_UI_SCALE_DRAG") != nullptr;
    int controllerMappingWidgets = 0;
    int controllerRumbleWidgets = 0;
    bool controllerRestoreAvailable = false;
    bool controllerRestoreSucceeded = false;

    const bool webGpuRenderer =
        mdkr_render_backend() == MDKR_BACKEND_WEBGPU;
    /* Video.Widescreen is normally hidden because no preset ever selects its
     * 0 branch (see AppUi_videoSettingVisible). A config that already resolved
     * to 0 is the exception: the player is looking at the pre-widescreen
     * stretch and needs a control to leave it. */
    const bool legacyStretchActive =
        mdkr_video_config_current()
            ->values[MDKR_VIDEO_WIDESCREEN].number == 0.0f;

    /*
     * WHY THE GROUPS ARE NAMED HERE AND NOT TAKEN FROM MdkrVideoCategory.
     *
     * The schema's categories answer "which subsystem owns this key", which is
     * the right question for the config layer and the wrong one for a player.
     * It put Camera and Menu languages under Presentation because both are
     * published by the presentation path, and it put Simulation cadence beside
     * Frame limit under Pacing because both are about time -- which is exactly
     * the confusion behind the reports that Enhanced was a frame-rate setting.
     *
     * So the page groups by what a player came to do, and the schema stays the
     * source of truth for everything else. The safety net is `drawnKey`: any
     * visible key no group claims is rendered under Other settings at the
     * bottom, so a key added to the schema tomorrow can never silently vanish
     * from the panel the way an explicit list would normally allow.
     */
    bool drawnKey[MDKR_VIDEO_KEY_COUNT] = {};
    const auto visible = [&](MdkrVideoKey key) {
        return AppUi_videoSettingVisible(key, webGpuRenderer,
                                         legacyStretchActive);
    };
    const auto row = [&](MdkrVideoKey key) {
        if (!visible(key)) return;
        // Keys the routing policy assigns to a dedicated section
        // (Accessibility, Enhancements, Content) are drawn by that section,
        // never under a group here -- one owner per control.
        if (AppUi_settingsSection(key) != AppUiSettingsSection::Category) return;
        drawnKey[static_cast<size_t>(key)] = true;
        changed |= drawKey(window, key, compact);
    };
    const auto flagsFor = [](bool open) {
        return open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    };

    /* The packaged-default proof. verify_unsigned_release greps this exact
     * row, and check_app_ui_input's recovery arm reads it after a reload. It
     * reports the DESIRED value from the config, deliberately not the drawn
     * row: the frame-limit control lives inside a collapsed-by-default tree,
     * and a proof of the shipped default must not depend on whether a player
     * (or a passive smoke) happened to open it. */
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedFrameLimit = false;
        const MdkrVideoValue *frameLimit = desired(MDKR_VIDEO_FRAME_LIMIT);
        if (!tracedFrameLimit && frameLimit != nullptr) {
            std::fprintf(stderr,
                         "[app-ui] frame-limit value=%s label=%s restartPending=%d\n",
                         frameLimit->text,
                         optionLabel(MDKR_VIDEO_FRAME_LIMIT, frameLimit->text),
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

    if (mdkr_video_config_is_readonly()) {
        // Explicit --pure locks this session's resolved values. Timing and
        // smoothing are intentionally independent of art-direction presets,
        // so never describe a retained enhanced choice as "original".
        ui::CautionBox(
            "Original session — picture and timing are locked",
            "Frame limit, Motion smoothing, and Simulation cadence keep the values selected "
            "before launch, so a timing comparison stays honest. Sound, window and "
            "controller settings still work.");
        ui::Gap(ui::kGapM);
    }

    /*
     * NINE GROUPS, NAMED FOR WHAT A PLAYER CAME TO DO.
     *
     * There were eleven, and they answered three different questions. Picture,
     * Sound and Camera named a SUBSYSTEM. Accessibility and Gameplay named a
     * player CONCERN. App window named neither -- it was a lid over the window
     * mode, the update check and the developer-tools switch, which have nothing
     * to do with each other or with a window. Camera held exactly one row.
     *
     * Six of the eleven opened expanded, so the first screen was a wall rather
     * than a menu. Two open by default now -- Display, because it is what most
     * players came for, and Accessibility, because someone who needs it needs
     * it before they can use anything below it.
     *
     * See docs/architecture/launcher-design.md for the full mapping. The safety
     * net at the bottom is unchanged and still catches any visible key no group
     * claims, so regrouping cannot silently lose a setting.
     */
    const ImGuiTreeNodeFlags openUnlessScripted =
        flagsFor(!controllerSettingsSmoke && !draggingUiScale);

    // --- Display ------------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Display",
            "Restored is the recommended default: widescreen and sharper "
            "output, with the original art direction untouched.",
            openUnlessScripted, compact)) {
        row(MDKR_VIDEO_MODE);
        // The preset's third state, said out loud. A default session reads
        // "Custom (Individual Settings)" -- a value that is not in the control's
        // own list -- and the page used to leave the player to work out why.
        const MdkrVideoValue *mode = desired(MDKR_VIDEO_MODE);
        if (mode != nullptr && std::strcmp(mode->text, "custom") == 0 &&
            !compact) {
            ui::TextSubtleWrapped(
                "Custom means your Graphics settings below no longer match any "
                "of the three presets. Choosing one here sets them all again.");
            ui::Gap(ui::kGapS);
        }
        if (webGpuRenderer) {
            ui::TextSubtle("Graphics backend: WebGPU (recommended)");
            ui::Gap(ui::kGapS);
        } else if (!compact) {
            if (ui::CardBegin("##renderer-warning", AppTheme::accent(), 0.0f)) {
                ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
                ImGui::TextUnformatted("OpenGL diagnostic renderer active");
                ImGui::PopStyleColor();
                ui::TextSubtleWrapped(
                    "For the qualified visual path, use WebGPU with Restored.");
            }
            ui::CardEnd();
            ui::Gap(ui::kGapS);
        }

        const bool pacingVisible = visible(MDKR_VIDEO_FRAME_LIMIT) &&
                                   visible(MDKR_VIDEO_MOTION_SMOOTHING);
        if (pacingVisible) {
            changed |= drawFrameRate(window, compact, selectingFrameLimit);
            // drawFrameRate owns these three rows, so it owns claiming them:
            // the safety net below asks "did any group draw this key", and a
            // key drawn by a dedicated control is drawn.
            for (MdkrVideoKey key : {MDKR_VIDEO_FRAME_LIMIT,
                                     MDKR_VIDEO_MOTION_SMOOTHING,
                                     MDKR_VIDEO_ALLOW_TEARING}) {
                drawnKey[static_cast<size_t>(key)] = true;
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
        }

        // The Camera group held this one row and nothing else. It changes what
        // you see and not how the game plays, so it belongs here; Camera shake
        // is an access need first and the routing policy keeps it under
        // Accessibility.
        row(MDKR_VIDEO_CAMERA_OBSTRUCTION);
        row(MDKR_VIDEO_GAMEPLAY_FOV);
        row(MDKR_VIDEO_ASPECT);
        row(MDKR_WINDOW_MODE);
        row(MDKR_VIDEO_WIDESCREEN);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Graphics -----------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Graphics",
            "Image quality only. None of these change the art direction.",
            flagsFor(false), compact)) {
        row(MDKR_VIDEO_RENDER_SCALE);
        row(MDKR_VIDEO_MSAA);
        row(MDKR_VIDEO_ANISOTROPY);
        row(MDKR_VIDEO_MIPMAPS);
        row(MDKR_VIDEO_HIRES_TEXT);
        row(MDKR_VIDEO_WORLD_SHADOWS);
        row(MDKR_VIDEO_REMASTER_FX);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Audio --------------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Audio", "Volume levels, remembered as you set them.",
            flagsFor(false), compact)) {
        row(MDKR_AUDIO_MASTER_VOLUME);
        row(MDKR_AUDIO_MUSIC_VOLUME);
        row(MDKR_AUDIO_EFFECTS_VOLUME);
        ImGui::Unindent(ui::kGapM);
    }

    // --- Controls -----------------------------------------------------------
    if (g_controllerSectionRequested) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (drawSettingsSectionHeader(
            "Controls",
            "Rumble, and which N64 button each control presses. The left "
            "stick is always steering.",
            flagsFor(controllerSettingsSmoke), compact)) {
        if (g_controllerSectionRequested) ImGui::SetScrollHereY(0.0f);
        row(MDKR_INPUT_RUMBLE_ENABLED);
        row(MDKR_INPUT_RUMBLE_PROFILE);
        if (visible(MDKR_INPUT_RUMBLE_ENABLED)) controllerRumbleWidgets++;
        if (visible(MDKR_INPUT_RUMBLE_PROFILE)) controllerRumbleWidgets++;
        if (g_controllerSectionRequested) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (ImGui::TreeNodeEx("Button mapping",
                              ImGuiTreeNodeFlags_SpanAvailWidth |
                                  flagsFor(controllerSettingsSmoke))) {
            for (int i = MDKR_INPUT_CONTROLLER_A;
                 i <= MDKR_INPUT_CONTROLLER_RIGHT_STICK_RIGHT; ++i) {
                const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
                if (!visible(key)) continue;
                row(key);
                controllerMappingWidgets++;
            }
            const bool restorePressed = ImGui::Button(
                "Restore defaults", ui::kBtnWide());
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
            ImGui::TreePop();
        }
        ImGui::Unindent(ui::kGapM);
    }
    g_controllerSectionRequested = false;

    // --- Accessibility ------------------------------------------------------
    // Open by default: a player who needs it needs it before they can use
    // anything else on the page. Rows come from the routing policy, so an
    // accessibility key added tomorrow is drawn -- and voiced -- here.
    if (drawSettingsSectionHeader(
            "Accessibility",
            "How the game reads and speaks. None of these change how it "
            "plays.",
            flagsFor(draggingUiScale ||
                     (!controllerSettingsSmoke && !selectingFrameLimit)),
            compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawAccessibilitySection(window, compact, webGpuRenderer,
                                            legacyStretchActive);
    }

    // --- Gameplay -----------------------------------------------------------
    if (drawSettingsSectionHeader(
            "Gameplay",
            "The only settings on this page that change how the game plays.",
            flagsFor(false), compact)) {
        if (visible(MDKR_VIDEO_SIMULATION_CADENCE) && !compact) {
            const MdkrVideoValue *cadence =
                desired(MDKR_VIDEO_SIMULATION_CADENCE);
            const bool enhanced =
                cadence != nullptr &&
                std::strcmp(cadence->text, "enhanced") == 0;
            ui::CautionBox(
                enhanced ? "Enhanced is on — experimental, and races run off pace"
                         : "Enhanced is experimental and changes how the game plays",
                "Enhanced runs DKR's logic at 60 Hz instead of the 30 Hz it "
                "was written for. The boss rematches are winnable, but they "
                "still finish measurably off the pace the game intends, "
                "because the original physics is written around the 30 Hz "
                "step rather than derived from it. Original is exact — "
                "bit for bit the game as it shipped. Frame rate under Display "
                "gives you a smooth 60 FPS picture without changing the game "
                "at all, and is the recommended way to get one.");
            ui::Gap(ui::kGapS);
        }
        row(MDKR_VIDEO_SIMULATION_CADENCE);
        row(MDKR_VIDEO_MENU_LANGUAGES);
        ImGui::Unindent(ui::kGapM);
    }

    // Two sections that are not a schema category. Both gather keys the
    // categories would otherwise scatter -- see AppUi_settingsSection -- and
    // both add something no generated row can: the extras get one action that
    // resets them and nothing else, and the content packs get the list of what
    // the scan actually found.
    if (drawSettingsSectionHeader(
            "Extras",
            "Things you opt into. Each one says whether it changes how the "
            "game plays.",
            ImGuiTreeNodeFlags_None, compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawEnhancementsSection(window, compact, webGpuRenderer,
                                           legacyStretchActive);
    }

    const MdkrModRegistry *packs = platform_content_packs_registry();
    const MdkrVideoConfig *liveConfig = mdkr_video_config_current();
    const char *disabledList = liveConfig != nullptr
        ? liveConfig->values[MDKR_CONTENT_PACK_DISABLED].text : "";
    traceContentPacks(packs, disabledList);
    // Open when the scan found anything at all, including something it could
    // not read. A player who installed a pack has a question this section
    // answers; a player who has never installed one does not, and a collapsed
    // header keeps the panel that player's size.
    const bool anyPacks = mdkr_mod_registry_count(packs) > 0 ||
                          mdkr_mod_registry_skipped(packs) > 0;
    if (drawSettingsSectionHeader(
            "Content packs",
            "Packs that replace artwork or music, from the mods folder.",
            anyPacks ? ImGuiTreeNodeFlags_DefaultOpen
                     : ImGuiTreeNodeFlags_None,
            compact)) {
        ImGui::Unindent(ui::kGapM);  // the section helper manages its own indent
        changed |= drawContentSection(window, compact, packs, disabledList);
    }

    // --- Advanced -----------------------------------------------------------
    // What "App window" used to hold once the window mode moved to Display: the
    // update check and the developer-tools switch, which are diagnostic rather
    // than about a window. The catch-all loop comes with them, so a key like a
    // future Interface option still cannot end up with a schema row, an
    // environment variable, and no control anywhere in the product. The
    // unclaimed-key safety net is folded in here too, so the page has one
    // last-resort home rather than two.
    int unclaimed = 0;
    for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
        const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
        if (drawnKey[i] || !visible(key)) continue;
        if (AppUi_settingsSection(key) != AppUiSettingsSection::Category)
            continue;  // owned by a dedicated section above
        if (mdkr_video_schema(key) == nullptr) continue;
        ++unclaimed;
    }
    if (unclaimed > 0 && drawSettingsSectionHeader(
            "Advanced",
            "Updates, developer tools, and anything without a home above.",
            flagsFor(false), compact)) {
        for (int i = 0; i < MDKR_VIDEO_KEY_COUNT; ++i) {
            const MdkrVideoKey key = static_cast<MdkrVideoKey>(i);
            if (drawnKey[i]) continue;
            row(key);
        }
        ui::Gap(ui::kGapS);
        ImGui::Unindent(ui::kGapM);
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

    /* No third "your changes are staged" line here. The card at the top of the
     * page names the action, and each staged row already says what will run
     * next and what is running now; a footer repeating it made one fact appear
     * three times on one page. */

    if (!g_status.empty()) {
        ui::Gap(ui::kGapS);
        ImGui::PushStyleColor(ImGuiCol_Text, g_statusColor);
        ImGui::TextWrapped("%s", g_status.c_str());
        ImGui::PopStyleColor();
    }

    return changed;
}
