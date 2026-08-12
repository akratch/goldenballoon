#include "ui_online_room.h"

#include "app_theme.h"
#include "ui_common.h"
#include "ui_launcher.h"
#include "ui_settings.h"

#include "a11y_model.h"
#include "online/lobby_fake_adapter.h"

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct OnlineRoomUiState {
    MdkrOnlineFakeAdapter adapter{};
    bool initialized = false;
    bool detailsOpen = false;
    bool connectionDoctorOpen = false;
    bool updateHelpOpen = false;
    bool leaveRaceConfirm = false;
    std::uint64_t nextRequestId = 1u;
    std::uint32_t scheduledToken = 0u;
    unsigned scheduledFrames = 0u;
    MdkrOnlineViewKind announcedKind = static_cast<MdkrOnlineViewKind>(0);
    MdkrOnlineViewFailure announcedFailure = MDKR_ONLINE_VIEW_FAILURE_NONE;
    const MdkrOnlineFakeGallerySpec *gallerySpec = nullptr;
    bool galleryTraced = false;
    MdkrOnlineViewAction focusedAction = MDKR_ONLINE_VIEW_ACTION_NONE;
    MdkrOnlineViewAction completedAction = MDKR_ONLINE_VIEW_ACTION_NONE;
    bool focusApplied = false;
    bool completedActionAccepted = false;
};

OnlineRoomUiState g_online;

constexpr const char *kCharacters[] = {
    "Diddy", "Timber", "Pipsy", "Tiptup", "Conker",
    "Bumper", "Banjo", "Krunch", "Drumstick", "T.T.",
};
constexpr const char *kVehicles[] = {"Car", "Hovercraft", "Plane"};
struct TrackChoice { const char *name; unsigned id; };
constexpr TrackChoice kTracks[] = {
    {"Ancient Lake", 5u},
    {"Fossil Canyon", 3u},
    {"Jungle Falls", 29u},
};

bool fakeEnabled() {
    return std::getenv("MDKR_APP_ONLINE_FAKE") != nullptr;
}

bool actionSmokeEnabled() {
    const char *token = std::getenv("MDKR_APP_ONLINE_ACTION_TOKEN");
    return token != nullptr &&
        std::strcmp(token, "mdkr64-online-action-v1") == 0;
}

MdkrOnlineViewAction focusedAction() {
    if (!actionSmokeEnabled()) return MDKR_ONLINE_VIEW_ACTION_NONE;
    if (g_online.focusedAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        return g_online.focusedAction;
    }
    const char *value = std::getenv("MDKR_APP_ONLINE_FOCUS_ACTION");
    char *end = nullptr;
    const long parsed = value != nullptr ? std::strtol(value, &end, 10) : 0;
    if (value == nullptr || end == value || *end != '\0' ||
        parsed <= MDKR_ONLINE_VIEW_ACTION_NONE ||
        parsed > MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE) {
        return MDKR_ONLINE_VIEW_ACTION_NONE;
    }
    g_online.focusedAction = static_cast<MdkrOnlineViewAction>(parsed);
    return g_online.focusedAction;
}

void maybeFocusAction(MdkrOnlineViewAction action) {
    if (g_online.focusApplied || action != focusedAction()) return;
    ImGui::SetKeyboardFocusHere();
    // Programmatic focus intentionally hides Dear ImGui's navigation cursor
    // when the previous input source was the mouse. The action smoke still
    // activates through real SDL keyboard/gamepad events, but it needs the
    // same visible-nav precondition a player's first arrow/Tab press creates.
    // This is reachable only through the token-gated smoke contract above.
    ImGui::GetIO().ConfigNavCursorVisibleAlways = true;
    g_online.focusApplied = true;
    std::fprintf(stderr, "[online-ui-action] focus action=%u\n",
                 static_cast<unsigned>(action));
}

void recordAction(MdkrOnlineViewAction action, bool accepted) {
    if (!actionSmokeEnabled() || action != focusedAction()) return;
    g_online.completedAction = action;
    g_online.completedActionAccepted = accepted;
    std::fprintf(stderr, "[online-ui-action] activate action=%u accepted=%u\n",
                 static_cast<unsigned>(action), accepted ? 1u : 0u);
}

