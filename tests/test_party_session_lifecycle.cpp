/*
 * Phone Party sessions survive minimize, DPI change and rematch.
 *
 * Scope note, stated plainly because it bounds what this gate proves.  The
 * packaged launcher owns the only live party session (UiLauncher constructs
 * MdkrNativePartyHost in its constructor and services it from the launcher and
 * overlay ticks).  There is no environment seam that opens a party room from a
 * hidden run: the launcher's hidden-run seams are MDKR_APP_SMOKE_DPI_SEQUENCE,
 * MDKR_APP_SMOKE_WINDOW_SIZE, MDKR_APP_RESTART_GAME and
 * MDKR_APP_TEST_SESSION_ROUNDTRIPS, and none of them reaches Phone Party.
 * Adding one would mean editing platform/app/, which this gate deliberately
 * does not do.  So the three window/session events are asserted where they are
 * observable honestly: at MdkrNativePartyHost plus the launcher-owned remote
 * pad ingress, driven through the lifecycle each event actually produces.
 *
 *   minimize          -> the launcher stops calling service() for a long
 *                        wall-clock stretch while the transport keeps queueing
 *                        bounded events, then service() resumes.
 *   DPI change        -> the launcher re-lays out over the same three frames
 *                        the DPI smoke seam drives (scale 1, 2, 1) while the
 *                        same host object is serviced with no room traffic.
 *   rematch           -> an engine session drains the pad loan and ends, the
 *                        launcher services between sessions, and a new session
 *                        starts.  Engine teardown must not reset the ingress:
 *                        the seat is launcher-owned across the round trip.
 *
 * The property asserted after each event: leases and seats retained, no
 * controller dropped, no invite silently revoked, input routing intact, and
 * the bounded ingress queue / custody counters not tripped.
 */
#include "party/native_party_host.h"
#include "party/native_remote_pad_ingress.h"
#include "party/party_protocol.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <array>
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public MdkrPartyTransport {
public:
    bool availableValue = true;
    bool commandResult = true;
    std::string reason;
    std::deque<MdkrPartyTransportEvent> events;
    std::vector<std::string> calls;

    bool available() const override { return availableValue; }
    const char *unavailableReason() const override { return reason.c_str(); }
    bool open(const std::string &origin) override {
        calls.push_back("open:" + origin);
        return commandResult;
    }
    bool approve(const std::string &id, unsigned seat) override {
        calls.push_back("approve:" + id + ":" + std::to_string(seat));
        return commandResult;
    }
    bool reject(const std::string &id) override {
        calls.push_back("reject:" + id);
        return commandResult;
    }
    bool remove(const std::string &id) override {
        calls.push_back("remove:" + id);
        return commandResult;
    }
    bool rotateInvite(unsigned generation) override {
        calls.push_back("rotate:" + std::to_string(generation));
        return commandResult;
    }
    bool revokeInvite() override {
        calls.push_back("revoke");
        return commandResult;
    }
    bool closeRoom() override {
        calls.push_back("close");
        return commandResult;
    }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        calls.push_back("rumble:" + id + ":" + std::to_string(strength));
        return commandResult;
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        if (events.empty()) return false;
        event = std::move(events.front());
        events.pop_front();
        return true;
    }
    void shutdown() override { calls.push_back("shutdown"); }
};

MdkrNativePartyController approved(
    std::string id, unsigned seat, uint32_t lease, uint32_t connection) {
    MdkrNativePartyController value;
    value.id = std::move(id);
    value.name = "A friend's phone";
    value.publicKey = std::string(87u, 'C');
    value.pairingPhrase = "amber comet";
    value.phase = MdkrNativePartyControllerPhase::Leased;
    value.seat = seat;
    value.leaseGeneration = lease;
    value.connectionSequence = connection;
    return value;
}

MdkrPartyTransportEvent roomEvent(
    uint64_t transition, unsigned generation, uint64_t expires,
    std::vector<MdkrNativePartyController> controllers) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::RoomState;
    event.room.transitionId = transition;
    event.room.inviteGeneration = generation;
    event.room.inviteExpiresAtMs = expires;
    event.room.controllerUrl = "https://party.example/controller/#secret";
    event.room.fallbackCode = "123456";
    event.room.inviteActive = true;
    event.room.controllers = std::move(controllers);
    return event;
}

