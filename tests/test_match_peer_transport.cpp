/*
 * MdkrMatchPeerMesh: the launcher-owned <=4-endpoint full mesh of
 * WebRTC DataChannels, driven end to end over loopback with REAL
 * libdatachannel peers (real DTLS, the test_lan_party_transport discipline)
 * and an in-process fake of the match-signaling relay.
 *
 * The fake hub implements the MdkrMatchPeerSignalFeed seam: every outbound
 * signal message a mesh sends is captured, translated exactly the way
 * services/party/src/match relays it (authenticated from-fields injected,
 * delivery to one exact endpoint/generation) and delivered to the target
 * feed's inbox. Everything runs on the test thread except libdatachannel's
 * own peer threads, whose callbacks only enqueue.
 */
#include "online/match_peer_transport.h"

#include "net/match_peer_crypto.h"
#include "net/match_peer_transcript.h"
#include "party/party_webrtc_signaling.h"

/* Assert-driven test: NDEBUG would compile every check away. */
#undef NDEBUG

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::json;

constexpr uint32_t kEpoch = 7u;
constexpr unsigned kPhaseMs = 20000u;

/* ---- Deterministic fixtures --------------------------------------------- */

std::string roomIdFixture() {
    uint8_t raw[16];
    for (unsigned index = 0u; index < 16u; index++) {
        raw[index] = static_cast<uint8_t>(index + 1u);
    }
    return mdkr_party::base64Url(raw, sizeof(raw));
}

MdkrOnlineCompatibilityV1 compatibilityFixture() {
    MdkrOnlineCompatibilityV1 value{};
    value.protocol_version = 1u;
    for (unsigned index = 0u; index < sizeof(value.build_id); index++) {
        value.build_id[index] = static_cast<uint8_t>(index + 1u);
    }
    for (unsigned index = 0u; index < sizeof(value.gameplay_digest); index++) {
        value.gameplay_digest[index] = static_cast<uint8_t>(0x80u + index);
    }
    value.rom_revision = 1u;
    value.cadence_hz = 30u;
    return value;
}

/* ---- Shared fake clock (the mesh's nowMs seam) --------------------------- */

struct FakeClock {
    uint64_t nowMs = 1'000'000u; /* nonzero: 0 means "no offer sent" to the
                                  * retry ladder, exactly like the party
                                  * policy tests. */
    std::function<uint64_t()> fn() {
        return [this]() { return nowMs; };
    }
};

/* ---- In-process signaling hub (the loopback relay) ----------------------- */

struct SentRecord {
    uint64_t from = 0u;
    MdkrMatchSignalOutbound message;
};

class FakeHub;

class FakeFeed final : public MdkrMatchPeerSignalFeed {
public:
    FakeFeed(FakeHub *hub, uint64_t endpointId) : hub_(hub), self_(endpointId) {}
    MdkrMatchSignalSendResult send(const MdkrMatchSignalOutbound &m) override;
    void drainEvents(std::vector<MdkrMatchSignalEvent> &out) override;

    std::deque<MdkrMatchSignalEvent> inbox;

private:
    FakeHub *hub_;
    uint64_t self_;
};

/* Thread-safe: RawPeer's libdatachannel callbacks route answers/candidates
 * from peer threads while the test thread pumps the mesh feeds. */
class FakeHub {
public:
    struct Endpoint {
        uint32_t generation = 0u;
        std::unique_ptr<FakeFeed> feed;
        bool blackholed = false; /* outbound to this endpoint is dropped */
    };

    FakeFeed *addEndpoint(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_[id];
        endpoint.generation = generation;
        endpoint.feed = std::make_unique<FakeFeed>(this, id);
        return endpoint.feed.get();
    }

    void blackhole(uint64_t id, bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_[id].blackholed = value;
    }

    /* A signaling reconnect: the relay tracks a strictly higher generation
     * for this endpoint from now on. */
    void setGeneration(uint64_t id, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_[id].generation = generation;
    }

    /* M1 seam: fail the Nth peer_hello send (1-based, counted per sender)
     * with the relay's peer_unavailable refusal instead of delivering. */
    void failHelloSend(uint64_t from, unsigned index) {
        std::lock_guard<std::mutex> lock(mutex_);
        helloFailPlan_[from].insert(index);
    }

    /* Deliver a welcome to `target` naming every OTHER endpoint present. */
    void welcome(uint64_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        Endpoint &endpoint = endpoints_.at(target);
        MdkrMatchSignalEvent event;
        event.type = MdkrMatchSignalEventType::Welcome;
        event.endpointId = std::to_string(target);
        event.connectionGeneration = endpoint.generation;
        for (const auto &entry : endpoints_) {
            if (entry.first == target) continue;
            MdkrMatchSignalPeerRef ref;
            ref.endpointId = std::to_string(entry.first);
            ref.connectionGeneration = entry.second.generation;
            event.peers.push_back(std::move(ref));
        }
        endpoint.feed->inbox.push_back(std::move(event));
    }

    /* Inject one raw event into `target`'s inbox (stale-answer cases). */
    void inject(uint64_t target, MdkrMatchSignalEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        endpoints_.at(target).feed->inbox.push_back(std::move(event));
    }

    MdkrMatchSignalSendResult route(uint64_t from,
                                    const MdkrMatchSignalOutbound &m) {
        std::lock_guard<std::mutex> lock(mutex_);
        sentLog.push_back(SentRecord{from, m});
        MdkrMatchSignalSendResult result;
        result.sequence = ++sequence_;
        result.ok = true;
        if (m.type == "peer_hello") {
            const unsigned index = ++helloSendCount_[from];
            const auto plan = helloFailPlan_.find(from);
            if (plan != helloFailPlan_.end() &&
                plan->second.count(index) != 0u) {
                result.ok = false;
                result.error = kMdkrMatchSignalPeerUnavailable;
                return result;
            }
        }
        uint64_t to = 0u;
        try { to = std::stoull(m.toEndpointId); } catch (...) { to = 0u; }
        const auto found = endpoints_.find(to);
        if (found == endpoints_.end() ||
            found->second.generation != m.toConnectionGeneration) {
            /* The real relay answers with a bounded signal_error; delivery
             * simply does not happen. */
            return result;
        }
        if (found->second.blackholed) return result;
        MdkrMatchSignalEvent event;
        event.fromEndpointId = std::to_string(from);
        event.fromConnectionGeneration = endpoints_.at(from).generation;
        if (m.type == "peer_hello") {
            event.type = MdkrMatchSignalEventType::PeerHello;
            event.publicKey = m.publicKey;
        } else if (m.type == "webrtc_offer") {
            event.type = MdkrMatchSignalEventType::WebrtcOffer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_answer") {
            event.type = MdkrMatchSignalEventType::WebrtcAnswer;
            event.sdp = m.sdp;
        } else if (m.type == "webrtc_ice") {
            event.type = MdkrMatchSignalEventType::WebrtcIce;
            event.candidate = m.candidate;
            event.hasSdpMid = m.hasSdpMid;
            event.sdpMid = m.sdpMid;
            event.hasSdpMLineIndex = m.hasSdpMLineIndex;
            event.sdpMLineIndex = m.sdpMLineIndex;
            event.hasUsernameFragment = m.hasUsernameFragment;
            event.usernameFragment = m.usernameFragment;
        } else if (m.type == "peer_end") {
            event.type = MdkrMatchSignalEventType::PeerEnd;
            event.reason = m.reason;
        } else {
            result.ok = false;
            result.error = kMdkrMatchSignalInvalidClientMessage;
            return result;
        }
        found->second.feed->inbox.push_back(std::move(event));
        return result;
    }

    /* Every peer_hello blob `from` put on the wire, in order. hello #1 of a
     * (re)exchange is the round-1 commitment (encodeHelloBody(ownCommitment));
     * a fresh local nonce changes that blob, so a rekey that refreshes our
     * entropy is observable here (W3 CRITICAL-2). */
    std::vector<std::string> helloBlobs(uint64_t from) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const SentRecord &record : sentLog) {
            if (record.from == from && record.message.type == "peer_hello") {
                out.push_back(record.message.publicKey);
            }
        }
        return out;
    }

    unsigned countSent(uint64_t from, const std::string &type,
                       const std::string &reason = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        unsigned count = 0u;
        for (const SentRecord &record : sentLog) {
            if (record.from == from && record.message.type == type &&
                (reason.empty() || record.message.reason == reason)) {
                count++;
            }
        }
        return count;
    }

    void drainInbox(FakeFeed *feed, std::vector<MdkrMatchSignalEvent> &out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.clear();
        while (!feed->inbox.empty()) {
            out.push_back(std::move(feed->inbox.front()));
            feed->inbox.pop_front();
        }
    }

    std::vector<SentRecord> sentLog;

private:
    std::map<uint64_t, Endpoint> endpoints_;
    std::map<uint64_t, std::set<unsigned>> helloFailPlan_;
    std::map<uint64_t, unsigned> helloSendCount_;
    std::mutex mutex_;
    uint32_t sequence_ = 0u;
};

MdkrMatchSignalSendResult FakeFeed::send(const MdkrMatchSignalOutbound &m) {
    return hub_->route(self_, m);
}

void FakeFeed::drainEvents(std::vector<MdkrMatchSignalEvent> &out) {
    hub_->drainInbox(this, out);
}

/* ---- Mesh harness -------------------------------------------------------- */

struct MeshHarness {
    FakeHub hub;
    FakeClock clock;
    std::vector<std::unique_ptr<MdkrMatchPeerMesh>> meshes;
    std::map<uint64_t, std::vector<MdkrMatchPeerMeshEvent>> events;
    std::vector<uint64_t> ids;

    ~MeshHarness() { meshes.clear(); /* before the hub's feeds */ }

    MdkrMatchPeerMesh *mesh(uint64_t id) {
        for (size_t index = 0u; index < ids.size(); index++) {
            if (ids[index] == id) return meshes[index].get();
        }
        return nullptr;
    }

    MdkrMatchPeerMesh *add(uint64_t id, uint32_t generation,
                           const std::vector<MdkrMatchPeerSlotOwner> &roster) {
        FakeFeed *feed = hub.addEndpoint(id, generation);
        MdkrMatchPeerMeshOptions options;
        options.signal = feed;
        options.roomId = roomIdFixture();
        options.localEndpointId = id;
        options.localGeneration = generation;
        options.matchEpoch = kEpoch;
        options.compatibility = compatibilityFixture();
        options.roster = roster;
        options.nowMs = clock.fn();
        std::string error;
        std::unique_ptr<MdkrMatchPeerMesh> mesh =
            MdkrMatchPeerMesh::create(options, &error);
        if (!mesh) {
            std::fprintf(stderr, "mesh create refused: %s\n", error.c_str());
            assert(mesh);
        }
        ids.push_back(id);
        meshes.push_back(std::move(mesh));
        return meshes.back().get();
    }

    void pumpOnce() {
        std::vector<MdkrMatchPeerMeshEvent> drained;
        for (size_t index = 0u; index < meshes.size(); index++) {
            meshes[index]->pump();
            meshes[index]->drainEvents(drained);
            for (MdkrMatchPeerMeshEvent &event : drained) {
                events[ids[index]].push_back(std::move(event));
            }
        }
    }

    bool pumpUntil(const std::function<bool()> &done,
                   unsigned maxMillis = kPhaseMs, uint64_t fakeStepMs = 0u) {
        for (unsigned elapsed = 0u; elapsed <= maxMillis; elapsed += 10u) {
            pumpOnce();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            clock.nowMs += fakeStepMs;
        }
        pumpOnce();
        return done();
    }

    unsigned countEvents(uint64_t owner, MdkrMatchPeerMeshEventType type,
                         uint64_t aboutEndpoint = 0u) const {
        unsigned count = 0u;
        const auto found = events.find(owner);
        if (found == events.end()) return 0u;
        for (const MdkrMatchPeerMeshEvent &event : found->second) {
            if (event.type != type) continue;
            if (aboutEndpoint != 0u && event.endpointId != aboutEndpoint)
                continue;
            count++;
        }
        return count;
    }

    const MdkrMatchPeerMeshEvent *lastEvent(
        uint64_t owner, MdkrMatchPeerMeshEventType type,
        uint64_t aboutEndpoint = 0u) const {
        const auto found = events.find(owner);
        if (found == events.end()) return nullptr;
        for (auto it = found->second.rbegin(); it != found->second.rend();
             ++it) {
            if (it->type != type) continue;
            if (aboutEndpoint != 0u && it->endpointId != aboutEndpoint)
                continue;
            return &*it;
        }
        return nullptr;
    }
};

std::vector<MdkrMatchPeerSlotOwner> rosterOf(
    std::initializer_list<uint64_t> ids) {
    std::vector<MdkrMatchPeerSlotOwner> roster;
    uint8_t slot = 1u;
    for (uint64_t id : ids) {
        MdkrMatchPeerSlotOwner owner;
        owner.endpointId = id;
        owner.slotMask = slot;
        slot = static_cast<uint8_t>(slot << 1u);
        roster.push_back(owner);
    }
    return roster;
}

