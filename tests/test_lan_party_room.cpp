/*
 * MdkrLanPartyRoom: the in-process local room that speaks the phones' own
 * pairing protocol. These cases pin the worker-mirroring semantics as pure
 * function-call facts against a fake controller socket and an in-memory host
 * sink -- no network, no threads, no ROM, deterministic clock and RNG.
 *
 * The property under everything is the authentication boundary: a freshly
 * attached socket is anonymous and hostile and does nothing until it
 * redeems. The relay, the seat CAS, the throttle and the identity echo are
 * the worker's, reproduced natively.
 */
#include "party/lan_party_room.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

/* ---- Fake controller socket -------------------------------------------- */

/* Single-threaded stand-in for MdkrLanPartyWebSocket. The room registers its
 * callbacks in attachController(); the test drives a client frame with
 * inject(). Every room send lands in `sent`; a room close latches closeCode
 * and flips isOpen(). */
class FakeSocket : public MdkrLanPartyControllerSocket {
public:
    std::vector<std::string> sent;
    bool closed = false;
    uint16_t closeCode = 0u;

    bool sendText(const std::string &payload) override {
        if (closed) return false;
        sent.push_back(payload);
        return true;
    }
    void close(uint16_t code) override {
        if (closed) return;
        closed = true;
        closeCode = code;
        if (onClosed_) onClosed_();
    }
    bool isOpen() const override { return !closed; }
    void onMessage(std::function<void(const std::string &)> callback) override {
        onMessage_ = std::move(callback);
    }
    void onClosed(std::function<void()> callback) override {
        onClosed_ = std::move(callback);
    }

    /* Simulate one inbound client frame. */
    void inject(const std::string &payload) {
        if (closed || !onMessage_) return;
        onMessage_(payload);
    }

    /* Simulate the phone dropping the connection (server-side onClosed). */
    void peerClose() {
        if (closed) return;
        closed = true;
        closeCode = 1006u;
        if (onClosed_) onClosed_();
    }