std::vector<uint8_t> padPacket(uint32_t connection, uint32_t sequence) {
    MdkrPartyPadPacket source{};
    source.flags = MDKR_PARTY_PAD_FLAG_PRESENT;
    source.connection_sequence = connection;
    source.sample_sequence = sequence;
    source.sender_time_ms = sequence;
    source.buttons = 0x8000u;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> output{};
    size_t length = 0u;
    assert(mdkr_party_pad_encode(
        &source, output.data(), output.size(), &length));
    return std::vector<uint8_t>(output.begin(), output.begin() + length);
}

void queuePacket(
    FakeTransport &transport, const std::string &id, uint32_t connection,
    uint32_t sequence) {
    MdkrPartyTransportEvent event;
    event.type = MdkrPartyTransportEventType::ControllerPacket;
    event.controllerId = id;
    event.packet = padPacket(connection, sequence);
    transport.events.push_back(std::move(event));
}

/* Engine-thread read side: drain the loan and report the sample sequence of
 * every packet that actually reached the seat, in delivery order. */
std::vector<uint32_t> drainSeat(
    unsigned port, uint64_t owner, uint32_t connection) {
    std::vector<uint32_t> sequences;
    std::array<uint8_t, MDKR_PARTY_PAD_MAX_BYTES> buffer{};
    for (;;) {
        const size_t length = mdkr_native_remote_pad_pop(
            port, owner, connection, buffer.data(), buffer.size());
        if (length == 0u) break;
        MdkrPartyPadPacket decoded{};
        assert(mdkr_party_pad_decode(buffer.data(), length, &decoded) ==
               MDKR_PARTY_DECODE_OK);
        sequences.push_back(decoded.sample_sequence);
    }
    return sequences;
}

struct Snapshot {
    MdkrNativePartyPhase phase;
    std::string controllerUrl;
    std::string fallbackCode;
    uint64_t inviteExpiresAtMs;
    uint64_t transitionId;
    unsigned inviteGeneration;
    bool inviteVisible;
    bool busy;
    std::vector<MdkrNativePartyController> controllers;
};

Snapshot snapshot(const MdkrNativePartyHost &host) {
    const MdkrNativePartyView &view = host.view();
    return Snapshot{view.phase, view.controllerUrl, view.fallbackCode,
                    view.inviteExpiresAtMs, view.transitionId,
                    view.inviteGeneration, view.inviteVisible, view.busy,
                    view.controllers};
}

bool sameController(
    const MdkrNativePartyController &left,
    const MdkrNativePartyController &right) {
    return left.id == right.id && left.name == right.name &&
        left.publicKey == right.publicKey &&
        left.pairingPhrase == right.pairingPhrase && left.phase == right.phase &&
        left.seat == right.seat &&
        left.leaseGeneration == right.leaseGeneration &&
        left.connectionSequence == right.connectionSequence &&
        left.direct == right.direct && left.haptics == right.haptics &&
        left.commandPending == right.commandPending;
}

bool sameSession(const Snapshot &before, const Snapshot &after) {
    if (before.phase != after.phase ||
        before.controllerUrl != after.controllerUrl ||
        before.fallbackCode != after.fallbackCode ||
        before.inviteExpiresAtMs != after.inviteExpiresAtMs ||
        before.transitionId != after.transitionId ||
        before.inviteGeneration != after.inviteGeneration ||
        before.inviteVisible != after.inviteVisible ||
        before.busy != after.busy ||
        before.controllers.size() != after.controllers.size()) {
        return false;
    }
    for (size_t index = 0u; index < before.controllers.size(); ++index) {
        if (!sameController(before.controllers[index], after.controllers[index])) {
            return false;
        }
    }
    return true;
}

constexpr unsigned kSeat = 2u;
constexpr unsigned kPort = kSeat - 1u;
constexpr uint32_t kLease = 4u;
constexpr uint32_t kConnection = 9u;
/* (lease << 3) | seat, the stable owner derivation the host shares with the
 * browser bridge. */
constexpr uint64_t kOwner = (static_cast<uint64_t>(kLease) << 3u) | kSeat;
constexpr uint64_t kInviteExpiry = 600000u;

/* One connected phone on seat 2, direct, with haptics, its first three pad
 * samples already delivered to the engine. */
