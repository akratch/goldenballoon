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
/* F5: join/drop toast state, latched by the same announceState hook that
 * already narrates seat events for accessibility -- one event source, two
 * surfaces. Primed after the first observation so opening the overlay onto
 * an existing room never fires a stale toast. */
std::string g_toastText;
uint64_t g_toastUntilMs = 0u;
unsigned g_lastConnectedCount = 0u;
bool g_presencePrimed = false;
constexpr uint64_t kToastMs = 4000u;
/* Item 4: a new phone waiting for approval is the join-request cue. With no
 * launcher audio path, the cue is visual: the pending card pulses and the
 * in-game card shows a brief attention line, armed here once per NEW pending id
 * (mdkr_party_note_new_pending) and cleared after kToastMs. */
MdkrPartyPendingAttention g_pendingAttention;
uint64_t g_pendingAttentionUntilMs = 0u;

/* Item 3 (native): the invite QR enlarges to a modal so a phone camera can scan
 * from across the room. Display-only ImGui state; no model change. */
bool g_qrEnlarged = false;

/* MDKR_APP_PARTY_TRACE=1: one line, once per process, on the first frame the
 * party surface actually commits draws — the packaged-build lanes assert the
 * surface exists in the shipped launcher without a phone or a ROM. The flags are
 * set AT the draw sites, so a frame that skipped the invite/QR/code cannot
 * report them. cloud/local name WHICH surface drew: the release lanes rely on it
 * to prove an origin-less build shows the local-play card (allowed) but never a
 * cloud/online card (which still needs a compiled origin). */
bool g_traceDrewInvite = false;
bool g_traceDrewQr = false;
bool g_traceDrewCode = false;
bool g_traceDrewCloud = false;
bool g_traceDrewLocal = false;

const char *partyPhaseName(MdkrNativePartyPhase phase) {
    switch (phase) {
        case MdkrNativePartyPhase::Closed: return "closed";
        case MdkrNativePartyPhase::Opening: return "opening";
        case MdkrNativePartyPhase::Open: return "open";
        case MdkrNativePartyPhase::InviteRevoked: return "invite-revoked";
        case MdkrNativePartyPhase::Recovering: return "recovering";
        case MdkrNativePartyPhase::Error: return "error";
        case MdkrNativePartyPhase::RoomEnded: return "room-ended";
    }
    return "unknown";
}

void partyTraceEmitOnce(MdkrNativePartyHost &host, const char *serviceOrigin) {
    static bool emitted = false;
    const char *armed = std::getenv("MDKR_APP_PARTY_TRACE");
    if (emitted || armed == nullptr || armed[0] != '1') return;
    emitted = true;
    std::printf("[app] party: origin=%s transport=%s phase=%s "
                "invite=%d qr=%d code=%d cloud=%d local=%d\n",
                serviceOrigin != nullptr && serviceOrigin[0] != '\0'
                    ? serviceOrigin : "unset",
                host.transportAvailable() ? "available" : "unavailable",
                partyPhaseName(host.view().phase),
                g_traceDrewInvite ? 1 : 0, g_traceDrewQr ? 1 : 0,
                g_traceDrewCode ? 1 : 0, g_traceDrewCloud ? 1 : 0,
                g_traceDrewLocal ? 1 : 0);
    std::fflush(stdout);
}


/* Counts both surfaces need: pending approvals and connected phones. */
void partyPresence(const MdkrNativePartyView &view,
                   unsigned &pending, unsigned &connected) {
    pending = 0u;
    connected = 0u;
    for (const auto &controller : view.controllers) {
        if (controller.phase == MdkrNativePartyControllerPhase::Pending) pending++;
        if (controller.phase == MdkrNativePartyControllerPhase::Connected) connected++;
    }
}