std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payloadFixture(uint8_t seed) {
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payload{};
    for (unsigned index = 0u; index < payload.size(); index++) {
        payload[index] = static_cast<uint8_t>(seed + index * 3u);
    }
    return payload;
}

/* ---- A raw protocol peer: real libdatachannel + the crypto C API --------- */

/* Answers the mesh's offer with a real PeerConnection, performs the
 * three-hello commitment dance by hand, and can seal/open envelopes with
 * its own identity -- full control over every byte the mesh receives, which
 * the misbehavior cases need. Channel callbacks only enqueue. */
struct RawPeer {
    uint64_t selfId;
    uint32_t selfGeneration;
    uint64_t meshId;
    uint32_t meshGeneration;
    FakeHub *hub;
    bool sendHellos = true;
    bool answerPings = true;
    /* Offerer mode: this peer builds the connection and chooses which channels
     * it carries, so a lane can present the two-channel set an endpoint that
     * predates the authority channel would offer. Unset, the peer answers. */
    bool offerer = false;
    bool offerAuthorityChannel = true;
    bool offerSent = false;

    MdkrMatchPeerIdentity *identity = nullptr;
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> publicKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> nonce{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> commitment{};

    unsigned meshHellos = 0u;
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> meshCommitment{};
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> meshKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> meshNonce{};
    bool sentReveal = false;
    bool sentCommit = false;

    std::shared_ptr<rtc::PeerConnection> pc;
    /* Indexed by MDKR_MATCH_PEER_LANE_*, like the mesh's own runtime. */
    std::shared_ptr<rtc::DataChannel> channels[MDKR_MATCH_PEER_LANE_COUNT];
    std::mutex mutex;
    std::deque<std::string> controlText;

    MdkrMatchPeerKeyring ring{};
    MdkrMatchPeerSealingKey *sealKeys[MDKR_MATCH_PEER_LANE_COUNT] = {};
    bool keysDerived = false;

    RawPeer(uint64_t self, uint32_t generation, uint64_t mesh,
            uint32_t meshGen, FakeHub *owner)
        : selfId(self), selfGeneration(generation), meshId(mesh),
          meshGeneration(meshGen), hub(owner) {
        identity = mdkr_match_peer_identity_create();
        assert(identity != nullptr);
        assert(mdkr_match_peer_identity_public_key(identity, publicKey.data()));
        for (unsigned index = 0u; index < nonce.size(); index++) {
            nonce[index] = static_cast<uint8_t>(0x21u + index);
        }
        assert(mdkr_match_peer_commitment(kEpoch, selfId, selfGeneration,
                                          nonce.data(), publicKey.data(),
                                          commitment.data()));
    }

    ~RawPeer() {
        // close() starts transport retirement but does not join callbacks.
        // Stop channel adoption first, then drain callbacks capturing this
        // before controlText/mutex are destroyed. The pinned library's
        // resetCallbacks synchronizes with any callback already executing.
        // Do not hold mutex here: the callbacks being drained acquire it.
        if (pc) pc->resetCallbacks();
        for (auto &channel : channels) {
            if (channel) channel->resetCallbacks();
        }
        if (pc) pc->close();
        mdkr_match_peer_keyring_forget(&ring);
        mdkr_match_peer_identity_destroy(identity);
    }

    void sendSignal(const std::string &type,
                    const std::function<void(MdkrMatchSignalOutbound &)> &fill) {
        MdkrMatchSignalOutbound message;
        message.type = type;
        message.toEndpointId = std::to_string(meshId);
        message.toConnectionGeneration = meshGeneration;
        fill(message);
        hub->route(selfId, message);
    }

    std::string helloBlob(const uint8_t body[32]) const {
        uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        raw[0] = 0x04u;
        std::memcpy(raw + 1u, body, 32u);
        return mdkr_party::base64Url(raw, sizeof(raw));
    }

    void sendCommitHello() {
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = helloBlob(commitment.data());
        });
        sentCommit = true;
    }

    void sendRevealHellos() {
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(publicKey.data(),
                                                publicKey.size());
        });
        sendSignal("peer_hello", [this](MdkrMatchSignalOutbound &m) {
            m.publicKey = helloBlob(nonce.data());
        });
        sentReveal = true;
    }

    /* Drive from the test thread with events drained from the hub inbox. */
    void onSignalEvent(const MdkrMatchSignalEvent &event) {
        if (event.type == MdkrMatchSignalEventType::PeerHello) {
            std::array<uint8_t, 65> raw{};
            assert(mdkr_party::decodePublicKey(event.publicKey, raw));
            if (meshHellos == 0u) {
                std::memcpy(meshCommitment.data(), raw.data() + 1u, 32u);
            } else if (meshHellos == 1u) {
                std::memcpy(meshKey.data(), raw.data(), raw.size());
            } else if (meshHellos == 2u) {
                std::memcpy(meshNonce.data(), raw.data() + 1u, 32u);
                assert(mdkr_match_peer_commitment_verify(
                    kEpoch, meshId, meshGeneration, meshNonce.data(),
                    meshKey.data(), meshCommitment.data()));
            }
            meshHellos++;
            if (sendHellos && !sentReveal && meshHellos >= 1u) {
                if (!sentCommit) sendCommitHello();
                sendRevealHellos();
            }
        } else if (event.type == MdkrMatchSignalEventType::WebrtcOffer) {
            startPeer();
            try {
                pc->setRemoteDescription(rtc::Description(event.sdp, "offer"));
            } catch (...) { assert(false && "offer rejected"); }
        } else if (event.type == MdkrMatchSignalEventType::WebrtcAnswer) {
            if (!pc) return;
            try {
                pc->setRemoteDescription(rtc::Description(event.sdp, "answer"));
            } catch (...) { assert(false && "answer rejected"); }
        } else if (event.type == MdkrMatchSignalEventType::WebrtcIce) {
            if (!pc) return;
            try {
                pc->addRemoteCandidate(
                    rtc::Candidate(event.candidate, event.sdpMid));
            } catch (...) {}
        }
    }

    /* Build the connection and offer a chosen channel set. The mesh peers the
     * numerically HIGHER id, so a harness that wants the mesh to answer gives
     * this peer the lower one. */
    void startOffer() {
        if (pc || offerSent) return;
        offerer = true;
        startPeer();
        rtc::DataChannelInit stateConfiguration;
        stateConfiguration.reliability.unordered = true;
        stateConfiguration.reliability.maxRetransmits = 0u;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channels[MDKR_MATCH_PEER_LANE_STATE] = pc->createDataChannel(
                kMdkrMatchStateChannelLabel, stateConfiguration);
            channels[MDKR_MATCH_PEER_LANE_CONTROL] =
                pc->createDataChannel(kMdkrMatchControlChannelLabel);
            if (offerAuthorityChannel) {
                rtc::DataChannelInit authorityConfiguration;
                authorityConfiguration.reliability.unordered = true;
                channels[MDKR_MATCH_PEER_LANE_AUTHORITY] =
                    pc->createDataChannel(kMdkrMatchAuthorityChannelLabel,
                                          authorityConfiguration);
            }
        }
        offerSent = true;
    }

    void startPeer() {
        if (pc) return;
        rtc::Configuration configuration; /* loopback: no ICE servers */
        pc = std::make_shared<rtc::PeerConnection>(configuration);
        pc->onLocalDescription([this](rtc::Description description) {
            sendSignal(offerer ? "webrtc_offer" : "webrtc_answer",
                       [&](MdkrMatchSignalOutbound &m) {
                m.sdp = std::string(description);
            });
        });
        pc->onLocalCandidate([this](rtc::Candidate candidate) {
            sendSignal("webrtc_ice", [&](MdkrMatchSignalOutbound &m) {
                m.candidate = std::string(candidate);
                m.hasSdpMid = true;
                m.sdpMid = candidate.mid();
            });
        });
        pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            const std::string label = channel->label();
            std::lock_guard<std::mutex> lock(mutex);
            if (label == kMdkrMatchControlChannelLabel) {
                channels[MDKR_MATCH_PEER_LANE_CONTROL] = channel;
                channel->onMessage([this](rtc::message_variant message) {
                    if (!std::holds_alternative<std::string>(message)) return;
                    std::lock_guard<std::mutex> inner(mutex);
                    controlText.push_back(std::get<std::string>(message));
                });
            } else if (label == kMdkrMatchStateChannelLabel) {
                channels[MDKR_MATCH_PEER_LANE_STATE] = channel;
            } else if (label == kMdkrMatchAuthorityChannelLabel) {
                channels[MDKR_MATCH_PEER_LANE_AUTHORITY] = channel;
            }
        });
    }

    unsigned openChannelCount() {
        std::lock_guard<std::mutex> lock(mutex);
        unsigned open = 0u;
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            if (channels[lane] && channels[lane]->isOpen()) open++;
        }
        return open;
    }

    bool channelsOpen() {
        return openChannelCount() == MDKR_MATCH_PEER_LANE_COUNT;
    }

    /* Answer mesh pings so the stale ladder stays quiet where wanted. */
    void serviceControl() {
        std::deque<std::string> pending;
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(controlText);
            channel = channels[MDKR_MATCH_PEER_LANE_CONTROL];
        }
        for (const std::string &text : pending) {
            Json value = Json::parse(text, nullptr, false);
            if (value.is_discarded() || !value.is_object()) continue;
            if (answerPings && value.value("type", std::string{}) == "ping" &&
                channel && channel->isOpen()) {
                try {
                    channel->send(Json{{"type", "pong"}, {"protocol", 1u},
                        {"nonce", value.value("nonce", 0u)}}.dump());
                } catch (...) {}
            }
        }
    }

    void deriveKeys() {
        assert(meshHellos >= 3u);
        MdkrMatchPeerTranscript transcript{};
        uint8_t roomRaw[16];
        for (unsigned index = 0u; index < 16u; index++) {
            roomRaw[index] = static_cast<uint8_t>(index + 1u);
        }
        std::memcpy(transcript.room_id, roomRaw, sizeof(roomRaw));
        transcript.match_epoch = kEpoch;
        transcript.compatibility = compatibilityFixture();
        transcript.entry_count = 2u;
        transcript.entries[0].endpoint_id = meshId;
        transcript.entries[0].generation = meshGeneration;
        std::memcpy(transcript.entries[0].public_key, meshKey.data(),
                    meshKey.size());
        std::memcpy(transcript.entries[0].commitment, meshCommitment.data(),
                    meshCommitment.size());
        std::memcpy(transcript.entries[0].commit_nonce, meshNonce.data(),
                    meshNonce.size());
        transcript.entries[1].endpoint_id = selfId;
        transcript.entries[1].generation = selfGeneration;
        std::memcpy(transcript.entries[1].public_key, publicKey.data(),
                    publicKey.size());
        std::memcpy(transcript.entries[1].commitment, commitment.data(),
                    commitment.size());
        std::memcpy(transcript.entries[1].commit_nonce, nonce.data(),
                    nonce.size());
        uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
        assert(mdkr_match_peer_transcript_digest(&transcript, digest));
        MdkrMatchPeerKeyContext outbound{};
        outbound.match_epoch = kEpoch;
        outbound.source_endpoint_id = selfId;
        outbound.source_generation = selfGeneration;
        outbound.destination_endpoint_id = meshId;
        outbound.destination_generation = meshGeneration;
        for (unsigned lane = 0u; lane < MDKR_MATCH_PEER_LANE_COUNT; ++lane) {
            outbound.lane = static_cast<uint8_t>(lane);
            sealKeys[lane] = mdkr_match_peer_identity_derive_key(
                &ring, identity, meshKey.data(), digest, &outbound);
            assert(sealKeys[lane] != nullptr);
        }
        keysDerived = true;
    }

    bool sendLaneBytes(uint8_t lane, const std::vector<uint8_t> &bytes) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = channels[lane];
        }
        if (!channel || !channel->isOpen()) return false;
        std::vector<std::byte> raw(bytes.size());
        std::memcpy(raw.data(), bytes.data(), bytes.size());
        try { return channel->send(raw.data(), raw.size()); }
        catch (...) { return false; }
    }

    bool sendStateBytes(const std::vector<uint8_t> &bytes) {
        return sendLaneBytes(MDKR_MATCH_PEER_LANE_STATE, bytes);
    }

    bool sendControlBytes(const std::vector<uint8_t> &bytes) {
        return sendLaneBytes(MDKR_MATCH_PEER_LANE_CONTROL, bytes);
    }

    bool sendAuthorityBytes(const std::vector<uint8_t> &bytes) {
        return sendLaneBytes(MDKR_MATCH_PEER_LANE_AUTHORITY, bytes);
    }

    bool sendControlText(const std::string &text) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = channels[MDKR_MATCH_PEER_LANE_CONTROL];
        }
        if (!channel || !channel->isOpen()) return false;
        try { return channel->send(text); }
        catch (...) { return false; }
    }

    std::vector<uint8_t> sealed(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload,
        uint8_t lane, uint8_t payloadType) {
        assert(keysDerived);
        MdkrMatchPeerSendContext context{};
        context.key = sealKeys[lane]->direction;
        context.intermediate_endpoint_id = 0u;
        context.payload_type = payloadType;
        std::vector<uint8_t> envelope(MDKR_MATCH_PEER_ENVELOPE_BYTES);
        assert(mdkr_match_peer_seal(sealKeys[lane], &context, payload.data(),
                                    envelope.data()));
        return envelope;
    }

    std::vector<uint8_t> sealedInput(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload) {
        return sealed(payload, MDKR_MATCH_PEER_LANE_STATE,
                      MDKR_MATCH_PEER_PAYLOAD_INPUT);
    }

    std::vector<uint8_t> sealedFragment(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload) {
        return sealed(payload, MDKR_MATCH_PEER_LANE_CONTROL,
                      MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
    }

    std::vector<uint8_t> sealedRepairAnswer(
        const std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> &payload) {
        return sealed(payload, MDKR_MATCH_PEER_LANE_AUTHORITY,
                      MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_ANSWER);
    }
};