void establishConnectedPhone(
    FakeTransport &transport, MdkrNativePartyHost &host) {
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, kInviteExpiry, {approved("phone-a", kSeat, kLease, kConnection)}));
    host.service(1000u);
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    assert(host.view().controllers.size() == 1u);

    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    for (uint32_t sequence = 1u; sequence <= 3u; ++sequence) {
        queuePacket(transport, "phone-a", kConnection, sequence);
    }
    host.service(1001u);
    assert(host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);

    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(owner == kOwner && connection == kConnection);
    const std::vector<uint32_t> delivered =
        drainSeat(kPort, kOwner, kConnection);
    assert((delivered == std::vector<uint32_t>{1u, 2u, 3u}));
}

void assertSeatCustodyIntact(const MdkrNativePartyHost &host) {
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(owner == kOwner && connection == kConnection);
    assert(host.view().controllers.size() == 1u);
    assert(host.view().controllers[0].seat == kSeat);
    assert(host.view().controllers[0].leaseGeneration == kLease);
    assert(host.view().controllers[0].connectionSequence == kConnection);
}

/* Bounded queue and custody counters: nothing overflowed, nothing was rejected
 * as stale or malformed, and the seat was bound exactly once and never
 * released. A dropped controller or a churned lease moves one of these. */
void assertQueuesAndWatchdogsUntripped(uint64_t expectedPackets) {
    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(kPort, &stats);
    assert(stats.overflows == 0u);
    assert(stats.malformed == 0u);
    assert(stats.stale == 0u);
    assert(stats.releases == 0u);
    assert(stats.binds == 1u);
    assert(stats.packets == expectedPackets);
}

/*
 * Minimize: the launcher stops presenting, so service() is not called for a
 * long wall-clock stretch. The transport keeps its own bounded queue during
 * that stretch. On resume the session must be exactly where it was.
 */
void minimizeKeepsTheSession() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    establishConnectedPhone(transport, host);
    const Snapshot before = snapshot(host);

    /* Thirty seconds minimized. Eight samples arrive and stay in the
     * transport's queue, comfortably inside the ingress bound of 32. */
    for (uint32_t sequence = 4u; sequence <= 11u; ++sequence) {
        queuePacket(transport, "phone-a", kConnection, sequence);
    }
    host.service(31000u);

    assert(sameSession(before, snapshot(host)));
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    assert(host.view().inviteVisible);
    assert(!host.view().controllerUrl.empty());
    assert(host.view().controllers[0].direct);
    assertSeatCustodyIntact(host);
    const std::vector<uint32_t> delivered =
        drainSeat(kPort, kOwner, kConnection);
    assert((delivered == std::vector<uint32_t>{4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u}));
    assertQueuesAndWatchdogsUntripped(11u);

    /* Engine -> phone haptics still route after the resume. */
    assert(mdkr_native_remote_pad_request_rumble(kPort, 4321u));
    host.service(31001u);
    assert(transport.calls.back() == "rumble:phone-a:4321");
}

/*
 * DPI change: the launcher rebuilds its atlas over the scale 1 -> 2 -> 1
 * frames the DPI smoke seam drives. The party host is serviced on every one of
 * those frames and must be untouched by re-layout, while input that lands in
 * the middle of the transition still reaches its seat.
 */
void dpiTransitionIsInvisibleToTheSession() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    establishConnectedPhone(transport, host);
    const Snapshot before = snapshot(host);
    const size_t callsBefore = transport.calls.size();

    host.service(1100u);
    assert(sameSession(before, snapshot(host)));
    queuePacket(transport, "phone-a", kConnection, 4u);
    host.service(1116u);
    assert(sameSession(before, snapshot(host)));
    host.service(1132u);

    assert(sameSession(before, snapshot(host)));
    assert(transport.calls.size() == callsBefore);
    assertSeatCustodyIntact(host);
    const std::vector<uint32_t> delivered =
        drainSeat(kPort, kOwner, kConnection);
    assert((delivered == std::vector<uint32_t>{4u}));
    assertQueuesAndWatchdogsUntripped(4u);
}

/*
 * Rematch: MDKR_APP_TEST_SESSION_ROUNDTRIPS drives up to three engine sessions
 * inside one launcher process. Each round the engine borrows the pads, drains
 * them and tears its session down; the launcher services in between. Engine
 * teardown must never reset the ingress, so the approved phone keeps its
 * launcher-owned seat across all three rounds without a rebind.
 */