    const std::string &last() const {
        assert(!sent.empty());
        return sent.back();
    }
    size_t count() const { return sent.size(); }
    bool sawText(const std::string &needle) const {
        for (const std::string &message : sent) {
            if (message.find(needle) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::function<void(const std::string &)> onMessage_;
    std::function<void()> onClosed_;
};

/* ---- Fixtures ----------------------------------------------------------- */

const std::string kHostKey(87u, 'A');
const std::string kControllerKey(87u, 'B');

std::string quote(const std::string &value) { return "\"" + value + "\""; }

bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

/* Pull the value of "<key>":"<value>" out of a compact JSON message. */
std::string stringField(const std::string &message, const std::string &key) {
    const std::string anchor = quote(key) + ":\"";
    const size_t start = message.find(anchor);
    assert(start != std::string::npos);
    const size_t valueStart = start + anchor.size();
    const size_t valueEnd = message.find('"', valueStart);
    assert(valueEnd != std::string::npos);
    return message.substr(valueStart, valueEnd - valueStart);
}

/* A room wired to a deterministic clock and RNG, with the host sink captured
 * into `hostMessages`. */
struct Fixture {
    std::shared_ptr<MdkrLanPartyRoom> room;
    std::vector<std::string> hostMessages;
    MdkrLanPartyInvite invite;
    uint64_t clockMs = 1'000'000u;

    Fixture() {
        MdkrLanPartyRoomConfig config;
        uint64_t *clock = &clockMs;
        config.nowMs = [clock]() { return *clock; };
        /* splitmix64 stream: distinct, well-spread bytes every call so the
         * six-digit rejection sampler almost never rerolls and every minted
         * id/capability differs. */
        auto counter = std::make_shared<uint64_t>(0x9E3779B97F4A7C15ull);
        config.randomBytes = [counter](uint8_t *out, size_t length) {
            for (size_t index = 0u; index < length; index++) {
                uint64_t z = (*counter += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                out[index] = static_cast<uint8_t>(z & 0xffu);
            }
        };
        room = std::make_shared<MdkrLanPartyRoom>(config);
        room->open(kHostKey, "http://192.168.1.5:49200",
                   [this](const std::string &json) {
                       hostMessages.push_back(json);
                   });
        invite = room->invite();
    }

    std::shared_ptr<FakeSocket> attach() {
        auto socket = std::make_shared<FakeSocket>();
        room->attachController(socket);
        return socket;
    }

    std::string lastHost() const {
        assert(!hostMessages.empty());
        return hostMessages.back();
    }
    bool hostSaw(const std::string &needle) const {
        for (const std::string &message : hostMessages) {
            if (contains(message, needle)) return true;
        }
        return false;
    }
};

std::string redeemCapability(const std::string &capability,
                             const std::string &name = "Pixel") {
    return std::string("{\"type\":\"redeem\",\"capability\":") +
           quote(capability) + ",\"protocol\":2,\"name\":" + quote(name) +
           ",\"controllerPublicKey\":" + quote(kControllerKey) + "}";
}

std::string redeemCode(const std::string &code) {
    return std::string("{\"type\":\"redeem\",\"code\":") + quote(code) +
           ",\"protocol\":2,\"name\":\"Pixel\",\"controllerPublicKey\":" +
           quote(kControllerKey) + "}";
}

/* ---- Cases -------------------------------------------------------------- */

void openMintsA256BitCapabilityAndSixDigitCode() {
    Fixture fixture;
    assert(fixture.invite.active);
    assert(fixture.invite.capability.size() == 43u); /* 32 bytes base64url */
    assert(fixture.invite.roomId.size() == 22u);     /* 16 bytes base64url */
    assert(fixture.invite.fallbackCode.size() == 6u);
    for (char digit : fixture.invite.fallbackCode) {
        assert(digit >= '0' && digit <= '9');
    }
    assert(fixture.invite.hostPublicKey == kHostKey);
    assert(fixture.invite.inviteGeneration == 1u);
    assert(contains(fixture.invite.controllerUrl,
                    "http://192.168.1.5:49200/controller/#"));
    assert(contains(fixture.invite.controllerUrl, fixture.invite.capability));
    /* The host is told the initial (empty) room the instant it opens. */
    assert(fixture.hostMessages.size() == 1u);
    assert(contains(fixture.hostMessages[0], "\"type\":\"room_state\""));
    assert(contains(fixture.hostMessages[0], "\"controllers\":[]"));
}

/* THE closed boundary: an upgraded socket proves nothing. Anything but a
 * valid redeem from an un-redeemed socket closes it (4001) and never touches
 * room state or the host. */
void anonymousSocketDoesNothingUntilItRedeems() {
    Fixture fixture;
    const size_t hostBefore = fixture.hostMessages.size();

    auto hello = fixture.attach();
    hello->inject("{\"type\":\"controller_hello\",\"controllerId\":\"" +
                  std::string(22u, 'C') + "\"}");
    assert(hello->closed);
    assert(hello->closeCode == 4001u);

    auto answer = fixture.attach();
    answer->inject("{\"type\":\"webrtc_answer\",\"controllerId\":\"" +
                   std::string(22u, 'C') +
                   "\",\"peerGeneration\":1,\"sdp\":{\"type\":\"answer\","
                   "\"sdp\":\"v=0\"}}");
    assert(answer->closed);
    assert(answer->closeCode == 4001u);

    auto garbage = fixture.attach();
    garbage->inject("not even json");
    assert(garbage->closed);

    /* Nothing an anonymous socket did was allowed to move room state. */
    assert(fixture.hostMessages.size() == hostBefore);
}

void redeemByCapabilityLeasesASeatEndToEnd() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));

    /* redeem_result then the pending controller_state, on the same socket. */
    assert(phone->count() >= 2u);
    assert(contains(phone->sent[0], "\"type\":\"redeem_result\""));
    assert(contains(phone->sent[0], "\"ok\":true"));
    assert(contains(phone->sent[0], "\"protocol\":2"));
    assert(contains(phone->sent[0], "\"hostPublicKey\":" + quote(kHostKey)));
    const std::string controllerId = stringField(phone->sent[0], "controllerId");
    assert(controllerId.size() == 22u);
    assert(phone->sawText("\"phase\":\"pending\""));

    /* Host learns of the pending controller via room_state. */
    assert(fixture.hostSaw("\"phase\":\"pending\""));
    assert(fixture.hostSaw(quote(controllerId)));
    assert(fixture.hostSaw("\"controllerPublicKey\":" + quote(kControllerKey)));

    /* Host approves seat 3. */
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"approve\",\"controllerId\":" +
        quote(controllerId) + ",\"seat\":3}");
    assert(phone->sawText("\"phase\":\"leased\""));
    assert(phone->sawText("\"seat\":3"));
    assert(phone->last().find("\"leaseGeneration\":0") == std::string::npos);
    assert(fixture.hostSaw("\"phase\":\"leased\""));
}

