// ui_launcher.cpp — nav rail, panel router, Play button, About.
#include "ui_launcher.h"
#include "app_host.h"
#include "app_launch_hold.h"
#include "app_cleanup_completion.h"
#include "online_teardown_tracker.h"
#include "online/async_work_budget.h"
#include "net/network_lifetime.h"
#include "app_ui_policy.h"
#include "file_dialog.h"
#include "file_browser_ui.h"
#include "pack_install_job.h"
#include <future>
#include <atomic>
#include <memory>

extern "C" {
#include "../user_paths.h"
#include "../platform_os.h"
}
#include <filesystem>
#include <system_error>
#include "app_theme.h"
#include "app_brand.h"
#include "ui_common.h"
#include "ui_online_room.h"
#include "ui_settings.h"
#include "party/libdatachannel_party_transport.h"
#include "party/lan_party_transport.h"
#include "party/lan_party_launch.h"
#include "party/native_party_host.h"
#include "ui_overlay.h"

#include "imgui.h"
#include "SDL.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
bool g_smokeChooseRomFocused = false;
bool g_smokePackButtonFocused = false;
bool g_smokePackPublished = false;
bool g_smokePackFocusRequested = false;


struct Panel {
    const char *label;
    void (*draw)(LauncherState &, LauncherAction &);
};

void drawSettingsPanel(LauncherState &s, LauncherAction &out);
void drawCharacterWorkshopPanel(LauncherState &s, LauncherAction &out);
void drawContentPanel(LauncherState &s, LauncherAction &out);
void servicePackInstall(bool quitting);
void cancelPackInstall();
void finishPackInstallForExit();
void reclaimAbandonedPackStaging();
void drawAboutPanel(LauncherState &s, LauncherAction &out);

const Panel kPanels[] = {
    // Panel 0 is the home. It was "Game ROM": a first-run chore, named after a
    // file format, permanently occupying the destination the launcher opens on
    // and that a returning player never needs again. It is now "Play", and the
    // ROM flow is the onboarding STATE of that home rather than a panel to
    // hunt for. Indices are unchanged -- kLauncherPanelCount and the nav smoke
    // contract pin them.
    {"Play",        RomPanel_draw},
    {"Online Room", OnlineRoomPanel_draw},
    {"Settings",    drawSettingsPanel},
    {"Diagnostics", DiagPanel_draw},
    {"About",       drawAboutPanel},
    {"Character Workshop", drawCharacterWorkshopPanel},
    // The Content hub: what a player adds to the game, in one place. Appended
    // so the Workshop keeps index 5.
    {"Content",     drawContentPanel},
};
constexpr int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
static_assert(kPanelCount == kLauncherPanelCount,
              "launcher panel count must match the public smoke contract");

/*
 * Online Room has three deliberate build states. A native-online-beta build
 * always exposes the live panel; a beta-OFF development preview exposes its
 * fail-closed fake panel only under MDKR_ONLINE_ROOM_PREVIEW=1; and a build
 * with both compile-time gates off omits the surface completely. Thus a
 * packaged 1.7 beta needs no environment variable, while an ordinary local
 * build cannot advertise a mode it does not contain.
 *
 * The panel keeps its INDEX either way, so the smoke-contract arrays and panel
 * routing stay stable and check_launcher_tabs' MDKR_APP_SMOKE_NAV_TARGET=1
 * still names the same destination.
 */
static bool panelVisible(int index) {
    if (index < 0 || index >= kPanelCount) return false;
    if (std::strcmp(kPanels[index].label, "Online Room") != 0) return true;
#if MDKR_ENABLE_ONLINE_ROOM_PREVIEW
#if MDKR_ENABLE_ONLINE_BETA
    // Native online beta: the Online Room panel is always reachable so beta
    // testers set no env; the build gate replaces the MDKR_ONLINE_ROOM_PREVIEW=1
    // preview env. The 1.7 native release workflows define this beta gate.
    return true;
#else
    static const bool preview = [] {
        const char *value = std::getenv("MDKR_ONLINE_ROOM_PREVIEW");
        return value != nullptr && value[0] == '1';
    }();
    return preview;
#endif
#else
    return false;
#endif
}


ImVec2 g_smokeTopTabMin[kPanelCount];
ImVec2 g_smokeTopTabMax[kPanelCount];
bool g_smokeTopTabValid[kPanelCount] = {};
ImVec2 g_smokeSettingsScrollMin;
ImVec2 g_smokeSettingsScrollMax;
float g_smokeSettingsScrollY = 0.0f;
bool g_smokeSettingsScrollValid = false;
ImVec2 g_smokePanelScrollMin;
ImVec2 g_smokePanelScrollMax;
float g_smokePanelScrollY = 0.0f;
bool g_smokePanelScrollValid = false;
bool g_smokePrimaryActionLabelContained = true;
bool g_characterWorkshopReturnFocusRequested = false;

#if MDKR_ENABLE_ONLINE_BETA
// Modal lobby takeover witness (beta only; vanishes in the OFF build, so the
// shipped object stays byte-identical). Each launcher control that must be
// SUPPRESSED during an active online session stamps the current frame index as
// it draws; the probe then reports whether it drew this frame. This is a
// truthful witness: if the takeover ever fell through to the shell, the stamp
// would show the generic Play / nav as drawn and the takeover check would fail.
int g_betaFrame = 0;
int g_betaPlayDrawnFrame = -1;
int g_betaNavDrawnFrame = -1;

void emitLobbyTakeoverProbe(bool onlineActive, bool tookTakeover) {
    static const bool probe =
        std::getenv("MDKR_APP_LOBBY_TAKEOVER_PROBE") != nullptr;
    if (!probe) return;
    std::fprintf(stderr,
                 "[app-lobby-takeover] frame=%d online_active=%d "
                 "took_takeover=%d play_drawn=%d nav_drawn=%d view_kind=%d\n",
                 g_betaFrame, onlineActive ? 1 : 0, tookTakeover ? 1 : 0,
                 g_betaPlayDrawnFrame == g_betaFrame ? 1 : 0,
                 g_betaNavDrawnFrame == g_betaFrame ? 1 : 0,
                 OnlineRoom_lobbyProbeViewKind());
}
#endif  // MDKR_ENABLE_ONLINE_BETA

void fillBootConfig(LauncherState &state, MdkrBootConfig &boot) {
    boot = MdkrBootConfig{};
    boot.rom_path = state.romPath.empty() ? nullptr : state.romPath.c_str();
    // -1: let the engine resolve the mode from the ini the settings panel wrote,
    // rather than the launcher second-guessing it with a preset flag that would
    // re-expand over the individual keys the player just staged.
    boot.video_mode = -1;
    boot.override_count =
        Settings_collectStagedOverrides(boot.overrides, MDKR_BOOT_MAX_OVERRIDES);
    if (!state.characterPreviewPackage.empty() &&
        state.characterPreviewContext != MDKR_CHARACTER_PREVIEW_NONE) {
        boot.character_preview_package =
            state.characterPreviewPackage.c_str();
        boot.character_preview_context = state.characterPreviewContext;
        boot.character_preview_scene = state.characterPreviewScene;
        boot.character_preview_players = state.characterPreviewPlayers;
        boot.character_preview_pose = state.characterPreviewPose;
        boot.character_preview_pose_phase_milli =
            state.characterPreviewPosePhaseMilli;
        boot.character_preview_transition_from_pose =
            state.characterPreviewTransitionFromPose;
        boot.character_preview_transition_from_phase_milli =
            state.characterPreviewTransitionFromPhaseMilli;
        boot.character_preview_view_yaw_degrees =
            state.characterPreviewViewYawDegrees;
        boot.character_preview_view_pitch_degrees =
            state.characterPreviewViewPitchDegrees;
        boot.character_preview_lighting = state.characterPreviewLighting;
        boot.character_preview_capture_png =
            state.characterPreviewCapturePng.empty()
                ? nullptr : state.characterPreviewCapturePng.c_str();
        boot.character_preview_capture_kind =
            state.characterPreviewCaptureKind;
        boot.character_preview_donor_reference =
            state.characterPreviewDonorReference ? 1 : 0;
        boot.character_preview_auto_return =
            state.characterPreviewAutoReturn ? 1 : 0;
        boot.character_preview_studio =
            state.characterPreviewInteractiveStudio ? 1 : 0;
        boot.character_motion_review =
            state.characterPreviewRepresentativeMotionReview ? 1 : 0;
        state.characterPreviewResult = MdkrCharacterPreviewResult{};
        boot.character_preview_result = &state.characterPreviewResult;
        state.characterMotionReviewResult =
            MdkrCharacterMotionReviewResult{};
        boot.character_motion_review_result =
            state.characterPreviewRepresentativeMotionReview
                ? &state.characterMotionReviewResult : nullptr;
    }
}

void selectPanelFromEnvironment(int &activePanel) {
    const char *requested = std::getenv("MDKR_APP_PANEL");
    if (requested == nullptr) return;

    if (requested[0] >= '0' && requested[0] <= '9') {
        const int index = std::atoi(requested);
        if (index >= 0 && index < kPanelCount && panelVisible(index)) {
            activePanel = index;
        }
        return;
    }

    for (int i = 0; i < kPanelCount; ++i) {
        if (!panelVisible(i)) continue;
        if (std::strcmp(kPanels[i].label, requested) == 0) {
            activePanel = i;
            return;
        }
    }
}

bool hasCharacterSourceExtension(const std::string &path) {
    const auto matches = [&path](const char *extension) {
        const size_t extensionLength = std::strlen(extension);
        if (path.size() < extensionLength) return false;
        const size_t offset = path.size() - extensionLength;
        for (size_t index = 0u; index < extensionLength; ++index) {
            const unsigned char actual =
                static_cast<unsigned char>(path[offset + index]);
            if (std::tolower(actual) != extension[index]) return false;
        }
        return true;
    };
    return matches(".mdkrchar") || matches(".mdkrsource") ||
        matches(".glb") || matches(".dae") ||
        matches(".zip") || matches(".gltf") || matches(".fbx") ||
        matches(".obj") || matches(".blend") || matches(".usd") ||
        matches(".usda") || matches(".usdc") || matches(".usdz") ||
        matches(".ma") || matches(".mb") || matches(".max") ||
        matches(".c4d") || matches(".3ds");
}

void acceptDroppedFile(AppHost &host, LauncherState &state, int &activePanel) {
    const std::string dropped = host.takeDroppedFile();
    if (dropped.empty()) return;

    if (hasCharacterSourceExtension(dropped)) {
        (void)Settings_importCharacterPackage(dropped.c_str());
        activePanel = kLauncherPanelCharacterWorkshop;
        return;
    }

    RomPanel_ensureInit(state);
    RomPanel_setRom(state, dropped.c_str());
    activePanel = kLauncherPanelPlay;   // show the validation verdict
}

void preparePlay(LauncherState &state) {
    RomPanel_requestPlayValidation(state);
}

void requestLauncherQuit(LauncherState &state) {
    if (state.quitRequested) return;
    state.quitRequested = true;
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        std::fprintf(
            stderr,
            "[app-ui] character-workshop-quit request=%s\n",
            Settings_characterWorkPending() ? "deferred" : "ready");
    }
}

bool drawLauncherQuitButton(LauncherState &state, const ImVec2 &size) {
    const bool waiting = state.quitRequested &&
                         Settings_characterWorkPending();
    if (waiting) ImGui::BeginDisabled();
    const bool pressed = ImGui::Button(waiting ? "Closing…" : "Quit", size);
    if (waiting) ImGui::EndDisabled();
    ui::SpeakFocusedItem(
        waiting ? "Closing after character work" : "Quit",
        waiting ? "waiting for the current character job" : nullptr,
        waiting
            ? "The launcher remains open until the current transactional character operation publishes safely. Use Keep launcher open in the progress card to cancel the quit request."
            : "Closes the launcher without starting the game. A current character operation finishes visibly before the process exits.");
    if (pressed) requestLauncherQuit(state);
    return pressed;
}