void announceState(const MdkrNativePartyView &view) {
    unsigned pending = 0u;
    unsigned connected = 0u;
    partyPresence(view, pending, connected);
    /* F5: which room phases are a live room (join/drop toast + pending cue). */
    const bool liveRoom = view.phase == MdkrNativePartyPhase::Open ||
        view.phase == MdkrNativePartyPhase::InviteRevoked ||
        view.phase == MdkrNativePartyPhase::Recovering;
    /* Item 4: arm the visual join-request cue once per NEW pending phone. Run
     * every frame (before the announcement dedup below) so a fast pending swap
     * that leaves the count unchanged is never missed; the helper primes on its
     * first observation so opening onto an existing room stays quiet. */
    const unsigned freshPending =
        mdkr_party_note_new_pending(g_pendingAttention, view.controllers);
    if (freshPending != 0u && liveRoom) {
        g_pendingAttentionUntilMs = SDL_GetTicks64() + kToastMs;
    }
    std::string identity = std::to_string(view.transitionId) + ":" +
        std::to_string(static_cast<unsigned>(view.phase)) + ":" +
        std::to_string(pending) + ":" + std::to_string(connected) + ":" + view.message;
    if (identity == g_lastAnnouncement) return;
    g_lastAnnouncement = std::move(identity);
    /* F5: the same seat events this hook already narrates for
     * accessibility surface as a brief visual toast on the in-game card --
     * latched here so no second event source exists. Only a LIVE room's
     * join/drop is a toast: a room the player closed (or that ended) says
     * what happened on its own surface. */
    if (g_presencePrimed && liveRoom && connected != g_lastConnectedCount) {
        g_toastText = connected > g_lastConnectedCount
            ? "Phone controller connected."
            : "Phone controller dropped; its controls are neutral.";
        g_toastUntilMs = SDL_GetTicks64() + kToastMs;
    }
    g_lastConnectedCount = connected;
    g_presencePrimed = true;
    if (!ui::SpeechEnabled()) return;
    char message[MDKR_A11Y_TEXT_MAX] = {};
    if (pending != 0u) {
        std::snprintf(message, sizeof(message),
            "%u phone%s waiting for approval. The pairing phrase appears on "
            "both screens when the phone connects.",
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
    /* I2: a version-mismatched phone must never read as the "Reconnecting"
     * its Leased phase would otherwise show -- reconnecting is a promise the
     * transport keeps, while this state only the player can fix. Same
     * sentence as the room message, from the one shared constant. */
    if (controller.protocolMismatch) return kMdkrPartyProtocolMismatchCopy;
    /* F2: a lease that has never reached Connected is connecting for the
     * first time -- "Reconnecting" would promise a recovery of something
     * that never existed. Only a lease that connected and then dropped may
     * say so. */
    if (!controller.everConnected &&
        (controller.phase == MdkrNativePartyControllerPhase::Approved ||
         controller.phase == MdkrNativePartyControllerPhase::Leased)) {
        return "Connecting…";
    }
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

// Returns true when the QR area was clicked (item 3: enlarge for scanning).
bool drawQr(const std::string &url, float maxSize) {
    try {
        g_traceDrewQr = true;
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            url.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
        const float available = ImGui::GetContentRegionAvail().x;
        const float size = (std::max)(64.0f, (std::min)(maxSize, available));
        const int quiet = 4;
        const int modules = qr.getSize() + quiet * 2;
        const float pixel = std::floor(size / static_cast<float>(modules));
        const float actual = pixel * static_cast<float>(modules);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        // The quiet zone and modules read from the theme's fixed QR pair rather
        // than raw IM_COL32 literals: app_theme owns these two values precisely
        // so a light-on-dark launcher cannot accidentally recolour a code a
        // phone camera has to read.
        const ImU32 light = ImGui::GetColorU32(AppTheme::qrLight());
        const ImU32 dark = ImGui::GetColorU32(AppTheme::qrDark());
        draw->AddRectFilled(origin, ImVec2(origin.x + actual, origin.y + actual),
                            light);
        for (int y = 0; y < qr.getSize(); y++) {
            for (int x = 0; x < qr.getSize(); x++) {
                if (!qr.getModule(x, y)) continue;
                const float left = origin.x + (x + quiet) * pixel;
                const float top = origin.y + (y + quiet) * pixel;
                draw->AddRectFilled(ImVec2(left, top),
                    ImVec2(left + pixel, top + pixel), dark);
            }
        }
        // The QR occupies its own space AND is the click target that enlarges
        // it; an InvisibleButton over the drawn modules keeps the whole code
        // clickable without recoloring or overdrawing it.
        const bool clicked = ImGui::InvisibleButton("##qr", ImVec2(actual, actual));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return clicked;
    } catch (...) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("The QR code could not be rendered. Use the 6-digit code below.");
        ImGui::PopStyleColor();
    }
    return false;
}

/* Item 3: the enlarge modal. Redraws the SAME invite URL as large as the
 * viewport allows so a phone camera scans from across the room. Display-only:
 * the invite and its capability are untouched. Dismissed by the window close
 * box, Escape (both via the &open flag), or the Close button. */
void drawQrEnlargeModal(const std::string &url) {
    if (!g_qrEnlarged) return;
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    const float big = (std::max)(160.0f, (std::min)(work.x, work.y) * 0.8f);
    ImGui::SetNextWindowSize(ImVec2(big + 80.0f * AppTheme::uiScale(), 0.0f),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Scan from across the room", &g_qrEnlarged,
                               ImGuiWindowFlags_NoResize)) {
        drawQr(url, big);
        ui::TextSubtleWrapped(
            "Point a phone camera at this code from anywhere in the room.");
        if (ImGui::Button("Close", ui::kBtnSecondary())) {
            g_qrEnlarged = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void drawInvite(MdkrNativePartyHost &host) {
    const MdkrNativePartyView &view = host.view();
    g_traceDrewInvite = view.inviteVisible;
    if (!view.inviteVisible) {
        g_qrEnlarged = false;
        ImGui::TextUnformatted("Invite closed");
        ui::TextSubtleWrapped(
            "Connected phones keep their seats. Create a fresh code when someone else joins.");
        if (ImGui::Button("Create New Invite", ui::kBtnWide())) host.rotateInvite();
        return;
    }
    if (drawQr(view.controllerUrl, 360.0f * AppTheme::uiScale())) {
        g_qrEnlarged = true;
        ImGui::OpenPopup("Scan from across the room");
    }
    ui::TextSubtle("Click the code to enlarge it for scanning across the room.");
    drawQrEnlargeModal(view.controllerUrl);
    ui::Gap(ui::kGapS);
    ImGui::PushFont(AppTheme::fonts().title);
    g_traceDrewCode = true;
    /* F11: two groups of three, display only -- the phone's entry field
     * groups the same way, and every input path still takes six digits. */
    const std::string groupedCode =
        mdkr_party_grouped_fallback_code(view.fallbackCode);
    ImGui::Text("Code  %s", groupedCode.c_str());
    ImGui::PopFont();
    const uint64_t now = static_cast<uint64_t>(SDL_GetTicks64());
    /* F6: this frame really drew the QR and code, so tell the host -- while
     * the card stays on screen it rotates the invite before its TTL lapses,
     * keeping the displayed code redeemable. The rotated invite replaces
     * QR, code and countdown right here in place. */
    host.noteInviteDisplayed(now);
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
    ui::SpeakFocusedItem("Copy Invite Link", groupedCode.c_str(),
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
    /* Item 4: while the join-request cue is armed, pulse this card's border
     * between the accent and the gameplay-warn color so a host looking away
     * notices the phone that just arrived. Purely visual; fades after kToastMs. */
    ImVec4 border = AppTheme::accent();
    if (SDL_GetTicks64() < g_pendingAttentionUntilMs) {
        const float phase =
            static_cast<float>(SDL_GetTicks64() % 900u) / 900.0f;
        const float pulse = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
        const ImVec4 hot = AppTheme::warn();
        border.x += (hot.x - border.x) * pulse;
        border.y += (hot.y - border.y) * pulse;
        border.z += (hot.z - border.z) * pulse;
    }
    if (ui::CardBegin(("##pending-" + controller.id).c_str(), border, 0.0f)) {
        ImGui::TextWrapped("%s", controller.name.empty()
            ? "Phone controller" : controller.name.c_str());
        /* SAS v2: the pairing phrase binds the phone's direct connection, so
         * it cannot exist before that connection does. The compare surface
         * moved to the seat row (drawControllers below); this card only says
         * when to expect it -- naming the order (approve first, compare next)
         * so the button below does not read as "match a phrase" that is not on
         * screen yet. */
        ui::TextSubtleWrapped(
            "Pick a slot and approve. After the phone connects, its pairing "
            "phrase appears to compare — choose Words Match to give it a "
            "controller.");
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
        ui::SpeakFocusedItem("Controller slot", preview,
            "Choose which local controller slot this phone will use.");
        ui::TextSubtleWrapped(
            "The phone takes this numbered slot. Any keyboard, gamepad, or touch source there moves out of the way; other slots are unchanged.");
        if (choice == 0u) {
            /* A disabled Approve with no reason is a dead end: say why, and
             * point at the fix (the connected phones are drawn just below). */
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ui::TextSubtleWrapped(
                "All four controller slots are taken. Remove a connected phone "
                "below to free one.");
            ImGui::PopStyleColor();
        }
        const bool disabled = controller.commandPending || choice == 0u;
        if (disabled) ImGui::BeginDisabled();
        if (ui::PrimaryButton("Approve This Phone", ui::kBtnWide())) {
            host.approve(controller.id, choice);
        }
        ui::SpeakFocusedItem("Approve This Phone", nullptr,
            "Approves this phone into the chosen slot. After it connects, a "
            "pairing phrase appears on both screens — compare them, then choose "
            "Words Match to give it a controller or Words Differ to remove it.");
        if (disabled) ImGui::EndDisabled();
        if (ImGui::Button("Decline", ui::kBtnSecondary())) host.reject(controller.id);
        ui::SpeakFocusedItem("Decline", nullptr,
            "Removes this pending phone without assigning a controller slot.");
    }
    ui::CardEnd();
}

/* P2.1 compare-then-trust: a phone the host approved holds a PROVISIONAL
 * connection only -- its direct channel is up and its channel-bound phrase is
 * on screen, but it holds no seat and its input is discarded at the ingress
 * until the human compares the words and grants the seat. This card is that
 * compare surface: it leads with the phrase and offers the Words Match /
 * Words Differ decision, mirroring the online room's phrase ceremony. */
void drawProvisional(MdkrNativePartyHost &host,
                     const MdkrNativePartyController &controller) {
    if (ui::CardBegin(("##provisional-" + controller.id).c_str(),
                      AppTheme::accent(), 0.0f)) {
        ImGui::Text("Controller %u  %s", controller.seat,
                    controller.name.empty() ? "Phone" : controller.name.c_str());
        if (controller.pairingPhrase.empty()) {
            /* The channel is coming up; the phrase binds it and appears here to
             * compare. No words yet means nothing to match, so no decision is
             * offered -- only a way to remove a phone that will not connect. */
            ui::TextSubtle("%s", statusText(controller));
            ui::TextSubtleWrapped(
                "The pairing phrase to compare appears here once the phone "
                "connects.");
            if (ImGui::Button("Remove Phone", ui::kBtnSecondary())) {
                host.reject(controller.id);
            }
            ui::SpeakFocusedItem("Remove Phone", nullptr,
                "Removes this phone before it takes a controller.");
            ui::CardEnd();
            return;
        }
        ui::TextSubtle("Compare on both screens:");
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::TextUnformatted(controller.pairingPhrase.c_str());
        ImGui::PopFont();
        ui::TextSubtleWrapped(
            "Give this phone its controller only if the words match exactly on "
            "both screens.");
        const bool busy = controller.commandPending;
        if (busy) ImGui::BeginDisabled();
        if (ui::PrimaryButton("Words Match", ui::kBtnWide())) {
            host.confirmPairing(controller.id);
        }
        ui::SpeakFocusedItem("Words Match", controller.pairingPhrase.c_str(),
            "The words match on both screens. Gives this phone its controller "
            "and starts its input.");
        if (busy) ImGui::EndDisabled();
        if (ImGui::Button("Words Differ", ui::kBtnSecondary())) {
            host.reject(controller.id);
        }
        ui::SpeakFocusedItem("Words Differ", nullptr,
            "The words are not the same. Removes this phone; it never takes a "
            "controller.");
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
        /* P2.1: an approved-but-unconfirmed phone is provisional -- draw the
         * compare-and-confirm card instead of an occupied seat row. Only once
         * the human's Words Match granted the seat does it read as a normal
         * connected controller. */
        if (!controller.confirmed) {
            ImGui::PushID(controller.id.c_str());
            drawProvisional(host, controller);
            ImGui::PopID();
            ui::Gap(ui::kGapS);
            continue;
        }
        ImGui::PushID(controller.id.c_str());
        ImGui::Text("Controller %u  %s", controller.seat,
                    controller.name.empty() ? "Phone" : controller.name.c_str());
        ui::TextSubtle("%s", statusText(controller));
        /* RTT: the newest control-channel round trip, refreshed per pong.
         * Only a live direct channel gets a number -- the model clears it
         * the moment that channel ends or demotes. */
        if (controller.phase == MdkrNativePartyControllerPhase::Connected &&
            controller.rttMs != 0u) {
            ui::TextSubtle("%u ms · direct", controller.rttMs);
        }
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
    if (view.phase == MdkrNativePartyPhase::Opening) {
        ImGui::TextUnformatted("Opening secure room…");
    } else if (view.phase == MdkrNativePartyPhase::Recovering) {
        /* F7: the ROOM connection is recovering; the seats are not gone.
         * Connected phones keep working over their direct channels (a fact
         * the transports guarantee -- recovery never touches a live peer),
         * so the roster stays on screen under an honest banner instead of
         * vanishing into a bare status line. No seat state is invented:
         * each row still shows exactly what the model knows. */
        ImGui::TextUnformatted("Reconnecting securely…");
        if (!view.message.empty()) ui::TextSubtleWrapped("%s", view.message.c_str());
        ui::TextSubtleWrapped(
            "Connected phones keep working over their direct connections. "
            "New phones can join once the room returns.");
        if (!view.controllers.empty()) {
            ui::Gap(ui::kGapM);
            ImGui::SeparatorText("Phones");
            drawControllers(host);
        }
    } else if (view.phase == MdkrNativePartyPhase::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
        ImGui::TextWrapped("%s", view.message.c_str());
        ImGui::PopStyleColor();
    } else if (view.phase == MdkrNativePartyPhase::RoomEnded) {
        /* I4 terminal state: the room is over, not broken -- plain text,
         * not the error color. The one sentence (view.message, set from
         * kMdkrPartyRoomEndedCopy) names the way forward; drawFull puts the
         * Create New Invite button right under it. */
        ImGui::TextWrapped("%s", view.message.c_str());
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

void drawOpenButton(MdkrNativePartyHost &host, const char *serviceOrigin,
                    const char *label = "Add Phone Controllers") {
    const bool configured = serviceOrigin != nullptr && serviceOrigin[0] != '\0';
    if (!configured) ImGui::BeginDisabled();
    if (ui::PrimaryButton(label, ui::kBtnWide())) {
        host.open(serviceOrigin);
    }
    if (!configured) ImGui::EndDisabled();
    if (!configured) {
        ui::TextSubtleWrapped(
            "The optional zero-cost Party service is not configured in this build. Keyboard and gamepads remain fully local.");
    }
}

void drawCloseRoom(MdkrNativePartyHost &host) {
    /* An ended room (I4) is already over -- a close button beside "Create
     * New Invite" would offer a second way to do nothing. Only the fresh
     * invite moves forward from RoomEnded. */
    if (host.view().phase == MdkrNativePartyPhase::Closed ||
        host.view().phase == MdkrNativePartyPhase::RoomEnded) {
        return;
    }
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

/* M3: g_seatChoices remembers each pending phone's chosen slot by
 * controller id, and previously only ever grew -- decline a hundred phones
 * over a long party evening and all hundred choices stayed resident for the
 * process's life. A choice is only meaningful while its controller is in
 * the roster, so drop the rest here; a phone that pairs again arrives under
 * a fresh id and simply gets a fresh default slot. (No UI test harness
 * exists to pin this; the transport-side twin of this prune is pinned by
 * tests/test_native_party_sas.cpp.) */
void pruneSeatChoices(const MdkrNativePartyView &view) {
    for (auto iterator = g_seatChoices.begin();
         iterator != g_seatChoices.end();) {
        const std::string &id = iterator->first;
        const bool present = std::any_of(
            view.controllers.begin(), view.controllers.end(),
            [&id](const MdkrNativePartyController &controller) {
                return controller.id == id;
            });
        if (present) ++iterator;
        else iterator = g_seatChoices.erase(iterator);
    }
}

/* Said once when local play starts, and again in the entry card: the OS asks
 * to allow incoming connections the first time the port opens, and the player
 * has to say yes for phones to reach the game. Kept concrete and short. */
const char *kLanFirewallGuidance =
    "The first time you start this, macOS or Windows may ask whether to allow "
    "incoming connections. Choose Allow so phones on your network can join.";

/* The live local-play surface: the same QR / six-digit code / pairing-phrase
 * flow the cloud room shows (drawRoom, reused whole), plus the firewall line and
 * a Stop that hands the launcher the revert-to-default request. Only drawn while
 * the LAN transport is the live one, so the cloud card is never on screen at the
 * same time -- the on-screen half of the runtime mutual exclusion. */
void drawLanActive(MdkrNativePartyHost &host, PhonePartyLanControls &lan) {
    const MdkrNativePartyView &view = host.view();
    if (view.phase != MdkrNativePartyPhase::Closed) {
        if (view.phase == MdkrNativePartyPhase::Opening ||
            view.phase == MdkrNativePartyPhase::Open ||
            view.phase == MdkrNativePartyPhase::InviteRevoked) {
            ui::TextSubtleWrapped("%s", kLanFirewallGuidance);
            ui::Gap(ui::kGapS);
        }
        drawRoom(host);
    } else {
        ui::TextSubtle("Local play stopped.");
    }
    ui::Gap(ui::kGapM);
    if (ImGui::Button("Stop Local Play", ui::kBtnSecondary())) {
        /* Say goodbye to the phones now (closeRoom), then ask the launcher to
         * release the port and return to the default transport after the frame
         * -- the surface is still drawing from this host. */
        host.closeRoom();
        lan.request = PhonePartyLanControls::Request::Stop;
    }
    ui::SpeakFocusedItem("Stop Local Play", nullptr,
        "Ends the local controller room and releases the network port. Phones "
        "disconnect; keyboard and gamepads are unchanged.");
}

/* The entry card beneath the cloud card (or standing alone in a build with no
 * cloud origin): the one place a player starts local play. A build with no LAN
 * address shows the reason instead of a dead button -- fail closed. */
void drawLanEntryCard(PhonePartyLanControls &lan) {
    ui::Gap(ui::kGapM);
    // Keep the entry in the parent's navigation and scrolling domain. An
    // off-screen NavFlattened child cannot be scored by directional navigation,
    // leaving gamepad users unable to reach this action in short windows.
    ImGui::PushID("##lan-entry");
    ImGui::BeginGroup();
    {
        ImGui::PushFont(AppTheme::fonts().title);
        ImGui::TextUnformatted("Local play (no internet)");
        ImGui::PopFont();
        ui::TextSubtleWrapped(
            "Start a controller room on your own Wi-Fi or wired network. Phones "
            "scan a code and connect straight to this game — no internet, no "
            "account, no app.");
        if (!lan.available) {
            /* The launcher always sets an honest reason (no network, or the
             * controller page is not in this build) when unavailable. */
            if (lan.unavailableReason != nullptr &&
                lan.unavailableReason[0] != '\0') {
                ui::TextSubtleWrapped("%s", lan.unavailableReason);
            }
        } else {
            if (ui::PrimaryButton("Start Local Play", ui::kBtnWide())) {
                lan.request = PhonePartyLanControls::Request::Start;
            }
            ui::SpeakFocusedItem("Start Local Play", nullptr,
                "Starts a controller room on this network. Phones scan a code to "
                "join; no internet is used.");
            ui::TextSubtleWrapped("%s", kLanFirewallGuidance);
        }
        if (lan.note != nullptr && lan.note[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::bad());
            ui::TextSubtleWrapped("%s", lan.note);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void drawFull(MdkrNativePartyHost &host, const char *serviceOrigin) {
    pruneSeatChoices(host.view());
    announceState(host.view());
    if (host.view().phase == MdkrNativePartyPhase::Closed) {
        drawOpenButton(host, serviceOrigin);
    } else if (host.view().phase == MdkrNativePartyPhase::Error) {
        drawRoom(host);
        ui::Gap(ui::kGapS);
        drawOpenButton(host, serviceOrigin);
    } else if (host.view().phase == MdkrNativePartyPhase::RoomEnded) {
        /* I4: the ended room's copy promises exactly this button. It is the
         * same fresh open() the Error surface offers, labeled with the
         * remedy the sentence names. */
        drawRoom(host);
        ui::Gap(ui::kGapS);
        drawOpenButton(host, serviceOrigin, "Create New Invite");
    } else {
        drawRoom(host);
    }
    drawCloseRoom(host);
}

}  // namespace

bool PhoneParty_availableInBuild(const char *serviceOrigin) {
    return serviceOrigin != nullptr && serviceOrigin[0] != '\0';
}

void PhoneParty_drawLauncher(MdkrNativePartyHost &host,
                             const char *serviceOrigin,
                             PhonePartyLanControls &lan) {
    ui::Gap(ui::kGapL);
    ImGui::Separator();
    ui::Gap(ui::kGapM);
    ui::SectionHeader("Use a Phone as a Controller",
        "Scan the code with any phone to play with it — just you, or up to four "
        "players on this screen. Approve the phone and it becomes Controller 1 "
        "(or the next open slot); when it connects, compare the pairing phrase "
        "and choose Words Match to give it a controller.");
    /* Exactly one surface at a time. While a local room is the live transport it
     * owns the whole card; otherwise the cloud flow does (unchanged), and local
     * play is offered beneath it only when no cloud room is open -- so a cloud
     * room and a local room can never be started or shown at once. A build with
     * no cloud origin skips the cloud flow entirely and makes local play the
     * whole surface. */
    if (lan.active) {
        g_traceDrewLocal = true;
        drawLanActive(host, lan);
    } else if (PhoneParty_availableInBuild(serviceOrigin)) {
        g_traceDrewCloud = true;
        drawFull(host, serviceOrigin);
        if (host.view().phase == MdkrNativePartyPhase::Closed) {
            g_traceDrewLocal = true;
            drawLanEntryCard(lan);
        }
    } else {
        g_traceDrewLocal = true;
        drawLanEntryCard(lan);
    }
    partyTraceEmitOnce(host, serviceOrigin);
}

void PhoneParty_drawOverlay(MdkrNativePartyHost &host,
                            const char *serviceOrigin) {
    ui::Gap(ui::kGapM);
    const auto &view = host.view();
    /* F5: run the shared presence hook here too -- with the manage popup
     * closed, drawFull is not running, and the in-game card is the one
     * surface left to notice a join, a drop, or a phone waiting. */
    announceState(view);
    unsigned pending = 0u;
    unsigned connected = 0u;
    partyPresence(view, pending, connected);
    const char *label = view.phase == MdkrNativePartyPhase::Closed
        ? "Add Phone Controllers" : "Manage Phone Controllers";
    if (ImGui::Button(label, ui::kBtnWide())) g_manageOverlay = true;
    ui::SpeakFocusedItem(label, nullptr,
        "Open the phone controller room without leaving the game.");
    /* F5: a pending approval is the one thing a host mid-race must not
     * miss -- put the count right in the card's subtitle. */
    if (pending != 0u) {
        ui::TextSubtle("%u connected · %u waiting for approval",
                       connected, pending);
    } else {
        ui::TextSubtle("%u connected", connected);
    }
    /* Item 4: the launcher has no sound, so a phone that just asked to join
     * flashes this brief line here -- the in-game card is the surface a host
     * mid-race is looking at. Same one-shot arming as the pending-card pulse;
     * fades after kToastMs. Audio, if ever wanted, is an owner decision. */
    if (SDL_GetTicks64() < g_pendingAttentionUntilMs) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::warn());
        ImGui::TextWrapped("A phone is asking to join — open to approve it.");
        ImGui::PopStyleColor();
    }
    /* F5: the join/drop toast the announceState hook latched, shown
     * briefly on this same card. */
    if (!g_toastText.empty() && SDL_GetTicks64() < g_toastUntilMs) {
        ImGui::PushStyleColor(ImGuiCol_Text, AppTheme::accent());
        ImGui::TextWrapped("%s", g_toastText.c_str());
        ImGui::PopStyleColor();
    }
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