void protocolOneIsRefusedAsProtocolUpdateRequired() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(std::string("{\"type\":\"redeem\",\"capability\":") +
                  quote(fixture.invite.capability) +
                  ",\"protocol\":1,\"controllerPublicKey\":" +
                  quote(kControllerKey) + "}");
    assert(phone->sawText("\"error\":\"protocol_update_required\""));
    assert(phone->closed);
}

void redeemByCodeWorksAndWrongCodesThrottleAfterTwelve() {
    Fixture fixture;

    /* The minted code redeems cleanly. */
    auto good = fixture.attach();
    good->inject(redeemCode(fixture.invite.fallbackCode));
    assert(good->sawText("\"type\":\"redeem_result\""));
    assert(good->sawText("\"ok\":true"));

    /* Twelve wrong six-digit codes are each refused as a rotated invite; the
     * shared bucket then locks the thirteenth as rate_limited -- one bucket,
     * no per-source identity on a LAN. */
    for (int attempt = 0; attempt < 11; attempt++) {
        auto miss = fixture.attach();
        miss->inject(redeemCode("000001"));
        assert(miss->sawText("\"ok\":false"));
        assert(miss->sawText("\"error\":\"invite_rotated\""));
        assert(!miss->sawText("\"error\":\"rate_limited\""));
    }
    /* The good redeem above already consumed one of the twelve attempts, so
     * the twelfth total attempt is the last allowed and the thirteenth locks. */
    auto locked = fixture.attach();
    locked->inject(redeemCode("000002"));
    assert(locked->sawText("\"error\":\"rate_limited\""));

    /* The window slides: past the 10-minute horizon the bucket admits again.
     * Rotate first so the invite's own 2-minute TTL is not what refuses us --
     * the point under test is that the throttle, not the invite, has reset. */
    fixture.clockMs += kMdkrLanPartyCodeWindowMs + 1u;
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"rotate\","
        "\"expectedInviteGeneration\":1}");
    auto later = fixture.attach();
    later->inject(redeemCode("000003"));
    assert(!later->sawText("\"error\":\"rate_limited\""));
    assert(later->sawText("\"error\":\"invite_rotated\""));
}

/* Host-authoritative seat CAS: two controllers cannot hold the same seat.
 * The second approve fails closed with room_full, and the failure names the
 * command and the controller it targeted (the Task 6 identity echo). */
void seatCasRefusesDoubleOccupancyWithIdentityEcho() {
    Fixture fixture;
    auto first = fixture.attach();
    first->inject(redeemCapability(fixture.invite.capability, "One"));
    const std::string firstId = stringField(first->sent[0], "controllerId");

    auto second = fixture.attach();
    second->inject(redeemCapability(fixture.invite.capability, "Two"));
    const std::string secondId = stringField(second->sent[0], "controllerId");
    assert(firstId != secondId);

    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"approve\",\"controllerId\":" +
        quote(firstId) + ",\"seat\":2}");
    assert(first->sawText("\"phase\":\"leased\""));

    const size_t hostBefore = fixture.hostMessages.size();
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"approve\",\"controllerId\":" +
        quote(secondId) + ",\"seat\":2}");
    /* Second phone is NOT leased; the room did not double-book. */
    assert(!second->sawText("\"phase\":\"leased\""));
    assert(fixture.hostMessages.size() > hostBefore);
    const std::string result = fixture.lastHost();
    assert(contains(result, "\"type\":\"host_command_result\""));
    assert(contains(result, "\"ok\":false"));
    assert(contains(result, "\"error\":\"room_full\""));
    assert(contains(result, "\"command\":\"approve\""));
    assert(contains(result, "\"controllerId\":" + quote(secondId)));
}