/* Connect a 2-mesh pair to PeerChannelsReady + PhraseReady on both sides. */
struct PairHarness {
    MeshHarness harness;
    MdkrMatchPeerMesh *low = nullptr;
    MdkrMatchPeerMesh *high = nullptr;

    bool connect() {
        const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
        low = harness.add(100u, 1u, roster);
        high = harness.add(200u, 2u, roster);
        harness.hub.welcome(100u);
        harness.hub.welcome(200u);
        return harness.pumpUntil([this]() {
            return harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                       200u) >= 1u &&
                   harness.countEvents(
                       200u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                       100u) >= 1u &&
                   harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
                   harness.countEvents(
                       200u, MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
        });
    }
};

/* ======================================================================== */

void twoPeerHappyPath() {
    PairHarness pair;
    assert(pair.connect());

    /* Phrase equality across both sides -- the whole point of the committed
     * transcript. */
    std::string phraseLow;
    std::string phraseHigh;
    assert(pair.low->phrase(phraseLow));
    assert(pair.high->phrase(phraseHigh));
    assert(!phraseLow.empty() && phraseLow == phraseHigh);

    /* Input envelope round trip, both directions, bundle bytes identical. */
    const auto payloadLow = payloadFixture(0x11u);
    const auto payloadHigh = payloadFixture(0x77u);
    assert(pair.low->sendInput(payloadLow.data()) == 1u);
    assert(pair.high->sendInput(payloadHigh.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u &&
               pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *toHigh = pair.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    const MdkrMatchPeerMeshEvent *toLow = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u);
    assert(toHigh != nullptr && toHigh->payload == payloadLow);
    assert(toLow != nullptr && toLow->payload == payloadHigh);
    assert(toHigh->context.payload_type == MDKR_MATCH_PEER_PAYLOAD_INPUT);

    /* Preflight fragment on the reliable control channel. */
    const auto fragment = payloadFixture(0x40u);
    assert(pair.low->sendPreflightFragment(200u, fragment.data()));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PreflightFragment,
                   100u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *preflight = pair.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::PreflightFragment, 100u);
    assert(preflight != nullptr && preflight->payload == fragment);
    assert(preflight->context.payload_type ==
           MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
    std::printf("twoPeerHappyPath: ok (phrase='%s')\n", phraseLow.c_str());
}

void threePeerMeshEveryoneReachesEveryone() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster =
        rosterOf({100u, 200u, 300u});
    harness.add(100u, 1u, roster);
    harness.add(200u, 2u, roster);
    harness.add(300u, 3u, roster);
    harness.hub.welcome(100u);
    harness.hub.welcome(200u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
    }, 30000u));

    std::string phraseA;
    std::string phraseB;
    std::string phraseC;
    assert(harness.mesh(100u)->phrase(phraseA));
    assert(harness.mesh(200u)->phrase(phraseB));
    assert(harness.mesh(300u)->phrase(phraseC));
    assert(!phraseA.empty() && phraseA == phraseB && phraseB == phraseC);

    /* One input from the lowest endpoint reaches both others. */
    const auto payload = payloadFixture(0x05u);
    assert(harness.mesh(100u)->sendInput(payload.data()) == 2u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    const MdkrMatchPeerMeshEvent *atB = harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    const MdkrMatchPeerMeshEvent *atC = harness.lastEvent(
        300u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u);
    assert(atB != nullptr && atB->payload == payload);
    assert(atC != nullptr && atC->payload == payload);
    std::printf("threePeerMeshEveryoneReachesEveryone: ok (phrase='%s')\n",
                phraseA.c_str());
}

void staleGenerationAnswerIgnored() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 7u); /* feed unused: 200 never answers */
    harness.hub.welcome(100u);
    /* Let the offer go out. */
    assert(harness.pumpUntil([&]() {
        return harness.hub.countSent(100u, "webrtc_offer") >= 1u;
    }));

    /* An answer from a RETIRED generation of 200 must be ignored: no state
     * change, no PeerLost, only the stale counter moves. */
    MdkrMatchSignalEvent stale;
    stale.type = MdkrMatchSignalEventType::WebrtcAnswer;
    stale.fromEndpointId = "200";
    stale.fromConnectionGeneration = 6u; /* tracked current is 7 */
    stale.sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\n";
    harness.hub.inject(100u, stale);
    /* And an offer from an endpoint outside the roster is ignored too. */
    MdkrMatchSignalEvent unknown;
    unknown.type = MdkrMatchSignalEventType::WebrtcOffer;
    unknown.fromEndpointId = "999";
    unknown.fromConnectionGeneration = 1u;
    unknown.sdp = "v=0\r\n";
    harness.hub.inject(100u, unknown);
    harness.pumpOnce();
    const MdkrMatchPeerMeshStats stats = mesh->stats();
    assert(stats.ignoredStaleSignals >= 2u);
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::PeerLost) ==
           0u);
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::Failure) ==
           0u);
    std::printf("staleGenerationAnswerIgnored: ok (ignored=%llu)\n",
                static_cast<unsigned long long>(stats.ignoredStaleSignals));
}

void phraseRefusedBeforeCommitmentCompletes() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 2u);
    RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
    raw.sendHellos = false; /* hand-drive each round */
    harness.hub.welcome(100u);

    std::string phrase;
    harness.pumpOnce();
    assert(!mesh->phrase(phrase)); /* nothing yet */

    /* Round 1 only: the peer's commitment. Still no phrase. */
    raw.sendCommitHello();
    harness.pumpOnce();
    assert(!mesh->phrase(phrase));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PhraseReady) == 0u);

    /* Key revealed but the commitment not yet OPENED: still refused. */
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                            raw.publicKey.size());
    });
    harness.pumpOnce();
    assert(!mesh->phrase(phrase));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PhraseReady) == 0u);

    /* The opening nonce completes the two-round handshake: phrase appears. */
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(raw.nonce.data());
    });
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
    }, 2000u));
    assert(mesh->phrase(phrase));
    assert(!phrase.empty());
    std::printf("phraseRefusedBeforeCommitmentCompletes: ok\n");
}

void sealWindowExhaustionIsTypedPeerLoss() {
    PairHarness pair;
    assert(pair.connect());
    /* The seam leaves the outbound window exactly as one UINT64_MAX seal
     * would: exhausted, requiring reconnect + fresh generation-bound key. */
    assert(mdkr_match_peer_mesh_exhaust_seal_for_test(*pair.low, 200u));
    const auto payload = payloadFixture(0x0Au);
    assert(pair.low->sendInput(payload.data()) == 0u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 2000u));
    const MdkrMatchPeerMeshEvent *lost = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::SealWindowExhausted);
    /* Terminal for that peer: later sends refuse rather than re-reporting. */
    assert(pair.low->sendInput(payload.data()) == 0u);
    std::printf("sealWindowExhaustionIsTypedPeerLoss: ok\n");
}

/* One mesh + one RawPeer wired through the hub. */
struct RawHarness {
    MeshHarness harness;
    MdkrMatchPeerMesh *mesh = nullptr;
    std::unique_ptr<RawPeer> raw;
    FakeFeed *rawFeed = nullptr;

    void drainRawInbox() {
        /* RawPeer consumes its own hub inbox on the test thread. */
        if (rawFeed == nullptr) return;
        std::vector<MdkrMatchSignalEvent> drained;
        rawFeed->drainEvents(drained);
        for (const MdkrMatchSignalEvent &event : drained) {
            raw->onSignalEvent(event);
        }
    }
};

/* D1: the room-departure grace asks one question of the transport -- has
 * ANYTHING authenticated arrived from this endpoint since the verdict? -- and
 * a wrong answer either drops a peer that is still racing or keeps a survivor
 * racing a ghost. So the counter moves for exactly the traffic that proves a
 * peer is still there: an envelope that OPENED under its own derived lane key.
 * Garbage does not move it (anyone can send garbage), and neither does the
 * control channel's plaintext JSON, which is authenticated by DTLS alone and
 * says only that a socket is alive. */
void authenticatedPacketCountTracksOpenedEnvelopesOnly() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    uint64_t count = 1u;
    assert(rig.mesh->authenticatedPacketCount(200u, &count) && count == 0u);
    /* An endpoint outside the roster has no count to read. */
    uint64_t stranger = 7u;
    assert(!rig.mesh->authenticatedPacketCount(999u, &stranger) &&
           stranger == 7u);

    /* Garbage on the lossy lane: rejected, and no evidence of anything. */
    const std::vector<uint8_t> junk(MDKR_MATCH_PEER_ENVELOPE_BYTES, 0xEEu);
    assert(rig.raw->sendStateBytes(junk));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedStateEnvelopes >= 1u;
    }, 5000u));
    assert(rig.mesh->authenticatedPacketCount(200u, &count) && count == 0u);

    /* A sealed input bundle -- the traffic a peer that is still racing
     * produces every tick -- counts. */
    assert(rig.raw->sendStateBytes(
        rig.raw->sealedInput(payloadFixture(0x33u))));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 1u;
    }, 5000u));
    assert(rig.mesh->authenticatedPacketCount(200u, &count) && count == 1u);

    /* So does a sealed reliable-lane envelope -- and the plaintext control
     * message sent AHEAD of it on that same ordered lane does not. The order
     * is what makes this exact: the ping is delivered and answered first, so a
     * count of 2 rather than 3 is proof it was not evidence of anything. */
    assert(rig.raw->sendControlText(
        Json{{"type", "ping"}, {"protocol", 2u}, {"nonce", 7u}}.dump()));
    assert(rig.raw->sendControlBytes(
        rig.raw->sealedFragment(payloadFixture(0x44u))));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PreflightFragment, 200u) >= 1u;
    }, 5000u));
    assert(rig.mesh->authenticatedPacketCount(200u, &count) && count == 2u);
    /* The ping was WELL FORMED -- a malformed control message is terminal for
     * the peer -- so its exclusion is a rule about what counts as evidence,
     * not a message the mesh refused. */
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);
}

void badStateEnvelopeDroppedAndCounted() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    /* Garbage on the LOSSY state channel: dropped + counted, never fatal. */
    std::vector<uint8_t> junk(MDKR_MATCH_PEER_ENVELOPE_BYTES, 0xEEu);
    assert(rig.raw->sendStateBytes(junk));
    std::vector<uint8_t> runt(20u, 0x55u);
    assert(rig.raw->sendStateBytes(runt));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedStateEnvelopes >= 2u;
    }, 5000u));
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);

    /* The mesh is still healthy: a genuine sealed envelope right after the
     * garbage opens and surfaces normally. */
    const auto payload = payloadFixture(0x33u);
    assert(rig.raw->sendStateBytes(rig.raw->sealedInput(payload)));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *input = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::InputEnvelope, 200u);
    assert(input != nullptr && input->payload == payload);

    /* A correctly sized control envelope that fails to open is the
     * rekey-window race shape: counted, dropped, NOT terminal (I2). */
    assert(rig.raw->sendControlBytes(junk));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedControlEnvelopes >= 1u;
    }, 5000u));
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);

    /* STRUCTURAL garbage on the reliable channel -- a frame no conforming
     * peer can ever produce -- is terminal. */
    assert(rig.raw->sendControlBytes(runt));
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr && lost->lostReason ==
           MdkrMatchPeerLostReason::ControlChannelViolation);
    std::printf("badStateEnvelopeDroppedAndCounted: ok (state=%llu "
                "control=%llu)\n",
                static_cast<unsigned long long>(
                    rig.mesh->stats().rejectedStateEnvelopes),
                static_cast<unsigned long long>(
                    rig.mesh->stats().rejectedControlEnvelopes));
}

void controlPingTimeoutIsTypedPeerLoss() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.raw->sendHellos = false;   /* keys are irrelevant to the ping ladder */
    rig.raw->answerPings = false;  /* the peer goes silent */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen();
    }));

    /* Advance the fake clock past one ping interval, then past the stale
     * deadline with the pong never arriving. */
    rig.harness.clock.nowMs += kMdkrMatchControlPingIntervalMs + 1u;
    rig.harness.pumpOnce();
    rig.harness.clock.nowMs += kMdkrMatchControlPingTimeoutMs + 1u;
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::PingTimeout);
    std::printf("controlPingTimeoutIsTypedPeerLoss: ok\n");
}

