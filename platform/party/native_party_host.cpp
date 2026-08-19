#include "native_party_host.h"

#include "native_remote_pad_ingress.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace {

constexpr size_t kMaxControllers = 8u;
constexpr size_t kMaxEventsPerService = 64u;
constexpr size_t kMaxControllerId = 64u;
constexpr size_t kMaxName = 48u;
constexpr size_t kMaxPhrase = 48u;
constexpr size_t kPublicKeyLength = 87u;
constexpr size_t kMaxUrl = 2048u;
constexpr size_t kMaxMessage = 240u;
/* C1 self-heal: at most one rebind attempt per controller per this window,
 * so a phone that keeps overflowing the queue cannot make service() spin. */
constexpr uint64_t kRebindRateLimitMs = 500u;

bool printable(const std::string &value, size_t maximum) {
    if (value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20u && byte != 0x7fu;
    });
}

bool validPublicKey(const std::string &value) {
    return value.size() == kPublicKeyLength &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return std::isalnum(byte) != 0 || byte == '-' || byte == '_';
        });
}

uint64_t ownerFor(const MdkrNativePartyController &controller) {
    if (controller.seat < 1u || controller.seat > 4u ||
        controller.leaseGeneration == 0u) {
        return 0u;
    }
    /* Same stable derivation as the browser bridge, widened before shifting. */
    const uint64_t owner =
        (static_cast<uint64_t>(controller.leaseGeneration) << 3u) |
        static_cast<uint64_t>(controller.seat);
    return owner == 0u ? 1u : owner;
}

bool occupiesSeat(const MdkrNativePartyController &controller) {
    return controller.phase != MdkrNativePartyControllerPhase::Pending &&
        controller.seat >= 1u && controller.seat <= 4u;
}

std::string safeMessage(const std::string &value, const char *fallback) {
    return printable(value, kMaxMessage) && !value.empty() ? value : fallback;
}

/* I3: the worker's typed host_command_result codes, mapped to honest
 * player-facing copy (services/party/src/party-room.ts commandError and
 * room-model.ts supply the codes). An unmapped code returns nullptr and the
 * caller keeps the generic copy exactly as before -- a future code can
 * never crash the launcher or lie to the player, it is just not yet
 * specific. */
const char *typedCommandErrorCopy(const std::string &code) {
    if (code == "service_budget_safe") {
        return "The controller service has reached today's limit. "
               "Try again after midnight UTC.";
    }
    if (code == "invite_rotated") {
        return "That invite was replaced. Use the newest code.";
    }
    if (code == "room_full") return "No free phone slot.";
    return nullptr;
}

}  // namespace

MdkrNativePartyHost::MdkrNativePartyHost(MdkrPartyTransport &transport)
    : transport_(transport) {}

MdkrNativePartyHost::~MdkrNativePartyHost() {
    /* M4: quitting the app with a live room must tell the phones goodbye.
     * The transport's closeRoom sends the worker the `close` command --
     * relayed to every controller as host_closed
     * (services/party/src/party-room.ts) -- and flushes it, bounded by
     * kMdkrPartyCloseFlushDeadlineMs, before the shutdown below hangs up,
     * so the phones read "the host ended the session" instead of misreading
     * a silent socket drop as a network fault. Closed and the terminal
     * phases are excluded: their transports are already shut down
     * (closeRoom / setTerminal), so there is no live socket to say goodbye
     * on and no room left listening for one. */
    if (view_.phase != MdkrNativePartyPhase::Closed &&
        view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded) {
        (void)transport_.closeRoom();
    }
    releaseAll();
    transport_.shutdown();
}

bool MdkrNativePartyHost::open(const std::string &serviceOrigin) {
    /* I4: RoomEnded joins Closed and Error as a from-scratch start. The
     * ended room's copy names exactly this call as the way forward; the
     * fresh open creates a brand-new room, never resumes the dead one. */
    if (view_.phase != MdkrNativePartyPhase::Closed &&
        view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded) {
        return false;
    }
    releaseAll();
    view_ = MdkrNativePartyView{};
    if (!transport_.available()) {
        setError(safeMessage(
            transport_.unavailableReason(),
            "Phone controllers are not included in this build."));
        return false;
    }
    if (serviceOrigin.size() > kMaxUrl ||
        (serviceOrigin.rfind("https://", 0u) != 0u &&
         !mdkr_party_loopback_test_url_allowed(serviceOrigin))) {
        setError("Phone controllers require the configured secure Party service.");
        return false;
    }
    if (!transport_.open(serviceOrigin)) {
        setError("Could not open Phone Party. Local controllers still work.");
        return false;
    }
    view_.phase = MdkrNativePartyPhase::Opening;
    view_.busy = true;
    view_.message = "Creating a private controller room…";
    return true;
}