void rematchKeepsLauncherOwnedSeats() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    establishConnectedPhone(transport, host);
    const Snapshot before = snapshot(host);

    uint32_t sequence = 3u;
    uint64_t packets = 3u;
    for (int round = 0; round < 3; ++round) {
        const uint64_t base = 2000u + 1000u * static_cast<uint64_t>(round);
        /* Engine session: pad traffic arrives and the engine drains its loan. */
        for (int sample = 0; sample < 4; ++sample) {
            queuePacket(transport, "phone-a", kConnection, ++sequence);
            packets++;
        }
        host.service(base);
        const std::vector<uint32_t> delivered =
            drainSeat(kPort, kOwner, kConnection);
        assert(delivered.size() == 4u);
        assert(delivered.front() == sequence - 3u && delivered.back() == sequence);

        /* Session teardown and the launcher-only gap before the next round.
         * Deliberately no mdkr_native_remote_pad_reset_all() here: that is
         * process teardown, not engine teardown. */
        host.service(base + 100u);
        host.service(base + 200u);
        assert(sameSession(before, snapshot(host)));
        assertSeatCustodyIntact(host);
        assert(host.view().controllers[0].direct);
        assert(host.view().inviteVisible);
    }

    assertQueuesAndWatchdogsUntripped(packets);
    /* Exactly one bind across three round trips: the seat was never dropped
     * and re-acquired behind the player's back. */
    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(kPort, &stats);
    assert(stats.binds == 1u);
}

/*
 * The one state change a long minimize may legitimately cause is the invite
 * timing out. That must be announced with the documented copy and must not
 * cost the connected phone its seat, and rotating a fresh code must still work.
 */
void inviteExpiryDuringMinimizeKeepsSeatsAndSaysSo() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    assert(host.open("https://party.example"));
    transport.events.push_back(roomEvent(
        1u, 1u, 20000u, {approved("phone-a", kSeat, kLease, kConnection)}));
    host.service(1000u);
    MdkrPartyTransportEvent connected;
    connected.type = MdkrPartyTransportEventType::ControllerConnected;
    connected.controllerId = "phone-a";
    connected.haptics = true;
    transport.events.push_back(connected);
    host.service(1001u);
    assert(host.view().controllers[0].direct);

    /* Ninety seconds minimized: the code times out while nobody is looking. */
    queuePacket(transport, "phone-a", kConnection, 1u);
    host.service(90000u);

    assert(host.view().phase == MdkrNativePartyPhase::InviteRevoked);
    assert(!host.view().inviteVisible);
    assert(host.view().controllerUrl.empty());
    assert(host.view().fallbackCode.empty());
    assert(host.view().message ==
           "Controller code expired. Connected phones keep their seats.");
    assertSeatCustodyIntact(host);
    assert(host.view().controllers[0].direct);
    const std::vector<uint32_t> delivered =
        drainSeat(kPort, kOwner, kConnection);
    assert((delivered == std::vector<uint32_t>{1u}));
    assert(host.rotateInvite());
    assert(transport.calls.back() == "rotate:1");
}

/*
 * The untripped-counter assertions above are only worth something if the
 * counters can move. Flood the ingress past its bound and prove the documented
 * congestion behaviour: custody is revoked, the pad goes neutral rather than
 * stale, the host says so in player-facing words, and local play is untouched
 * because the other three seats never had anything taken from them.
 */
void boundedQueueOverflowIsObservableAndFailNeutral() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    establishConnectedPhone(transport, host);

    const uint32_t flood = MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY + 8u;
    for (uint32_t sequence = 4u; sequence < 4u + flood; ++sequence) {
        queuePacket(transport, "phone-a", kConnection, sequence);
    }
    host.service(2000u);

    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(kPort, &stats);
    assert(stats.overflows == 1u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(drainSeat(kPort, kOwner, kConnection).empty());
    assert(!host.view().controllers[0].direct);
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().message == "Phone input paused safely. Reconnecting…");
    assert(host.view().phase == MdkrNativePartyPhase::Open);
    for (unsigned other = 0u; other < MDKR_NATIVE_REMOTE_PAD_PORTS; ++other) {
        if (other == kPort) continue;
        MdkrNativeRemotePadIngressStats idle{};
        mdkr_native_remote_pad_stats(other, &idle);
        assert(idle.binds == 0u && idle.releases == 0u && idle.overflows == 0u);
    }
}