/* A7: linkStats()'s consecutivePingMisses counts full ping intervals
 * elapsed with an outstanding, unanswered ping -- 1 after exactly one
 * interval, still short of the mesh's own hard PingTimeout verdict -- and
 * the peer is still very much alive from the mesh's own point of view at
 * that point (channelsReady, not lost). Once the full timeout elapses the
 * mesh's normal PingTimeout path fires, unaffected by this diagnostics
 * field having been read along the way. */
void controlPingMissCountsBeforeTimeout() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.raw->sendHellos = false;
    rig.raw->answerPings = false;
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen();
    }));

    MdkrMatchPeerLinkStats links[MDKR_ONLINE_MAX_ENDPOINTS];
    unsigned count = rig.mesh->linkStats(links, MDKR_ONLINE_MAX_ENDPOINTS);
    assert(count == 1u);
    assert(links[0].consecutivePingMisses == 0u);

    /* First ping goes out (nextPingAtMs was armed on channels-ready): still
     * within its first interval, 0 misses. */
    rig.harness.clock.nowMs += kMdkrMatchControlPingIntervalMs + 1u;
    rig.harness.pumpOnce();
    count = rig.mesh->linkStats(links, MDKR_ONLINE_MAX_ENDPOINTS);
    assert(count == 1u);
    assert(links[0].consecutivePingMisses == 0u);

    /* One full interval past THAT ping with no pong: exactly one miss,
     * peer still up (well short of the timeout=3-interval budget). */
    rig.harness.clock.nowMs += kMdkrMatchControlPingIntervalMs + 1u;
    rig.harness.pumpOnce();
    count = rig.mesh->linkStats(links, MDKR_ONLINE_MAX_ENDPOINTS);
    assert(count == 1u);
    assert(links[0].consecutivePingMisses == 1u);
    assert(rig.mesh->peerChannelsReady(200u));
    assert(rig.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);

    /* Past the full stale deadline: the mesh's own hard verdict fires,
     * unaffected by having read the diagnostics field above. */
    rig.harness.clock.nowMs += kMdkrMatchControlPingTimeoutMs + 1u;
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::PingTimeout);
    std::printf("controlPingMissCountsBeforeTimeout: ok\n");
}

void iceRestartRecoversAfterChannelDeath() {
    PairHarness pair;
    assert(pair.connect());
    const unsigned readyLowBefore = pair.harness.countEvents(
        100u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u);
    const unsigned readyHighBefore = pair.harness.countEvents(
        200u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u);

    /* Kill the pair's connection out from under the offerer. */
    assert(mdkr_match_peer_mesh_kill_channels_for_test(*pair.low, 200u));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >
                   readyLowBefore &&
               pair.harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >
                   readyHighBefore;
    }, 30000u));
    /* The restart was announced on the wire, the way the signaling doc's
     * peer_end contract requires. */
    assert(pair.harness.hub.countSent(100u, "peer_end", "restart") >= 1u);

    /* And the same generation-bound keys still carry input after recovery. */
    const auto payload = payloadFixture(0x59u);
    assert(pair.low->sendInput(payload.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    std::printf("iceRestartRecoversAfterChannelDeath: ok\n");
}

void offerLadderGivesUpBounded() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 7u);
    harness.hub.blackhole(200u, true); /* every offer vanishes, like a peer
                                        * whose socket dropped mid-relay */
    harness.hub.welcome(100u);
    /* Fake time marches; the mdkr_party_retry_decide ladder must recreate
     * on the 20 s deadline and give up after 3 attempts -- typed, bounded,
     * never a silent hang. */
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 30000u, 1000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ConnectTimeout);
    assert(harness.hub.countSent(100u, "webrtc_offer") >= 3u);
    /* Given up means given up: no more offers after the verdict. */
    const unsigned offersAtVerdict = harness.hub.countSent(100u, "webrtc_offer");
    harness.clock.nowMs += 60000u;
    for (unsigned index = 0u; index < 5u; index++) harness.pumpOnce();
    assert(harness.hub.countSent(100u, "webrtc_offer") == offersAtVerdict);
    std::printf("offerLadderGivesUpBounded: ok (offers=%u)\n", offersAtVerdict);
}

void closeTearsDownBounded() {
    PairHarness pair;
    assert(pair.connect());
    const auto begin = std::chrono::steady_clock::now();
    pair.low->close();
    pair.low->close(); /* idempotent */
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    assert(elapsed.count() < 5000);
    /* A closed mesh refuses work instead of crashing or hanging. */
    const auto payload = payloadFixture(0x01u);
    assert(pair.low->sendInput(payload.data()) == 0u);
    std::string phrase;
    assert(!pair.low->phrase(phrase));
    pair.low->pump(); /* harmless */
    /* The survivor keeps running (its own pump never hangs on the corpse). */
    for (unsigned index = 0u; index < 20u; index++) {
        pair.harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("closeTearsDownBounded: ok (%lld ms)\n",
                static_cast<long long>(elapsed.count()));
}

/* ---- Fix round 1 ---------------------------------------------------------*/

/* C1: a roster peer must never be able to throw through pump(). Every one
 * of these frames used to reach nlohmann typed extraction with a mismatched
 * type and terminate the launcher via json::type_error. */
void malformedControlJsonIsTypedLossNotCrash() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.raw->sendHellos = false; /* the control plane predates the keys */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen();
    }));
    assert(rig.raw->sendControlText("{\"type\":123}"));
    assert(rig.raw->sendControlText(
        "{\"type\":\"ping\",\"protocol\":true,\"nonce\":1}"));
    assert(rig.raw->sendControlText(
        "{\"type\":\"pong\",\"protocol\":1,\"nonce\":\"x\"}"));
    assert(rig.harness.pumpUntil([&]() {
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr && lost->lostReason ==
           MdkrMatchPeerLostReason::ControlChannelViolation);
    /* The mesh survives its peer's garbage: still pumpable, still typed. */
    (void)rig.mesh->stats();
    rig.harness.pumpOnce();
    std::printf("malformedControlJsonIsTypedLossNotCrash: ok\n");
}

/* I1: state and control are independent streams; one shared 64-deep replay
 * window let >64 state envelopes retire a delayed-but-honest control
 * fragment as REPLAY -- a terminal kill of a healthy peer. */
void delayedControlFragmentSurvivesInputBurst() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    /* The fragment is sealed FIRST (sequence 1), then delayed while 70
     * inputs (sequences 2..71) advance the direction. */
    const auto fragment = payloadFixture(0x66u);
    const std::vector<uint8_t> delayed = rig.raw->sealedFragment(fragment);
    for (unsigned index = 0u; index < 70u; index++) {
        assert(rig.raw->sendStateBytes(rig.raw->sealedInput(
            payloadFixture(static_cast<uint8_t>(index)))));
    }
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 65u;
    }));
    assert(rig.raw->sendControlBytes(delayed));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PreflightFragment, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *preflight = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PreflightFragment, 200u);
    assert(preflight != nullptr && preflight->payload == fragment);
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("delayedControlFragmentSurvivesInputBurst: ok\n");
}

/* N7: the authority channel is a third independent stream. Its key, and
 * therefore its sequence space and replay window, is its own: a burst on the
 * lossy state channel cannot age out a valid authority packet, and an envelope
 * sealed for one lane is refused on another rather than opened. */
void authorityLaneKeepsItsOwnReplayWindow() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();

    /* The repair answer is sealed FIRST (authority sequence 1), then delayed
     * while 70 inputs advance the STATE lane through sequences 1..70. Under
     * one shared key that burst would carry the shared window past the
     * delayed message's sequence and retire it as a replay. */
    const auto answer = payloadFixture(0x77u);
    const std::vector<uint8_t> delayed = rig.raw->sealedRepairAnswer(answer);
    for (unsigned index = 0u; index < 70u; index++) {
        assert(rig.raw->sendStateBytes(rig.raw->sealedInput(
            payloadFixture(static_cast<uint8_t>(index)))));
    }
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 200u) >= 65u;
    }));
    assert(rig.raw->sendAuthorityBytes(delayed));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::InputRepairMessage, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *repair = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::InputRepairMessage, 200u);
    assert(repair != nullptr && repair->payload == answer);
    assert(repair->context.payload_type ==
           MDKR_MATCH_PEER_PAYLOAD_INPUT_REPAIR_ANSWER);
    assert(repair->context.key.lane == MDKR_MATCH_PEER_LANE_AUTHORITY);

    /* A message sealed for the STATE lane and replayed onto the authority
     * channel cannot open under the authority key: a counted drop, not a
     * terminal verdict and never a delivered repair. */
    const uint64_t rejectedBefore =
        rig.mesh->stats().rejectedAuthorityEnvelopes;
    assert(rig.raw->sendAuthorityBytes(
        rig.raw->sealedInput(payloadFixture(0x5au))));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.mesh->stats().rejectedAuthorityEnvelopes > rejectedBefore;
    }, 5000u));
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::InputRepairMessage, 200u) == 1u);
    assert(rig.harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("authorityLaneKeepsItsOwnReplayWindow: ok\n");
}

/* An envelope authenticated under the peer's own authority key but carrying a
 * payload type that channel does not serve is proven misbehavior on a
 * reliable channel, exactly as on the control channel. */
void authorityWrongPayloadTypeIsTypedLoss() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               rig.raw->channelsOpen() && rig.raw->meshHellos >= 3u;
    }));
    rig.raw->deriveKeys();
    assert(rig.raw->sendAuthorityBytes(rig.raw->sealed(
        payloadFixture(0x31u), MDKR_MATCH_PEER_LANE_AUTHORITY,
        MDKR_MATCH_PEER_PAYLOAD_INPUT)));
    assert(rig.harness.pumpUntil([&]() {
        rig.raw->serviceControl();
        return rig.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason ==
               MdkrMatchPeerLostReason::ControlChannelViolation);
    std::printf("authorityWrongPayloadTypeIsTypedLoss: ok\n");
}

/* N7: an endpoint that predates the authority channel offers only the two
 * older ones. Its connection comes up and its channels open, so it looks
 * exactly like a peer whose ICE never completed -- the answerer must name the
 * channel-set difference instead of reporting a connect timeout. */
void shortChannelSetOfferIsNamedAtTheSetupDeadline() {
    RawHarness rig;
    /* The mesh takes the HIGHER id so it answers and the raw peer offers. */
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(200u, 2u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(100u, 1u);
    rig.raw = std::make_unique<RawPeer>(100u, 1u, 200u, 2u, &rig.harness.hub);
    rig.raw->offerAuthorityChannel = false;
    rig.harness.hub.welcome(200u);
    /* Both older channels open on the mesh side, and the third never does. */
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->startOffer();
        rig.raw->serviceControl();
        return rig.raw->openChannelCount() >= 2u;
    }));
    assert(rig.harness.countEvents(
               200u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) == 0u);
    /* The answerer's setup deadline is the bounded verdict. */
    rig.harness.clock.nowMs += kMdkrMatchAnswererSetupDeadlineMs + 1u;
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PeerLost, 100u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = rig.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::PeerLost, 100u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ChannelSetMismatch);
    /* The reason reaches the forensics ring by NAME, not as an ordinal. */
    assert(std::strcmp(mdkr_match_peer_lost_reason_name(lost->lostReason),
                       "channel_set_mismatch") == 0);
    std::printf("shortChannelSetOfferIsNamedAtTheSetupDeadline: ok\n");
}

/* Control: the SAME offerer carrying the full channel set reaches
 * PeerChannelsReady and is never lost, so the verdict above reads the missing
 * channel and not merely "this peer offered". */
void fullChannelSetOfferReachesReady() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(200u, 2u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(100u, 1u);
    rig.raw = std::make_unique<RawPeer>(100u, 1u, 200u, 2u, &rig.harness.hub);
    rig.harness.hub.welcome(200u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->startOffer();
        rig.raw->serviceControl();
        return rig.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                   100u) >= 1u;
    }));
    assert(rig.harness.countEvents(
               200u, MdkrMatchPeerMeshEventType::PeerLost, 100u) == 0u);
    std::printf("fullChannelSetOfferReachesReady: ok\n");
}

/* Hello-only stand-in for a reconnected endpoint: one identity, the
 * three-hello dance with several targets, no WebRTC (the phrase never
 * needed channels). */
struct HelloDriver {
    struct Target {
        uint64_t id = 0u;
        uint32_t generation = 0u;
        unsigned received = 0u;
        bool committed = false;
        bool revealed = false;
    };

    uint64_t selfId;
    FakeHub *hub;
    FakeFeed *feed;
    MdkrMatchPeerIdentity *identity = nullptr;
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> publicKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES> nonce{};
    std::array<uint8_t, MDKR_MATCH_PEER_COMMIT_BYTES> commitment{};
    std::vector<Target> targets;