MdkrNativePartyController *MdkrNativePartyHost::controller(
    const std::string &id) {
    const auto found = std::find_if(
        view_.controllers.begin(), view_.controllers.end(),
        [&id](const MdkrNativePartyController &candidate) {
            return candidate.id == id;
        });
    return found == view_.controllers.end() ? nullptr : &*found;
}

const MdkrNativePartyController *MdkrNativePartyHost::controller(
    const std::string &id) const {
    const auto found = std::find_if(
        view_.controllers.begin(), view_.controllers.end(),
        [&id](const MdkrNativePartyController &candidate) {
            return candidate.id == id;
        });
    return found == view_.controllers.end() ? nullptr : &*found;
}

bool MdkrNativePartyHost::approve(
    const std::string &controllerId, unsigned seat) {
    /* SAS v2: no phrase gate here. The phrase commits to both DTLS
     * fingerprints, so it cannot exist until after approval starts the
     * WebRTC exchange -- it arrives as a ControllerPhrase event once the
     * phone connects, and the player verifies it on the seat row then. */
    MdkrNativePartyController *candidate = controller(controllerId);
    if (candidate == nullptr || candidate->commandPending ||
        candidate->phase != MdkrNativePartyControllerPhase::Pending ||
        seat < 1u || seat > 4u) {
        return false;
    }
    const bool occupied = std::any_of(
        view_.controllers.begin(), view_.controllers.end(),
        [seat](const MdkrNativePartyController &other) {
            return occupiesSeat(other) && other.seat == seat;
        });
    if (occupied || !transport_.approve(controllerId, seat)) return false;
    candidate->commandPending = true;
    view_.message = "Approving this phone…";
    return true;
}

bool MdkrNativePartyHost::reject(const std::string &controllerId) {
    MdkrNativePartyController *candidate = controller(controllerId);
    if (candidate == nullptr || candidate->commandPending ||
        !(candidate->phase == MdkrNativePartyControllerPhase::Pending
            ? transport_.reject(controllerId)
            : transport_.remove(controllerId))) {
        return false;
    }
    candidate->commandPending = true;
    view_.message = "Removing this phone…";
    return true;
}

bool MdkrNativePartyHost::rotateInvite() {
    if ((view_.phase != MdkrNativePartyPhase::Open &&
         view_.phase != MdkrNativePartyPhase::InviteRevoked) || view_.busy ||
        view_.inviteGeneration == 0u ||
        !transport_.rotateInvite(view_.inviteGeneration)) {
        return false;
    }
    view_.busy = true;
    view_.message = "Making a fresh controller code…";
    return true;
}

bool MdkrNativePartyHost::dismissInvite() {
    if (!view_.inviteVisible || view_.busy || !transport_.revokeInvite()) {
        return false;
    }
    view_.busy = true;
    view_.message = "Closing this invite…";
    return true;
}

bool MdkrNativePartyHost::closeRoom() {
    if (view_.phase == MdkrNativePartyPhase::Closed) return true;
    const bool requested = transport_.closeRoom();
    transport_.shutdown();
    releaseAll();
    view_ = MdkrNativePartyView{};
    view_.message = "Phone controllers closed.";
    return requested;
}