MdkrOnlineCompatibilityV1 fakeCompatibility() {
    MdkrOnlineCompatibilityV1 value{};
    value.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    for (unsigned i = 0u; i < sizeof(value.build_id); ++i) {
        value.build_id[i] = static_cast<std::uint8_t>(i + 1u);
    }
    for (unsigned i = 0u; i < sizeof(value.gameplay_digest); ++i) {
        value.gameplay_digest[i] = static_cast<std::uint8_t>(0x80u + i);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

void ensureInitialized() {
    if (g_online.initialized || !fakeEnabled()) return;
    const MdkrOnlineCompatibilityV1 compatibility = fakeCompatibility();
    g_online.initialized = mdkr_online_fake_init(
        &g_online.adapter, UINT64_C(0x4f4e303342), &compatibility,
        std::getenv("MDKR_APP_ONLINE_FAKE_ALLOW_START") != nullptr);
    const char *gallery = std::getenv("MDKR_APP_ONLINE_GALLERY");
    if (!g_online.initialized || gallery == nullptr || gallery[0] == '\0') return;
    g_online.gallerySpec = mdkr_online_fake_gallery_find(gallery);
    if (g_online.gallerySpec == nullptr ||
        !mdkr_online_fake_prepare_gallery(&g_online.adapter, gallery)) {
        std::fprintf(stderr,
                     "[online-gallery] rejected slug=%s admission=%u\n",
                     gallery,
                     g_online.adapter.race_admission_enabled ? 1u : 0u);
        g_online.initialized = false;
        g_online.gallerySpec = nullptr;
        return;
    }
    // Gallery construction intentionally traverses the public command seam,
    // so it consumes request IDs just like a resumed persisted room would.
    // Continue after its high-water mark: restarting at 1 turns the first
    // player action into a correctly rejected replay.
    g_online.nextRequestId = g_online.adapter.last_request_id + 1u;
    if (g_online.nextRequestId == 0u) g_online.nextRequestId = 1u;
}

void scheduleIfPending(const MdkrOnlineFakeStep &step) {
    if (!step.accepted || step.pending_token == 0u) return;
    g_online.scheduledToken = step.pending_token;
    g_online.scheduledFrames = 1u;
}

void serviceFakeCallback() {
    if (!g_online.initialized || g_online.scheduledToken == 0u) return;
    if (g_online.scheduledFrames != 0u) {
        --g_online.scheduledFrames;
        return;
    }
    const std::uint32_t token = g_online.scheduledToken;
    g_online.scheduledToken = 0u;
    const MdkrOnlineFakeStep step = mdkr_online_fake_complete(
        &g_online.adapter, token, MDKR_ONLINE_VIEW_FAILURE_NONE);
    if (!step.accepted) {
        std::fprintf(stderr,
                     "[online-ui] ignored callback token=%u error=%u\n",
                     token, static_cast<unsigned>(step.error));
    }
}

MdkrOnlineFakeStep dispatch(MdkrOnlineViewAction action,
                            unsigned seat = 0u, unsigned value = 0u) {
    MdkrOnlineFakeCommand command{};
    command.expected_revision = g_online.adapter.revision;
    command.request_id = g_online.nextRequestId++;
    command.action = action;
    command.seat = seat;
    command.value = value;
    const MdkrOnlineFakeStep step =
        mdkr_online_fake_dispatch(&g_online.adapter, &command);
    scheduleIfPending(step);
    if (actionSmokeEnabled() && action == focusedAction()) {
        std::fprintf(stderr,
                     "[online-ui-action] dispatch action=%u seat=%u value=%u "
                     "error=%u\n",
                     static_cast<unsigned>(action), seat, value,
                     static_cast<unsigned>(step.error));
    }
    recordAction(action, step.accepted);
    return step;
}

void speakFocused(const MdkrOnlineViewControl &control, const char *help) {
    ui::SpeakFocusedItem(control.label, control.enabled ? "Available" : "Unavailable",
                         help);
}

bool drawActionButton(const MdkrOnlineViewControl &control, bool primary) {
    if (!control.visible || control.label == nullptr) return false;
    if (!control.enabled) ImGui::BeginDisabled();
    const ImVec2 size(-1.0f, primary ? ui::kBtnPrimary().y
                                    : ui::kBtnSecondary().y);
    maybeFocusAction(control.action);
    const bool pressed = primary
        ? ui::BrandPrimaryButton(control.label, size)
        : ImGui::Button(control.label, size);
    speakFocused(control, "Activates the named launcher action.");
    if (!control.enabled) ImGui::EndDisabled();
    return pressed && control.enabled;
}

void drawUnavailablePanel() {
    ui::SectionHeader(
        "Online Room",
        "Private online rooms are being qualified. Local play and phone "
        "controllers remain available without this feature.");
    ui::CautionBox(
        "Online Racing Is Not Enabled in This Build",
        "The rollback release gate is still open. No room is created, no "
        "network request is made, and the Play action remains local.");
    ui::Gap(ui::kGapM);
    ui::GroupHeader("What Will Live Here",
                    "Create or join a private room, check compatibility, choose "
                    "racers, vote, load together and keep the party for a rematch.");
    ImGui::BulletText("Matchmaking, invites and recovery stay in the launcher.");
    ImGui::BulletText("The game receives only a frozen race descriptor and inputs.");
    ImGui::BulletText("Start Race stays absent until the written rollback GO.");
}

void announceView(const MdkrOnlineViewModel &model) {
    if (!ui::SpeechEnabled() ||
        (model.kind == g_online.announcedKind &&
         model.failure == g_online.announcedFailure)) return;
    g_online.announcedKind = model.kind;
    g_online.announcedFailure = model.failure;
    char message[MDKR_A11Y_TEXT_MAX];
    std::snprintf(message, sizeof(message), "%s. %s", model.title,
                  model.explanation);
    mdkr_a11y_announce(
        MDKR_A11Y_CAT_STATUS,
        model.announcement == MDKR_ONLINE_ANNOUNCE_ASSERTIVE
            ? MDKR_A11Y_PRI_CRITICAL : MDKR_A11Y_PRI_NORMAL,
        message);
}

void drawConnectionDetails(const MdkrOnlineViewModel &model) {
    if (!g_online.detailsOpen) return;
    ui::Gap(ui::kGapS);
    if (ui::CardBegin("##online-details", AppTheme::subtle(), 0.0f)) {
        ImGui::TextUnformatted("Connection Details");
        ui::TextSubtleWrapped(
            "Preview adapter • no external service • no addresses, invite "
            "secrets, names or input samples in diagnostics");
        ui::TextSubtle("Members %u • Racer seats %u • Ready %u",
                       model.member_count, model.seat_count, model.ready_count);
    }
    ui::CardEnd();
    if (g_online.connectionDoctorOpen &&
        ui::CardBegin("##online-connection-doctor", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("Connection Doctor");
        ui::TextSubtleWrapped(
            "1. Keep this display and router online. 2. Ask each friend to "
            "retry from the newest invite. 3. If direct play still fails, "
            "use local play now and try the room again later.");
        ui::TextSubtleWrapped(
            "Diagnostics stay privacy-safe: no IP addresses, invite secrets, "
            "player names or controller inputs are shown or copied.");
    }
    if (g_online.connectionDoctorOpen) ui::CardEnd();
}

void drawUpdateHelp() {
    if (!g_online.updateHelpOpen) return;
    ui::Gap(ui::kGapS);
    if (ui::CardBegin("##online-update-help", AppTheme::accent(), 0.0f)) {
        ImGui::TextUnformatted("Update Safely");
        ui::TextSubtleWrapped(
            "Automatic updates are not enabled in this build. Close the room, "
            "download the newest published release from the same trusted source "
            "you installed, then reopen the newest invite. Your ROM and local "
            "settings are not uploaded or replaced.");
    }
    ui::CardEnd();
}

bool drawChoiceCombo(MdkrOnlineViewAction action,
                     const char *label, const char *preview,
                     const char *const *choices, unsigned count,
                     unsigned *selected) {
    ImGui::SetNextItemWidth(ui::kControlWidth());
    bool changed = false;
    maybeFocusAction(action);
    const bool open = ImGui::BeginCombo(label, preview);
    if (open) {
        for (unsigned i = 0u; i < count; ++i) {
            const bool isSelected = selected != nullptr && *selected == i;
            if (ImGui::Selectable(choices[i], isSelected)) {
                if (selected != nullptr) *selected = i;
                changed = true;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ui::SpeakFocusedItem(label, preview,
                         "Choose with keyboard, gamepad, mouse or touch.");
    return changed;
}

bool drawSelectionControl(const MdkrOnlineViewModel &model) {
    static unsigned character = 0u;
    static unsigned vehicle = 0u;
    static unsigned track = 0u;
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_CHARACTER) {
        if (drawChoiceCombo(model.primary.action,
                            "Choose Character", "Select a character…",
                            kCharacters,
                            sizeof(kCharacters) / sizeof(kCharacters[0]),
                            &character)) {
            dispatch(model.primary.action, 0u, character);
        }
        return true;
    }
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_VEHICLE) {
        if (drawChoiceCombo(model.primary.action,
                            "Choose Vehicle", "Select a vehicle…", kVehicles,
                            sizeof(kVehicles) / sizeof(kVehicles[0]), &vehicle)) {
            dispatch(model.primary.action, 0u, vehicle);
        }
        return true;
    }
    if (model.primary.action == MDKR_ONLINE_VIEW_ACTION_VOTE_TRACK) {
        const char *names[sizeof(kTracks) / sizeof(kTracks[0])];
        for (unsigned i = 0u; i < sizeof(kTracks) / sizeof(kTracks[0]); ++i) {
            names[i] = kTracks[i].name;
        }
        if (drawChoiceCombo(model.primary.action,
                            "Choose Track", "Vote for a track…", names,
                            sizeof(kTracks) / sizeof(kTracks[0]), &track)) {
            dispatch(model.primary.action, 0u, kTracks[track].id);
        }
        return true;
    }
    return false;
}

void handleAction(MdkrOnlineViewAction action, LauncherState &state) {
    if (action == MDKR_ONLINE_VIEW_ACTION_PLAY_HERE ||
        action == MDKR_ONLINE_VIEW_ACTION_CHOOSE_ROM ||
        action == MDKR_ONLINE_VIEW_ACTION_RETURN_HOME) {
        if (g_online.adapter.session.state.intent != MDKR_INTENT_NONE) {
            const MdkrOnlineFakeStep step = dispatch(action);
            if (!step.accepted) return;
        } else {
            recordAction(action, true);
        }
        Launcher_requestTab(state, kLauncherPanelGameRom,
                            kLauncherTabPlayer);
        return;
    }
    if (action == MDKR_ONLINE_VIEW_ACTION_CHANGE_TRACK) {
        const MdkrOnlineFakeStep step =
            dispatch(MDKR_ONLINE_VIEW_ACTION_RACE_AGAIN);
        recordAction(action, step.accepted);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_START_RACE) {
        dispatch(action, 0u, 1u);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DETAILS) {
        g_online.detailsOpen = !g_online.detailsOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_CONNECTION_DOCTOR) {
        g_online.detailsOpen = true;
        g_online.connectionDoctorOpen = !g_online.connectionDoctorOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_UPDATE_GAME) {
        g_online.updateHelpOpen = !g_online.updateHelpOpen;
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_SETUP_CONTROLLER) {
        Settings_requestControllerSection();
        Launcher_requestTab(state, kLauncherPanelSettings,
                            kLauncherTabPlayer);
        recordAction(action, true);
    } else if (action == MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE) {
        if (!g_online.leaveRaceConfirm) {
            g_online.leaveRaceConfirm = true;
            recordAction(action, true);
        } else {
            const MdkrOnlineFakeStep step = dispatch(action);
            if (step.accepted) {
                g_online.leaveRaceConfirm = false;
                Launcher_requestTab(state, kLauncherPanelGameRom,
                                    kLauncherTabPlayer);
            }
        }
    } else {
        dispatch(action);
    }
}

void drawLeaveRaceConfirmation(LauncherState &state) {
    if (!g_online.leaveRaceConfirm) return;
    ui::Gap(ui::kGapM);
    ui::CautionBox(
        "Leave This Race?",
        "The race keeps running for your friends. This display will stop the "
        "game and leave the private room.");
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Keep Racing", ImVec2(-1.0f, ui::kBtnSecondary().y))) {
        g_online.leaveRaceConfirm = false;
    }
    ui::SpeakFocusedItem("Keep Racing", "Recommended",
                         "Closes this confirmation without changing the race.");
    ui::Gap(ui::kGapS);
    if (ImGui::Button("Leave Race and Room",
                      ImVec2(-1.0f, ui::kBtnSecondary().y))) {
        handleAction(MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE, state);
    }
    ui::SpeakFocusedItem(
        "Leave Race and Room", "Destructive",
        "Stops this display's game and disconnects it from the private room.");
}

void drawFakePanel(LauncherState &state) {
    serviceFakeCallback();
    MdkrOnlineViewModel model{};
    if (!mdkr_online_fake_view(&g_online.adapter, &model)) {
        ui::CautionBox("Preview State Rejected",
                       "The fake adapter produced an invalid composition. "
                       "Return to the Game ROM panel and inspect Diagnostics.");
        return;
    }
    if (g_online.gallerySpec != nullptr && !g_online.galleryTraced) {
        g_online.galleryTraced = true;
        std::fprintf(
            stderr,
            "[online-gallery] rendered slug=%s kind=%u failure=%u primary=%u "
            "secondary=%u cancel=%u timeout=%u timeout-visible=%u admission=%u\n",
            g_online.gallerySpec->slug, static_cast<unsigned>(model.kind),
            static_cast<unsigned>(model.failure),
            static_cast<unsigned>(model.primary.action),
            static_cast<unsigned>(model.secondary.action),
            static_cast<unsigned>(model.cancel.action),
            model.timeout.present ? 1u : 0u,
            g_online.adapter.timeout_expired ? 1u : 0u,
            g_online.adapter.race_admission_enabled ? 1u : 0u);
    }
    announceView(model);
    ui::SectionHeader(model.title, model.explanation);

    if (ui::CardBegin("##online-status", AppTheme::brandSky(), 0.0f)) {
        ImGui::TextUnformatted(model.status != nullptr ? model.status
                                                       : "Private Room Preview");
        ui::TextSubtle("%u members • %u racer seats • %u ready",
                       model.member_count, model.seat_count, model.ready_count);
        if (!g_online.adapter.race_admission_enabled) {
            ui::TextSubtleWrapped(
                "Interaction preview only. Start Race is held by the local "
                "rollback release gate; this screen cannot enable it.");
        }
    }
    ui::CardEnd();
    ui::Gap(ui::kGapM);

    const MdkrOnlineViewAction timeoutAction =
        model.timeout.present && g_online.adapter.timeout_expired
            ? model.timeout.primary.action : MDKR_ONLINE_VIEW_ACTION_NONE;
    if (timeoutAction != MDKR_ONLINE_VIEW_ACTION_NONE) {
        ui::CautionBox(model.timeout.title, model.timeout.explanation);
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.timeout.primary, true)) {
            handleAction(model.timeout.primary.action, state);
        }
        ui::Gap(ui::kGapM);
    }

    const bool selectionDrawn = drawSelectionControl(model);
    if (!selectionDrawn && model.primary.action != timeoutAction &&
        drawActionButton(model.primary, true)) {
        handleAction(model.primary.action, state);
    }
    if (model.secondary.visible && model.secondary.action != timeoutAction) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.secondary, false)) {
            handleAction(model.secondary.action, state);
        }
    }
    if (model.cancel.visible) {
        ui::Gap(ui::kGapS);
        if (drawActionButton(model.cancel, false)) {
            handleAction(model.cancel.action, state);
        }
    }

    if (model.kind == MDKR_ONLINE_VIEW_RACING) {
        ui::Gap(ui::kGapM);
        ImGui::BeginDisabled(!g_online.adapter.race_admission_enabled);
        if (ImGui::Button("Finish Preview Race",
                          ImVec2(-1.0f, ui::kBtnSecondary().y))) {
            mdkr_online_fake_finish_race(
                &g_online.adapter, g_online.adapter.revision);
        }
        ui::SpeakFocusedItem(
            "Finish Preview Race",
            g_online.adapter.race_admission_enabled ? "Available" : "Unavailable",
            "A development-only stand-in for a confirmed engine result.");
        ImGui::EndDisabled();
    }
    drawLeaveRaceConfirmation(state);
    drawConnectionDetails(model);
    drawUpdateHelp();

}

}  // namespace

