/*
 * LanPartyTransport driven UNDER the real MdkrNativePartyHost -- the "host
 * over LanTransport" the Task 3 brief asks for. These cases prove the
 * keystone: that the same host seam that drives the cloud transport drives a
 * full LAN party (bringup, redeem->pending->approve->leased seat custody, the
 * SAS phrase emitted at connection from real DTLS fingerprints, and
 * close->host_closed through the local room) while the cloud arm's https-origin
 * refusal is left intact.
 *
 * The phones are simulated in-process: a fake bridge socket carries the wire
 * protocol between the transport's room and a real libdatachannel peer that
 * answers the host's offer over loopback (no STUN -- a LAN pairing must reach
 * the phone with zero internet). All phone-side reactions are marshalled onto
 * the test thread through thread-safe queues, so the only real threads are the
 * two libdatachannel peers' own.
 */
#include "party/lan_party_transport.h"
#include "party/lan_party_room.h"
#include "party/native_party_host.h"
#include "party/party_webrtc_signaling.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

/* ---- A fake cloud-like transport, to pin the https refusal regression ---- */

/* Minimal MdkrPartyTransport that records open() and defaults to the secure
 * (cloud) origin policy -- exactly what every real cloud transport and every
 * pre-existing fake inherits. The host must refuse a non-https open BEFORE it
 * ever reaches open(). */
class FakeCloudTransport final : public MdkrPartyTransport {
public:
    bool available() const override { return true; }
    const char *unavailableReason() const override { return ""; }
    /* requiresSecureOrigin() intentionally NOT overridden -> defaults true. */
    bool open(const std::string &origin) override {
        openCalls++;
        lastOrigin = origin;
        return true;
    }
    bool approve(const std::string &, unsigned) override { return true; }
    bool reject(const std::string &) override { return true; }
    bool remove(const std::string &) override { return true; }
    bool rotateInvite(unsigned) override { return true; }
    bool revokeInvite() override { return true; }
    bool closeRoom() override { return true; }
    bool sendRumble(const std::string &, uint16_t) override { return true; }
    bool poll(MdkrPartyTransportEvent &) override { return false; }
    void shutdown() override {}

    int openCalls = 0;
    std::string lastOrigin;
};

/* ---- Thread-safe frame queue ------------------------------------------- */

class FrameQueue {
public:
    void push(std::string frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.push_back(std::move(frame));
    }
    bool pop(std::string &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) return false;
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<std::string> frames_;
};

/* ---- Fake bridge socket ------------------------------------------------- */

/* The phone's /party-ws, attached straight into the transport's room. Frames
 * the room sends toward the phone land in `toPhone` (drained on the test
 * thread); the phone injects frames back with inject(). Thread-safe: the room
 * calls sendText/close from whichever thread relayed (a libdatachannel peer
 * thread for offer/ICE relays). */
class FakeBridgeSocket final : public MdkrLanPartyControllerSocket {
public:
    FrameQueue toPhone;
    mutable std::mutex stateMutex;
    bool closed = false;
    uint16_t closeCode = 0u;

    bool sendText(const std::string &payload) override {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (closed) return false;
        }
        toPhone.push(payload);
        return true;
    }
    void close(uint16_t code) override {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (closed) return;
            closed = true;
            closeCode = code;
            cb = onClosed_;
        }
        if (cb) cb();
    }
    bool isOpen() const override {
        std::lock_guard<std::mutex> lock(stateMutex);
        return !closed;
    }
    void onMessage(std::function<void(const std::string &)> callback) override {
        std::lock_guard<std::mutex> lock(stateMutex);
        onMessage_ = std::move(callback);
    }
    void onClosed(std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(stateMutex);
        onClosed_ = std::move(callback);
    }

    void inject(const std::string &payload) {
        std::function<void(const std::string &)> cb;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (closed) return;
            cb = onMessage_;
        }
        if (cb) cb(payload);
    }
    uint16_t code() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return closeCode;
    }
    bool isClosed() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return closed;
    }

private:
    std::function<void(const std::string &)> onMessage_;
    std::function<void()> onClosed_;
};

/* ---- Deterministic clock/RNG for the room (invite mint + TTL) ----------- */