void approveOfUnknownControllerEchoesIdentity() {
    Fixture fixture;
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"approve\",\"controllerId\":\"" +
        std::string(22u, 'Z') + "\",\"seat\":1}");
    const std::string result = fixture.lastHost();
    assert(contains(result, "\"type\":\"host_command_result\""));
    assert(contains(result, "\"error\":\"not_found\""));
    assert(contains(result, "\"command\":\"approve\""));
    assert(contains(result, "\"controllerId\":\"" + std::string(22u, 'Z') + "\""));
}

/* Signaling is relayed in order and rebuilt from a whitelist: the controller
 * id on the wire is ignored in favour of the socket's authenticated id, so an
 * authenticated peer cannot address another seat or smuggle fields. */
void signalRelayPreservesOrderAndForcesIdentity() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const std::string realId = stringField(phone->sent[0], "controllerId");

    /* Host -> controller: offer, ice, ice, delivered to this socket in order. */
    const size_t before = phone->count();
    fixture.room->deliverFromHost(
        "{\"type\":\"webrtc_offer\",\"to\":" + quote(realId) +
        ",\"peerGeneration\":1,\"sdp\":{\"type\":\"offer\",\"sdp\":\"O1\"}}");
    fixture.room->deliverFromHost(
        "{\"type\":\"webrtc_ice\",\"to\":" + quote(realId) +
        ",\"peerGeneration\":1,\"candidate\":{\"candidate\":\"A1\"}}");
    fixture.room->deliverFromHost(
        "{\"type\":\"webrtc_ice\",\"to\":" + quote(realId) +
        ",\"peerGeneration\":1,\"candidate\":{\"candidate\":\"A2\"}}");
    assert(phone->count() == before + 3u);
    assert(contains(phone->sent[before + 0u], "\"type\":\"webrtc_offer\""));
    assert(contains(phone->sent[before + 0u], "\"sdp\":\"O1\""));
    assert(contains(phone->sent[before + 1u], "\"candidate\":\"A1\""));
    assert(contains(phone->sent[before + 2u], "\"candidate\":\"A2\""));

    /* Controller -> host: answer then ice, in order, addressed by the socket's
     * OWN id even though the frame lies about controllerId. */
    const size_t hostBefore = fixture.hostMessages.size();
    const std::string spoof(22u, 'Q');
    assert(spoof != realId);
    phone->inject("{\"type\":\"webrtc_answer\",\"controllerId\":" + quote(spoof) +
                  ",\"peerGeneration\":1,\"sdp\":{\"type\":\"answer\","
                  "\"sdp\":\"ANS\"}}");
    phone->inject("{\"type\":\"webrtc_ice\",\"controllerId\":" + quote(spoof) +
                  ",\"peerGeneration\":1,\"candidate\":{\"candidate\":\"CICE\"}}");
    assert(fixture.hostMessages.size() == hostBefore + 2u);
    const std::string relayedAnswer = fixture.hostMessages[hostBefore + 0u];
    const std::string relayedIce = fixture.hostMessages[hostBefore + 1u];
    assert(contains(relayedAnswer, "\"type\":\"webrtc_answer\""));
    assert(contains(relayedAnswer, "\"sdp\":\"ANS\""));
    assert(contains(relayedAnswer, "\"controllerId\":" + quote(realId)));
    assert(!contains(relayedAnswer, spoof));
    assert(contains(relayedIce, "\"candidate\":\"CICE\""));
    assert(contains(relayedIce, "\"controllerId\":" + quote(realId)));
}

void malformedSignalFromAuthenticatedControllerIsRefused() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const size_t hostBefore = fixture.hostMessages.size();
    /* Extra unknown field -> not an exact-keys match -> invalid signaling. */
    phone->inject("{\"type\":\"webrtc_answer\",\"controllerId\":\"" +
                  stringField(phone->sent[0], "controllerId") +
                  "\",\"peerGeneration\":1,\"sdp\":{\"type\":\"answer\","
                  "\"sdp\":\"ANS\"},\"smuggled\":true}");
    assert(phone->closed);
    assert(phone->closeCode == 4003u);
    assert(fixture.hostMessages.size() == hostBefore); /* nothing relayed */
}

void inviteExpiryRefusesRedeem() {
    Fixture fixture;
    fixture.clockMs += kMdkrLanPartyInviteTtlMs + 1u;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    assert(phone->sawText("\"error\":\"invite_expired\""));
    assert(phone->closed);
}