void OnlineRoomPanel_draw(LauncherState &state, LauncherAction &action) {
    (void)action;
    ensureInitialized();
    if (!fakeEnabled() || !g_online.initialized) {
        drawUnavailablePanel();
        return;
    }
    drawFakePanel(state);
}

int OnlineRoom_dumpGalleryContract() {
    const size_t count = mdkr_online_fake_gallery_count();
    const MdkrOnlineCompatibilityV1 compatibility = fakeCompatibility();
    std::printf("online-gallery-v1 count=%zu\n", count);
    for (size_t index = 0u; index < count; ++index) {
        const MdkrOnlineFakeGallerySpec *spec =
            mdkr_online_fake_gallery_at(index);
        MdkrOnlineFakeAdapter adapter{};
        MdkrOnlineViewModel model{};
        if (spec == nullptr ||
            !mdkr_online_fake_init(&adapter, UINT64_C(0x47414c4c) + index,
                                   &compatibility,
                                   spec->requires_race_admission) ||
            !mdkr_online_fake_prepare_gallery(&adapter, spec->slug) ||
            !mdkr_online_fake_view(&adapter, &model)) return 1;
        std::printf(
            "online-gallery-v1 slug=%s kind=%u failure=%u primary=%u "
            "timeout=%u admission=%u timeout-visible=%u\n",
            spec->slug, static_cast<unsigned>(spec->kind),
            static_cast<unsigned>(spec->failure),
            static_cast<unsigned>(spec->primary_action),
            spec->timeout_present ? 1u : 0u,
            spec->requires_race_admission ? 1u : 0u,
            std::strncmp(spec->slug, "timeout-", 8u) == 0 ? 1u : 0u);
        const char *copy[] = {
            spec->slug,
            model.title,
            model.primary.visible ? model.primary.label : "",
            model.secondary.visible ? model.secondary.label : "",
            model.cancel.visible ? model.cancel.label : "",
            adapter.timeout_expired && model.timeout.present &&
                    model.timeout.primary.visible
                ? model.timeout.primary.label : "",
        };
        for (const char *field : copy) {
            if (field == nullptr || std::strchr(field, '|') != nullptr ||
                std::strchr(field, '\n') != nullptr ||
                std::strchr(field, '\r') != nullptr) return 1;
        }
        std::printf("online-gallery-copy-v1|%s|%s|%s|%s|%s|%s\n",
                    copy[0], copy[1], copy[2], copy[3], copy[4], copy[5]);
        std::printf(
            "online-gallery-actions-v1|%s|%u|%u|%u|%u\n",
            spec->slug,
            model.primary.visible
                ? static_cast<unsigned>(model.primary.action) : 0u,
            model.secondary.visible
                ? static_cast<unsigned>(model.secondary.action) : 0u,
            model.cancel.visible
                ? static_cast<unsigned>(model.cancel.action) : 0u,
            adapter.timeout_expired && model.timeout.present &&
                    model.timeout.primary.visible
                ? static_cast<unsigned>(model.timeout.primary.action) : 0u);
    }
    return 0;
}

bool OnlineRoom_smokeActionResult(unsigned action, bool *accepted) {
    if (accepted == nullptr || action == MDKR_ONLINE_VIEW_ACTION_NONE ||
        action > MDKR_ONLINE_VIEW_ACTION_LEAVE_RACE ||
        g_online.completedAction != static_cast<MdkrOnlineViewAction>(action)) {
        return false;
    }
    *accepted = g_online.completedActionAccepted;
    return true;
}