void drawPrimaryLauncherAction(LauncherState &state, const ImVec2 &size,
                               bool workshopActive) {
#if MDKR_ENABLE_ONLINE_BETA
    // The generic offline Play. During an active online session the lobby
    // takeover must never call this; the stamp witnesses that it did not.
    g_betaPlayDrawnFrame = g_betaFrame;
#endif
    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool characterBusy = Settings_characterWorkPending();
    // romPlayAwaitingReplacement: Play was already pressed once and is
    // waiting on a pending replacement check (see RomPanel_requestPlayValidation).
    // Show that immediately so a second press cannot queue a duplicate wait.
    const bool busy = state.quitRequested || characterBusy ||
        (!workshopActive &&
         (state.romPlayValidationPending ||
          state.romPlayAwaitingReplacement ||
          (!ready && state.romValidationPending)));
    const float actionWidth = size.x > 0.0f
        ? size.x : ImGui::GetContentRegionAvail().x;
    SettingsCharacterWorkshopPrimaryAction workshopAction;
    if (workshopActive && !state.quitRequested && !characterBusy) {
        workshopAction = Settings_characterWorkshopPrimaryAction();
    }
    if (characterBusy && std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedCharacterPrimaryGate = false;
        if (!tracedCharacterPrimaryGate) {
            tracedCharacterPrimaryGate = true;
            std::fprintf(
                stderr,
                "[app-ui] character-workshop-lifecycle primary-gated=1 play-gated=1 import-gated=1\n");
        }
    }
    const char *label = "Play";
    if (state.quitRequested) {
        label = "Closing…";
    } else if (characterBusy) {
        label = "Character job running…";
    } else if (workshopActive) {
        const float fullLabelWidth = ImGui::CalcTextSize(
            workshopAction.label).x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        label = actionWidth >= fullLabelWidth
            ? workshopAction.label : workshopAction.compactLabel;
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            static std::string tracedWorkshopPrimary;
            const std::string traceKey =
                std::string(workshopAction.id) +
                "\n" + label;
            if (tracedWorkshopPrimary != traceKey) {
                tracedWorkshopPrimary = traceKey;
                std::fprintf(
                    stderr,
                    "[app-ui] character-workshop-primary kind=%s label=%s tab=%s\n",
                    workshopAction.id, label,
                    workshopAction.destination);
            }
        }
    } else if (busy) {
        label = "Checking ROM…";
    } else if (!ready) {
        label = "Choose ROM";
    } else if (Settings_restartPending()) {
        label = "Play with Changes";
    }

    g_smokePrimaryActionLabelContained =
        ImGui::CalcTextSize(label).x +
            ImGui::GetStyle().FramePadding.x * 2.0f <= actionWidth + 0.5f;

    if (g_characterWorkshopReturnFocusRequested && workshopActive && !busy) {
        ImGui::SetKeyboardFocusHere();
    }
    if (busy) ImGui::BeginDisabled();
    /*
     * Always gold. This is the launcher's persistent primary action -- the one
     * control present on every destination, in the same place, whatever state
     * the app is in.
     *
     * An earlier revision made it secondary whenever the Play home drew its own
     * gold "Choose your game file", reasoning that one screen should hold one
     * primary. That reasoning was right about the Play home and wrong about
     * everywhere else: Settings, Content and About & support draw no gold of
     * their own, so the rule left those destinations with no primary action at
     * all. It also silently disarmed tests/check_launcher_tabs.py, whose
     * primary_action_taper exists to prove THIS control is not clipped by its
     * container and finds it by being the largest gold component in the header.
     * Two golds offering the same action is a much smaller cost than a screen
     * with none and a gate that no longer measures what it was written for.
     */
    const bool pressed = ui::BrandPrimaryButton(label, size);
    g_smokeChooseRomFocused = !ready && !busy && ImGui::IsItemFocused();
    if (busy) ImGui::EndDisabled();
    if (g_characterWorkshopReturnFocusRequested && workshopActive && !busy) {
        g_characterWorkshopReturnFocusRequested = false;
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-workshop-return focus=primary\n");
        }
    }
    // The launcher's single most important control was the one control it never
    // said out loud: the settings rows, the ROM controls and the phone-party
    // buttons all voice on focus, but the persistent Play action did not, so a
    // blind player tabbing onto it heard nothing. Route it through the same
    // choke point. A disabled (busy) button is not focusable, so this no-ops
    // during a check without a special case.
    ui::SpeakFocusedItem(
        label, nullptr,
        workshopActive
              ? workshopAction.description
              : ready ? "Starts the game with your current ROM and settings."
              : "Opens a file picker to choose the game ROM before you can play.");
    if (!pressed) return;

    if (workshopActive) {
        (void)Settings_activateCharacterWorkshopPrimaryAction();
    } else if (ready) {
        preparePlay(state);
    } else {
        // A disabled-looking dead Play button gave first-run players no useful
        // action. Open the native picker where one exists, then show the ROM
        // panel for its validation result or its typed-path/drop alternatives.
        RomPanel_chooseRom(state);
        Launcher_requestTab(state, kLauncherPanelPlay, kLauncherTabPlayer);
    }
}

/*
 * One rail destination.
 *
 * The fill is painted here rather than left to Selectable, because Selectable's
 * own fill is a square, full-bleed rectangle and that slab -- fully saturated
 * cobalt, hard corners, edge to edge -- was the loudest element in the window
 * and the one that most dated the interface. It out-shouted the gold launch
 * action, which is the thing a launcher should be pointing at.
 *
 * What replaces it is the same solid cobalt in a rounded pill with a gold
 * leading rule and the label indented clear of that rule. Keeping the FILL
 * solid is deliberate: tests/check_launcher_tabs.py finds the selected
 * destination by locating exactly one connected #315C98 component above a
 * minimum area, so a low-alpha tint would have looked calmer and left that
 * assertion unable to fail. The geometry carries the redesign; the colour
 * carries the gate.
 *
 * Hover is resolved from the row rectangle before the item is submitted, which
 * is how the rounded fill can be painted UNDER the label. Painting it after
 * would cover the text, and the alternative -- an ImDrawList channel split for
 * one rectangle -- costs more than it explains.
 */
bool drawRailPanelItem(const char *label, bool selected) {
    const float height = ui::kTouchRowHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    const bool hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(min, max);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (selected || hovered) {
        draw->AddRectFilled(
            min, max,
            ImGui::GetColorU32(selected ? AppTheme::navSelected()
                                        : AppTheme::navHover()),
            ui::kNavPillRounding());
    }
    if (selected) {
        draw->AddRectFilled(
            min, ImVec2(min.x + ui::kNavRuleWidth(), max.y),
            ImGui::GetColorU32(AppTheme::accent()),
            ui::kNavRuleWidth() * 0.5f);
    }

    // The fill above is the whole visual state, so the widget contributes only
    // its label and its hit box.
    const ImVec4 clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_Header, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(
        ImGuiCol_HeaderActive,
        selected ? clear : AppTheme::navPressed());
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,
                        ImVec2(0.0f, 0.5f));
    ImGui::Indent(ui::kGapM);
    const bool pressed = ImGui::Selectable(
        label, selected, 0,
        ImVec2((std::max)(1.0f, width - ui::kGapM), height));
    ImGui::Unindent(ui::kGapM);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    // Now that the rail shares the shell's focus scope (see panelChildFlags),
    // Tab lands on these destinations, so they announce like every other
    // control rather than becoming reachable-but-silent.
    ui::SpeakFocusedItem(label, selected ? "current section" : nullptr,
                         "Switches the launcher to this section.");
    return pressed;
}

bool drawTopPanelTab(const char *label, bool selected) {
    // Buttons already carry the style's FrameRounding, so the compact strip
    // needs only the shared colours -- the same three the rail reads, so the
    // two navigation surfaces cannot drift apart.
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? AppTheme::navSelected() : AppTheme::surface());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        selected ? AppTheme::navSelected() : AppTheme::navHover());
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        selected ? AppTheme::navSelectedActive() : AppTheme::navPressed());
    const bool pressed = ImGui::Button(
        label, ImVec2(0.0f, ui::kTouchRowHeight()));
    if (selected) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float inset = 6.0f * AppTheme::uiScale();
        const float thickness = 3.0f * AppTheme::uiScale();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(min.x + inset, max.y - thickness),
            ImVec2(max.x - inset, max.y),
            ImGui::GetColorU32(AppTheme::accent()), thickness * 0.5f);
    }
    ImGui::PopStyleColor(3);
    ui::SpeakFocusedItem(label, selected ? "current section" : nullptr,
                         "Switches the launcher to this section.");
    return pressed;
}