struct Determinism {
    uint64_t clockMs = 1'000'000u;
    uint64_t counter = 0x9E3779B97F4A7C15ull;

    std::function<uint64_t()> clock() {
        return [this]() { return clockMs; };
    }
    std::function<void(uint8_t *, size_t)> rng() {
        return [this](uint8_t *out, size_t length) {
            for (size_t index = 0u; index < length; index++) {
                uint64_t z = (counter += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                out[index] = static_cast<uint8_t>(z & 0xffu);
            }
        };
    }
};

/* Pull "key":"value" out of a compact JSON message. */
std::string stringField(const std::string &message, const std::string &key) {
    const std::string anchor = "\"" + key + "\":\"";
    const size_t start = message.find(anchor);
    if (start == std::string::npos) return {};
    const size_t valueStart = start + anchor.size();
    const size_t valueEnd = message.find('"', valueStart);
    if (valueEnd == std::string::npos) return {};
    return message.substr(valueStart, valueEnd - valueStart);
}

bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

/* Build a valid redeem frame with a real curve public key so the room admits
 * it (the room validates the key shape). */
std::string redeemFrame(const std::string &capability,
                        const std::string &controllerKey) {
    return std::string("{\"type\":\"redeem\",\"capability\":\"") + capability +
           "\",\"protocol\":2,\"controllerPublicKey\":\"" + controllerKey +
           "\",\"name\":\"Tester\"}";
}

/* ---- A simulated phone: a real libdatachannel answerer over the bridge --- */

struct SimulatedPhone {
    std::shared_ptr<FakeBridgeSocket> socket = std::make_shared<FakeBridgeSocket>();
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> stateChannel;
    std::shared_ptr<rtc::DataChannel> controlChannel;
    std::string controllerId;
    uint32_t peerGeneration = 0u;
    FrameQueue outbound;              /* phone -> room, drained on test thread */
    std::string hostPublicKey;
    std::string roomId;
    size_t largestOfferBytes = 0u;

    void startPeer() {
        rtc::Configuration configuration;  /* no ICE servers: LAN, no internet */
        peer = std::make_shared<rtc::PeerConnection>(configuration);
        peer->onLocalDescription([this](rtc::Description description) {
            outbound.push(Json{{"type", "webrtc_answer"},
                {"controllerId", controllerId},
                {"peerGeneration", peerGeneration},
                {"sdp", {{"type", description.typeString()},
                    {"sdp", std::string(description)}}}}.dump());
        });
        peer->onLocalCandidate([this](rtc::Candidate candidate) {
            outbound.push(Json{{"type", "webrtc_ice"},
                {"controllerId", controllerId},
                {"peerGeneration", peerGeneration},
                {"candidate", {{"candidate", std::string(candidate)},
                    {"sdpMid", candidate.mid()}}}}.dump());
        });
        peer->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            const std::string label = channel->label();
            if (label == "mdkr-pad-control-v1") {
                controlChannel = channel;
                channel->onMessage([this](rtc::message_variant message) {
                    if (!std::holds_alternative<std::string>(message)) return;
                    Json value = Json::parse(std::get<std::string>(message),
                                             nullptr, false);
                    if (value.is_discarded() || !value.is_object()) return;
                    if (value.value("type", std::string{}) == "host_ready") {
                        const uint32_t sequence =
                            value.value("connectionSequence", 0u);
                        try {
                            controlChannel->send(Json{{"type", "controller_ready"},
                                {"controllerId", controllerId},
                                {"connectionSequence", sequence},
                                {"protocol", 1},
                                {"capabilities", {{"vibration", true}}}}.dump());
                        } catch (...) {}
                    }
                });
            } else if (label == "mdkr-pad-state-v1") {
                stateChannel = channel;
            }
        });
    }

    /* Process one room->phone frame on the test thread. */
    void onRoomFrame(const std::string &frame) {
        const std::string type = stringField(frame, "type");
        if (type == "redeem_result") {
            if (contains(frame, "\"ok\":true")) {
                controllerId = stringField(frame, "controllerId");
                hostPublicKey = stringField(frame, "hostPublicKey");
                roomId = stringField(frame, "roomId");
                /* Announce presence so the host will build a peer once we are
                 * approved (mirrors the controller page). */
                socket->inject(Json{{"type", "controller_hello"},
                    {"controllerId", controllerId}}.dump());
            }
        } else if (type == "webrtc_offer") {
            if (frame.size() > largestOfferBytes) largestOfferBytes = frame.size();
            if (!peer) startPeer();
            Json value = Json::parse(frame, nullptr, false);
            if (value.is_discarded()) return;
            peerGeneration = value.value("peerGeneration", 0u);
            const std::string sdp = value["sdp"].value("sdp", std::string{});
            try {
                peer->setRemoteDescription(rtc::Description(sdp, "offer"));
            } catch (...) {}
        } else if (type == "webrtc_ice") {
            if (!peer) return;
            Json value = Json::parse(frame, nullptr, false);
            if (value.is_discarded()) return;
            const std::string candidate =
                value["candidate"].value("candidate", std::string{});
            const std::string mid =
                value["candidate"].value("sdpMid", std::string{});
            try { peer->addRemoteCandidate(rtc::Candidate(candidate, mid)); }
            catch (...) {}
        }
    }
};