    HelloDriver(uint64_t self, uint32_t generation, FakeHub *owner,
                FakeFeed *ownFeed,
                const std::vector<std::pair<uint64_t, uint32_t>> &list)
        : selfId(self), hub(owner), feed(ownFeed) {
        identity = mdkr_match_peer_identity_create();
        assert(identity != nullptr);
        assert(mdkr_match_peer_identity_public_key(identity, publicKey.data()));
        for (unsigned index = 0u; index < nonce.size(); index++) {
            nonce[index] = static_cast<uint8_t>(0x51u + index);
        }
        assert(mdkr_match_peer_commitment(kEpoch, selfId, generation,
                                          nonce.data(), publicKey.data(),
                                          commitment.data()));
        for (const auto &entry : list) {
            Target target;
            target.id = entry.first;
            target.generation = entry.second;
            targets.push_back(target);
        }
    }

    ~HelloDriver() { mdkr_match_peer_identity_destroy(identity); }

    std::string blob(const uint8_t body[32]) const {
        uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        raw[0] = 0x04u;
        std::memcpy(raw + 1u, body, 32u);
        return mdkr_party::base64Url(raw, sizeof(raw));
    }

    void sendHello(const Target &target, const std::string &publicKeyBlob) {
        MdkrMatchSignalOutbound message;
        message.type = "peer_hello";
        message.toEndpointId = std::to_string(target.id);
        message.toConnectionGeneration = target.generation;
        message.publicKey = publicKeyBlob;
        hub->route(selfId, message);
    }

    void pump() {
        for (Target &target : targets) {
            if (!target.committed) {
                sendHello(target, blob(commitment.data()));
                target.committed = true;
            }
        }
        std::vector<MdkrMatchSignalEvent> drained;
        feed->drainEvents(drained);
        for (const MdkrMatchSignalEvent &event : drained) {
            if (event.type != MdkrMatchSignalEventType::PeerHello) continue;
            uint64_t from = 0u;
            try { from = std::stoull(event.fromEndpointId); } catch (...) {}
            for (Target &target : targets) {
                if (target.id != from) continue;
                target.received++;
                if (!target.revealed && target.received >= 1u) {
                    sendHello(target, mdkr_party::base64Url(publicKey.data(),
                                                            publicKey.size()));
                    sendHello(target, blob(nonce.data()));
                    target.revealed = true;
                }
            }
        }
    }
};

/* I2: a generation bump for one peer retires every transcript-salted key;
 * an honest third peer's in-flight old-digest control fragment during that
 * window must be a counted drop, never a peer kill -- and the whole roster
 * must re-derive to one fresh phrase afterwards. */
void rekeyGenerationBumpKeepsHonestPeers() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster =
        rosterOf({100u, 200u, 300u});
    MdkrMatchPeerMesh *meshA = harness.add(100u, 1u, roster);
    MdkrMatchPeerMesh *meshB = harness.add(200u, 2u, roster);
    MdkrMatchPeerMesh *meshC = harness.add(300u, 3u, roster);
    harness.hub.welcome(100u);
    harness.hub.welcome(200u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u;
    }, 30000u));
    std::string phraseBefore;
    assert(meshA->phrase(phraseBefore));

    /* B's signaling socket reconnects: gone at generation 2, back at 5. The
     * DEFAULT close is the internal/rebuild teardown and must stay silent --
     * a goodbye here would present the reconnect as a departure. */
    meshB->close();
    assert(harness.hub.countSent(200u, "peer_end", "close") == 0u);
    harness.hub.setGeneration(200u, 5u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = "200";
    bump.connectionGeneration = 5u;
    bump.present = true;
    harness.hub.inject(100u, bump); /* A learns first: presence skew */
    harness.pumpOnce();

    /* C, still keyed on the old digest, has a control fragment in flight
     * toward A. A must drop it non-terminally. */
    const auto fragment = payloadFixture(0x2Au);
    assert(meshC->sendPreflightFragment(100u, fragment.data()));
    for (unsigned index = 0u; index < 30u; index++) {
        harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(harness.countEvents(100u, MdkrMatchPeerMeshEventType::PeerLost,
                               300u) == 0u);

    /* C learns of the bump; a fresh endpoint 200 completes the exchange at
     * generation 5 (hello-only: the phrase never needed its channels). */
    harness.hub.inject(300u, bump);
    harness.pumpOnce();
    FakeFeed *replacementFeed = harness.hub.addEndpoint(200u, 5u);
    HelloDriver replacement(200u, 5u, &harness.hub, replacementFeed,
                            {{100u, 1u}, {300u, 3u}});
    assert(harness.pumpUntil([&]() {
        replacement.pump();
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 2u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PhraseReady) >= 2u;
    }));
    std::string phraseA;
    std::string phraseC;
    assert(meshA->phrase(phraseA));
    assert(meshC->phrase(phraseC));
    assert(!phraseA.empty() && phraseA == phraseC && phraseA != phraseBefore);

    /* The A<->C lane rides the re-derived keys immediately. */
    const auto payload = payloadFixture(0x77u);
    assert(meshA->sendInput(payload.data()) >= 1u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    assert(harness.countEvents(300u,
               MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("rekeyGenerationBumpKeepsHonestPeers: ok\n");
}

/* I3: the invented three-hello convention, pinned case by case. Nothing
 * malformed, duplicated, reordered or replayed may ever yield a phrase. */
void helloProtocolPinning() {
    const auto runCase =
        [](const char *name,
           const std::function<void(MeshHarness &, RawPeer &)> &drive,
           bool expectLost, MdkrMatchPeerLostReason expected) {
            MeshHarness harness;
            const std::vector<MdkrMatchPeerSlotOwner> roster =
                rosterOf({100u, 200u});
            MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
            harness.hub.addEndpoint(200u, 2u);
            RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
            raw.sendHellos = false;
            harness.hub.welcome(100u);
            harness.pumpOnce();
            drive(harness, raw);
            for (unsigned index = 0u; index < 5u; index++) {
                harness.pumpOnce();
            }
            if (expectLost) {
                const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
                    100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
                assert(lost != nullptr && lost->lostReason == expected);
            } else {
                assert(harness.countEvents(
                           100u, MdkrMatchPeerMeshEventType::PeerLost) == 0u);
            }
            /* Fails closed: no phrase from unverified material, ever. */
            std::string phrase;
            assert(!mesh->phrase(phrase));
            assert(harness.countEvents(
                       100u, MdkrMatchPeerMeshEventType::PhraseReady) == 0u);
            std::printf("  helloProtocolPinning[%s]: ok\n", name);
        };

    runCase("fourth-hello", [](MeshHarness &, RawPeer &raw) {
        raw.sendCommitHello();
        raw.sendRevealHellos();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.commitment.data());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("duplicate-commit", [](MeshHarness &, RawPeer &raw) {
        raw.sendCommitHello();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.commitment.data());
        });
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                                raw.publicKey.size());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("out-of-order", [](MeshHarness &, RawPeer &raw) {
        /* The real key where the commit belongs: its tail is not zero. */
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(raw.publicKey.data(),
                                                raw.publicKey.size());
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("commit-mismatch", [](MeshHarness &, RawPeer &raw) {
        /* Committed to one key, revealed another (the grinding shape). */
        MdkrMatchPeerIdentity *other = mdkr_match_peer_identity_create();
        assert(other != nullptr);
        std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> otherKey{};
        assert(mdkr_match_peer_identity_public_key(other, otherKey.data()));
        raw.sendCommitHello();
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(otherKey.data(),
                                                otherKey.size());
        });
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = raw.helloBlob(raw.nonce.data());
        });
        mdkr_match_peer_identity_destroy(other);
    }, true, MdkrMatchPeerLostReason::CommitmentMismatch);

    runCase("dirty-tail", [](MeshHarness &, RawPeer &raw) {
        uint8_t blob[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        blob[0] = 0x04u;
        std::memcpy(blob + 1u, raw.commitment.data(), 32u);
        blob[64] = 0x01u; /* reserved bytes must be zero */
        raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
            m.publicKey = mdkr_party::base64Url(blob, sizeof(blob));
        });
    }, true, MdkrMatchPeerLostReason::HelloViolation);

    runCase("stale-generation-replay", [](MeshHarness &harness, RawPeer &raw) {
        /* A hello claiming a generation other than the tracked one is
         * ignored, before AND after a generation bump. */
        MdkrMatchSignalEvent stale;
        stale.type = MdkrMatchSignalEventType::PeerHello;
        stale.fromEndpointId = "200";
        stale.fromConnectionGeneration = 99u;
        stale.publicKey = raw.helloBlob(raw.commitment.data());
        harness.hub.inject(100u, stale);
        harness.pumpOnce();
        MdkrMatchSignalEvent bump;
        bump.type = MdkrMatchSignalEventType::PeerPresence;
        bump.endpointId = "200";
        bump.connectionGeneration = 3u;
        bump.present = true;
        harness.hub.inject(100u, bump);
        harness.pumpOnce();
        MdkrMatchSignalEvent replay;
        replay.type = MdkrMatchSignalEventType::PeerHello;
        replay.fromEndpointId = "200";
        replay.fromConnectionGeneration = 2u; /* the retired generation */
        replay.publicKey = raw.helloBlob(raw.commitment.data());
        harness.hub.inject(100u, replay);
        harness.pumpOnce();
        assert(harness.mesh(100u)->stats().ignoredStaleSignals >= 2u);
    }, false, MdkrMatchPeerLostReason::HelloViolation);

    std::printf("helloProtocolPinning: ok\n");
}

/* M3: a peer whose committed-and-opened key is not a curve point is that
 * peer's typed loss, not a whole-mesh failure. */
void offCurveRevealIsPerPeerLoss() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 2u);
    RawPeer raw(200u, 2u, 100u, 1u, &harness.hub);
    raw.sendHellos = false;
    harness.hub.welcome(100u);
    harness.pumpOnce();

    /* Commit to garbage bytes, open the commitment honestly over them: the
     * commitment round passes, ECDH cannot. */
    uint8_t garbage[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES];
    garbage[0] = 0x04u;
    for (unsigned index = 1u; index < sizeof(garbage); index++) {
        garbage[index] = static_cast<uint8_t>(0xB0u + index * 7u);
    }
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES];
    assert(mdkr_match_peer_commitment(kEpoch, 200u, 2u, raw.nonce.data(),
                                      garbage, commitment));
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(commitment);
    });
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = mdkr_party::base64Url(garbage, sizeof(garbage));
    });
    raw.sendSignal("peer_hello", [&](MdkrMatchSignalOutbound &m) {
        m.publicKey = raw.helloBlob(raw.nonce.data());
    });
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::HelloViolation);
    /* Per-peer, not mesh-fatal: no Failure event, mesh stays serviceable. */
    assert(harness.countEvents(100u,
               MdkrMatchPeerMeshEventType::Failure) == 0u);
    std::string phrase;
    assert(!mesh->phrase(phrase));
    (void)mesh->stats();
    harness.pumpOnce();
    std::printf("offCurveRevealIsPerPeerLoss: ok\n");
}

/* M1: a refused opening-nonce hello (relay said peer_unavailable) must be
 * retried like hellos 1 and 2, not dead-end the exchange forever. */
void helloNonceSendFailureRetries() {
    RawHarness rig;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    rig.mesh = rig.harness.add(100u, 1u, roster);
    rig.rawFeed = rig.harness.hub.addEndpoint(200u, 2u);
    rig.raw = std::make_unique<RawPeer>(200u, 2u, 100u, 1u, &rig.harness.hub);
    rig.harness.hub.failHelloSend(100u, 3u); /* the opening nonce, once */
    rig.harness.hub.welcome(100u);
    assert(rig.harness.pumpUntil([&]() {
        rig.drainRawInbox();
        rig.raw->serviceControl();
        return rig.raw->meshHellos >= 3u; /* the peer got all three */
    }, 10000u));
    std::printf("helloNonceSendFailureRetries: ok\n");
}

/* M4: an answerer whose offer never arrives must reach a typed, bounded
 * verdict instead of waiting forever. */
void answererSetupDeadlineBounded() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({200u, 300u});
    harness.add(300u, 3u, roster); /* higher id: pure answerer */
    harness.hub.addEndpoint(200u, 2u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 20000u, 5000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        300u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ConnectTimeout);
    std::printf("answererSetupDeadlineBounded: ok\n");
}

/* W3 N4: the race mesh must not reuse the phones' 20 s human-in-the-loop
 * offer deadline -- a relay-dropped offer recreates in single-digit seconds
 * (kMdkrMatchOfferRetryDeadlineMs), 3 attempts kept. The phones' default is
 * pinned separately by test_party_transport_retry. */
void meshOfferLadderRetriesInSingleDigitSeconds() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    harness.add(100u, 1u, roster);
    harness.hub.addEndpoint(200u, 7u);
    harness.hub.blackhole(200u, true); /* every offer vanishes in the relay */
    harness.hub.welcome(100u);
    assert(harness.pumpUntil([&]() {
        return harness.hub.countSent(100u, "webrtc_offer") >= 1u;
    }, 10000u));
    /* Just under the mesh deadline (fake clock): no recreate yet. */
    harness.clock.nowMs += kMdkrMatchOfferRetryDeadlineMs - 500u;
    for (unsigned index = 0u; index < 10u; index++) {
        harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(harness.hub.countSent(100u, "webrtc_offer") == 1u);
    /* Cross it: the second offer must appear WITHOUT 20 s of fake time. */
    harness.clock.nowMs += 1000u;
    assert(harness.pumpUntil([&]() {
        return harness.hub.countSent(100u, "webrtc_offer") >= 2u;
    }, 10000u));
    std::printf("meshOfferLadderRetriesInSingleDigitSeconds: ok\n");
}