bool MdkrNativePartyHost::roomStateValid(
    const MdkrPartyTransportRoomState &room) const {
    if (room.transitionId == 0u || room.inviteGeneration == 0u ||
        room.controllers.size() > kMaxControllers ||
        !printable(room.controllerUrl, kMaxUrl) ||
        !printable(room.fallbackCode, 12u) ||
        (room.inviteActive &&
         ((room.controllerUrl.rfind("https://", 0u) != 0u &&
           !mdkr_party_loopback_test_url_allowed(room.controllerUrl)) ||
          room.fallbackCode.size() != 6u ||
          !std::all_of(room.fallbackCode.begin(), room.fallbackCode.end(),
                       [](unsigned char byte) { return std::isdigit(byte) != 0; })))) {
        return false;
    }
    std::set<std::string> ids;
    std::array<bool, 4> seats{};
    for (const MdkrNativePartyController &candidate : room.controllers) {
        if (candidate.id.empty() || !printable(candidate.id, kMaxControllerId) ||
            !printable(candidate.name, kMaxName) ||
            !validPublicKey(candidate.publicKey) ||
            !printable(candidate.pairingPhrase, kMaxPhrase)) {
            return false;
        }
        if (!ids.insert(candidate.id).second) return false;
        if (candidate.phase == MdkrNativePartyControllerPhase::Pending) {
            if (candidate.seat != 0u || candidate.leaseGeneration != 0u ||
                candidate.connectionSequence == 0u || candidate.direct) {
                return false;
            }
            continue;
        }
        if (candidate.seat < 1u || candidate.seat > 4u ||
            candidate.leaseGeneration == 0u ||
            candidate.connectionSequence == 0u ||
            seats[candidate.seat - 1u]) {
            return false;
        }
        seats[candidate.seat - 1u] = true;
        if (candidate.direct &&
            candidate.phase != MdkrNativePartyControllerPhase::Connected) {
            return false;
        }
    }
    return true;
}

void MdkrNativePartyHost::releaseController(
    const MdkrNativePartyController &candidate) {
    const uint64_t owner = ownerFor(candidate);
    if (owner != 0u && candidate.connectionSequence != 0u) {
        (void)mdkr_native_remote_pad_release(
            candidate.seat - 1u, owner, candidate.connectionSequence);
    }
}

void MdkrNativePartyHost::releaseAll() {
    for (const MdkrNativePartyController &candidate : view_.controllers) {
        releaseController(candidate);
    }
    lastRumble_.fill(0u);
}

void MdkrNativePartyHost::applyRoomState(
    const MdkrPartyTransportRoomState &room, uint64_t nowMs) {
    if (!roomStateValid(room)) {
        setError("The controller service returned an invalid room update.");
        return;
    }
    if (room.transitionId < view_.transitionId) return;
    if (room.transitionId == view_.transitionId &&
        view_.phase != MdkrNativePartyPhase::Opening &&
        view_.phase != MdkrNativePartyPhase::Recovering) {
        return;
    }

    for (const MdkrNativePartyController &existing : view_.controllers) {
        const auto replacement = std::find_if(
            room.controllers.begin(), room.controllers.end(),
            [&existing](const MdkrNativePartyController &candidate) {
                return candidate.id == existing.id;
            });
        if (replacement == room.controllers.end() ||
            ownerFor(*replacement) != ownerFor(existing) ||
            replacement->connectionSequence != existing.connectionSequence) {
            releaseController(existing);
        }
    }

    /* SAS v2: room updates never carry a phrase -- it arrives on its own
     * ControllerPhrase event once the phone's WebRTC descriptions are set.
     * Carry a seat's verified phrase across a room transition only while
     * the update still describes the same channel: same phone, same key,
     * same connectionSequence. A new sequence is a new channel whose phrase
     * has not arrived yet, and the old words must not vouch for it. */
    const std::vector<MdkrNativePartyController> previous =
        std::move(view_.controllers);
    view_.controllers = room.controllers;
    for (MdkrNativePartyController &candidate : view_.controllers) {
        candidate.commandPending = false;
        if (candidate.pairingPhrase.empty()) {
            const auto former = std::find_if(
                previous.begin(), previous.end(),
                [&candidate](const MdkrNativePartyController &other) {
                    return other.id == candidate.id;
                });
            if (former != previous.end() &&
                former->connectionSequence == candidate.connectionSequence &&
                former->publicKey == candidate.publicKey) {
                candidate.pairingPhrase = former->pairingPhrase;
            }
        }
        const uint64_t owner = ownerFor(candidate);
        if (owner != 0u && candidate.connectionSequence != 0u) {
            if (!mdkr_native_remote_pad_bind(
                    candidate.seat - 1u, owner,
                    candidate.connectionSequence)) {
                setError("A phone controller could not reserve its seat safely.");
                return;
            }
        }
    }

    view_.transitionId = room.transitionId;
    view_.inviteGeneration = room.inviteGeneration;
    /* I1 fix: room.inviteExpiresInMs is relative (ms remaining as of the
     * transport's parse), specifically so it can be anchored here in the
     * HOST's own service clock (nowMs) rather than compared as if it were
     * already an absolute instant in some other clock's domain. Every
     * downstream read of view_.inviteExpiresAtMs (the tail of service()
     * below, ui_phone_party.cpp's countdown) already compares it against
     * this same nowMs domain, so latching it here is the entire fix. */
    view_.inviteExpiresAtMs = nowMs + room.inviteExpiresInMs;
    view_.controllerUrl = room.inviteActive ? room.controllerUrl : std::string{};
    view_.fallbackCode = room.inviteActive ? room.fallbackCode : std::string{};
    view_.inviteVisible = room.inviteActive && view_.inviteExpiresAtMs > nowMs;
    view_.phase = view_.inviteVisible ? MdkrNativePartyPhase::Open
                                     : MdkrNativePartyPhase::InviteRevoked;
    view_.busy = false;
    view_.message = view_.inviteVisible
        ? "Scan to add a phone controller."
        : "The invite is closed. Connected phones keep their seats.";
}