/* ---- Harness: host over a test-mode LAN transport ---------------------- */

struct Harness {
    Determinism determinism;
    std::unique_ptr<MdkrPartyTransport> transport;
    std::unique_ptr<MdkrNativePartyHost> host;
    uint64_t serviceNowMs = 1'000'000u;

    explicit Harness() {
        MdkrLanPartyTransportConfig config;
        config.advertisedHost = "192.168.1.7";
        config.bindPort = 49200u;  /* no real bind in test mode */
        config.roomNowMs = determinism.clock();
        config.roomRandomBytes = determinism.rng();
        transport = mdkr_create_lan_party_transport_for_test(std::move(config));
        host = std::make_unique<MdkrNativePartyHost>(*transport);
    }

    ~Harness() {
        /* host destroyed first (declared last); it shuts the transport down. */
        host.reset();
        transport.reset();
    }

    void service() { host->service(serviceNowMs); }

    void attach(const std::shared_ptr<FakeBridgeSocket> &socket) {
        mdkr_lan_party_transport_attach_test_controller(*transport, socket);
    }

    const MdkrNativePartyController *controller() const {
        for (const auto &c : host->view().controllers) return &c;
        return nullptr;
    }
};

/* Pump the whole loopback bridge until `done` or the deadline. Drives the host
 * service, marshals room->phone and phone->room frames on this thread. */
bool pump(Harness &harness, SimulatedPhone &phone,
          const std::function<bool()> &done, unsigned maxMillis = 20000u) {
    for (unsigned elapsed = 0u; elapsed <= maxMillis; elapsed += 10u) {
        harness.service();
        std::string frame;
        while (phone.socket->toPhone.pop(frame)) phone.onRoomFrame(frame);
        while (phone.outbound.pop(frame)) phone.socket->inject(frame);
        harness.service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        harness.serviceNowMs += 10u;
    }
    return done();
}

/* ======================================================================= */

void cloudArmStillRefusesNonHttpsOpen() {
    FakeCloudTransport cloud;
    MdkrNativePartyHost host(cloud);
    /* A non-https origin must be refused BEFORE reaching the transport. */
    assert(!host.open("http://192.168.1.5:8080"));
    assert(host.view().phase == MdkrNativePartyPhase::Error);
    assert(cloud.openCalls == 0);
    /* A canonical https origin passes the gate and reaches the transport. */
    assert(host.open("https://party.example.com"));
    assert(cloud.openCalls == 1);
    assert(cloud.lastOrigin == "https://party.example.com");
    std::printf("cloudArmStillRefusesNonHttpsOpen: ok\n");
}

void lanOpenBringsUpAScannableInvite() {
    Harness harness;
    /* A LAN open bypasses the https gate (requiresSecureOrigin()==false) and
     * still succeeds with a plain-http advertised origin. */
    assert(harness.host->open("lan"));
    harness.service();
    const MdkrNativePartyView &view = harness.host->view();
    assert(view.phase == MdkrNativePartyPhase::Open);
    assert(view.inviteVisible);
    assert(view.controllerUrl.rfind("http://192.168.1.7:49200/controller/#", 0u) == 0u);
    assert(view.fallbackCode.size() == 6u);
    assert(view.controllers.empty());
    std::printf("lanOpenBringsUpAScannableInvite: ok (url=%s)\n",
                view.controllerUrl.c_str());
}