/* W3 N4: the answerer's bounded verdict drops with the ladder it mirrors
 * (3 x the mesh deadline -- the offerer's total budget), so a failed
 * pairing resolves in ~21 s instead of 60 s. */
void meshAnswererVerdictWithinTheMeshBudget() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({200u, 300u});
    harness.add(300u, 3u, roster); /* higher id: pure answerer */
    harness.hub.addEndpoint(200u, 2u);
    harness.hub.welcome(300u);
    harness.pumpOnce(); /* arm the setup deadline */
    harness.clock.nowMs += 22000u; /* > 3 x 7 s, far under the old 60 s */
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
        300u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(lost != nullptr &&
           lost->lostReason == MdkrMatchPeerLostReason::ConnectTimeout);
    std::printf("meshAnswererVerdictWithinTheMeshBudget: ok\n");
}

/* W3 CRITICAL-2: a peer-presence-bump rekey must refresh OUR OWN commitment
 * (fresh nonce), not reuse the already-revealed one. Pre-fix, rekeyPeer left
 * ownNonce/ownCommitment untouched on a peer bump, so our transcript
 * contribution was a stable identity key + an already-public nonce -- fully
 * known to the relay, which could then offline-grind ~2^20 fresh peer
 * keypairs to force a colliding phrase a human would wave through. Observed
 * on the wire: the local endpoint's round-1 commitment hello blob MUST
 * differ before vs after the bump, and the phrase still converges across the
 * pair on the re-derived transcript. */
void peerBumpRefreshesLocalCommitment() {
    PairHarness pair;
    assert(pair.connect());
    std::string phraseBefore;
    assert(pair.low->phrase(phraseBefore));
    const std::vector<std::string> lowHellosBefore =
        pair.harness.hub.helloBlobs(100u);
    assert(!lowHellosBefore.empty());
    const std::string commitBefore = lowHellosBefore.front();
    const size_t sentBefore = lowHellosBefore.size();

    /* The peer (200) reconnects its signal socket: higher generation,
     * announced to us as a presence bump; a replacement peer completes the
     * fresh hello exchange at generation 5. Close the old real mesh first so
     * addEndpoint can retire its borrowed feed safely (the 3-peer rekey
     * test's discipline). */
    pair.high->close();
    pair.harness.hub.setGeneration(200u, 5u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = "200";
    bump.connectionGeneration = 5u;
    bump.present = true;
    pair.harness.hub.inject(100u, bump);
    FakeFeed *replacementFeed = pair.harness.hub.addEndpoint(200u, 5u);
    HelloDriver replacement(200u, 5u, &pair.harness.hub, replacementFeed,
                            {{100u, 1u}});
    assert(pair.harness.pumpUntil([&]() {
        replacement.pump();
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PhraseReady) >= 2u;
    }, 30000u));

    /* Our round-1 commitment after the bump is a DIFFERENT blob: the local
     * nonce was refreshed (pre-fix this was byte-identical to commitBefore). */
    const std::vector<std::string> lowHellosAfter =
        pair.harness.hub.helloBlobs(100u);
    assert(lowHellosAfter.size() > sentBefore);
    const std::string commitAfter = lowHellosAfter[sentBefore];
    assert(commitAfter != commitBefore);

    /* And the phrase converged on the new transcript -- fresh, not reused. */
    std::string phraseLow;
    assert(pair.low->phrase(phraseLow));
    assert(!phraseLow.empty() && phraseLow != phraseBefore);
    std::printf("peerBumpRefreshesLocalCommitment: ok\n");
}

/* W3 MINOR (batched-single-pump): a malicious relay can batch
 * [presence-bump, hello1, hello2, hello3] into ONE feed drain, so the mesh's
 * keysDerived flips false->true INSIDE a single pump() and an observer that
 * only polls phrase() BETWEEN pumps never sees the retire window. This pins
 * the property the adapter's digest-keyed detection relies on: after that one
 * pump the phrase is available again (no refusal to observe) BUT the
 * transcript digest has CHANGED -- so a digest compare arms the barrier where
 * a phrase()-refusal poll would miss it entirely. */
void batchedRewelcomeChangesDigestInOnePump() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    MdkrMatchPeerMesh *mesh = harness.add(100u, 1u, roster);
    FakeFeed *feed2 = harness.hub.addEndpoint(200u, 2u);
    HelloDriver driver2(200u, 2u, &harness.hub, feed2, {{100u, 1u}});
    harness.hub.welcome(100u);
    assert(harness.pumpUntil([&]() {
        driver2.pump();
        return harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PhraseReady) >= 1u;
    }));
    uint8_t digestBefore[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
    assert(mesh->transcriptDigest(digestBefore));
    std::string phraseBefore;
    assert(mesh->phrase(phraseBefore));

    /* Queue the WHOLE re-welcome before a single pump: the peer's higher
     * generation bump, then a fresh peer's three hellos back-to-back. */
    harness.hub.setGeneration(200u, 5u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = "200";
    bump.connectionGeneration = 5u;
    bump.present = true;
    harness.hub.inject(100u, bump);
    FakeFeed *feed5 = harness.hub.addEndpoint(200u, 5u);
    HelloDriver driver5(200u, 5u, &harness.hub, feed5, {{100u, 1u}});
    const HelloDriver::Target &target = driver5.targets.front();
    driver5.sendHello(target, driver5.blob(driver5.commitment.data()));
    driver5.sendHello(target, mdkr_party::base64Url(driver5.publicKey.data(),
                                                    driver5.publicKey.size()));
    driver5.sendHello(target, driver5.blob(driver5.nonce.data()));

    /* ONE pump: presence + all three hellos drain, deriveAll runs, keysDerived
     * ends true -- phrase() never observed as refusing across pumps. */
    mesh->pump();
    std::vector<MdkrMatchPeerMeshEvent> drained;
    mesh->drainEvents(drained);

    std::string phraseAfter;
    uint8_t digestAfter[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
    assert(mesh->phrase(phraseAfter));          /* no refusal to observe */
    assert(mesh->transcriptDigest(digestAfter));
    assert(std::memcmp(digestBefore, digestAfter,
                       sizeof(digestBefore)) != 0); /* 256-bit change */
    std::printf("batchedRewelcomeChangesDigestInOnePump: ok\n");
}

/* W3 N6b (mesh half): the local endpoint's own signal socket dies (the
 * feed's terminal Failure) and a REPLACEMENT socket's fresh welcome -- a
 * strictly higher connection generation, the wire contract's first-class
 * replacement shape -- arrives through the same feed. The mesh must adopt
 * the new generation as its own replacement (fresh commitment, full pair
 * rekey, fresh phrase, keys that still carry input), and -- the actual hole
 * -- have its recovery ladders back: a later channel death must run the
 * peer_end/restart ladder again instead of PeerLost(TransportFailed). */
void reWelcomeAfterSignalLossRestoresRecoveryLadders() {
    PairHarness pair;
    assert(pair.connect());
    std::string phraseBefore;
    assert(pair.low->phrase(phraseBefore));

    MdkrMatchSignalEvent failure;
    failure.type = MdkrMatchSignalEventType::Failure;
    failure.failureCode = kMdkrMatchSignalTransportLost;
    pair.harness.hub.inject(100u, failure);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::Failure) >= 1u;
    }, 5000u));

    /* The service assigns the replacement socket generation 4 and
     * announces it to the peer as a presence bump. */
    pair.harness.hub.setGeneration(100u, 4u);
    MdkrMatchSignalEvent bump;
    bump.type = MdkrMatchSignalEventType::PeerPresence;
    bump.endpointId = "100";
    bump.connectionGeneration = 4u;
    bump.present = true;
    pair.harness.hub.inject(200u, bump);
    pair.harness.hub.welcome(100u); /* the fresh welcome, generation 4 */

    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PhraseReady) >= 2u &&
               pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PhraseReady) >= 2u &&
               pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                   200u) >= 2u &&
               pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PeerChannelsReady,
                   100u) >= 2u;
    }, 30000u));
    assert(pair.low->connectionGeneration() == 4u);
    std::string phraseLow;
    std::string phraseHigh;
    assert(pair.low->phrase(phraseLow));
    assert(pair.high->phrase(phraseHigh));
    assert(!phraseLow.empty() && phraseLow == phraseHigh);
    assert(phraseLow != phraseBefore); /* a replaced endpoint re-keys */

    /* Input crosses on the re-derived generation-4 keys. */
    const auto payload = payloadFixture(0x66u);
    assert(pair.low->sendInput(payload.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }));

    /* THE hole: restarts must work again after the reconnect. */
    const unsigned readyLow = pair.harness.countEvents(
        100u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u);
    const unsigned readyHigh = pair.harness.countEvents(
        200u, MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u);
    assert(mdkr_match_peer_mesh_kill_channels_for_test(*pair.low, 200u));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >
                   readyLow &&
               pair.harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >
                   readyHigh;
    }, 30000u));
    assert(pair.harness.hub.countSent(100u, "peer_end", "restart") >= 1u);
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    std::printf("reWelcomeAfterSignalLossRestoresRecoveryLadders: ok\n");
}

/* ---- Mid-race loss vs a vanished peer ------------------------------------ */

/* Pump ONLY the surviving mesh (100), stepping the shared fake clock -- the
 * frozen peer (200) is a corpse: its libdatachannel threads still ack at the
 * SCTP layer (exactly like a killed remote whose kernel/socket lingers), but
 * its pump never runs, so it never pongs, never answers offers, never drains
 * its feed. */
bool pumpSurvivorUntil(PairHarness &pair, const std::function<bool()> &done,
                       unsigned iterations, uint64_t fakeStepMs) {
    for (unsigned index = 0u; index < iterations; index++) {
        std::vector<MdkrMatchPeerMeshEvent> drained;
        pair.low->pump();
        pair.low->drainEvents(drained);
        for (MdkrMatchPeerMeshEvent &event : drained) {
            pair.harness.events[100u].push_back(std::move(event));
        }
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pair.harness.clock.nowMs += fakeStepMs;
    }
    return done();
}

MdkrMatchSignalEvent presenceEvent(uint64_t about, uint32_t generation,
                                   bool present) {
    MdkrMatchSignalEvent event;
    event.type = MdkrMatchSignalEventType::PeerPresence;
    event.endpointId = std::to_string(about);
    event.connectionGeneration = generation;
    event.present = present;
    return event;
}

/* THE production kill signature (real-cloud mid-race SIGKILL): the killed
 * process's signal socket dies, the service broadcasts presence=false, and
 * the peer goes silent with its channels still nominally up. The survivor's
 * control-ping ladder MUST still run -- the ladder is the transport's own
 * liveness probe and presence is irrelevant to it -- so the loss resolves as
 * a typed PingTimeout within kMdkrMatchMidRaceLossPingBoundMs. Pre-fix the
 * `!peer.present` gate froze EVERY ladder for exactly this peer, so nothing
 * ever fired and the survivor ghost-raced to a watchdog. */
void presenceDropDoesNotStarveTheLossLadders() {
    PairHarness pair;
    assert(pair.connect());
    /* The kill: presence drops (service saw the socket die), the peer's pump
     * freezes (the process is gone). Channels stay nominally open. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    const uint64_t droppedAtMs = pair.harness.clock.nowMs;
    const bool lost = pumpSurvivorUntil(pair, [&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, /*iterations=*/80u, /*fakeStepMs=*/500u);
    if (!lost) {
        std::fprintf(stderr,
                     "FAIL presenceDropDoesNotStarveTheLossLadders: no PeerLost "
                     "within %llu fake ms of the presence drop -- the loss "
                     "ladders are starved for a vanished peer (the mid-race "
                     "kill signature: the survivor would ghost-race to the "
                     "watchdog)\n",
                     (unsigned long long)(pair.harness.clock.nowMs - droppedAtMs));
        assert(lost);
    }
    const MdkrMatchPeerMeshEvent *event = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(event != nullptr &&
           event->lostReason == MdkrMatchPeerLostReason::PingTimeout);
    const uint64_t detectMs = pair.harness.clock.nowMs - droppedAtMs;
    /* The named ping-path bound + one fake step of slack (the in-process
     * corpse's ICE stays alive, so the ping ordering is deterministic
     * here; the composite kMdkrMatchMidRaceLossDetectBoundMs covers the
     * ICE-first ordering the real cloud can take). */
    assert(detectMs <= kMdkrMatchMidRaceLossPingBoundMs + 1000u);
    std::printf("presenceDropDoesNotStarveTheLossLadders: ok (%llu fake ms)\n",
                (unsigned long long)detectMs);
}