void drawTopPanelTabs(int activePanel, LauncherState &state) {
    // Render from one immutable selection snapshot. A click updates the model
    // for the next frame, so one frame can never paint both the previous and
    // newly clicked destinations as active.
    const int selectedPanel = activePanel;
    int requestedPanel = selectedPanel;
    ImGui::PushID("top-panel-tabs");
    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        ImVec2(8.0f * AppTheme::uiScale(), ImGui::GetStyle().ItemSpacing.y));
    /*
     * Destinations, in the same order and with the same names the rail uses.
     * This strip is the intermediate-width responsive mode, not a second
     * navigation: drawing panels here while the rail drew destinations left the
     * launcher with two different information architectures depending on how
     * wide the window happened to be.
     *
     * The smoke rects stay indexed by PANEL, and every panel resolves to the
     * rect of the destination that OWNS it -- so MDKR_APP_SMOKE_NAV_TARGET=3
     * (Diagnostics) still finds a target, and clicking it lands on About &
     * support, which is where Diagnostics now lives.
     */
    for (int i = 0; i < kPanelCount; ++i) g_smokeTopTabValid[i] = false;
    const AppUiDestination kAll[] = {
        AppUiDestination::Play, AppUiDestination::Content,
        AppUiDestination::Settings, AppUiDestination::Support};
    bool firstTab = true;
    for (AppUiDestination destination : kAll) {
        if (!firstTab) ImGui::SameLine();
        firstTab = false;
        ImGui::PushID(static_cast<int>(destination));
        if (drawTopPanelTab(
                AppUi_destinationLabel(destination),
                AppUi_destinationSelected(destination, selectedPanel))) {
            requestedPanel = AppUi_defaultPanelForDestination(destination);
        }
        const ImVec2 tabMin = ImGui::GetItemRectMin();
        const ImVec2 tabMax = ImGui::GetItemRectMax();
        for (int i = 0; i < kPanelCount; ++i) {
            if (AppUi_destinationForPanel(i) != destination) continue;
            if (i == kLauncherPanelOnlineRoom && !panelVisible(i)) continue;
            g_smokeTopTabMin[i] = tabMin;
            g_smokeTopTabMax[i] = tabMax;
            g_smokeTopTabValid[i] = true;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    if (requestedPanel != selectedPanel) {
        Launcher_requestTab(state, requestedPanel, kLauncherTabPlayer);
    }
    ImGui::Separator();
}

/*
 * Height a just-drawn child region actually needed, in its own coordinates.
 *
 * ImGui advances the layout cursor at full size even where a child clips, so
 * the last item's rectangle measures the requirement whether or not the region
 * was given enough room. `regionTop` is the child's window origin and
 * `contentTop` its first cursor position, which together give the top padding
 * (children without a border have none); the bottom padding mirrors it.
 *
 * Call this immediately after the region's last item and before EndChild().
 */
float measuredRegionHeight(float regionTop, float contentTop) {
    const float padding = contentTop - regionTop;
    return ImGui::GetItemRectMax().y - regionTop + padding;
}

void drawNavigation(int &activePanel, LauncherState &state) {
#if MDKR_ENABLE_ONLINE_BETA
    g_betaNavDrawnFrame = g_betaFrame;   // suppressed by the lobby takeover
#endif
    /*
     * The footer reservation splits the rail, so overstating it steals rows
     * from the destination list rather than from anything the footer owns. It
     * reserved five BODY-font lines for a footer whose text is at most a
     * status line plus a three-line wrapped hint in the SMALL font, which at
     * 1.00x cut the fourth destination ("About") in half for every window
     * height in [620, 672). The seed below is the real composition; from the
     * second frame the value the footer measured for itself replaces it, so an
     * unusually long ROM build string widens the footer instead of being
     * clipped by a constant that could not know about it.
     */
    static float measuredFooterHeight = 0.0f;
    ImGui::BeginChild("##nav", ImVec2(ui::kNavWidth(), 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(AppTheme::fonts().small);
    const float footerTextHeight = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
    ImGui::PopFont();
    const float footerSeed =
        footerTextHeight + ui::kGapS + ImGui::GetStyle().ItemSpacing.y * 3.0f +
        ui::kBtnPrimary().y + ui::kBtnSecondary().y;
    const float footerHeight =
        measuredFooterHeight > 0.0f ? measuredFooterHeight : footerSeed;
    const float bodyHeight =
        (std::max)(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
    ImGui::BeginChild("##nav-body", ImVec2(0, bodyHeight),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ui::BrandWordmark();
    ImGui::PushFont(AppTheme::fonts().small);
    ui::TextSubtle("v%s", AppVersion());
    ui::TextSubtle("Native source port");
    ImGui::PopFont();

    ui::Gap(ui::kGapM);
    ui::BrandRule();
    ui::Gap(ui::kGapM);

    // Destinations, not panels. The panel indices stay exactly where they were
    // (they are a public smoke contract); this list is the layer above them, so
    // the rail can say what a player wants -- Play, Content, Settings -- while
    // six surfaces keep their numbers underneath.
    const AppUiDestination kMainDestinations[] = {AppUiDestination::Play,
                                                  AppUiDestination::Content,
                                                  AppUiDestination::Settings};
    for (AppUiDestination destination : kMainDestinations) {
        if (drawRailPanelItem(
                AppUi_destinationLabel(destination),
                AppUi_destinationSelected(destination, activePanel))) {
            Launcher_requestTab(state,
                                AppUi_defaultPanelForDestination(destination),
                                kLauncherTabPlayer);
        }
        /*
         * Online Room is a way to play, so it is nested under Play rather than
         * standing beside it -- but it must stay REACHABLE while it is nested,
         * which is why it is drawn here and not merely routed. Its three build
         * states are unchanged: panelVisible() still decides whether a build
         * has the surface at all.
         */
        if (destination == AppUiDestination::Play &&
            panelVisible(kLauncherPanelOnlineRoom)) {
            ImGui::Indent(ui::kGapM);
            if (drawRailPanelItem(kPanels[kLauncherPanelOnlineRoom].label,
                                  activePanel == kLauncherPanelOnlineRoom)) {
                Launcher_requestTab(state, kLauncherPanelOnlineRoom,
                                    kLauncherTabPlayer);
            }
            ImGui::Unindent(ui::kGapM);
        }
    }

    // About & support sits under a rule: one click away, never competing with
    // the three destinations a player opened the launcher for.
    ui::Gap(ui::kGapS);
    ui::BrandRule();
    ui::Gap(ui::kGapS);
    if (drawRailPanelItem(
            AppUi_destinationLabel(AppUiDestination::Support),
            AppUi_destinationSelected(AppUiDestination::Support,
                                      activePanel))) {
        Launcher_requestTab(
            state, AppUi_defaultPanelForDestination(AppUiDestination::Support),
            kLauncherTabPlayer);
    }

    ImGui::EndChild();

    // This measured footer is a separate non-scrolling region. Readiness and
    // Play therefore stay put while another panel scrolls independently.
    ImGui::BeginChild("##nav-footer", ImVec2(0, 0),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const float footerTop = ImGui::GetWindowPos().y;
    const float footerContentTop = ImGui::GetCursorScreenPos().y;

    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool checking = !ready && state.romValidationPending;
    const bool workshopWithoutRom = !ready && !checking &&
        activePanel == kLauncherPanelCharacterWorkshop;
    const char *status = workshopWithoutRom ? "Workshop ready"
                                            : "ROM required";
    if (checking) {
        status = "Checking ROM…";
    } else if (ready && state.romInfo.integrity_verified) {
        status = "ROM verified";
    } else if (ready) {
        status = "ROM ready";
    }
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ready ? AppTheme::good()
                                : (checking || workshopWithoutRom)
                                      ? AppTheme::accent()
                                           : AppTheme::subtle());
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    if (ready && state.romInfo.build[0]) {
        ui::TextSubtleWrapped(
            "%s • %s", state.romInfo.build,
            state.romPersistenceWarning[0]
                ? "path not remembered"
                : state.romInfo.integrity_verified
                ? "full-image integrity checked"
                : "modified-ROM developer override active");
    } else if (checking) {
        ui::TextSubtleWrapped("Verifying the complete 12 MB image.");
    } else if (workshopWithoutRom) {
        ui::TextSubtleWrapped(
            "Import and author now; choose a ROM only to test or play.");
    } else {
        ui::TextSubtleWrapped("Choose your own US 1.1 or EU 1.1 ROM.");
    }
    ImGui::PopFont();
    ui::Gap(ui::kGapS);

    drawPrimaryLauncherAction(
        state, ImVec2(-1, ui::kBtnPrimary().y),
        activePanel == kLauncherPanelCharacterWorkshop);

    drawLauncherQuitButton(state, ui::kBtnFullWidth());
    measuredFooterHeight = measuredRegionHeight(footerTop, footerContentTop);
    ImGui::EndChild();
    ImGui::EndChild();
}

void drawTopNavigation(int &activePanel, LauncherState &state) {
#if MDKR_ENABLE_ONLINE_BETA
    g_betaNavDrawnFrame = g_betaFrame;   // suppressed by the lobby takeover
#endif
    const float scale = AppTheme::uiScale();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool dense = availableWidth < 720.0f * scale;
    const float denseFirstRowHeight =
        (std::max)(ImGui::GetFrameHeight(), ui::kBtnSecondary().y);
    const float denseHeight = ImGui::GetStyle().WindowPadding.y * 2.0f +
                              denseFirstRowHeight +
                              ImGui::GetStyle().ItemSpacing.y +
                              ui::kBtnPrimary().y;
    /*
     * The non-dense header owns the wordmark, brand rule, 44 px tabs, a
     * separator, status, and the 48 px primary action. Deriving that from live
     * metrics is the same discipline the dense branch above already uses. The
     * constant it replaces (196 * scale) sat below both the content, which ends
     * at 201 at 1.00x, and the 217 that content plus the bordered child's own
     * padding needs: the gold primary action lost its lower edge and its bottom
     * corner radius at exactly the 800x600 layout check_launcher_tabs asserts
     * against. As in the nav rail, the header's own measurement takes over from
     * the second frame, so a font or style change moves the height with it
     * instead of re-opening the same clipping bug.
     */
    static float measuredWideHeight = 0.0f;
    ImGui::PushFont(AppTheme::fonts().title);
    const float wordmarkHeight = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    // Five rows and the four gaps between them. The wordmark shares its row
    // with Quit, so that row is the taller of the two exactly as the dense
    // branch's denseFirstRowHeight is; a horizontal Separator adds
    // style.SeparatorSize plus the ordinary spacing on each side.
    const float wideFirstRowHeight =
        (std::max)(wordmarkHeight, ui::kBtnSecondary().y);
    const float wideSeed =
        ImGui::GetStyle().WindowPadding.y * 2.0f + wideFirstRowHeight +
        ui::kBrandRuleHeight() + ui::kTouchRowHeight() +
        (std::max)(ImGui::GetStyle().SeparatorSize, 1.0f) +
        ui::kBtnPrimary().y + ImGui::GetStyle().ItemSpacing.y * 4.0f;
    const float wideHeight =
        measuredWideHeight > 0.0f ? measuredWideHeight : wideSeed;
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##topnav",
                      ImVec2(0, dense ? denseHeight : wideHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    const float navTop = ImGui::GetWindowPos().y;
    const float navContentTop = ImGui::GetCursorScreenPos().y;
    ImVec2 sectionMin, sectionMax, quitMin, quitMax;
    ImVec2 statusMin, statusMax, playMin, playMax;
    const float quitWidth = dense ? 96.0f * scale : 92.0f * scale;
    if (dense) {
        ImGui::SetNextItemWidth(
            (std::max)(1.0f, ImGui::GetContentRegionAvail().x - quitWidth -
                                 ImGui::GetStyle().ItemSpacing.x));
        // drawActivePanel range-checks the same index before dispatching; this
        // preview label is the only other place it is dereferenced, so it
        // carries the identical guard rather than trusting the caller.
        // Name the destination, so the closed combo and the list agree.
        const char *activeLabel =
            AppUi_destinationLabel(AppUi_destinationForPanel(activePanel));
        if (ImGui::BeginCombo("##compact-section", activeLabel)) {
            // The same four destinations the rail and the tab strip offer. A
            // narrow window is a smaller screen, not a different product.
            for (AppUiDestination destination :
                 {AppUiDestination::Play, AppUiDestination::Content,
                  AppUiDestination::Settings, AppUiDestination::Support}) {
                const bool selected =
                    AppUi_destinationSelected(destination, activePanel);
                if (ImGui::Selectable(
                        AppUi_destinationLabel(destination), selected, 0,
                        ImVec2(0.0f, ui::kTouchRowHeight()))) {
                    Launcher_requestTab(
                        state, AppUi_defaultPanelForDestination(destination),
                        kLauncherTabPlayer);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        sectionMin = ImGui::GetItemRectMin();
        sectionMax = ImGui::GetItemRectMax();
        const char *spokenLabel =
            AppUi_destinationLabel(AppUi_destinationForPanel(activePanel));
        ui::SpeakFocusedItem("Section", spokenLabel,
                             "Choose which launcher section to view.");
        ImGui::SameLine();
        drawLauncherQuitButton(
            state, ImVec2(quitWidth, ui::kBtnSecondary().y));
        quitMin = ImGui::GetItemRectMin();
        quitMax = ImGui::GetItemRectMax();
    } else {
        ui::BrandWordmark();
        ImGui::SameLine();
        ImGui::PushFont(AppTheme::fonts().small);
        ui::TextSubtle("v%s", AppVersion());
        ImGui::PopFont();

        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - quitWidth);
        drawLauncherQuitButton(
            state, ImVec2(quitWidth, ui::kBtnSecondary().y));

        ui::BrandRule();
        drawTopPanelTabs(activePanel, state);
    }

    // At compact widths the side rail is gone, but readiness and the primary
    // action remain available on every section.
    const bool ready = !state.romPath.empty() && state.romInfo.valid;
    const bool checking = !ready && state.romValidationPending;
    const bool workshopWithoutRom = !ready && !checking &&
        activePanel == kLauncherPanelCharacterWorkshop;
    const char *status = workshopWithoutRom ? "Workshop ready"
                                            : "ROM required";
    if (checking) {
        status = "Checking ROM…";
    } else if (ready && dense) {
        status = "ROM ready";
    } else if (ready && state.romPersistenceWarning[0]) {
        status = "ROM ready • path not remembered";
    } else if (ready && state.romInfo.integrity_verified) {
        status = "ROM verified";
    } else if (ready) {
        status = "ROM ready";
    }
    ImGui::PushFont(AppTheme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ready ? AppTheme::good()
                                : (checking || workshopWithoutRom)
                                      ? AppTheme::accent()
                                           : AppTheme::subtle());
    ImGui::TextUnformatted(status);
    statusMin = ImGui::GetItemRectMin();
    statusMax = ImGui::GetItemRectMax();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    const float playWidth = dense
        ? (std::min)(190.0f * scale, ImGui::GetContentRegionAvail().x * 0.62f)
        : 190.0f * scale;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - playWidth);
    drawPrimaryLauncherAction(
        state, ImVec2(playWidth, ui::kBtnPrimary().y),
        activePanel == kLauncherPanelCharacterWorkshop);
    playMin = ImGui::GetItemRectMin();
    playMax = ImGui::GetItemRectMax();
    // The primary action is the header's last and lowest item in both branches,
    // so this is the whole region's requirement.
    if (!dense) {
        measuredWideHeight = measuredRegionHeight(navTop, navContentTop);
    }
    const bool traceLayout = std::getenv("MDKR_APP_UI_TRACE") != nullptr;
    if (traceLayout) {
        static bool tracedHeaderHeight = false;
        if (!tracedHeaderHeight) {
            std::fprintf(stderr,
                         "[app-ui] header dense=%d height=%.1f seed=%.1f "
                         "measured=%.1f playBottom=%.1f navBottom=%.1f\n",
                         dense ? 1 : 0, (double)(dense ? denseHeight : wideHeight),
                         (double)wideSeed, (double)measuredWideHeight,
                         (double)(playMax.y - navTop),
                         (double)ImGui::GetWindowSize().y);
            tracedHeaderHeight = true;
        }
    }
    const bool traceDenseLayout = dense && traceLayout;
    bool denseOverlap = false;
    bool denseControlsContained = false;
    if (traceDenseLayout) {
        const auto overlaps = [](const ImVec2 &aMin, const ImVec2 &aMax,
                                 const ImVec2 &bMin, const ImVec2 &bMax) {
            return aMin.x < bMax.x && aMax.x > bMin.x &&
                   aMin.y < bMax.y && aMax.y > bMin.y;
        };
        const ImVec2 windowMin = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowMax(windowMin.x + windowSize.x,
                               windowMin.y + windowSize.y);
        const ImVec2 mins[] = {sectionMin, quitMin, statusMin, playMin};
        const ImVec2 maxs[] = {sectionMax, quitMax, statusMax, playMax};
        denseControlsContained = true;
        for (int first = 0; first < 4; ++first) {
            denseControlsContained = denseControlsContained &&
                mins[first].x >= windowMin.x &&
                mins[first].y >= windowMin.y &&
                maxs[first].x <= windowMax.x &&
                maxs[first].y <= windowMax.y;
            for (int second = first + 1; second < 4; ++second) {
                denseOverlap = denseOverlap ||
                    overlaps(mins[first], maxs[first],
                             mins[second], maxs[second]);
            }
        }
    }
    ImGui::EndChild();
    if (traceDenseLayout) {
        static bool tracedDenseLayout = false;
        if (!tracedDenseLayout) {
            const float contentStartY = ImGui::GetCursorScreenPos().y;
            const float navBottomY = ImGui::GetItemRectMax().y;
            const bool contentSeparated = contentStartY >= navBottomY;
            std::fprintf(stderr,
                         "[app-ui] compact-layout dense=1 contained=%d "
                         "overlap=%d contentSeparated=%d "
                         "primaryLabelContained=%d\n",
                         denseControlsContained ? 1 : 0,
                         denseOverlap ? 1 : 0,
                         contentSeparated ? 1 : 0,
                         g_smokePrimaryActionLabelContained ? 1 : 0);
            tracedDenseLayout = true;
        }
    }
}

// Nav can cross into a child window only when the child shares the parent's
// focus scope. The launcher is meant to be ONE keyboard/gamepad surface -- Tab
// off the nav rail, through the panel, onto the persistent Play and Quit
// actions, with no scope you cannot leave -- so every launcher child is
// flattened into the root window's scope. This used to be armed only for the
// scripted accessibility walk, which meant the walk could Tab everywhere but an
// ordinary keyboard player could not reach Play at all: the shell and its panel
// were separate scopes and Tab stopped at the boundary. Flattening always is
// what makes "you can reach Play with the keyboard" true off the test bench.
ImGuiChildFlags panelChildFlags(ImGuiChildFlags flags) {
    return flags | ImGuiChildFlags_NavFlattened;
}

// Last panel announced. Guarding on the CHANGE, not on ui::SpeakSection's own
// repeat filter, is what keeps this quiet: the panel draws every frame while
// the settings headers announce themselves as the keyboard passes over them,
// so an unguarded call here and a header call there would take turns being
// "new" and the shell would talk without pause.
int g_spokenPanel = -1;

void drawActivePanel(int activePanel, LauncherState &state, LauncherAction &action) {
    // Which panel you are in, said once per arrival. The launcher's counterpart
    // to a settings section header; both go through the one announcement point
    // in ui_common.cpp.
    if (activePanel != g_spokenPanel && activePanel >= 0 &&
        activePanel < kPanelCount) {
        g_spokenPanel = activePanel;
        ui::SpeakSection(kPanels[activePanel].label);
    }
    const bool workshopActive =
        activePanel == kLauncherPanelCharacterWorkshop;
    if (!workshopActive || Settings_characterWorkPending()) {
        g_characterWorkshopReturnFocusRequested = false;
    }
    const ImGuiInputFlags returnShortcutFlags =
        ImGuiInputFlags_RouteGlobal |
        ImGuiInputFlags_RouteOverFocused |
        ImGuiInputFlags_RouteUnlessBgFocused;
    if (workshopActive && !Settings_characterWorkPending() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::Shortcut(ImGuiKey_Escape, returnShortcutFlags) ||
         ImGui::Shortcut(ImGuiKey_GamepadFaceRight,
                         returnShortcutFlags))) {
        g_characterWorkshopReturnFocusRequested = true;
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(
                stderr,
                "[app-ui] character-workshop-return shortcut=1 mutation=0\n");
        }
    }
    ImGui::BeginChild("##content", ImVec2(0, 0), panelChildFlags(0));
    if (state.quitRequested && Settings_characterWorkPending()) {
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            static bool tracedCharacterQuitProgress = false;
            if (!tracedCharacterQuitProgress) {
                tracedCharacterQuitProgress = true;
                std::fprintf(
                    stderr,
                    "[app-ui] character-workshop-quit progress-visible=1 cancel-visible=1\n");
            }
        }
        if (ui::CardBegin("##character-work-quit", AppTheme::accent(), 0.0f)) {
            ImGui::PushFont(AppTheme::fonts().section);
            ImGui::TextUnformatted("Finishing character work before closing");
            ImGui::PopFont();
            ui::TextSubtleWrapped(
                "The launcher is still responsive. The current bounded, transactional operation will publish its complete result, then Golden Balloon will close automatically; the installed last-known-good character remains usable throughout.");
            if (ImGui::Button("Keep launcher open")) {
                state.quitRequested = false;
                if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
                    std::fprintf(
                        stderr,
                        "[app-ui] character-workshop-quit cancelled=1\n");
                }
            }
            ui::SpeakFocusedItem(
                "Keep launcher open", nullptr,
                "Cancels only the pending quit request. The current character operation continues and no source, draft, or installed character is changed by this button.");
        }
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }
    if (state.bootErrorVisible) {
        if (ui::CardBegin("##boot-recovery", AppTheme::bad(), 0.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ImGui::PushFont(AppTheme::fonts().title);
            ImGui::TextUnformatted("The Game Did Not Start");
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", state.bootError);
            ui::Gap(ui::kGapS);
            const AppUiButtonPairLayout actions = AppUi_fitButtonPair(
                ImGui::GetContentRegionAvail().x,
                ImGui::GetStyle().ItemSpacing.x,
                ui::kBtnSecondary().x, ui::kBtnSecondary().x,
                ui::kPairMinWidth());
            const ImVec2 first(actions.firstWidth, ui::kBtnSecondary().y);
            const ImVec2 second(actions.secondWidth, ui::kBtnSecondary().y);
            if (ImGui::Button("View Diagnostics", first)) {
                Launcher_requestTab(state, kLauncherPanelDiagnostics,
                                    kLauncherTabPlayer);
            }
            if (actions.sameLine) ImGui::SameLine();
            if (ImGui::Button("Dismiss", second)) {
                state.bootErrorVisible = false;
            }
        }
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }
    if (activePanel >= 0 && activePanel < kPanelCount) {
        kPanels[activePanel].draw(state, action);
    }
    ui::TouchScrollCurrentWindow();
    g_smokePanelScrollMin = ImGui::GetWindowPos();
    const ImVec2 panelScrollSize = ImGui::GetWindowSize();
    g_smokePanelScrollMax = ImVec2(
        g_smokePanelScrollMin.x + panelScrollSize.x,
        g_smokePanelScrollMin.y + panelScrollSize.y);
    g_smokePanelScrollY = ImGui::GetScrollY();
    g_smokePanelScrollValid = ImGui::GetScrollMaxY() > 0.0f;
    ImGui::EndChild();
}

#if MDKR_ENABLE_ONLINE_BETA
// The modal online lobby. Owns the whole launcher window: a persistent header
// (title, live status and the single Leave Room exit) over the state-driven
// room body. The body is drawn through the SAME panel path the
// normal router uses, so the online-room controls keep their ImGui IDs -- and
// therefore their keyboard/gamepad focus -- across the shell->takeover
// transition. The nav rail, top tabs and generic offline Play are simply never
// drawn here, which is what makes the offline launch unreachable during a
// session.
void drawLobbyTakeover(LauncherState &state, LauncherAction &action) {
    const float scale = AppTheme::uiScale();
    OnlineLobbyHeaderInfo info{};
    const bool haveInfo = OnlineRoom_lobbyHeaderInfo(&info);

    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::brandSky());
    ImGui::TextUnformatted("Private Online Room");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    const float leaveWidth = 160.0f * scale;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - leaveWidth);
    // Every live-room exit shares one name; the body's cancel is also "Leave
    // Room", and there may be no race yet (this shows from the first CONNECTING
    // frame), so "Leave Race" overclaimed.
    if (ImGui::Button("Leave Room", ImVec2(leaveWidth, ui::kBtnSecondary().y))) {
        OnlineRoom_requestLeave();
    }
    ui::SpeakFocusedItem(
        "Leave Room", "Exit online",
        "Leaves the private room, closes the connection and returns to the "
        "launcher home.");

    if (haveInfo) {
        ImGui::PushFont(AppTheme::fonts().small);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::subtle());
        // The composed, lobby- and reentry-aware line from the room panel; it
        // agrees with the body below. Per-player "ready" chips were dropped: the
        // model reports only aggregate counts, so the projection could crown the
        // wrong player Ready, it duplicated the roster strip, and readiness is a
        // game-owned concept after the takeover.
        ImGui::TextUnformatted(info.statusLine != nullptr ? info.statusLine
                                                          : "Online race");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ui::BrandRule();

    drawActivePanel(kLauncherPanelOnlineRoom, state, action);

    // Deferred, non-blocking teardown once the body has finished drawing.
    OnlineRoom_serviceLobbyLeave(state);
}
#endif  // MDKR_ENABLE_ONLINE_BETA

}  // namespace

struct LauncherNetworkShutdown {
    struct PartyOwners {
        // Destruction is reversed: the host calls its still-owned transport.
        std::unique_ptr<MdkrPartyTransport> transport;
        std::unique_ptr<MdkrNativePartyHost> host;
    };
    OnlineRoomTeardownTracker retiring;
    AppCleanupCompletion completion;
    bool started = false;
    bool finished = false;
    bool failureReported = false;
};

Launcher::Launcher()
    : networkShutdown_(std::make_unique<LauncherNetworkShutdown>()) {
    reclaimAbandonedPackStaging();
    /* The shipped default is the cloud transport (the compiled https Party
     * service). Task 5 wires a UI toggle to selectPartyTransport(Lan) for
     * zero-internet local play; routing both factories through this one seam is
     * also what links the LAN transport surface into the binary. */
    selectPartyTransport(PartyTransportKind::Cloud);
}

void Launcher::selectPartyTransport(PartyTransportKind kind,
                                   MdkrLanPartyTransportConfig config) {
    if (networkShutdown_->started) return;
    /* Runtime mutual-exclusion: exactly one live party transport. Tear the
     * current host down first -- ~MdkrNativePartyHost tells the phones goodbye
     * and shuts its transport down -- then release the transport before
     * building the next, so a cloud and a LAN transport can never both be live.
     * The assert pins the invariant the seam exists to guarantee. */
    state_.phoneParty = nullptr;
    Overlay_setPhonePartyHost(nullptr);
    phoneParty_.reset();
    partyTransport_.reset();
    assert(!partyTransport_ && !phoneParty_);
    switch (kind) {
        case PartyTransportKind::Cloud:
            partyTransport_ = mdkr_create_native_party_transport();
            break;
        case PartyTransportKind::Lan:
            partyTransport_ = mdkr_create_lan_party_transport(std::move(config));
            break;
    }
    phoneParty_ = std::make_unique<MdkrNativePartyHost>(*partyTransport_);
    state_.phoneParty = phoneParty_.get();
    partyKind_ = kind;
    /* Keep the in-game overlay's cached host pointer valid across a switch: the
     * pointer was published once at startup, and a rebuild would leave it
     * dangling otherwise. Harmless at construction (main_app republishes the
     * same value right after). */
    Overlay_setPhonePartyHost(state_.phoneParty);
}

void Launcher::refreshLanControls() {
    /* Availability gates on BOTH a reachable LAN host AND the controller assets
     * actually resolving -- never a live "Start" button that would fail only
     * after the click in a build whose assets were not staged. Both are cheap
     * (a getifaddrs pick and a handful of small file reads), but rechecked only
     * about once a second so joining Wi-Fi or mounting the bundle lights up the
     * card without paying for it every frame. */
    const uint64_t now = static_cast<uint64_t>(SDL_GetTicks64());
    if (!lanChecked_ || now - lanCheckedMs_ >= 1000u) {
        lanChecked_ = true;
        lanCheckedMs_ = now;
        const std::string host = mdkr_lan_party_advertised_host();
        lanHostReachable_ = !host.empty();
        lanAvailable_ = mdkr_lan_party_can_start(host, mdkr_lan_party_web_root());
    }
    state_.lanParty.active = (partyKind_ == PartyTransportKind::Lan);
    state_.lanParty.available = lanAvailable_;
    /* Honest reason for the disabled card: no network vs. assets not in this
     * build (the two shared constants). */
    state_.lanParty.unavailableReason = lanAvailable_ ? nullptr
        : (lanHostReachable_ ? kMdkrLanPartyNoAssetsReason
                             : kMdkrLanPartyNoNetworkReason);
    state_.lanParty.note = lanNote_.empty() ? nullptr : lanNote_.c_str();
}

void Launcher::applyLanStart() {
    MdkrLanPartyTransportConfig config;
    std::string reason;
    if (!mdkr_lan_party_build_launch_config(config, reason)) {
        lanNote_ = reason;   /* stays on the default transport; card shows why */
        return;
    }
    lanNote_.clear();
    selectPartyTransport(PartyTransportKind::Lan, std::move(config));
    if (!phoneParty_->open("lan")) {
        /* Bind or bring-up failed: fall back to the default transport so the
         * card's cloud/entry surface returns rather than a dead LAN surface. */
        lanNote_ = "Local play could not start. Keyboard and gamepads still work.";
        selectPartyTransport(PartyTransportKind::Cloud);
    }
}

void Launcher::applyLanStop() {
    /* The card already told the phones goodbye (closeRoom). Rebuilding the
     * default transport releases the LAN port and returns the cloud/entry
     * surface; the rebuild's destructor is an idempotent second goodbye. */
    lanNote_.clear();
    selectPartyTransport(PartyTransportKind::Cloud);
}

Launcher::~Launcher() {
    finishPackInstallForExit();
    // Explicit terminal paths run this before host/log shutdown. Also cover
    // an early scope exit: never let a retirement worker outlive its tracker.
    if (!networkShutdown_->started) finishCharacterWorkForExit();
    (void)finishNetworkShutdownForExit();
    /* Backstop for the hold-sampling window. Every ordinary exit already
     * releases (dispatch, disarm, any published boot); this covers a launcher
     * torn down before one of those happened -- a quit from the ROM-less first
     * run, say -- so the borrowed pads never outlive the object watching them. */
    AppLaunchHold_release();
}

void Launcher::beginNetworkShutdown() {
    auto &shutdown = *networkShutdown_;
    if (shutdown.started) return;
    shutdown.started = true;
    cancelPackInstall();
#if MDKR_ENABLE_ONLINE_BETA
    OnlineRoom_beginAppExit();
#endif
    // Retract every borrowed alias before the host can die off-thread. No
    // normal panel service or engine dispatch is permitted after this point.
    state_.phoneParty = nullptr;
    Overlay_setPhonePartyHost(nullptr);
    std::unique_ptr<LauncherNetworkShutdown::PartyOwners> owners;
    try {
        // Allocate before moving ownership; failure retains both local owners.
        owners = std::make_unique<LauncherNetworkShutdown::PartyOwners>();
    } catch (...) {
        std::fprintf(stderr, "[app-network] retirement allocation failed; "
                             "closing phone connections synchronously\n");
        phoneParty_.reset();
        partyTransport_.reset();
        return;
    }
    owners->transport = std::move(partyTransport_);
    owners->host = std::move(phoneParty_);
    if (!shutdown.retiring.retire(std::move(owners))) {
        std::fprintf(stderr, "[app-network] retirement scheduling failed; "
                             "phone connections closed synchronously\n");
    }
}

bool Launcher::pollNetworkShutdown() {
    auto &shutdown = *networkShutdown_;
    if (shutdown.finished) return true;
    if (!shutdown.started) return false;
    // Poll both groups even while either is pending, so completed workers are
    // reaped independently. Cleanup is process-wide, never per-room.
    const bool partyRetired = shutdown.retiring.pollReady();
#if MDKR_ENABLE_ONLINE_BETA
    const bool roomRetired = OnlineRoom_pollAppExit();
#else
    const bool roomRetired = true;
#endif
    // A cancelled client can leave a self-owned OS lookup finishing in the
    // background. Wait for its results to be released before RTC tears down
    // shared socket-library state (including Winsock). No owner may start new
    // lookups once both retirement groups have completed.
    const bool resolverRetired = onlineResolverWorkInUse() == 0u;
    const bool finished = shutdown.completion.poll(
        partyRetired && roomRetired && resolverRetired, mdkr_native_party_cleanup);
    shutdown.finished = finished;
    if (finished && networkShutdownFailed() && !shutdown.failureReported) {
        shutdown.failureReported = true;
        if (mdkrFirstPartyNetworkCleanupFailed.load()) {
            std::fprintf(stderr, "[app-network] first-party socket-library cleanup FAILED\n");
        }
        if (shutdown.completion.failed()) {
            try {
                std::rethrow_exception(shutdown.completion.error());
            } catch (const std::exception &error) {
                std::fprintf(stderr, "[app-network] global cleanup FAILED: %s\n",
                             error.what());
            } catch (...) {
                std::fprintf(stderr, "[app-network] global cleanup FAILED: "
                                     "unknown exception\n");
            }
        }
    }
    return finished;
}

bool Launcher::networkShutdownFailed() const {
    return networkShutdown_->completion.failed() || mdkrFirstPartyNetworkCleanupFailed.load();
}

bool Launcher::finishNetworkShutdownForExit() {
    // Explicit terminal paths already finished before host/log shutdown. The
    // destructor backstop must not revisit global owners after that boundary.
    if (networkShutdown_->finished) return !networkShutdownFailed();
    beginNetworkShutdown();
#if MDKR_ENABLE_ONLINE_BETA
    OnlineRoom_shutdownForAppExit();
#endif
    networkShutdown_->retiring.drain(std::chrono::seconds(10), []() noexcept {
        std::fprintf(stderr, "[app-network] phone connections still closing; "
                             "waiting for owned workers\n");
    });
    // Exceptional exits cannot render. Never detach or infer completion from
    // elapsed time; an OS resolver may still keep library cleanup pending.
    const auto started = std::chrono::steady_clock::now();
    bool delayed = false;
    while (!pollNetworkShutdown()) {
        if (!delayed && std::chrono::steady_clock::now() - started >=
                            std::chrono::seconds(10)) {
            delayed = true;
            std::fprintf(stderr, "[app-network] global cleanup still pending; "
                                 "waiting for completion\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !networkShutdownFailed();
}

void Launcher::requestQuit() {
    requestLauncherQuit(state_);
}

bool Launcher::quitReady() const {
    // A returned preview publishes through serviceCharacterWork() before it can launch its
    // follow-up transaction. A queued OS close must not skip that publication
    // merely because no background worker has been started yet.
    return state_.quitRequested && !state_.characterPreviewDispatched &&
           !Settings_characterWorkPending();
}

bool Launcher::quitRequested() const { return state_.quitRequested; }

void Launcher::drawOnlineClosing(bool delayed) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##launcher-online-closing", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    const char *title = delayed ? "Still closing online connections…"
                                : "Closing online connections…";
    ui::SpeakSection(title);
    if (ui::CardBegin("##online-close-progress", AppTheme::accent(), 0.0f)) {
        ImGui::PushFont(AppTheme::fonts().section);
        ImGui::TextWrapped("%s", title);
        ImGui::PopFont();
        ui::TextSubtleWrapped(
            "Golden Balloon will close automatically when online cleanup finishes. "
            "No new race or room will start while closing.");
        ui::Gap(ui::kGapM);
        ui::TextSubtleWrapped(delayed
            ? "The connection is taking longer to close. The window remains responsive; "
              "cleanup is still in progress. There is no reliable time estimate."
            : "Finishing the room and releasing its connections. You can move or "
              "minimize this window while you wait.");
    }
    ui::CardEnd();
    ImGui::End();
}

void Launcher_requestTab(LauncherState &s, int panel, int priority) {
    if (panel < 0 || panel >= kPanelCount) return;
    // Equal priority keeps last-writer-wins, which is what makes a second click
    // in the same frame supersede the first. A lower priority may not displace
    // a request already staged: that is the whole point of the ordering.
    if (s.requestTab >= 0 && priority < s.requestTabPriority) return;
    s.requestTab = panel;
    s.requestTabPriority = priority;
}

bool Launcher_smokeChooseRomFocused() { return g_smokeChooseRomFocused; }
bool Launcher_smokeTopTabCenter(int panel, int *x, int *y) {
    if (!x || !y || panel < 0 || panel >= kPanelCount ||
        !g_smokeTopTabValid[panel]) {
        return false;
    }
    *x = static_cast<int>(
        (g_smokeTopTabMin[panel].x + g_smokeTopTabMax[panel].x) * 0.5f);
    *y = static_cast<int>(
        (g_smokeTopTabMin[panel].y + g_smokeTopTabMax[panel].y) * 0.5f);
    return true;
}

bool Launcher_smokeSettingsScrollRect(int *minX, int *minY,
                                      int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY ||
        !g_smokeSettingsScrollValid) {
        return false;
    }
    *minX = static_cast<int>(g_smokeSettingsScrollMin.x);
    *minY = static_cast<int>(g_smokeSettingsScrollMin.y);
    *maxX = static_cast<int>(g_smokeSettingsScrollMax.x);
    *maxY = static_cast<int>(g_smokeSettingsScrollMax.y);
    return true;
}

float Launcher_smokeSettingsScrollY() {
    return g_smokeSettingsScrollY;
}

bool Launcher_smokePanelScrollRect(int *minX, int *minY,
                                   int *maxX, int *maxY) {
    if (!minX || !minY || !maxX || !maxY) return false;
    const bool settings = g_spokenPanel == kLauncherPanelSettings;
    const bool valid = settings ? g_smokeSettingsScrollValid
                                : g_smokePanelScrollValid;
    if (!valid) return false;
    const ImVec2 &minimum = settings ? g_smokeSettingsScrollMin
                                    : g_smokePanelScrollMin;
    const ImVec2 &maximum = settings ? g_smokeSettingsScrollMax
                                    : g_smokePanelScrollMax;
    *minX = static_cast<int>(minimum.x);
    *minY = static_cast<int>(minimum.y);
    *maxX = static_cast<int>(maximum.x);
    *maxY = static_cast<int>(maximum.y);
    return true;
}

float Launcher_smokePanelScrollY() {
    return g_spokenPanel == kLauncherPanelSettings
        ? g_smokeSettingsScrollY : g_smokePanelScrollY;
}

namespace {

void acceptCharacterPreviewRequest(
    LauncherState &state, SettingsCharacterPreviewRequest preview) {
    state.characterPreviewPackage = std::move(preview.packageId);
    state.characterPreviewSourceSha256 = std::move(preview.sourceSha256);
    state.characterPreviewFitSha256 = std::move(preview.fitSha256);
    state.characterPreviewPresentationSha256 =
        std::move(preview.presentationSha256);
    state.characterPreviewContext = preview.context;
    state.characterPreviewScene = preview.scene;
    state.characterPreviewPlayers = preview.players;
    state.characterPreviewPose = preview.pose;
    state.characterPreviewPosePhaseMilli = preview.posePhaseMilli;
    state.characterPreviewTransitionFromPose = preview.transitionFromPose;
    state.characterPreviewTransitionFromPhaseMilli =
        preview.transitionFromPhaseMilli;
    state.characterPreviewViewYawDegrees = preview.viewYawDegrees;
    state.characterPreviewViewPitchDegrees = preview.viewPitchDegrees;
    state.characterPreviewLighting = preview.lighting;
    state.characterPreviewCapturePng = std::move(preview.capturePng);
    state.characterPreviewCaptureKind = preview.captureKind;
    state.characterPreviewAutoReturn = preview.autoReturnAfterCapture;
    state.characterPreviewCaptureLauncherOwned = preview.launcherOwnedCapture;
    state.characterPreviewPortraitSourceHandoff =
        preview.portraitSourceHandoff;
    state.characterPreviewInteractiveStudio = preview.interactiveStudio;
    state.characterPreviewRepresentativeMotionReview =
        preview.representativeMotionReview;
    state.characterPreviewDonorReference = preview.donorReference;
    Launcher_requestTab(state, kLauncherPanelPlay, kLauncherTabPlayer);
}

/*
 * Install a content pack by COPYING what the player picked into the mods
 * folder. Nothing is unpacked: the reader takes a `.zip` through the same path
 * validation as a directory, which is a property a gate asserts, so unzipping
 * here would add a second, weaker intake for no gain.
 *
 * Every outcome ends in one sentence the player can act on. "It did nothing and
 * did not say why" is the failure this whole destination exists to end.
 */
PackInstallJob g_packInstall;
FileBrowserUi g_packBrowser;

void cancelPackInstall() {
    g_packInstall.cancel();
}

void finishPackInstallForExit() {
    // Before launcher/host teardown. Never publishes a finished copy.
    g_packInstall.finishForExit();
}

void reclaimAbandonedPackStaging() {
    char modsDirectory[1024] = {0};
    if (mdkr_user_mods_directory(modsDirectory, sizeof modsDirectory) != 0)
        g_packInstall.reclaimAbandonedStaging(modsDirectory);
}

void startPackInstall(const std::string &source, const std::string &modsDirectory) {
    g_packInstall.start(source, modsDirectory);
}

void servicePackInstall(bool quitting) {
    // The inventory refresh stays on the launcher thread: it rebinds the
    // texture store and retires caches the renderer owns. It runs only while
    // the launcher owns the frame, never inside a live engine session.
    if (g_packInstall.service(quitting)) {
        g_smokePackPublished = true;
        platform_content_packs_init();
    }
}

#if !defined(_WIN32)
// Linux has no native file-manager call of its own. xdg-open is the desktop's
// "open this with its usual application" command: where it is installed the
// launcher offers Open mods folder, and where it is not, the folder's path
// shown under the buttons is the way in. macOS reveals the folder in Finder
// instead, but compiles these too so they are built wherever the suite runs.
[[maybe_unused]] bool desktopOpenerAvailable() {
    static const bool available = [] {
        const char *path = std::getenv("PATH");
        const std::string directories = path != nullptr ? path : "";
        size_t start = 0;
        for (;;) {
            const size_t end = directories.find(':', start);
            std::string directory = directories.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (directory.empty()) directory = ".";
            if (access((directory + "/xdg-open").c_str(), X_OK) == 0) return true;
            if (end == std::string::npos) return false;
            start = end + 1;
        }
    }();
    return available;
}

[[maybe_unused]] bool openWithDesktopOpener(const std::string &folder) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    // SDL starts xdg-open detached, so a file manager that stays open never
    // holds up the launcher.
    return SDL_OpenURL(folder.c_str()) == 0;
#else
    (void)folder;
    return false;
#endif
}
#endif

void installContentPack(const char *modsDirectory) {
    if (const char *path = FileBrowserUi_smokePath(true)) {
        g_packBrowser.open(path, BrowserFileKind::ZipPack);
        return;
    }
#if defined(__linux__)
    // The in-window browser installs through startPackInstall once a ZIP is
    // picked, which finds the mods folder again then.
    (void)modsDirectory;
    g_packBrowser.open("", BrowserFileKind::ZipPack);
#else
    std::string picked;
    if (filedialog::openContentPack(picked)) startPackInstall(picked, modsDirectory);
#endif
}

void drawContentPanel(LauncherState &s, LauncherAction &out) {
    (void)out;
    /*
     * Packs and characters are the same intent -- change what is in the game --
     * and they were two unrelated places: a read-only list buried in Settings,
     * and a panel named after a tool. This is the one destination that answers
     * "what have I added, and how do I add more?".
     */
    ui::SectionHeader(
        "Content",
        "Artwork, music and characters you add yourself. Everything here is "
        "optional, and nothing here changes how the game plays.");

    // No gap: SectionHeader already closes with its own rule and spacing, and
    // adding one here left a band of dead space above the first group.
    // "textures" named explicitly: a high-resolution texture pack is the
    // commonest pack there is, and a player holding one should see it in the
    // heading rather than have to infer it from "artwork".
    ui::GroupHeader("Packs",
                    "Replacement textures, artwork and music, loaded from your "
                    "mods folder at launch.");

    /*
     * The folder was the whole barrier. A player was told to "put a pack in the
     * mods folder beside your saves" and then had to find a directory that,
     * on macOS, lives inside ~/Library -- a folder Finder hides by default.
     * The folder is created on demand here rather than at startup, so a player
     * who never installs a pack still gets no directory they did not ask for.
     */
    char modsDirectory[1024] = {0};
    const bool haveModsDirectory =
        mdkr_user_mods_directory(modsDirectory, sizeof modsDirectory) != 0;
    bool canChoosePack = filedialog::isAvailable();
#if defined(__linux__)
    canChoosePack = true;
#endif
    if (haveModsDirectory && canChoosePack) {
        ImGui::BeginDisabled(g_packInstall.busy());
        if (ui::BrandPrimaryButton("Install pack…", ui::kBtnWide())) {
            installContentPack(modsDirectory);
        }
        if (FileBrowserUi_smokePath(true) && !g_smokePackFocusRequested) {
            ImGui::SetKeyboardFocusHere(-1);
            g_smokePackFocusRequested = true;
        }
        g_smokePackButtonFocused = ImGui::IsItemFocused();
        ImGui::EndDisabled();
        ui::SpeakFocusedItem(
            "Install pack", nullptr,
            "Choose a pack to copy into your mods folder.");
        if (g_packInstall.busy()) {
            if (ImGui::Button("Cancel installation", ui::kBtnWide())) {
                g_packInstall.cancel();
                g_packInstall.setNote("Cancelling pack installation…");
            }
            ui::SpeakFocusedItem("Cancel installation", nullptr,
                                 "Discard the pending copy without installing it.");
        }
#if defined(__linux__)
        const bool canOpenModsFolder = desktopOpenerAvailable();
#else
        const bool canOpenModsFolder = filedialog::isAvailable();
#endif
        if (canOpenModsFolder) {
            if (ImGui::GetContentRegionAvail().x > ui::kBtnWide().x * 2.0f)
                ImGui::SameLine();
            if (ImGui::Button("Open mods folder", ui::kBtnWide())) {
                // The location is UTF-8; on Windows a narrow path would be read
                // in the ANSI code page. A failure at any step shows the note,
                // so each step keeps its own error.
                const auto mods = std::filesystem::u8path(modsDirectory);
                std::error_code created;
                std::filesystem::create_directories(mods, created);
                bool opened = false;
                if (!created) {
#if defined(__linux__)
                    std::error_code located;
                    const auto folder = std::filesystem::absolute(mods, located);
                    opened = !located && openWithDesktopOpener(folder.u8string());
#else
                    opened = filedialog::revealInFileManager(modsDirectory);
#endif
                }
                if (!opened)
                    g_packInstall.setNote("Could not open the mods folder. Its location "
                                          "is shown below.");
            }
            ui::SpeakFocusedItem(
                "Open mods folder", nullptr,
                "Opens the folder packs are installed into, creating it if it "
                "does not exist yet.");
        }
        ui::Gap(ui::kGapS);
        if (!g_packInstall.note().empty()) {
            ui::TextSubtleUnformattedWrapped(g_packInstall.note().c_str());
            ui::Gap(ui::kGapS);
        }
    }
    std::string pickedPack;
    if (haveModsDirectory && g_packBrowser.draw("Choose a ZIP content pack", pickedPack))
        startPackInstall(pickedPack, modsDirectory);
    if (haveModsDirectory) {
        ImGui::PushFont(AppTheme::fonts().small);
        // A packaged build resolves this beside the save directory; a
        // command-line build stays CWD-relative and prints "mods", which is
        // true but reads as a stray word without the label.
        ui::TextSubtle("Folder");
        ImGui::SameLine();
        ui::TextSubtleUnformattedWrapped(modsDirectory);
        ImGui::PopFont();
        ui::Gap(ui::kGapS);
    }

    (void)Settings_drawContentPacks(s.hostWindow, /*compact=*/false);

    ui::Gap(ui::kGapL);
    ui::GroupHeader("Characters",
                    "Racers you import or author yourself. Appearance only -- "
                    "a built-in racer still controls handling and results.");
    (void)Settings_drawCustomCharacters(/*compact=*/false);
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Open Character Workshop", ui::kBtnWide())) {
        Launcher_requestTab(s, kLauncherPanelCharacterWorkshop,
                            kLauncherTabPlayer);
    }
    ui::SpeakFocusedItem("Open Character Workshop", nullptr,
                         "Opens the tool for importing and authoring racers.");
}

void drawSettingsPanel(LauncherState &s, LauncherAction &out) {
    (void)out;
    // One page, one scroll owner. Keeping the introduction outside this child
    // left no reachable settings viewport at the supported 640x480 / 2.00x
    // extreme and wasted scarce height on 7-inch handhelds.
    // The scrollbar is RESERVED, not on-demand. The settings page's height
    // crosses the viewport boundary as rows appear (an error line and its
    // Retry button, say), and an on-demand scrollbar then oscillates: it
    // appears, the narrower content re-wraps one line shorter, it vanishes,
    // the line un-wraps, and the page breathes by a text line every few
    // frames -- moving every widget under a pointer or a scripted click.
    ImGui::BeginChild("##settingsscroll", ImVec2(0, 0), panelChildFlags(0),
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ui::SectionHeader("Settings",
                      "Everything saves as you change it. Anything marked "
                      "“Next launch” waits for Play.");

    const bool restartPending = Settings_restartPending();
    if (restartPending) {
        ui::CardBegin("##settingsready", AppTheme::accent(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextUnformatted("Ready for next launch");
        ImGui::PopStyleColor();
        ui::TextSubtle(
            "Press Play with Changes to start with these settings.");
        ui::CardEnd();
        ui::Gap(ui::kGapM);
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static bool tracedSettingsAction = false;
        if (!tracedSettingsAction) {
            std::fprintf(stderr, "[app-ui] settings action=%s restartPending=%d\n",
                         restartPending ? "play-with-changes" : "play",
                         restartPending ? 1 : 0);
            tracedSettingsAction = true;
        }
    }
    Settings_setDonorGameplayProfiles(
        &s.romInfo.donor_profiles, s.romInfo.donor_profiles_message);
    Settings_draw(s.hostWindow, /*compact=*/false);
    if (Settings_takeCharacterWorkshopOpenRequest()) {
        Launcher_requestTab(
            s, kLauncherPanelCharacterWorkshop, kLauncherTabPlayer);
    }
    SettingsCharacterPreviewRequest preview;
    if (Settings_takeCharacterPreviewRequest(preview)) {
        acceptCharacterPreviewRequest(s, std::move(preview));
    }
    ui::TouchScrollCurrentWindow();
    g_smokeSettingsScrollMin = ImGui::GetWindowPos();
    const ImVec2 scrollSize = ImGui::GetWindowSize();
    g_smokeSettingsScrollMax = ImVec2(
        g_smokeSettingsScrollMin.x + scrollSize.x,
        g_smokeSettingsScrollMin.y + scrollSize.y);
    g_smokeSettingsScrollY = ImGui::GetScrollY();
    g_smokeSettingsScrollValid = ImGui::GetScrollMaxY() > 0.0f;
    ImGui::EndChild();
}

void drawCharacterWorkshopPanel(LauncherState &s, LauncherAction &out) {
    (void)out;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = AppTheme::uiScale();
    // The shell's top-navigation decision is viewport-wide, but the Workshop
    // must respond to the space it actually receives after that navigation is
    // laid out. Passing `false` unconditionally made the 640x480/200% smoke
    // exercise only the compact shell while every editor still chose its wide
    // tables, copy density, and multi-column controls.
    const bool compact = available.x < 720.0f * scale ||
                         available.y < 540.0f * scale;
    if (compact) {
        // The compact shell already labels this page "Workshop". A full
        // section hero at 200% UI scale consumed the entire 640x480 content
        // viewport, leaving the editor present in the document but neither
        // visible nor reachable through ImGui navigation.
        ImGui::TextDisabled(
            "Character Workshop · local appearance authoring");
    } else {
        ui::SectionHeader(
            "Character Workshop",
            "Import, author, test, and package local character presentation. "
            "Built-in donor profiles remain authoritative for gameplay.");
    }
    const bool romReady = !s.romPath.empty() && s.romInfo.valid;
    if (!romReady && !s.romValidationPending && !compact) {
        ui::TextSubtleWrapped(
            "A ROM is optional while you import and author. Add your own base-game ROM only when you want exact vehicle/scene previews, final tests, or play.");
        if (ImGui::SmallButton("Add ROM for exact tests…")) {
            if (RomPanel_chooseRom(s)) {
                Launcher_requestTab(
                    s, kLauncherPanelPlay, kLauncherTabPlayer);
            }
        }
        ui::SpeakFocusedItem(
            "Add ROM for exact character tests", nullptr,
            "Optionally chooses your base-game ROM and opens its validation page. Your character sources and drafts remain unchanged.");
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            static bool tracedWorkshopEntryHierarchy = false;
            if (!tracedWorkshopEntryHierarchy) {
                std::fprintf(
                    stderr,
                    "[app-ui] active-panel=Character Workshop workshop-primary=contextual rom=optional\n");
                tracedWorkshopEntryHierarchy = true;
            }
        }
    }
    Settings_setDonorGameplayProfiles(
        &s.romInfo.donor_profiles, s.romInfo.donor_profiles_message);
    Settings_drawCharacterWorkshop(s.hostWindow, compact);
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
        static int tracedCompact = -1;
        if (tracedCompact != (compact ? 1 : 0)) {
            tracedCompact = compact ? 1 : 0;
            std::fprintf(
                stderr,
                "[app-ui] character-workshop-layout compact=%d width=%.1f height=%.1f scale=%.2f\n",
                tracedCompact, static_cast<double>(available.x),
                static_cast<double>(available.y), static_cast<double>(scale));
        }
    }
    SettingsCharacterPreviewRequest preview;
    if (Settings_takeCharacterPreviewRequest(preview)) {
        acceptCharacterPreviewRequest(s, std::move(preview));
    }
}

void drawAboutPanel(LauncherState &s, LauncherAction &out) {
    /*
     * The support surface: what this build IS, and the report you copy into a
     * bug. Diagnostics used to be a top-level destination of its own, which put
     * a developer's readout beside Play for every player who never needs it.
     * Merging it here is what let the navigation collapse to three -- and it
     * has to be drawn, not merely routed: panel 3 has no navigation entry of
     * its own any more, so if this function did not call DiagPanel_draw the
     * diagnostics report would be unreachable from the interface.
     */
    ui::SectionHeader("About & support",
                      "An unofficial fan project: a decompilation-based native "
                      "source port, for research, preservation and education.");

    ImGui::TextUnformatted(AppBrandVersionLine());

    ui::Gap(ui::kGapM);
    ImGui::PushTextWrapPos(0.0f);
    ui::TextSubtle(
        MDKR_BRAND_NAME " contains no game data. It reads assets from a copy of "
        "the original game that you supply and legally own. The original game "
        "and all related trademarks are the property of their respective rights "
        "holders; this project is not affiliated with, endorsed by, or sponsored "
        "by any of them.");
    ImGui::PopTextWrapPos();

    ui::Gap(ui::kGapM);
    ui::TextSubtleWrapped(
        "F1 opens in-game settings. F10 toggles the FPS readout. F11 or "
        "Alt+Enter toggles fullscreen.");

    ui::Gap(ui::kGapL);
    // No heading here: DiagPanel_draw carries its own, and its subtitle already
    // says the thing a player needs (attach this to a bug report).
    DiagPanel_draw(s, out);
}

}  // namespace

const char *FileBrowserUi_smokePath(bool pack) {
    if (AppUi_smokeInputMode() != AppUiSmokeInputMode::Gamepad ||
        std::getenv("MDKR_APP_SMOKE_FRAMES") == nullptr) return nullptr;
    const char *path = std::getenv(pack ? "MDKR_APP_SMOKE_PACK_BROWSER" : "MDKR_APP_SMOKE_ROM_BROWSER");
    const char *token = std::getenv(pack ? "MDKR_APP_SMOKE_PACK_BROWSER_TOKEN" : "MDKR_APP_SMOKE_ROM_BROWSER_TOKEN");
    return path && path[0] && token &&
        std::strcmp(token, pack ? "mdkr64-app-pack-browser-v1" : "mdkr64-app-rom-browser-v1") == 0
        ? path : nullptr;
}

bool Launcher_smokePackInstallButtonFocused() { return g_smokePackButtonFocused; }
bool Launcher_smokePackBrowserOpen() { return g_packBrowser.visible(); }
bool Launcher_smokePackBrowserFocusedZip(const char *path) { return g_packBrowser.focusedPath(path, false); }
bool Launcher_smokePackInstalled() { return g_smokePackPublished && !g_packInstall.busy(); }
bool Launcher_smokePackInstallPending() { return g_packInstall.busy(); }

void Launcher::serviceCharacterWork() {
    // Character subprocesses publish through launcher-owned UI state. Service
    // them on every destination so leaving the Workshop cannot strand a ready
    // result, race Play against a directory transaction, or turn application
    // shutdown into an invisible global-destructor join.
    Settings_serviceCharacterWork();
    if (state_.characterPreviewDispatched) {
        SettingsCharacterPreviewDisposition disposition;
        disposition.launcherOwnedCapture =
            state_.characterPreviewCaptureLauncherOwned;
        disposition.portraitSourceHandoff =
            state_.characterPreviewPortraitSourceHandoff;
        disposition.interactiveStudio =
            state_.characterPreviewInteractiveStudio;
        disposition.representativeMotionReview =
            state_.characterPreviewRepresentativeMotionReview;
        disposition.donorReference =
            state_.characterPreviewDonorReference;
        disposition.scene = state_.characterPreviewScene;
        Settings_publishCharacterPreviewResult(
            state_.characterPreviewPackage,
            state_.characterPreviewSourceSha256,
            state_.characterPreviewInteractiveStudio
                ? Settings_characterPreviewCurrentFitSignature(
                      state_.characterPreviewPackage,
                      state_.characterPreviewContext)
                : state_.characterPreviewFitSha256,
            state_.characterPreviewPresentationSha256,
            state_.characterPreviewCapturePng,
            disposition,
            state_.characterPreviewResult,
            state_.characterPreviewRepresentativeMotionReview
                ? &state_.characterMotionReviewResult : nullptr);
        Launcher_requestTab(
            state_, kLauncherPanelCharacterWorkshop, kLauncherTabPlayer);
        state_.characterPreviewPackage.clear();
        state_.characterPreviewSourceSha256.clear();
        state_.characterPreviewFitSha256.clear();
        state_.characterPreviewPresentationSha256.clear();
        state_.characterPreviewContext = MDKR_CHARACTER_PREVIEW_NONE;
        state_.characterPreviewScene =
            MDKR_CHARACTER_PREVIEW_SCENE_BASELINE;
        state_.characterPreviewPlayers = 0;
        state_.characterPreviewPose = MDKR_CHARACTER_PREVIEW_POSE_LIVE;
        state_.characterPreviewPosePhaseMilli = 0u;
        state_.characterPreviewTransitionFromPose =
            MDKR_CHARACTER_PREVIEW_POSE_LIVE;
        state_.characterPreviewTransitionFromPhaseMilli = 0u;
        state_.characterPreviewViewYawDegrees = 0;
        state_.characterPreviewViewPitchDegrees = 0;
        state_.characterPreviewLighting =
            MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
        state_.characterPreviewCapturePng.clear();
        state_.characterPreviewCaptureKind =
            MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE;
        state_.characterPreviewAutoReturn = false;
        state_.characterPreviewCaptureLauncherOwned = false;
        state_.characterPreviewPortraitSourceHandoff = false;
        state_.characterPreviewInteractiveStudio = false;
        state_.characterPreviewRepresentativeMotionReview = false;
        state_.characterPreviewDonorReference = false;
        state_.characterPreviewDispatched = false;
    }
}

void Launcher::finishCharacterWorkForExit() {
    serviceCharacterWork();
    while (state_.characterPreviewDispatched || Settings_characterWorkPending()) {
        // Only the exceptional, non-renderable exit uses this wait. Normal
        // Quit keeps pumping and displaying progress in the launcher loop.
        SDL_Delay(10);
        serviceCharacterWork();
    }
}

LauncherAction Launcher::draw(AppHost &host) {
    if (networkShutdown_->started) return {};
#if MDKR_ENABLE_ONLINE_BETA
    ++g_betaFrame;
#endif
    serviceCharacterWork();
    state_.hostWindow = host.window();
    phoneParty_->service(static_cast<uint64_t>(SDL_GetTicks64()));
    refreshLanControls();
    LauncherAction action;
    const int panelAtFrameStart = active_;
    for (int i = 0; i < kPanelCount; ++i) g_smokeTopTabValid[i] = false;
    // Same one-frame lifetime as the tab rectangles above: the settings scroll
    // viewport is only real while the Settings panel is drawing. Leaving the
    // last value latched let Launcher_smokeSettingsScrollRect hand a touch or
    // capture gate a rectangle belonging to a panel that is no longer on screen.
    g_smokeSettingsScrollValid = false;
    g_smokePanelScrollValid = false;

    // Design-review / CI hook: MDKR_APP_PANEL=<index|name> opens a specific panel
    // on the first frame so a screenshot gate can capture it without input.
    if (!panelEnvChecked_) {
        panelEnvChecked_ = true;
        selectPanelFromEnvironment(active_);
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            std::fprintf(stderr, "[app-ui] active-panel=%s\n",
                         kPanels[active_].label);
        }
    }

    // A dropped character source opens its workshop/importer report. Every
    // other file keeps the established ROM flow and its full-image validation.
    acceptDroppedFile(host, state_, active_);
    // Navigation carries the global readiness/action state, so initialize the
    // remembered ROM even when a design-review hook opens another panel first.
    RomPanel_ensureInit(state_);
    RomPanel_serviceValidation(state_);
    if (!state_.characterPreviewPackage.empty() &&
        state_.characterPreviewContext != MDKR_CHARACTER_PREVIEW_NONE &&
        !state_.romValidationPending &&
        !state_.romPlayValidationPending &&
        !state_.romPlayValidationPassed &&
        !state_.romPath.empty() && state_.romInfo.valid) {
        RomPanel_requestPlayValidation(state_);
    }
    if (std::getenv("MDKR_APP_UI_TRACE") != nullptr &&
        !state_.characterPreviewPackage.empty() &&
        state_.characterPreviewContext != MDKR_CHARACTER_PREVIEW_NONE) {
        static unsigned previewTraceState = ~0u;
        const unsigned current =
            (state_.romValidationPending ? 1u : 0u) |
            (state_.romPlayValidationPending ? 2u : 0u) |
            (state_.romPlayValidationPassed ? 4u : 0u) |
            (!state_.romPath.empty() ? 8u : 0u) |
            (state_.romInfo.valid ? 16u : 0u) |
            ((state_.romValidationTotal != 0u
                  ? std::min(10u,
                        (state_.romValidationBytes * 10u) /
                            state_.romValidationTotal)
                  : 0u) << 8u);
        if (current != previewTraceState) {
            previewTraceState = current;
            std::fprintf(
                stderr,
                "[app-ui] character-preview handoff validation=%d play-pending=%d play-passed=%d rom-path=%d rom-valid=%d studio=%d progress=%u/%u\n",
                state_.romValidationPending ? 1 : 0,
                state_.romPlayValidationPending ? 1 : 0,
                state_.romPlayValidationPassed ? 1 : 0,
                state_.romPath.empty() ? 0 : 1,
                state_.romInfo.valid ? 1 : 0,
                state_.characterPreviewInteractiveStudio ? 1 : 0,
                state_.romValidationBytes, state_.romValidationTotal);
        }
    }
    /*
     * "Skip the launcher" (issue #60). The launch decision was made in main()
     * before this window drew anything; all that is left is to wait for the
     * remembered ROM's verdict and then press Play.
     *
     * Deliberately the ORDINARY route: RomPanel_requestPlayValidation() is the
     * same call the Play button makes, so the mandatory final ROM check runs
     * and its result -- not this decision -- is what publishes a boot. A ROM
     * that has been moved, swapped or unplugged since it was remembered lands
     * the player in the launcher looking at the reason, which is exactly what
     * pressing Play would have done.
     */
    {
        /*
         * Re-sample the hold every frame until the boot dispatches, and disarm
         * the first time it is seen. main()'s single pre-frame sample cannot be
         * the whole answer: SDL folds keyboard state from events, so a Shift
         * already down before the window existed is invisible there -- on macOS
         * the first thing SDL learns about that key is its RELEASE. Sampling
         * here turns the window a player has to aim at from one instant into
         * "while the launcher is on screen", which is at least as long as
         * hashing the ROM takes.
         *
         * Disarming is one-way. A hold seen at any point in that window means
         * the player asked for their launcher, and letting a later frame
         * re-arm the skip would boot them out of it mid-decision.
         */
        if (skipArmed_ && !skipDispatched_) {
            const AppUiLauncherHold hold = AppLaunchHold_sample(++holdSamples_);
            if (AppUi_launcherHoldOpensLauncher(hold)) {
                skipArmed_ = false;
                AppLaunchHold_release();
                std::fprintf(stderr,
                             "[app-ui] skip-launcher disarmed by hold "
                             "sample=%u shift=%d shoulderL=%d shoulderR=%d\n",
                             holdSamples_, hold.shift ? 1 : 0,
                             hold.leftShoulder ? 1 : 0,
                             hold.rightShoulder ? 1 : 0);
            }
        }

        AppUiLauncherSkipReadiness readiness;
        readiness.armed = skipArmed_;
        readiness.dispatched = skipDispatched_;
        readiness.romRemembered = !state_.romPath.empty();
        readiness.romValid = state_.romInfo.valid;
        readiness.validationPending = state_.romValidationPending;
        readiness.playValidationPending = state_.romPlayValidationPending;
        readiness.bootErrorVisible = state_.bootErrorVisible;
        readiness.otherWorkPending =
            Settings_characterWorkPending() || state_.quitRequested ||
            (!state_.characterPreviewPackage.empty() &&
             state_.characterPreviewContext != MDKR_CHARACTER_PREVIEW_NONE);
        if (AppUi_launcherSkipShouldBoot(readiness)) {
            skipDispatched_ = true;
            AppLaunchHold_release();
            RomPanel_requestPlayValidation(state_);
            std::fprintf(stderr,
                         "[app-ui] skip-launcher direct boot requested "
                         "rom=%s finalCheck=%d\n",
                         state_.romPath.c_str(),
                         state_.romPlayValidationPending ? 1 : 0);
        }
    }

    if (state_.romPlayValidationPassed &&
        !Settings_characterWorkPending() && !state_.quitRequested) {
        state_.romPlayValidationPassed = false;
        AppLaunchHold_release();
        action.type = LauncherActionType::Play;
        fillBootConfig(state_, action.boot);
    }
    if (state_.quitRequested && !Settings_characterWorkPending()) {
        action.type = LauncherActionType::Quit;
        if (std::getenv("MDKR_APP_UI_TRACE") != nullptr) {
            static bool tracedSafeQuit = false;
            if (!tracedSafeQuit) {
                tracedSafeQuit = true;
                std::fprintf(
                    stderr,
                    "[app-ui] character-workshop-quit completed=1 pending=0\n");
            }
        }
    }

    /* Font-coverage witness. The launcher never consults a host font, so any
     * codepoint the packaged subset omits renders as a box for every player.
     * The gate hands in the exact non-ASCII codepoints platform/app writes and
     * this answers with the ones the atlas cannot draw. */
    if (const char *coverage = std::getenv("MDKR_APP_SMOKE_FONT_COVERAGE")) {
        static bool tracedFontCoverage = false;
        if (!tracedFontCoverage) {
            tracedFontCoverage = true;
            unsigned    requested = 0u;
            std::string missing;
            for (const char *cursor = coverage; *cursor != '\0';) {
                char         *end = nullptr;
                const unsigned long codepoint =
                    std::strtoul(cursor, &end, 16);
                if (end == cursor) break;
                ++requested;
                if (!AppTheme::canDrawGlyph(
                        static_cast<unsigned>(codepoint))) {
                    char formatted[16];
                    std::snprintf(formatted, sizeof(formatted), "%s%04lX",
                                  missing.empty() ? "" : ",", codepoint);
                    missing += formatted;
                }
                cursor = (*end == ',') ? end + 1 : end;
            }
            std::fprintf(
                stderr,
                "[app-ui] font-coverage requested=%u missing=%s\n",
                requested, missing.empty() ? "none" : missing.c_str());
        }
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    // This root is a fixed viewport shell; its panel children own scrolling.
    // Selecting an off-axis compact tab must not let ImGui auto-scroll the
    // whole launcher and move the brand, Quit, or persistent Play row away.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);

#if MDKR_ENABLE_ONLINE_BETA
    // Evaluated at frame start (nothing before this touches the online adapter),
    // so it reflects the takeover decision rather than any state a mid-frame
    // draw would create.
    const bool onlineActiveTakeover = OnlineRoom_isLobbyTakeoverActive();
    if (onlineActiveTakeover) {
        // MODAL LOBBY TAKEOVER. A live online session owns the whole window: the
        // nav rail, top tabs, panel router and the generic offline Play are all
        // suppressed, so the offline launch is unreachable -- the root fix for
        // both reported bugs (originator launching offline over the room, joiner
        // beach-balling by launching offline into a live session). Defensively
        // drop any Play action a pending ROM re-check may have staged this
        // frame, so offline can never fire while a session is live.
        action = LauncherAction{};
        drawLobbyTakeover(state_, action);
        ImGui::End();
        // Consume a deferred navigation (a clean "Leave Race" asks for home).
        if (state_.requestTab >= 0 && state_.requestTab < kPanelCount) {
            active_ = state_.requestTab;
        }
        state_.requestTab = -1;
        state_.requestTabPriority = 0;
        emitLobbyTakeoverProbe(true, true);
        servicePackInstall(state_.quitRequested || action.type == LauncherActionType::Quit);
        return action;
    }
#endif
    const bool compactNavigation =
        vp->Size.x < 860.0f * AppTheme::uiScale() ||
        vp->Size.y < 620.0f * AppTheme::uiScale();
    if (compactNavigation) {
        drawTopNavigation(active_, state_);
    } else {
        drawNavigation(active_, state_);
        ImGui::SameLine();
    }
    drawActivePanel(active_, state_, action);
    ImGui::End();

    // Consume deferred navigation only after this frame has rendered from one
    // immutable selection snapshot.
    if (state_.requestTab >= 0 && state_.requestTab < kPanelCount) {
        active_ = state_.requestTab;
    }
    state_.requestTab = -1;
    state_.requestTabPriority = 0;

    // Apply the party card's deferred Start/Stop only now the frame is done: the
    // card drew from the live host, and a transport switch rebuilds it. Refresh
    // the controls so the same frame's later readers (the in-game overlay) see
    // the new host, not a stale one.
    if (state_.lanParty.request == PhonePartyLanControls::Request::Start) {
        applyLanStart();
    } else if (state_.lanParty.request == PhonePartyLanControls::Request::Stop) {
        applyLanStop();
    }
    state_.lanParty.request = PhonePartyLanControls::Request::None;

    if (panelAtFrameStart == kLauncherPanelSettings &&
        active_ != kLauncherPanelSettings) {
        Settings_cancelAudioPreview();
    }

    // A drop or Workshop action can start a character transaction after ROM
    // validation was consumed near the top of this frame. Preserve the passed
    // verdict and defer Play rather than entering the engine while that new
    // transaction owns the character directory. This closes the same-frame
    // edge, while the primary action's disabled state covers ordinary input.
    if (action.type == LauncherActionType::Play &&
        (Settings_characterWorkPending() || state_.quitRequested)) {
        state_.romPlayValidationPassed = true;
        action = LauncherAction{};
    }
    if (action.type == LauncherActionType::Play) {
        state_.characterPreviewDispatched =
            action.boot.character_preview_context !=
                MDKR_CHARACTER_PREVIEW_NONE;
    }

#if MDKR_ENABLE_ONLINE_BETA
    emitLobbyTakeoverProbe(onlineActiveTakeover, false);
#endif
    // Process Cancel/Quit before consuming a ready copy. Publishing at the top
    // of the frame could remove the Cancel button before its click is handled.
    // Refreshing the content registry here also cannot invalidate panel readers.
    servicePackInstall(state_.quitRequested || action.type == LauncherActionType::Quit);
    return action;
}

void Launcher::setBootError(const char *message) {
    if (message == nullptr || message[0] == '\0') return;
    std::snprintf(state_.bootError, sizeof(state_.bootError), "%s", message);
    state_.bootErrorVisible = true;
    active_ = 0;
    std::fprintf(stderr, "[app] boot recovery visible: %s\n", state_.bootError);
}

void Launcher::requestPlayValidationForSmoke() {
    RomPanel_requestPlayValidation(state_);
}

void Launcher::armSkipWhenReady() {
    skipArmed_ = true;
}