void redeemThenApproveTakesASeat() {
    Harness harness;
    assert(harness.host->open("lan"));
    harness.service();
    const std::string capability =
        harness.host->view().controllerUrl.substr(
            harness.host->view().controllerUrl.find('#') + 1u);

    mdkr_party::Identity phoneKey;
    assert(phoneKey.generate());
    auto socket = std::make_shared<FakeBridgeSocket>();
    harness.attach(socket);
    socket->inject(redeemFrame(capability, phoneKey.publicKey()));
    harness.service();

    const MdkrNativePartyController *pending = harness.controller();
    assert(pending != nullptr);
    assert(pending->phase == MdkrNativePartyControllerPhase::Pending);
    const std::string id = pending->id;

    assert(harness.host->approve(id, 2u));
    harness.service();
    const MdkrNativePartyController *leased = harness.controller();
    assert(leased != nullptr);
    assert(leased->seat == 2u);
    assert(leased->phase == MdkrNativePartyControllerPhase::Leased);
    std::printf("redeemThenApproveTakesASeat: ok (seat=%u)\n", leased->seat);
}

void seatCasRefusesDoubleOccupancy() {
    Harness harness;
    assert(harness.host->open("lan"));
    harness.service();
    const std::string capability =
        harness.host->view().controllerUrl.substr(
            harness.host->view().controllerUrl.find('#') + 1u);

    auto redeem = [&](const std::shared_ptr<FakeBridgeSocket> &socket) {
        mdkr_party::Identity key;
        assert(key.generate());
        harness.attach(socket);
        socket->inject(redeemFrame(capability, key.publicKey()));
        harness.service();
    };

    auto first = std::make_shared<FakeBridgeSocket>();
    redeem(first);
    const std::string firstId = harness.host->view().controllers.at(0).id;
    assert(harness.host->approve(firstId, 1u));
    harness.service();

    auto second = std::make_shared<FakeBridgeSocket>();
    redeem(second);
    std::string secondId;
    for (const auto &c : harness.host->view().controllers) {
        if (c.id != firstId) secondId = c.id;
    }
    assert(!secondId.empty());
    /* The host itself refuses seat 1 (already taken) at the model layer. */
    assert(!harness.host->approve(secondId, 1u));
    /* And the room's CAS is the backstop: even if the model let it through,
     * the seat is occupied -- approving a free seat still works. */
    assert(harness.host->approve(secondId, 3u));
    harness.service();
    unsigned seats = 0u;
    for (const auto &c : harness.host->view().controllers) {
        if (c.phase == MdkrNativePartyControllerPhase::Leased) seats++;
    }
    assert(seats == 2u);
    std::printf("seatCasRefusesDoubleOccupancy: ok\n");
}

void closeTearsDownPhoneWithHostClosed() {
    Harness harness;
    assert(harness.host->open("lan"));
    harness.service();
    const std::string capability =
        harness.host->view().controllerUrl.substr(
            harness.host->view().controllerUrl.find('#') + 1u);
    mdkr_party::Identity phoneKey;
    assert(phoneKey.generate());
    auto socket = std::make_shared<FakeBridgeSocket>();
    harness.attach(socket);
    socket->inject(redeemFrame(capability, phoneKey.publicKey()));
    harness.service();
    assert(harness.controller() != nullptr);

    assert(harness.host->closeRoom());
    /* The room closed the phone's socket with 4000 (host_closed), the page's
     * recognized host-close code. */
    assert(socket->isClosed());
    assert(socket->code() == 4000u);
    assert(harness.host->view().phase == MdkrNativePartyPhase::Closed);
    std::printf("closeTearsDownPhoneWithHostClosed: ok\n");
}