/* The other ordering: the transport verdict lands FIRST (ICE Disconnected /
 * channel close -> ConnectionDown) on a peer whose presence is already gone.
 * A restart can never complete against an endpoint signaling cannot reach,
 * so the absent+down state must resolve as a typed PeerVanished within the
 * kMdkrMatchPeerVanishTimeoutMs dwell instead of parking forever in the
 * (unreachable) restart ladder. */
void vanishedPeerConnectionDownResolvesBounded() {
    PairHarness pair;
    assert(pair.connect());
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    /* Absorb the presence drop, then the hard transport kill. */
    assert(pumpSurvivorUntil(pair, []() { return true; }, 1u, 0u));
    const unsigned offersBefore = pair.harness.hub.countSent(100u,
                                                             "webrtc_offer");
    assert(mdkr_match_peer_mesh_kill_channels_for_test(*pair.low, 200u));
    const uint64_t downAtMs = pair.harness.clock.nowMs;
    const bool lost = pumpSurvivorUntil(pair, [&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, /*iterations=*/80u, /*fakeStepMs=*/500u);
    if (!lost) {
        std::fprintf(stderr,
                     "FAIL vanishedPeerConnectionDownResolvesBounded: no "
                     "PeerLost within %llu fake ms of ConnectionDown on an "
                     "absent peer -- the restart ladder parks forever against "
                     "an endpoint signaling cannot reach\n",
                     (unsigned long long)(pair.harness.clock.nowMs - downAtMs));
        assert(lost);
    }
    const MdkrMatchPeerMeshEvent *event = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(event != nullptr &&
           event->lostReason == MdkrMatchPeerLostReason::PeerVanished);
    const uint64_t detectMs = pair.harness.clock.nowMs - downAtMs;
    assert(detectMs <= kMdkrMatchPeerVanishTimeoutMs + 1500u);
    /* No restart was wasted on the void: an absent peer can receive
     * neither the peer_end "restart" nor a fresh offer, so connectionDown
     * retires the dead connection without burning an episode or offering
     * into nowhere. */
    assert(pair.harness.hub.countSent(100u, "webrtc_offer") == offersBefore);
    assert(pair.harness.hub.countSent(100u, "peer_end", "restart") == 0u);
    std::printf("vanishedPeerConnectionDownResolvesBounded: ok (%llu fake ms)\n",
                (unsigned long long)detectMs);
}

/* The false-positive guard the ruling demands: a peer whose SIGNAL socket
 * blips (presence=false) while its channels stay healthy and responsive is
 * NOT lost -- pings keep ponging, no ladder fires, and a presence re-assert
 * plus live input prove the mesh carried play through the blip. This is the
 * documented "room updates reconnecting" posture, now with the liveness
 * ladder actually running through it. */
void presenceBlipWithHealthyChannelsIsNotPeerLoss() {
    PairHarness pair;
    assert(pair.connect());
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    /* 40 s of fake clock with BOTH endpoints alive and pumping. */
    for (unsigned index = 0u; index < 40u; index++) {
        pair.harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pair.harness.clock.nowMs += 1000u;
    }
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    assert(pair.harness.countEvents(
               200u, MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    /* The blip ends; input still crosses on the SAME keys (no rekey, no
     * teardown happened underneath). */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, true));
    const auto payload = payloadFixture(0x3Cu);
    assert(pair.low->sendInput(payload.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }, 5000u));
    std::printf("presenceBlipWithHealthyChannelsIsNotPeerLoss: ok\n");
}

/* The vanish dwell's DISARM arm, in the pre-race/pairing phase where the
 * dwell also runs (channels not yet ready): a peer that goes absent mid-setup
 * but RE-APPEARS before the dwell expires must NOT be lost -- the dwell
 * zeroes and the setup ladders resume. Staged on the ANSWERER (no offer
 * ladder of its own; its 21 s setup deadline is far beyond this window), with
 * the presence re-assert landing at 9 s of a 10 s dwell and the clock then
 * driven well PAST the would-be expiry. */
void presenceReassertBeforeDwellExpiryDisarms() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster = rosterOf({100u, 200u});
    harness.add(200u, 2u, roster); /* the answerer: 100 < 200 offers */
    harness.hub.addEndpoint(100u, 1u); /* never offers: setup never completes */
    harness.hub.welcome(200u);
    harness.pumpOnce();
    /* The peer drops off signaling during pairing: the dwell arms. */
    harness.hub.inject(200u, presenceEvent(100u, 1u, false));
    harness.pumpOnce();
    const uint64_t droppedAtMs = harness.clock.nowMs;
    /* 9 s of absence (inside the 10 s dwell)... */
    for (unsigned second = 0u; second < 9u; second++) {
        harness.clock.nowMs += 1000u;
        harness.pumpOnce();
        assert(harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerLost) == 0u);
    }
    /* ...then the peer re-appears (same generation: the same socket was
     * merely flapping at the relay). The dwell must ZERO, not pause. */
    harness.hub.inject(200u, presenceEvent(100u, 1u, true));
    /* Drive well past the original 10 s expiry (to +16 s; the answerer's
     * own 21 s setup deadline stays out of frame). An un-disarmed dwell
     * would fire PeerVanished right here. */
    while (harness.clock.nowMs - droppedAtMs < 16000u) {
        harness.clock.nowMs += 1000u;
        harness.pumpOnce();
        if (harness.countEvents(200u,
                MdkrMatchPeerMeshEventType::PeerLost) != 0u) {
            const MdkrMatchPeerMeshEvent *lost = harness.lastEvent(
                200u, MdkrMatchPeerMeshEventType::PeerLost, 100u);
            std::fprintf(stderr,
                         "FAIL presenceReassertBeforeDwellExpiryDisarms: peer "
                         "lost (reason=%d) at +%llu ms despite the presence "
                         "re-assert at 9 s -- the dwell did not disarm\n",
                         lost ? static_cast<int>(lost->lostReason) : -1,
                         (unsigned long long)(harness.clock.nowMs -
                                              droppedAtMs));
            assert(false);
        }
    }
    std::printf("presenceReassertBeforeDwellExpiryDisarms: ok\n");
}

/* The airtight laggy-alive guard: presence FLAPPING while the pong round
 * trip stretches to 14 s -- one second inside the 15 s stale bound -- must
 * not be a peer loss. The peer (200) is alive but pumps only after a long
 * gap, so its pong lands with a near-stale RTT while the survivor's view of
 * its signal presence toggles; then normal cadence resumes and input still
 * crosses. Keys the guard on the exact worst case the detection must
 * tolerate: lag near the bound + relay flapping, distinct from loss. */
void flappingPresenceWithSlowPongsIsNotPeerLoss() {
    PairHarness pair;
    assert(pair.connect());
    const auto pumpLowOnce = [&pair]() {
        std::vector<MdkrMatchPeerMeshEvent> drained;
        pair.low->pump();
        pair.low->drainEvents(drained);
        for (MdkrMatchPeerMeshEvent &event : drained) {
            pair.harness.events[100u].push_back(std::move(event));
        }
    };
    const auto pumpHighOnce = [&pair]() {
        std::vector<MdkrMatchPeerMeshEvent> drained;
        pair.high->pump();
        pair.high->drainEvents(drained);
        for (MdkrMatchPeerMeshEvent &event : drained) {
            pair.harness.events[200u].push_back(std::move(event));
        }
    };
    const auto noLoss = [&pair]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost) == 0u &&
               pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PeerLost) == 0u;
    };
    /* Phase 1: the peer goes SILENT (never pumps) while its presence flaps
     * every ~3 s at the relay. The survivor's ping goes out at +5 s of the
     * connect-time cadence and stays outstanding through +18 s -- reaching
     * 13-14 s, just inside the 15 s stale bound. No loss may fire. */
    bool present = false;
    for (unsigned second = 0u; second < 19u; second++) {
        if (second % 3u == 0u) {
            pair.harness.hub.inject(100u, presenceEvent(200u, 2u, present));
            present = !present;
        }
        pumpLowOnce();
        assert(noLoss());
        pair.harness.clock.nowMs += 1000u;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    /* Phase 2: the near-stale pong lands -- the peer finally pumps (pong
     * goes out) and the survivor absorbs it on a HELD clock (a real-time
     * beat with no fake-time advance, so the ladder judges the pong at its
     * true ~14 s round trip and clears it). Alive, not lost. */
    pumpHighOnce();
    for (unsigned spin = 0u; spin < 100u; spin++) {
        pumpLowOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(noLoss());
    /* Phase 3: normal cadence resumes with the presence still flapping for
     * another 20 s: pings/pongs flow, nothing fires. */
    for (unsigned second = 0u; second < 20u; second++) {
        if (second % 3u == 1u) {
            pair.harness.hub.inject(100u, presenceEvent(200u, 2u, present));
            present = !present;
        }
        pair.harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pair.harness.clock.nowMs += 1000u;
    }
    assert(noLoss());
    /* The flap ends present; input still crosses on the same keys. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, true));
    const auto payload = payloadFixture(0x5Au);
    assert(pair.low->sendInput(payload.data()) == 1u);
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::InputEnvelope, 100u) >= 1u;
    }, 5000u));
    std::printf("flappingPresenceWithSlowPongsIsNotPeerLoss: ok\n");
}

/* THE DOUBLE BLIP: both endpoints' signal sockets bounce near-simultaneously.
 * The peer reconnects (the service assigns it a strictly higher generation),
 * but its presence-bump broadcast went out while OUR socket was down, and our
 * own re-welcome listed the still-reconnecting peer absent -- so the only
 * evidence of the RETURNING peer is its re-driven signaling under the new
 * generation, service-stamped. That traffic must cancel the pending vanish
 * declaration; pre-fix it was dropped as a stale generation and the dwell
 * expired on a peer that was demonstrably back. The loss floors themselves
 * are untouched: nothing here shortens a ladder, it only kills the false
 * positive (a truly silent vanished peer still resolves at the dwell). */
void reconnectTrafficDuringDwellCancelsVanish() {
    PairHarness pair;
    assert(pair.connect());
    /* The blip: peer 200 drops off signaling AND its transport comes down --
     * the exact absent+down state that arms the vanish dwell. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    assert(pumpSurvivorUntil(pair, []() { return true; }, 1u, 0u));
    assert(mdkr_match_peer_mesh_kill_channels_for_test(*pair.low, 200u));
    /* Absorb the teardown and run 4 s into the 10 s dwell. */
    (void)pumpSurvivorUntil(pair, []() { return false; }, 8u, 500u);
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);
    /* The peer is back at generation 5; the bump was missed. All that
     * arrives is its re-driven hello-1, relayed with the service-stamped
     * sender generation. */
    const unsigned hellosBefore = pair.harness.hub.countSent(100u,
                                                             "peer_hello");
    MdkrMatchSignalEvent hello;
    hello.type = MdkrMatchSignalEventType::PeerHello;
    hello.fromEndpointId = "200";
    hello.fromConnectionGeneration = 5u;
    {
        uint8_t raw[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES] = {};
        raw[0] = 0x04u;
        for (unsigned index = 1u; index <= 32u; index++) {
            raw[index] = static_cast<uint8_t>(index);
        }
        hello.publicKey = mdkr_party::base64Url(raw, sizeof(raw));
    }
    pair.harness.hub.inject(100u, hello);
    /* Drive the clock 12 s further -- well past the original dwell expiry
     * (t0+10.5 s), inside the setup ladders' own ~21 s bound (deliberately
     * untouched: a re-admitted peer that never completes setup still
     * resolves there, typed). The returning peer must NOT be declared gone
     * at the dwell. */
    (void)pumpSurvivorUntil(pair, []() { return false; }, 24u, 500u);
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);
    /* And the lane actually RE-DRIVES: the adopted generation restarted the
     * pairwise exchange (fresh hellos toward the peer's new socket). */
    assert(pair.harness.hub.countSent(100u, "peer_hello") > hellosBefore);
    std::printf("reconnectTrafficDuringDwellCancelsVanish: ok\n");
}

/* A DELIBERATE local end (the announcing close: the player left / the adapter
 * was torn down) must resolve on the survivor IMMEDIATELY and typed: teardown
 * announces the wire contract's peer_end "close" goodbye to every viable peer,
 * and the receiving mesh maps it to PeerLost(PeerEnded) on its next pump --
 * never the restart episodes (~21 s) or the vanish dwell (10 s) that a CRASH
 * rightly takes. The clock is held to a 3 s fake window, far inside every
 * ladder bound, so a ladder-timed resolution cannot masquerade as the goodbye.
 * (The default close(false) stays silent -- the reconnect model in
 * rekeyGenerationBumpKeepsHonestPeers pins that no goodbye fires there.) */