void MdkrNativePartyHost::applyEvent(
    const MdkrPartyTransportEvent &event, uint64_t nowMs) {
    switch (event.type) {
        case MdkrPartyTransportEventType::RoomState:
            applyRoomState(event.room, nowMs);
            return;
        case MdkrPartyTransportEventType::ControllerConnected: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            candidate->phase = MdkrNativePartyControllerPhase::Connected;
            candidate->direct = true;
            candidate->haptics = event.haptics;
            /* I2 recovery: a genuine controller_ready at this build's own
             * protocol means the phone reloaded into a matching page. */
            candidate->protocolMismatch = false;
            (void)mdkr_native_remote_pad_set_haptics(
                candidate->seat - 1u, ownerFor(*candidate),
                candidate->connectionSequence, event.haptics);
            view_.message = "Phone connected directly.";
            return;
        }
        case MdkrPartyTransportEventType::ControllerDisconnected: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            candidate->direct = false;
            candidate->haptics = false;
            /* SAS v2: the phrase vouched for the channel that just ended,
             * so it ends here too. A legitimate reconnect re-derives fresh
             * words from its own answer; a reconnect whose fingerprints the
             * transport REFUSES derives none -- and without this clear that
             * refusal would leave the old channel's words standing
             * indefinitely against a live channel they do not bind. */
            candidate->pairingPhrase.clear();
            (void)mdkr_native_remote_pad_set_haptics(
                candidate->seat - 1u, ownerFor(*candidate),
                candidate->connectionSequence, false);
            if (candidate->phase == MdkrNativePartyControllerPhase::Connected) {
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
            }
            view_.message = "Phone reconnecting; its controls are neutral.";
            return;
        }
        case MdkrPartyTransportEventType::ControllerPacket: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !candidate->direct ||
                candidate->phase != MdkrNativePartyControllerPhase::Connected) {
                return;
            }
            const uint64_t owner = ownerFor(*candidate);
            if (!mdkr_native_remote_pad_push(
                    candidate->seat - 1u, owner,
                    candidate->connectionSequence,
                    event.packet.data(), event.packet.size())) {
                candidate->direct = false;
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
                /* Queue exhaustion revoked custody at the ingress crossing,
                 * but nothing here waits for a room transition to fix it --
                 * a stable mid-race room never sends one. Flag it so
                 * service() heals the seat on its own next tick. Leave
                 * candidate->haptics untouched: it is the phone's known
                 * hardware capability, not a liveness bit, and the healing
                 * loop needs it to re-assert ingress haptics support once the
                 * seat is rebound (a fresh bind always clears that bit). */
                candidate->needsRebind = true;
                view_.message =
                    "Phone input paused safely. Reconnecting…";
            }
            return;
        }
        case MdkrPartyTransportEventType::ControllerPhrase: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate != nullptr && printable(event.message, kMaxPhrase)) {
                candidate->pairingPhrase = event.message;
            }
            return;
        }
        case MdkrPartyTransportEventType::ControllerProtocolMismatch: {
            MdkrNativePartyController *candidate = controller(event.controllerId);
            if (candidate == nullptr || !occupiesSeat(*candidate)) return;
            /* I2: the phone's page completed the handshake but speaks a
             * different channel-protocol version. Say so, visibly distinct
             * from "Reconnecting", and demote the seat to neutral input.
             * The room and the seat's lease stay intact -- the transport
             * kept the peer up, and the phone reloading into a matching
             * page version (ControllerConnected) is the recovery. Clear
             * needsRebind: the C1 heal loop's fresh bind cannot fix a
             * version gap, and its "Phone input reconnected." copy would
             * paper over this honest state with a lie. */
            candidate->protocolMismatch = true;
            candidate->needsRebind = false;
            candidate->direct = false;
            candidate->haptics = false;
            (void)mdkr_native_remote_pad_set_haptics(
                candidate->seat - 1u, ownerFor(*candidate),
                candidate->connectionSequence, false);
            if (candidate->phase == MdkrNativePartyControllerPhase::Connected) {
                candidate->phase = MdkrNativePartyControllerPhase::Leased;
            }
            view_.message = kMdkrPartyProtocolMismatchCopy;
            return;
        }
        case MdkrPartyTransportEventType::CommandRejected: {
            if (event.controllerId.empty()) {
                /* No controller identity: a genuine host-command rejection
                 * (approve/reject/rotate/dismiss/close all funnel into the
                 * same undifferentiated host_command_result path today) is
                 * really room-wide -- the one pending action just failed,
                 * so every commandPending and the room-level busy flag
                 * clear exactly as before. */
                for (MdkrNativePartyController &candidate : view_.controllers) {
                    candidate.commandPending = false;
                }
                view_.busy = false;
            } else {
                /* A controller-scoped rejection (C3's connect_timeout
                 * give-up) must not clear an unrelated controller's
                 * genuinely in-flight command -- only the named
                 * controller's own commandPending is touched. busy tracks
                 * room-level invite actions, not any one controller, so it
                 * is left untouched here. */
                MdkrNativePartyController *candidate = controller(event.controllerId);
                if (candidate != nullptr) {
                    candidate->commandPending = false;
                    /* I2 review fix: a version-mismatched seat keeps the
                     * honest room copy. The transport's ladder no longer
                     * gives up on such a peer at the source, but any
                     * rejection that still names a mismatched controller
                     * -- the give-up shape included -- would recommend
                     * the wrong remedy ("remove it and pair again" cannot
                     * fix a page version) right beside the seat row's
                     * correct one. Bookkeeping above still happened; only
                     * the message overwrite is suppressed. */
                    if (candidate->protocolMismatch) return;
                }
            }
            /* I3: a recognized typed code wins over whatever prose the
             * transport attached; an unknown or absent code keeps the
             * pre-existing generic surface untouched. */
            const char *typedCopy = typedCommandErrorCopy(event.errorCode);
            view_.message = typedCopy != nullptr ? typedCopy : safeMessage(
                event.message, "That controller action did not complete. Try again.");
            return;
        }
        case MdkrPartyTransportEventType::Recovering:
            view_.phase = MdkrNativePartyPhase::Recovering;
            view_.busy = true;
            view_.message = "Reconnecting the controller room…";
            return;
        case MdkrPartyTransportEventType::Error:
            setError(safeMessage(
                event.message,
                "Phone controllers are unavailable. Local controllers still work."));
            return;
        case MdkrPartyTransportEventType::RoomGone:
            /* I4: the service refused to resume this room for good -- its
             * 24 h life ended or it was deleted outright; no ladder rung
             * can bring it back. Same fail-neutral teardown as a terminal
             * error, but a distinct phase and sentence: nothing broke, the
             * session is simply over, and a NEW invite (a fresh open())
             * is the one honest way forward. The copy is the host's own,
             * not the event's: this surface must never depend on prose
             * arriving over the wire. */
            setTerminal(MdkrNativePartyPhase::RoomEnded,
                        kMdkrPartyRoomEndedCopy);
            return;
        case MdkrPartyTransportEventType::Closed:
            releaseAll();
            view_ = MdkrNativePartyView{};
            view_.message = safeMessage(event.message, "Phone controllers closed.");
            return;
    }
}