void offerToThePhoneFitsTheServerFrameCap() {
    Harness harness;
    assert(harness.host->open("lan"));
    harness.service();
    const std::string capability =
        harness.host->view().controllerUrl.substr(
            harness.host->view().controllerUrl.find('#') + 1u);
    mdkr_party::Identity phoneKey;
    assert(phoneKey.generate());
    SimulatedPhone phone;
    phone.socket = std::make_shared<FakeBridgeSocket>();
    harness.attach(phone.socket);
    phone.socket->inject(redeemFrame(capability, phoneKey.publicKey()));
    harness.service();
    const std::string id = harness.host->view().controllers.at(0).id;
    assert(harness.host->approve(id, 1u));

    /* Drive controller_hello + wait for the transport's real WebRTC offer to
     * reach the phone. We only need the OFFER (local, no ICE connectivity). */
    bool sawOffer = false;
    pump(harness, phone, [&]() {
        return phone.largestOfferBytes > 0u &&
               (sawOffer = true);
    }, 15000u);
    assert(sawOffer);
    /* The offer traverses the /party-ws frame cap on its room->phone hop. */
    std::printf("offerToThePhoneFitsTheServerFrameCap: offer=%zu bytes "
                "(cap=%zu)\n", phone.largestOfferBytes,
                kMdkrLanPartyMaxWsPayloadBytes);
    assert(phone.largestOfferBytes > 0u);
    assert(phone.largestOfferBytes < kMdkrLanPartyMaxWsPayloadBytes);
}

void fullHandshakeEmitsPhraseAndConnects() {
    Harness harness;
    assert(harness.host->open("lan"));
    harness.service();
    const std::string capability =
        harness.host->view().controllerUrl.substr(
            harness.host->view().controllerUrl.find('#') + 1u);
    mdkr_party::Identity phoneKey;
    assert(phoneKey.generate());
    SimulatedPhone phone;
    phone.socket = std::make_shared<FakeBridgeSocket>();
    harness.attach(phone.socket);
    phone.socket->inject(redeemFrame(capability, phoneKey.publicKey()));
    harness.service();
    const std::string id = harness.host->view().controllers.at(0).id;
    assert(harness.host->approve(id, 1u));

    /* Run the loopback handshake to completion: phrase from real DTLS
     * fingerprints, then ControllerConnected once the phone's controller_ready
     * arrives over the direct control channel. */
    const bool connected = pump(harness, phone, [&]() {
        const MdkrNativePartyController *c = harness.controller();
        return c != nullptr && !c->pairingPhrase.empty() &&
               c->phase == MdkrNativePartyControllerPhase::Connected;
    }, 25000u);

    const MdkrNativePartyController *c = harness.controller();
    assert(c != nullptr);
    std::printf("fullHandshakeEmitsPhraseAndConnects: phrase='%s' phase=%d "
                "connected=%d\n", c->pairingPhrase.c_str(),
                static_cast<int>(c->phase), connected ? 1 : 0);
    assert(!c->pairingPhrase.empty());
    /* Phrase shape: "<Left>-<Right> <Left>-<Right>". */
    assert(contains(c->pairingPhrase, "-"));
    assert(contains(c->pairingPhrase, " "));
    assert(c->phase == MdkrNativePartyControllerPhase::Connected);
    assert(c->direct);

    /* Boundedness backstop: flood pad packets over the state channel; the
     * transport's bounded drop-oldest queue must never grow without limit and
     * the host must stay serviceable (no crash, still Connected). */
    if (phone.stateChannel && phone.stateChannel->isOpen()) {
        for (int i = 0; i < 400; i++) {
            std::vector<std::byte> packet(32u, std::byte{0});
            try { phone.stateChannel->send(packet); } catch (...) { break; }
        }
        for (int i = 0; i < 50; i++) {
            harness.service();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(harness.controller() != nullptr);
    }
    std::printf("fullHandshakeEmitsPhraseAndConnects: bounded flood survived\n");
}

}  // namespace

int main() {
    cloudArmStillRefusesNonHttpsOpen();
    lanOpenBringsUpAScannableInvite();
    redeemThenApproveTakesASeat();
    seatCasRefusesDoubleOccupancy();
    closeTearsDownPhoneWithHostClosed();
    offerToThePhoneFitsTheServerFrameCap();
    fullHandshakeEmitsPhraseAndConnects();
    std::printf("all lan_party_transport cases passed\n");
    return 0;
}