/* Rotation mints a fresh capability + code, bumps the generation, tells the
 * host the new invite, and retires the old one. */
void rotateInviteReplacesTheCapabilityAndCode() {
    Fixture fixture;
    const std::string oldCapability = fixture.invite.capability;
    const std::string oldCode = fixture.invite.fallbackCode;

    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"rotate\","
        "\"expectedInviteGeneration\":1}");
    const std::string rotated = fixture.lastHost();
    assert(contains(rotated, "\"type\":\"room_state\""));
    assert(contains(rotated, "\"inviteGeneration\":2"));
    assert(contains(rotated, "\"fallbackCode\":"));
    assert(contains(rotated, "\"controllerUrl\":"));

    const MdkrLanPartyInvite fresh = fixture.room->invite();
    assert(fresh.inviteGeneration == 2u);
    assert(fresh.capability != oldCapability);
    assert(fresh.fallbackCode != oldCode);

    /* The old capability is dead; the new one pairs. */
    auto stale = fixture.attach();
    stale->inject(redeemCapability(oldCapability));
    assert(stale->sawText("\"error\":\"invite_rotated\""));

    auto fresher = fixture.attach();
    fresher->inject(redeemCapability(fresh.capability));
    assert(fresher->sawText("\"ok\":true"));

    /* A stale expected generation is refused fail-closed. */
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"rotate\","
        "\"expectedInviteGeneration\":1}");
    assert(contains(fixture.lastHost(), "\"error\":\"invalid_state\""));
}

/* host_closed on close: every controller socket is closed 4000 host_closed
 * and the host is told the room is gone. */
void closeTearsDownEverySocketWithHostClosed() {
    Fixture fixture;
    auto a = fixture.attach();
    a->inject(redeemCapability(fixture.invite.capability, "A"));
    auto b = fixture.attach();
    b->inject(redeemCapability(fixture.invite.capability, "B"));

    fixture.room->close();
    assert(a->closed && a->closeCode == 4000u);
    assert(b->closed && b->closeCode == 4000u);
    assert(a->sawText("host_closed") || a->closeCode == 4000u);
    assert(fixture.hostSaw("\"type\":\"host_closed\""));

    /* A socket that attaches after close is refused; the room is terminal. */
    auto late = fixture.attach();
    late->inject(redeemCapability(fixture.invite.capability));
    assert(late->closed);
    assert(!late->sawText("\"ok\":true"));
}

void rejectAndRemoveCloseTheControllerSocket() {
    Fixture fixture;
    auto pending = fixture.attach();
    pending->inject(redeemCapability(fixture.invite.capability, "Pend"));
    const std::string pendingId = stringField(pending->sent[0], "controllerId");
    fixture.room->deliverFromHost(
        "{\"type\":\"host_command\",\"action\":\"reject\",\"controllerId\":" +
        quote(pendingId) + "}");
    assert(pending->sawText("\"phase\":\"closed\"") ||
           pending->closeCode == 4000u);
    assert(pending->closed);
    assert(pending->closeCode == 4000u);
}

/* A phone that drops its socket is deregistered from the relay: a later host
 * offer to its id is simply not delivered, and the room keeps working. This
 * also exercises the onClosed path that must never keep a socket alive
 * through its own callbacks. */
void peerCloseDeregistersTheControllerFromRelay() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const std::string id = stringField(phone->sent[0], "controllerId");

    phone->peerClose();
    const size_t before = phone->count();
    fixture.room->deliverFromHost(
        "{\"type\":\"webrtc_offer\",\"to\":" + quote(id) +
        ",\"peerGeneration\":1,\"sdp\":{\"type\":\"offer\",\"sdp\":\"O\"}}");
    assert(phone->count() == before); /* nothing pushed to a dead socket */

    /* A fresh phone can still redeem and lease afterwards. */
    auto next = fixture.attach();
    next->inject(redeemCapability(fixture.invite.capability, "Next"));
    assert(next->sawText("\"ok\":true"));
}

/* Post-auth flood defence: an authenticated controller gets 120 signal frames
 * per 10-second window; the 121st in the window closes the socket 4008 and is
 * not relayed (worker admitSignalMessage parity). */
