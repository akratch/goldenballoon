#include "ui_phone_party.h"

#include "app_theme.h"
#include "ui_common.h"
#include "party/native_party_host.h"
#include "a11y_model.h"
#include "qrcodegen.hpp"

#include "imgui.h"
#include "SDL.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace {

std::map<std::string, unsigned> g_seatChoices;
bool g_manageOverlay = false;
std::string g_lastAnnouncement;
std::string g_removeController;
std::string g_removeName;
uint64_t g_inviteCopiedUntilMs = 0u;

void announceState(const MdkrNativePartyView &view) {
    if (!ui::SpeechEnabled()) return;
    unsigned pending = 0u;
    unsigned connected = 0u;
    for (const auto &controller : view.controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Pending) pending++;
        if (controller.phase == MdkrNativePartyControllerPhase::Connected) connected++;
    }
    std::string identity = std::to_string(view.transitionId) + ":" +
        std::to_string(static_cast<unsigned>(view.phase)) + ":" +
        std::to_string(pending) + ":" + std::to_string(connected) + ":" + view.message;
    if (identity == g_lastAnnouncement) return;
    g_lastAnnouncement = std::move(identity);
    char message[MDKR_A11Y_TEXT_MAX] = {};
    if (pending != 0u) {
        std::snprintf(message, sizeof(message),
            "%u phone%s waiting for approval. Compare the pairing phrase on both screens.",
            pending, pending == 1u ? " is" : "s are");
    } else {
        std::snprintf(message, sizeof(message), "%s",
            view.message.empty() ? "Phone controller status updated." : view.message.c_str());
    }
    mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS,
        view.phase == MdkrNativePartyPhase::Error
            ? MDKR_A11Y_PRI_CRITICAL : MDKR_A11Y_PRI_NORMAL, message);
}

const char *statusText(const MdkrNativePartyController &controller) {
    if (controller.commandPending) return "Updating…";
    switch (controller.phase) {
        case MdkrNativePartyControllerPhase::Pending: return "Waiting for approval";
        case MdkrNativePartyControllerPhase::Approved: return "Approved";
        case MdkrNativePartyControllerPhase::Leased:
            return "Reconnecting — controls neutral";
        case MdkrNativePartyControllerPhase::Connected:
            return controller.haptics ? "Connected • vibration ready" : "Connected";
    }
    return "Unavailable";
}

unsigned firstFreeSeat(const MdkrNativePartyView &view) {
    std::array<bool, 4> occupied{};
    for (const auto &controller : view.controllers) {
        if (controller.phase != MdkrNativePartyControllerPhase::Pending &&
            controller.seat >= 1u && controller.seat <= 4u) {
            occupied[controller.seat - 1u] = true;
        }
    }
    for (unsigned seat = 1u; seat <= 4u; seat++) {
        if (!occupied[seat - 1u]) return seat;
    }
    return 0u;
}

bool seatOccupied(const MdkrNativePartyView &view, unsigned seat) {
    return std::any_of(view.controllers.begin(), view.controllers.end(),
        [seat](const MdkrNativePartyController &candidate) {
            return candidate.phase != MdkrNativePartyControllerPhase::Pending &&
                candidate.seat == seat;
        });
}

void drawQr(const std::string &url) {
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            url.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
        const float available = ImGui::GetContentRegionAvail().x;
        const float size = (std::max)(64.0f, (std::min)(
            360.0f * AppTheme::uiScale(), available));
        const int quiet = 4;
        const int modules = qr.getSize() + quiet * 2;
        const float pixel = std::floor(size / static_cast<float>(modules));
        const float actual = pixel * static_cast<float>(modules);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + actual, origin.y + actual),
                            IM_COL32_WHITE);
        for (int y = 0; y < qr.getSize(); y++) {
            for (int x = 0; x < qr.getSize(); x++) {
                if (!qr.getModule(x, y)) continue;
                const float left = origin.x + (x + quiet) * pixel;
                const float top = origin.y + (y + quiet) * pixel;
                draw->AddRectFilled(ImVec2(left, top),
                    ImVec2(left + pixel, top + pixel), IM_COL32_BLACK);
            }
        }
        ImGui::Dummy(ImVec2(actual, actual));
    } catch (...) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("The QR code could not be rendered. Use the 6-digit code below.");
        ImGui::PopStyleColor();
    }
}