void deliberateCloseIsImmediateTypedPeerEnd() {
    PairHarness pair;
    assert(pair.connect());
    const uint64_t closedAtMs = pair.harness.clock.nowMs;
    pair.low->close(/*announcePeerEnd=*/true);
    bool lost = false;
    for (unsigned index = 0u; index < 30u && !lost; index++) {
        pair.harness.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pair.harness.clock.nowMs += 100u;
        lost = pair.harness.countEvents(
                   200u, MdkrMatchPeerMeshEventType::PeerLost, 100u) >= 1u;
    }
    const uint64_t detectMs = pair.harness.clock.nowMs - closedAtMs;
    if (!lost) {
        std::fprintf(stderr,
                     "FAIL deliberateCloseIsImmediateTypedPeerEnd: no PeerLost "
                     "within %llu fake ms of the peer's deliberate close -- the "
                     "survivor is grinding the restart/vanish ladders against "
                     "an endpoint that said nothing on its way out\n",
                     (unsigned long long)detectMs);
        assert(lost);
    }
    /* The goodbye must be ON THE WIRE (not a ladder coincidence), and the
     * verdict must carry the deliberate-end reason. */
    assert(pair.harness.hub.countSent(100u, "peer_end", "close") >= 1u);
    const MdkrMatchPeerMeshEvent *event = pair.harness.lastEvent(
        200u, MdkrMatchPeerMeshEventType::PeerLost, 100u);
    assert(event != nullptr &&
           event->lostReason == MdkrMatchPeerLostReason::PeerEnded);
    assert(detectMs <= 3000u);
    std::printf("deliberateCloseIsImmediateTypedPeerEnd: ok (%llu fake ms)\n",
                (unsigned long long)detectMs);
}

}  // namespace

/* A3: the room's own membership verdict, surfaced the pump it arrives on.
 * PeerDeparted is an OBSERVATION, not a verdict: it changes nothing about this
 * peer's connection, so the loss ladders and the presence-blip invariant below
 * stay exactly as they were and the launcher -- which alone knows whether a
 * race is running -- decides what a departure means. */
void roomDepartureIsSurfacedOnTheArrivingPump() {
    PairHarness pair;
    assert(pair.connect());
    const uint64_t atMs = pair.harness.clock.nowMs;
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerDeparted, 200u) == 1u);
    /* No fake time passed, so nothing timed out: this came from the room. */
    assert(pair.harness.clock.nowMs == atMs);
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);
    /* Departure is edge-triggered: a repeated absence is not a second leave. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerDeparted, 200u) == 1u);
    /* A returning peer can depart again. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, true));
    pair.harness.pumpOnce();
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerDeparted, 200u) == 2u);
    /* A generation the relay has already superseded says nothing about the
     * peer the mesh is talking to. */
    pair.harness.hub.inject(100u, presenceEvent(200u, 1u, false));
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerDeparted, 200u) == 2u);
    std::printf("roomDepartureIsSurfacedOnTheArrivingPump: ok\n");
}

/* A3: the launcher's half of the departure. The mesh reports the room's
 * verdict; the launcher, which alone knows a race is running, retires the peer
 * -- and gets the ordinary typed loss, ring record and teardown for it, with
 * no second path beside the transport ladders' own. */
void retiringADepartedPeerIsTypedPeerDeparted() {
    PairHarness pair;
    assert(pair.connect());
    pair.harness.hub.inject(100u, presenceEvent(200u, 2u, false));
    pair.harness.pumpOnce();
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerDeparted, 200u) == 1u);
    const uint64_t atMs = pair.harness.clock.nowMs;
    assert(pair.harness.mesh(100u)->retireDepartedPeer(200u));
    pair.harness.pumpOnce();
    /* The launcher asked for the retirement, so the mesh does not tell it
     * back -- that round trip would cost the very pump this exists to save. */
    assert(pair.harness.countEvents(
               100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) == 0u);
    /* No ladder ran: the verdict came from the room, not from waiting. */
    assert(pair.harness.clock.nowMs == atMs);
    /* The peer really is retired: idempotent, and nothing reaches it. */
    assert(!pair.harness.mesh(100u)->retireDepartedPeer(200u));
    assert(!pair.harness.mesh(100u)->retireDepartedPeer(999u));
    assert(pair.harness.mesh(100u)->sendInput(
               payloadFixture(0x11u).data()) == 0u);
    assert(std::strcmp(mdkr_match_peer_lost_reason_name(
               MdkrMatchPeerLostReason::PeerDeparted), "peer_departed") == 0);
    std::printf("retiringADepartedPeerIsTypedPeerDeparted: ok\n");
}

/* A3: the proposer's finalisation tick reaches the other survivors on the
 * reliable ordered control channel, where ordering is what makes the first
 * proposal for a seat the one everyone commits to. The mesh carries the claim
 * -- sender, epoch and tick -- and leaves entitlement to the launcher, which
 * is the only layer that knows who has already departed. */
void raceDropCarriesTheAgreedTickToEverySurvivor() {
    MeshHarness harness;
    const std::vector<MdkrMatchPeerSlotOwner> roster =
        rosterOf({100u, 200u, 300u});
    harness.add(100u, 1u, roster);
    harness.add(200u, 2u, roster);
    harness.add(300u, 3u, roster);
    harness.hub.welcome(100u);
    harness.hub.welcome(200u);
    harness.hub.welcome(300u);
    assert(harness.pumpUntil([&]() {
        return harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 200u) >= 1u &&
               harness.countEvents(100u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 300u) >= 1u &&
               harness.countEvents(200u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u &&
               harness.countEvents(300u,
                   MdkrMatchPeerMeshEventType::PeerChannelsReady, 100u) >= 1u;
    }, 30000u));

    uint64_t sender = 0u;
    uint64_t departed = 0u;
    uint32_t epoch = 0u;
    uint32_t tick = 0u;
    /* Nothing to consume before a proposal lands. */
    assert(!harness.mesh(200u)->consumeRaceDrop(&sender, &departed, &epoch,
                                                &tick));
    /* 100 proposes 300's finalisation. The departed endpoint is not one of
     * the recipients. */
    assert(harness.mesh(100u)->sendRaceDrop(300u, 9u, 4242u) == 1u);
    /* An endpoint this room never had is refused before it reaches the wire. */
    assert(harness.mesh(100u)->sendRaceDrop(999u, 9u, 4242u) == 0u);
    assert(harness.pumpUntil([&]() {
        return harness.mesh(200u)->peekRaceDrop();
    }, 5000u));
    assert(harness.mesh(200u)->consumeRaceDrop(&sender, &departed, &epoch,
                                               &tick));
    /* The whole claim survives the trip: who said it, about which race. */
    assert(sender == 100u && departed == 300u && epoch == 9u && tick == 4242u);
    /* Read-and-clear, like the abort latch beside it. */
    assert(!harness.mesh(200u)->consumeRaceDrop(&sender, &departed, &epoch,
                                                &tick));
    assert(!harness.mesh(300u)->peekRaceDrop());

    /* First proposal for an endpoint wins: a later one cannot move a tick a
     * survivor may already have committed to. Proving that needs to know both
     * proposals ARRIVED, which a sleep can only guess at -- so a race_abort is
     * sent behind them on the SAME reliable ordered channel and used as the
     * arrival marker. Once 300 sees the abort, both proposals have certainly
     * landed and the map's contents are final. */
    assert(harness.mesh(100u)->sendRaceDrop(200u, 9u, 90u) == 1u);
    assert(harness.mesh(100u)->sendRaceDrop(200u, 9u, 91u) == 1u);
    assert(harness.mesh(100u)->sendRaceAbort() == 2u);
    assert(harness.pumpUntil([&]() {
        return harness.mesh(300u)->consumeRaceAbort();
    }, 5000u));
    assert(harness.mesh(300u)->consumeRaceDrop(&sender, &departed, &epoch,
                                               &tick));
    assert(sender == 100u && departed == 200u && epoch == 9u && tick == 90u);
    /* Exactly one entry for that endpoint: 91 was refused, not queued. */
    assert(!harness.mesh(300u)->consumeRaceDrop(&sender, &departed, &epoch,
                                                &tick));

    /* A proposal names one race; clearRaceDrops is what the launcher runs when
     * its race state resets, so a late one cannot reach the next race. */
    assert(harness.mesh(100u)->sendRaceDrop(300u, 9u, 55u) == 1u);
    assert(harness.pumpUntil([&]() {
        return harness.mesh(200u)->peekRaceDrop();
    }, 5000u));
    harness.mesh(200u)->clearRaceDrops();
    assert(!harness.mesh(200u)->peekRaceDrop());
    assert(!harness.mesh(200u)->consumeRaceDrop(&sender, &departed, &epoch,
                                                &tick));

    /* The proposal is not a verdict on the sender either: 200 keeps talking
     * to 100 exactly as before. */
    assert(harness.countEvents(
               200u, MdkrMatchPeerMeshEventType::PeerLost, 100u) == 0u);
    std::printf("raceDropCarriesTheAgreedTickToEverySurvivor: ok\n");
}

/* A drop naming an endpoint outside this room's fixed roster, or naming the
 * recipient itself, is garbage on the reliable channel -- the same verdict
 * every other malformed control message gets. */
void raceDropNamingANonRosterEndpointIsTypedLoss() {
    PairHarness pair;
    assert(pair.connect());
    assert(mdkr_match_peer_mesh_send_raw_race_drop_for_test(
        *pair.harness.mesh(200u), 999u, 9u, 7u));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *event = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(event != nullptr &&
           event->lostReason ==
               MdkrMatchPeerLostReason::ControlChannelViolation);
    std::printf("raceDropNamingANonRosterEndpointIsTypedLoss: ok\n");
}

/* A sender cannot name ITSELF as the departed endpoint: an endpoint that is
 * talking on this channel has plainly not left, so the claim is structurally
 * false and terminal, like any other garbage on a reliable channel. */
void raceDropNamingTheSenderIsTypedLoss() {
    PairHarness pair;
    assert(pair.connect());
    assert(mdkr_match_peer_mesh_send_raw_race_drop_for_test(
        *pair.harness.mesh(200u), 200u, 9u, 7u));
    assert(pair.harness.pumpUntil([&]() {
        return pair.harness.countEvents(
                   100u, MdkrMatchPeerMeshEventType::PeerLost, 200u) >= 1u;
    }, 5000u));
    const MdkrMatchPeerMeshEvent *self = pair.harness.lastEvent(
        100u, MdkrMatchPeerMeshEventType::PeerLost, 200u);
    assert(self != nullptr &&
           self->lostReason ==
               MdkrMatchPeerLostReason::ControlChannelViolation);
    std::printf("raceDropNamingTheSenderIsTypedLoss: ok\n");
}

int main() {
    twoPeerHappyPath();
    threePeerMeshEveryoneReachesEveryone();
    staleGenerationAnswerIgnored();
    phraseRefusedBeforeCommitmentCompletes();
    sealWindowExhaustionIsTypedPeerLoss();
    badStateEnvelopeDroppedAndCounted();
    authenticatedPacketCountTracksOpenedEnvelopesOnly();
    controlPingTimeoutIsTypedPeerLoss();
    controlPingMissCountsBeforeTimeout();
    iceRestartRecoversAfterChannelDeath();
    offerLadderGivesUpBounded();
    closeTearsDownBounded();
    /* Fix round 1: C1, I1, I2, I3, M1, M3, M4. */
    malformedControlJsonIsTypedLossNotCrash();
    delayedControlFragmentSurvivesInputBurst();
    authorityLaneKeepsItsOwnReplayWindow();
    authorityWrongPayloadTypeIsTypedLoss();
    fullChannelSetOfferReachesReady();
    shortChannelSetOfferIsNamedAtTheSetupDeadline();
    rekeyGenerationBumpKeepsHonestPeers();
    helloProtocolPinning();
    offCurveRevealIsPerPeerLoss();
    helloNonceSendFailureRetries();
    answererSetupDeadlineBounded();
    /* W3 connection robustness. */
    meshOfferLadderRetriesInSingleDigitSeconds();
    meshAnswererVerdictWithinTheMeshBudget();
    reWelcomeAfterSignalLossRestoresRecoveryLadders();
    peerBumpRefreshesLocalCommitment();
    batchedRewelcomeChangesDigestInOnePump();
    /* Mid-race loss of a VANISHED peer (presence dropped + silent). */
    presenceDropDoesNotStarveTheLossLadders();
    vanishedPeerConnectionDownResolvesBounded();
    presenceBlipWithHealthyChannelsIsNotPeerLoss();
    presenceReassertBeforeDwellExpiryDisarms();
    flappingPresenceWithSlowPongsIsNotPeerLoss();
    /* The double blip: service-stamped reconnect traffic cancels the dwell. */
    reconnectTrafficDuringDwellCancelsVanish();
    /* Deliberate end: the teardown goodbye reaches the survivor typed. */
    deliberateCloseIsImmediateTypedPeerEnd();
    /* A3 lobby-authoritative drop: the room's membership verdict and the
     * finalisation tick the survivors agree on. */
    roomDepartureIsSurfacedOnTheArrivingPump();
    retiringADepartedPeerIsTypedPeerDeparted();
    raceDropCarriesTheAgreedTickToEverySurvivor();
    raceDropNamingANonRosterEndpointIsTypedLoss();
    raceDropNamingTheSenderIsTypedLoss();
    std::printf("all match_peer_transport cases passed\n");
    return 0;
}