void signalFloodInAWindowClosesFourZeroZeroEight() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const std::string id = stringField(phone->sent[0], "controllerId");
    const std::string hello =
        "{\"type\":\"controller_hello\",\"controllerId\":" + quote(id) + "}";

    const size_t hostBefore = fixture.hostMessages.size();
    for (unsigned n = 0u; n < kMdkrLanPartySignalWindowMessages; n++) {
        phone->inject(hello);
        assert(!phone->closed);
    }
    assert(fixture.hostMessages.size() ==
           hostBefore + kMdkrLanPartySignalWindowMessages);

    phone->inject(hello); /* the 121st */
    assert(phone->closed);
    assert(phone->closeCode == 4008u);
    assert(fixture.hostMessages.size() ==
           hostBefore + kMdkrLanPartySignalWindowMessages);
}

/* A slow but sustained legitimate stream is never window-throttled, yet the
 * hard 512-frame lifetime cap still trips. */
void signalLifetimeCapClosesAtFiveTwelve() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const std::string id = stringField(phone->sent[0], "controllerId");
    const std::string hello =
        "{\"type\":\"controller_hello\",\"controllerId\":" + quote(id) + "}";

    for (unsigned n = 0u; n < kMdkrLanPartySignalLifetimeMessages; n++) {
        if (n % 100u == 0u) fixture.clockMs += kMdkrLanPartySignalWindowMs + 1u;
        phone->inject(hello);
        assert(!phone->closed); /* the window never trips for a spread stream */
    }
    fixture.clockMs += kMdkrLanPartySignalWindowMs + 1u; /* a fresh window */
    phone->inject(hello);                                 /* the 513th overall */
    assert(phone->closed);
    assert(phone->closeCode == 4008u);
}

/* Defense in depth + worker parity: the inbound controllerId must be a
 * 22-char base64url even though the relayed id is forced to the socket's. */
void controllerSignalWithMalformedIdIsRefused() {
    Fixture fixture;
    auto phone = fixture.attach();
    phone->inject(redeemCapability(fixture.invite.capability));
    const size_t hostBefore = fixture.hostMessages.size();
    phone->inject("{\"type\":\"controller_hello\",\"controllerId\":\"tooShort\"}");
    assert(phone->closed);
    assert(phone->closeCode == 4003u);
    assert(fixture.hostMessages.size() == hostBefore);
}

/* normalizeName strips the worker's zero-width and bidi code points, not just
 * C0 control bytes, so no invisible or direction-flipping byte reaches the
 * host's controller row. */
void nameStripsZeroWidthAndBidiControls() {
    Fixture fixture;
    auto phone = fixture.attach();
    const std::string sneaky = std::string("A\xe2\x80\x8b") + "\xe2\x80\xae" + "B";
    phone->inject(redeemCapability(fixture.invite.capability, sneaky));
    assert(fixture.hostSaw("\"name\":\"AB\""));
    assert(!fixture.hostSaw("\xe2\x80\x8b"));
    assert(!fixture.hostSaw("\xe2\x80\xae"));
}

} // namespace

int main() {
    openMintsA256BitCapabilityAndSixDigitCode();
    anonymousSocketDoesNothingUntilItRedeems();
    redeemByCapabilityLeasesASeatEndToEnd();
    protocolOneIsRefusedAsProtocolUpdateRequired();
    redeemByCodeWorksAndWrongCodesThrottleAfterTwelve();
    seatCasRefusesDoubleOccupancyWithIdentityEcho();
    approveOfUnknownControllerEchoesIdentity();
    signalRelayPreservesOrderAndForcesIdentity();
    malformedSignalFromAuthenticatedControllerIsRefused();
    inviteExpiryRefusesRedeem();
    rotateInviteReplacesTheCapabilityAndCode();
    closeTearsDownEverySocketWithHostClosed();
    rejectAndRemoveCloseTheControllerSocket();
    peerCloseDeregistersTheControllerFromRelay();
    signalFloodInAWindowClosesFourZeroZeroEight();
    signalLifetimeCapClosesAtFiveTwelve();
    controllerSignalWithMalformedIdIsRefused();
    nameStripsZeroWidthAndBidiControls();
    std::fprintf(stderr, "test_lan_party_room: all cases passed\n");
    return 0;
}