void MdkrNativePartyHost::setError(const std::string &message) {
    setTerminal(MdkrNativePartyPhase::Error, message);
}

/* One teardown for every terminal room state (Error, and I4's RoomEnded):
 * seats go back to local play, the transport is shut down and stays down,
 * and the surface shows exactly one sentence for the state it is in. */
void MdkrNativePartyHost::setTerminal(
    MdkrNativePartyPhase phase, const std::string &message) {
    releaseAll();
    transport_.shutdown();
    /* Review fix: a controller left needsRebind from a push failure earlier
     * in this same drain cycle must not survive into the terminal room --
     * releaseAll() above already revoked its seat's custody, so the next
     * service() heal loop must never see a reason to touch it again. */
    for (MdkrNativePartyController &candidate : view_.controllers) {
        candidate.needsRebind = false;
    }
    view_.phase = phase;
    view_.busy = false;
    view_.inviteVisible = false;
    view_.controllerUrl.clear();
    view_.fallbackCode.clear();
    view_.message = message;
}

void MdkrNativePartyHost::service(uint64_t nowMs) {
    /* C1 self-heal. Ingress queue exhaustion revokes a seat's custody at the
     * transport crossing (native_remote_pad_ingress.cpp) below the room state
     * machine entirely -- the WebRTC channel and its 5 s pings never notice.
     * The only other rebind site is applyRoomState, which runs solely on an
     * advancing room transitionId; a stable mid-race room sends none, so
     * without this loop "Reconnecting..." is a promise nothing fulfills. Heal
     * it ourselves: re-issuing mdkr_native_remote_pad_bind with a fresh lease
     * generation gives the seat a new owner identity (the same call
     * applyRoomState makes for a real reconnect), and the engine-side rebind
     * that a changed identity triggers publishes neutral before any packet
     * from the new epoch can reach the sim -- the same fail-neutral path a
     * genuine reconnect already relies on. Runs before events are drained so
     * a fresh packet queued for this very tick lands on a live seat. A fresh
     * bind always clears the ingress haptics bit (native_remote_pad_ingress.cpp
     * clearPayload), and in this exact scenario the WebRTC channel never
     * dropped, so no ControllerConnected event will ever come along to set it
     * again -- reassert it ourselves from the controller's own remembered
     * capability, the same call ControllerConnected makes.
     *
     * Review fix: also gate the whole loop on the room not being terminal
     * (Error, I4's RoomEnded) or Closed. setTerminal() already clears
     * needsRebind on its way out (belt), but a terminal or closed room must
     * never re-bind a seat regardless of how a flag got left standing
     * (suspenders).
     */
    if (view_.phase != MdkrNativePartyPhase::Error &&
        view_.phase != MdkrNativePartyPhase::RoomEnded &&
        view_.phase != MdkrNativePartyPhase::Closed) {
        for (MdkrNativePartyController &candidate : view_.controllers) {
            /* I2: a version-mismatched seat is excluded outright. A fresh
             * bind cannot fix a protocol gap, and this loop's "Phone input
             * reconnected." would overwrite the honest mismatch copy.
             * applyEvent already clears needsRebind at the mismatch site
             * (belt); this keeps the loop safe regardless (suspenders). */
            if (candidate.protocolMismatch || !candidate.needsRebind ||
                !occupiesSeat(candidate) || candidate.connectionSequence == 0u) {
                continue;
            }
            if (candidate.lastRebindMs != 0u &&
                nowMs >= candidate.lastRebindMs &&
                nowMs - candidate.lastRebindMs < kRebindRateLimitMs) {
                continue;
            }
            candidate.lastRebindMs = nowMs;
            candidate.leaseGeneration++;
            const uint64_t owner = ownerFor(candidate);
            if (owner == 0u ||
                !mdkr_native_remote_pad_bind(
                    candidate.seat - 1u, owner, candidate.connectionSequence)) {
                continue;
            }
            candidate.needsRebind = false;
            candidate.direct = true;
            candidate.phase = MdkrNativePartyControllerPhase::Connected;
            (void)mdkr_native_remote_pad_set_haptics(
                candidate.seat - 1u, owner, candidate.connectionSequence,
                candidate.haptics);
            view_.message = "Phone input reconnected.";
        }
    }

    MdkrPartyTransportEvent event;
    size_t count = 0u;
    while (count < kMaxEventsPerService && transport_.poll(event)) {
        applyEvent(event, nowMs);
        count++;
    }
    if (view_.inviteVisible && nowMs >= view_.inviteExpiresAtMs) {
        view_.inviteVisible = false;
        view_.controllerUrl.clear();
        view_.fallbackCode.clear();
        view_.phase = MdkrNativePartyPhase::InviteRevoked;
        view_.message = "Controller code expired. Connected phones keep their seats.";
    }

    for (MdkrNativePartyController &candidate : view_.controllers) {
        if (!candidate.direct || !candidate.haptics || !occupiesSeat(candidate)) {
            continue;
        }
        uint16_t strength = 0u;
        const uint64_t owner = ownerFor(candidate);
        if (mdkr_native_remote_pad_take_rumble(
                candidate.seat - 1u, owner, candidate.connectionSequence,
                &strength)) {
            if (transport_.sendRumble(candidate.id, strength)) {
                lastRumble_[candidate.seat - 1u] = strength;
            }
        }
    }
}