/*
 * Stall injection (C1): the phone's transport stays perfectly healthy and
 * keeps streaming pad packets across a host stall long enough to blow the
 * 32-deep ingress queue. Overflow revokes custody exactly as designed above
 * -- but no room transition ever follows a stall (a stable mid-race room
 * sends none), so the only rebind site in applyRoomState never fires and the
 * "Reconnecting..." promise has nothing behind it. The host must heal custody
 * itself from service(): within two subsequent service() calls -- one that
 * observes the overflow, one that heals and delivers -- a fresh live packet
 * must reach the engine side of the ingress crossing again.
 */
void stall_with_live_packets_recovers_input() {
    mdkr_native_remote_pad_reset_all();
    FakeTransport transport;
    MdkrNativePartyHost host(transport);
    establishConnectedPhone(transport, host);
    const uint64_t transitionBefore = host.view().transitionId;

    /* A stall long enough to fill the queue (OS sleep, window drag, load
     * hitch): the phone does not know service() stopped running and keeps
     * streaming at its normal cadence, so packets pile up in the transport's
     * own queue exactly as they would against the real one. */
    const uint32_t flood = MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY + 8u;
    uint32_t sequence = 3u;
    for (uint32_t index = 0u; index < flood; ++index) {
        queuePacket(transport, "phone-a", kConnection, ++sequence);
    }
    host.service(4000u);  // service() resumes after the gap and drains the flood.

    MdkrNativeRemotePadIngressStats stats{};
    mdkr_native_remote_pad_stats(kPort, &stats);
    assert(stats.overflows == 1u);
    uint64_t owner = 0u;
    uint32_t connection = 0u;
    assert(!mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Leased);
    assert(host.view().message == "Phone input paused safely. Reconnecting…");
    /* No room transition happened: a stable room sends none mid-race. */
    assert(host.view().transitionId == transitionBefore);
    /* Ingress custody is gone, so haptics support reads false at that layer,
     * but the host must still remember the phone had haptics before the
     * stall -- the WebRTC channel never dropped, so nothing will ever tell
     * it again. */
    assert(!mdkr_native_remote_pad_haptics_supported(kPort));
    assert(host.view().controllers[0].haptics);

    /* The phone, unaware anything happened locally, keeps streaming at its
     * normal cadence. One more service() call, still with no room_state
     * anywhere, must both heal custody and deliver this fresh packet. */
    queuePacket(transport, "phone-a", kConnection, ++sequence);
    host.service(4100u);

    assert(host.view().transitionId == transitionBefore);  // still no transition
    assert(host.view().controllers[0].phase ==
           MdkrNativePartyControllerPhase::Connected);
    assert(mdkr_native_remote_pad_info(kPort, &owner, &connection));
    assert(owner != 0u && connection == kConnection);
    /* Rumble must work again on the healed seat without waiting for a real
     * reconnect: the heal reasserts haptics support at the ingress layer from
     * the controller's remembered capability. */
    assert(mdkr_native_remote_pad_haptics_supported(kPort));

    const std::vector<uint32_t> delivered = drainSeat(kPort, owner, kConnection);
    assert(!delivered.empty());
    assert(delivered.back() == sequence);
}

}  // namespace

int main() {
    minimizeKeepsTheSession();
    dpiTransitionIsInvisibleToTheSession();
    rematchKeepsLauncherOwnedSeats();
    inviteExpiryDuringMinimizeKeepsSeatsAndSaysSo();
    boundedQueueOverflowIsObservableAndFailNeutral();
    stall_with_live_packets_recovers_input();
    mdkr_native_remote_pad_reset_all();
    std::printf(
        "party_session_lifecycle: PASS — a live Phone Party session keeps its "
        "leases, seats, invite and input routing across a 30s minimize, the "
        "1/2/1 DPI re-layout and three rematch round trips, with the bounded "
        "ingress queue and custody counters untripped, the documented "
        "expiry/congestion copy intact, and a stalled phone's input self-heals "
        "without waiting for a room transition\n");
    return 0;
}