void drawInvite(MdkrNativePartyHost &host) {
    const MdkrNativePartyView &view = host.view();
    if (!view.inviteVisible) {
        ImGui::TextUnformatted("Invite closed");
        ui::TextSubtleWrapped(
            "Connected phones keep their seats. Create a fresh code when someone else joins.");
        if (ImGui::Button("Create New Invite", ui::kBtnWide())) host.rotateInvite();
        return;
    }
    drawQr(view.controllerUrl);
    ui::Gap(ui::kGapS);
    ImGui::PushFont(AppTheme::fonts().title);
    ImGui::Text("Code  %s", view.fallbackCode.c_str());
    ImGui::PopFont();
    const uint64_t now = static_cast<uint64_t>(SDL_GetTicks64());
    const uint64_t seconds = view.inviteExpiresAtMs > now
        ? (view.inviteExpiresAtMs - now + 999u) / 1000u : 0u;
    ui::TextSubtle("Expires in %llu:%02llu",
        static_cast<unsigned long long>(seconds / 60u),
        static_cast<unsigned long long>(seconds % 60u));
    ui::TextSubtleWrapped(
        "Open the camera on a phone and scan. No app, account, microphone, or camera permission is needed on this display.");
    if (ImGui::Button("Copy Invite Link", ui::kBtnSecondary())) {
        ImGui::SetClipboardText(view.controllerUrl.c_str());
        g_inviteCopiedUntilMs = SDL_GetTicks64() + 2000u;
        mdkr_a11y_announce(MDKR_A11Y_CAT_STATUS, MDKR_A11Y_PRI_NORMAL,
                          "Phone controller invite copied.");
    }
    ui::SpeakFocusedItem("Copy Invite Link", view.fallbackCode.c_str(),
        "Copies this short-lived private invite. The same 6-digit code is visible above.");
    if (ImGui::GetContentRegionAvail().x >
        ui::kBtnSecondary().x + ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Close Invite", ui::kBtnSecondary())) host.dismissInvite();
    ui::SpeakFocusedItem("Close Invite", nullptr,
        "Stops new phones from joining. Connected phones keep their seats.");
    if (SDL_GetTicks64() < g_inviteCopiedUntilMs) {
        ui::TextSubtle("Invite copied");
    }
}

void drawPending(MdkrNativePartyHost &host,
                 const MdkrNativePartyController &controller) {
    const MdkrNativePartyView &view = host.view();
    if (ui::CardBegin(("##pending-" + controller.id).c_str(),
                      AppTheme::accent(), 0.0f)) {
        ImGui::TextWrapped("%s", controller.name.empty()
            ? "Phone controller" : controller.name.c_str());
        ui::TextSubtle("Compare on both screens:");
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::TextUnformatted(controller.pairingPhrase.empty()
            ? "Verifying…" : controller.pairingPhrase.c_str());
        ImGui::PopFont();
        unsigned &choice = g_seatChoices[controller.id];
        if (choice < 1u || choice > 4u || seatOccupied(view, choice)) {
            choice = firstFreeSeat(view);
        }
        ImGui::SetNextItemWidth(ui::kControlWidth());
        const char *preview = choice == 0u ? "No free phone slot" : nullptr;
        char previewBuffer[32] = {};
        if (choice != 0u) {
            std::snprintf(previewBuffer, sizeof(previewBuffer), "Controller %u", choice);
            preview = previewBuffer;
        }
        if (ImGui::BeginCombo("Controller slot", preview)) {
            for (unsigned seat = 1u; seat <= 4u; seat++) {
                const bool occupied = seatOccupied(view, seat);
                if (occupied) ImGui::BeginDisabled();
                char label[48] = {};
                std::snprintf(label, sizeof(label), "Controller %u%s", seat,
                              occupied ? " — phone in use" : " — assign phone");
                if (ImGui::Selectable(label, choice == seat) && !occupied) choice = seat;
                if (occupied) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ui::SpeakFocusedItem("Controller Slot", preview,
            "Choose which local controller slot this phone will use.");
        ui::TextSubtleWrapped(
            "The phone takes this numbered slot. Any keyboard, gamepad, or touch source there moves out of the way; other slots are unchanged.");
        const bool disabled = controller.commandPending ||
            controller.pairingPhrase.empty() || choice == 0u;
        if (disabled) ImGui::BeginDisabled();
        if (ui::PrimaryButton("Approve Matching Phone", ui::kBtnWide())) {
            host.approve(controller.id, choice);
        }
        ui::SpeakFocusedItem("Approve Matching Phone",
            controller.pairingPhrase.c_str(),
            "Approve only when this phrase matches the phone exactly.");
        if (disabled) ImGui::EndDisabled();
        if (ImGui::Button("Decline", ui::kBtnSecondary())) host.reject(controller.id);
        ui::SpeakFocusedItem("Decline Phone", nullptr,
            "Removes this pending phone without assigning a controller slot.");
    }
    ui::CardEnd();
}

void drawControllers(MdkrNativePartyHost &host) {
    const MdkrNativePartyView &view = host.view();
    for (const auto &controller : view.controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Pending) {
            drawPending(host, controller);
            ui::Gap(ui::kGapS);
            continue;
        }
        ImGui::PushID(controller.id.c_str());
        ImGui::Text("Controller %u  %s", controller.seat,
                    controller.name.empty() ? "Phone" : controller.name.c_str());
        ui::TextSubtle("%s", statusText(controller));
        if (ImGui::Button("Remove Phone", ui::kBtnSecondary())) {
            g_removeController = controller.id;
            g_removeName = controller.name.empty() ? "this phone" : controller.name;
            ImGui::OpenPopup("Remove phone controller?");
        }
        ui::SpeakFocusedItem("Remove Phone", statusText(controller),
            "Disconnects this phone and returns its controls to neutral.");
        ImGui::PopID();
        ui::Gap(ui::kGapS);
    }
    ui::ConfirmModalSize();
    if (ImGui::BeginPopupModal("Remove phone controller?", nullptr,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped(
            "Remove %s? Its controls will become neutral and its controller slot will become available.",
            g_removeName.c_str());
        if (ImGui::Button("Keep Phone", ui::kBtnSecondary())) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::GetContentRegionAvail().x >
            ui::kBtnSecondary().x + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Remove Phone", ui::kBtnSecondary())) {
            host.reject(g_removeController);
            g_removeController.clear();
            g_removeName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void drawRoom(MdkrNativePartyHost &host) {
    const MdkrNativePartyView &view = host.view();
    if (view.phase == MdkrNativePartyPhase::Opening ||
        view.phase == MdkrNativePartyPhase::Recovering) {
        ImGui::TextUnformatted(view.phase == MdkrNativePartyPhase::Opening
            ? "Opening secure room…" : "Reconnecting securely…");
    } else if (view.phase == MdkrNativePartyPhase::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", view.message.c_str());
        ImGui::PopStyleColor();
    } else {
        if (!view.message.empty()) ui::TextSubtleWrapped("%s", view.message.c_str());
        drawInvite(host);
        if (!view.controllers.empty()) {
            ui::Gap(ui::kGapM);
            ImGui::SeparatorText("Phones");
            drawControllers(host);
        }
    }
}

void drawOpenButton(MdkrNativePartyHost &host, const char *serviceOrigin) {
    const bool configured = serviceOrigin != nullptr && serviceOrigin[0] != '\0';
    if (!configured) ImGui::BeginDisabled();
    if (ui::PrimaryButton("Add Phone Controllers", ui::kBtnWide())) {
        host.open(serviceOrigin);
    }
    if (!configured) ImGui::EndDisabled();
    if (!configured) {
        ui::TextSubtleWrapped(
            "The optional zero-cost Party service is not configured in this build. Keyboard and gamepads remain fully local.");
    }
}

void drawCloseRoom(MdkrNativePartyHost &host) {
    if (host.view().phase == MdkrNativePartyPhase::Closed) return;
    ui::Gap(ui::kGapM);
    if (ImGui::Button("Close Phone Controller Room", ui::kBtnSecondary())) {
        ImGui::OpenPopup("Close phone controller room?");
    }
    ui::ConfirmModalSize();
    if (ImGui::BeginPopupModal("Close phone controller room?", nullptr,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped(
            "All phones will disconnect and their controls will become neutral. Local controllers are unchanged.");
        if (ImGui::Button("Cancel", ui::kBtnSecondary())) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Close Room", ui::kBtnSecondary())) {
            host.closeRoom();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void drawFull(MdkrNativePartyHost &host, const char *serviceOrigin) {
    announceState(host.view());
    if (host.view().phase == MdkrNativePartyPhase::Closed) {
        drawOpenButton(host, serviceOrigin);
    } else if (host.view().phase == MdkrNativePartyPhase::Error) {
        drawRoom(host);
        ui::Gap(ui::kGapS);
        drawOpenButton(host, serviceOrigin);
    } else {
        drawRoom(host);
    }
    drawCloseRoom(host);
}

}  // namespace

void PhoneParty_drawLauncher(MdkrNativePartyHost &host,
                             const char *serviceOrigin) {
    ui::Gap(ui::kGapL);
    ImGui::Separator();
    ui::Gap(ui::kGapM);
    ui::SectionHeader("Play Together Here",
        "Turn any phone into a private browser controller. Scan, compare the pairing phrase, approve a slot, and play on this screen.");
    drawFull(host, serviceOrigin);
}

void PhoneParty_drawOverlay(MdkrNativePartyHost &host,
                            const char *serviceOrigin) {
    ui::Gap(ui::kGapM);
    const auto &view = host.view();
    unsigned connected = 0u;
    for (const auto &controller : view.controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Connected) connected++;
    }
    const char *label = view.phase == MdkrNativePartyPhase::Closed
        ? "Add Phone Controllers" : "Manage Phone Controllers";
    if (ImGui::Button(label, ui::kBtnWide())) g_manageOverlay = true;
    ui::SpeakFocusedItem(label, nullptr,
        "Open the phone controller room without leaving the game.");
    ui::TextSubtle("%u connected", connected);
    if (g_manageOverlay) ImGui::OpenPopup("Phone Controllers");
    if (ImGui::BeginPopupModal("Phone Controllers", &g_manageOverlay,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Phone Controllers");
        ui::TextSubtleWrapped(
            "Phones connect directly to this game. When local pause is available, the race stays paused while this menu is open.");
        ui::Gap(ui::kGapS);
        const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
        const float margin = 80.0f * AppTheme::uiScale();
        const ImVec2 contentSize(
            (std::max)(120.0f, (std::min)(520.0f * AppTheme::uiScale(),
                                         work.x - margin)),
            (std::max)(120.0f, (std::min)(520.0f * AppTheme::uiScale(),
                                         work.y - margin - 100.0f * AppTheme::uiScale())));
        ImGui::BeginChild("##party-overlay-scroll", contentSize, true);
        drawFull(host, serviceOrigin);
        ui::TouchScrollCurrentWindow();
        ImGui::EndChild();
        if (ImGui::Button("Done", ui::kBtnSecondary())) {
            g_manageOverlay = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
