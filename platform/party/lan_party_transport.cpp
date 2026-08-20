/*
 * LanPartyTransport implementation.
 *
 * EXTRACTION vs DUPLICATION (stated per the task brief).
 *   - The self-contained signaling primitives -- SAS keypair + phrase, DTLS
 *     fingerprint capture, base64url, the defensive JSON readers -- are
 *     EXTRACTED to party_webrtc_signaling.{h,cpp} and shared byte-for-byte
 *     with the cloud transport, so the two can never derive a different
 *     verification phrase or accept a key one rejects.
 *   - The WebRTC PEER MACHINERY below (createPeer, the data-channel wiring,
 *     handleAnswer/emitPhrase/handleIce, stateMessage/controlMessage, the
 *     room-state roster, the ping keepalive) is DUPLICATED from
 *     libdatachannel_party_transport.cpp with this pointer to its origin,
 *     NOT extracted. In the cloud transport that machinery is inseparable
 *     from a WebSocket's lifecycle -- socket generations, the resume/reconnect
 *     ladder, the M7 socket-message cap, the close-flush -- none of which
 *     exist on a LAN. Hoisting a shared "peer engine" out of that file would
 *     have to restructure the very socket lifecycle the cloud transport's
 *     large, security-critical test surface pins, i.e. it would destabilize
 *     the cloud transport for a task that must leave it byte-behaviour
 *     identical. So the coupled machinery is mirrored here, minus everything
 *     LAN does not have:
 *       * no reconnect/resume ladder and no RoomGone from the network -- the
 *         "server" is in-process; RoomGone is emitted only on explicit
 *         stop()/host close (it is not, in fact, emitted at all -- see below).
 *       * no unanswered-offer C3 retry -- the room hands the offer straight
 *         to the phone; there is no Worker relay to drop it.
 *       * no M7 socket cycling, no close-flush wait.
 *
 * SIGNALING RELAY. Where the cloud transport speaks host-role frames over a
 * WebSocket to the Worker, this one IS its own relay: host-role frames
 * (host_command / webrtc_offer / webrtc_ice) go to MdkrLanPartyRoom::
 * deliverFromHost, and the room delivers room_state / relayed
 * controller_hello|webrtc_answer|webrtc_ice / host_command_result / host_closed
 * back through the HostMessage sink registered at open(). The phones reach the
 * room over the embedded MdkrLanPartyServer's /party-ws, adapted socket-for-
 * socket into the room's abstract controller socket.
 *
 * HOST-SINK CONTRACT (Task 2 review gate). The sink copies each frame into the
 * transport's bounded, drop-oldest MdkrPartyEventQueue (kMaxQueuedEvents) and
 * returns immediately -- it never blocks, never calls back into the room or
 * the launcher model, and is safe from either the connection thread
 * (controller-driven relays) or the launcher thread (command results). This is
 * the same bounded poll-queue the cloud transport honours.
 */
#include "lan_party_transport.h"

#include "party_event_queue.h"
#include "party_webrtc_signaling.h"
#include "party/lan_party_room.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxQueuedEvents = 128u;
constexpr size_t kMaxSignalBytes = 64u * 1024u;
/* Channel protocol tag for host_ready/ping/pong/rumble; equals
 * mdkr_party::kChannelProtocol by construction. */
constexpr unsigned kProtocol = mdkr_party::kChannelProtocol;

/* Shared signaling primitives (see party_webrtc_signaling.h). */
using PartyIdentity = mdkr_party::Identity;
using mdkr_party::commandRejectionFromSignal;
using mdkr_party::controllerReadyEventFromControl;
using mdkr_party::decodePublicKey;
using mdkr_party::safeString;
using mdkr_party::uintValue;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

/* Adapter: an embedded-server /party-ws socket presented as the room's
 * abstract controller socket (~the 12-line shim the Task 2 report anticipated).
 * Both directions are pass-through; sendText/close are already thread-safe on
 * MdkrLanPartyWebSocket, and onMessage/onClosed are registered by the room's
 * attachController before the first frame. */
class ServerSocketAdapter final : public MdkrLanPartyControllerSocket {
public:
    explicit ServerSocketAdapter(std::shared_ptr<MdkrLanPartyWebSocket> socket)
        : socket_(std::move(socket)) {}
    bool sendText(const std::string &payload) override {
        return socket_ && socket_->sendText(payload);
    }
    void close(uint16_t code, const std::string &reason) override {
        if (socket_) socket_->close(code, reason);
    }
    bool isOpen() const override { return socket_ && socket_->isOpen(); }
    void onMessage(std::function<void(const std::string &)> callback) override {
        if (socket_) socket_->onMessage(std::move(callback));
    }
    void onClosed(std::function<void()> callback) override {
        if (socket_) socket_->onClosed(std::move(callback));
    }

private:
    std::shared_ptr<MdkrLanPartyWebSocket> socket_;
};

/* Peer -- mirrors libdatachannel_party_transport.cpp's Peer, minus the C3
 * unanswered-offer retry fields (offerSentMs/offerAttempts/gaveUp): a LAN room
 * never drops an offer, so there is nothing to retry. */
struct Peer {
    std::string id;
    unsigned seat = 0u;
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
    uint32_t peerGeneration = 0u;
    bool failed = false;
    bool authenticated = false;
    bool protocolMismatch = false;
    uint32_t pingNonce = 0u;
    Clock::time_point nextPingAt{};
    Clock::time_point pingOutstandingAt{};
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> state;
    std::shared_ptr<rtc::DataChannel> control;
};

MdkrLanPartyRoomConfig roomConfigFrom(const MdkrLanPartyTransportConfig &config) {
    MdkrLanPartyRoomConfig roomConfig;
    roomConfig.randomBytes = config.roomRandomBytes;
    roomConfig.nowMs = config.roomNowMs;
    return roomConfig;
}

class LanTransportState final
    : public std::enable_shared_from_this<LanTransportState> {
public:
    explicit LanTransportState(MdkrLanPartyTransportConfig config)
        : config_(std::move(config)), room_(roomConfigFrom(config_)) {}

    bool initialize(bool startServer) {
        if (!identity_.generate()) return false;

        uint16_t port = config_.bindPort;
        if (startServer) {
            const std::weak_ptr<LanTransportState> weak = shared_from_this();
            server_ = std::make_unique<MdkrLanPartyServer>();
            server_->onWebSocket(
                [weak](std::shared_ptr<MdkrLanPartyWebSocket> socket) {
                    auto self = weak.lock();
                    if (!self || !socket) return;
                    self->room_.attachController(
                        std::make_shared<ServerSocketAdapter>(std::move(socket)));
                });
            if (!server_->start(config_.bindPort,
                                std::move(config_.manifest))) {
                return false;
            }
            port = server_->port();
            if (port == 0u) return false;
        }

        std::string origin;
        if (!config_.advertisedHost.empty()) {
            origin = "http://" + config_.advertisedHost;
            if (port != 0u) origin += ":" + std::to_string(port);
        }

        const std::weak_ptr<LanTransportState> weak = shared_from_this();
        const MdkrLanPartyInvite invite = room_.open(
            identity_.publicKey(), origin,
            [weak](const std::string &json) {
                if (auto self = weak.lock()) self->onHostMessage(json);
            });

        std::string initial;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            roomId_ = invite.roomId;
            inviteUrl_ = invite.controllerUrl;
            fallbackCode_ = invite.fallbackCode;
            inviteGeneration_ = invite.inviteGeneration;
            bootstrapped_ = true;
            initial.swap(initialRoomState_);
        }
        /* The room emitted its opening room_state synchronously above, before
         * the cache was seeded; replay it now that a room_state can resolve
         * its (omitted) invite URL/code from the cache. */
        if (!initial.empty()) onHostMessage(initial);
        return true;
    }

    /* Host-role frame -> the room. Never holds mutex_ across deliverFromHost:
     * that call re-enters the room and can fire the sink synchronously (a
     * command result), which takes mutex_. */
    bool command(const Json &message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return false;
        }
        room_.deliverFromHost(message.dump());
        return true;
    }

    bool sendRumble(const std::string &id, uint16_t strength) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(id);
            if (found == peers_.end() || !found->second->control ||
                !found->second->control->isOpen()) return false;
            channel = found->second->control;
        }
        try {
            return channel->send(Json{{"type", "rumble"}, {"protocol", kProtocol},
                {"strength", strength},
                {"durationMs", strength > 0u ? 250u : 0u}}.dump());
        } catch (...) { return false; }
    }

    bool poll(MdkrPartyTransportEvent &event) {
        tick();
        std::lock_guard<std::mutex> lock(mutex_);
        if (drainedIndex_ >= drained_.size()) {
            queue_.drainInto(drained_);
            drainedIndex_ = 0u;
            const uint64_t dropped = queue_.droppedPadPackets();
            if (dropped != loggedDroppedPadPackets_) {
                std::fprintf(stderr, "[PARTY-QUEUE] lan dropped=%llu\n",
                    static_cast<unsigned long long>(dropped));
                loggedDroppedPadPackets_ = dropped;
            }
        }
        if (drainedIndex_ >= drained_.size()) return false;
        event = std::move(drained_[drainedIndex_++]);
        return true;
    }

    void shutdown() {
        std::unique_ptr<MdkrLanPartyServer> server;
        std::map<std::string, std::shared_ptr<Peer>> peers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return;
            shuttingDown_ = true;
            server = std::move(server_);
            peers.swap(peers_);
            signaled_.clear();
        }
        /* Tell the phones goodbye (idempotent -- a prior closeRoom() already
         * closed the room, and this returns early then), then join the server
         * threads and close every peer. */
        room_.close();
        if (server) server->stop();
        for (auto &entry : peers) {
            if (entry.second->connection) entry.second->connection->close();
        }
    }

    void attachTestController(
        std::shared_ptr<MdkrLanPartyControllerSocket> socket) {
        room_.attachController(std::move(socket));
    }

private:
    void enqueue(MdkrPartyTransportEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) return;
        queue_.push(std::move(event));
    }

    uint64_t roomNow() const {
        return config_.roomNowMs ? config_.roomNowMs() : steadyNowMs();
    }

    /* ---- room -> host sink -------------------------------------------- */

    void onHostMessage(const std::string &text) {
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            const std::string type = value.value("type", std::string{});
            if (type == "room_state") {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!bootstrapped_) {
                        initialRoomState_ = text;
                        return;
                    }
                }
                handleRoomState(value);
            } else if (type == "controller_hello") {
                handleHello(value);
            } else if (type == "webrtc_answer") {
                handleAnswer(value);
            } else if (type == "webrtc_ice") {
                handleIce(value);
            } else if (type == "host_command_result") {
                MdkrPartyTransportEvent event;
                if (commandRejectionFromSignal(value, event)) {
                    enqueue(std::move(event));
                }
            }
            /* host_closed is ignored on purpose: the host drives its own
             * Closed transition from closeRoom(), and a LAN room never fails
             * a resume, so this transport emits no RoomGone. */
        } catch (...) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "The controller room sent an invalid update.";
            enqueue(std::move(event));
        }
    }

    /* LAN room_state parse. Same shape as the cloud's parseRoom
     * (libdatachannel_party_transport.cpp) EXCEPT the clock: inviteExpiresAt
     * arrives already in this transport's own clock domain (the room and the
     * transport share roomNow()), so it is used directly rather than run
     * through a wall->steady conversion. The controllerUrl/fallbackCode are
     * cached from the invite and refreshed only by a rotate that carries
     * them, exactly as the cloud caches the bootstrap's. */
    bool parseRoom(const Json &value, MdkrPartyTransportRoomState &room) {
        uint64_t transition = 0u;
        uint64_t generation = 0u;
        uint64_t expiresAt = 0u;
        if (!uintValue(value, "transitionId", transition) || transition == 0u ||
            !uintValue(value, "inviteGeneration", generation,
                std::numeric_limits<unsigned>::max()) || generation == 0u ||
            !uintValue(value, "inviteExpiresAt", expiresAt) ||
            !value.contains("controllers") || !value["controllers"].is_array() ||
            value["controllers"].size() > 8u) return false;
        room.transitionId = transition;
        room.inviteGeneration = static_cast<unsigned>(generation);
        const uint64_t now = roomNow();
        room.inviteActive =
            value.value("phase", std::string{}) == "open" && expiresAt > now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (room.inviteGeneration == inviteGeneration_) {
                room.controllerUrl = inviteUrl_;
                room.fallbackCode = fallbackCode_;
            }
        }
        if (safeString(value, "controllerUrl", room.controllerUrl, 2048u, false) &&
            safeString(value, "fallbackCode", room.fallbackCode, 6u, false)) {
            if (value.contains("controllerUrl") && value.contains("fallbackCode")) {
                std::lock_guard<std::mutex> lock(mutex_);
                inviteUrl_ = room.controllerUrl;
                fallbackCode_ = room.fallbackCode;
                inviteGeneration_ = room.inviteGeneration;
            }
        } else return false;
        for (const Json &item : value["controllers"]) {
            if (!item.is_object()) return false;
            MdkrNativePartyController controller;
            if (!safeString(item, "controllerId", controller.id, 64u) ||
                controller.id.empty() ||
                !safeString(item, "name", controller.name, 48u) ||
                !safeString(item, "controllerPublicKey", controller.publicKey, 87u) ||
                controller.publicKey.size() != 87u) return false;
            const std::string phase = item.value("phase", std::string{});
            if (phase == "pending") controller.phase = MdkrNativePartyControllerPhase::Pending;
            else if (phase == "approved") controller.phase = MdkrNativePartyControllerPhase::Approved;
            else if (phase == "leased") controller.phase = MdkrNativePartyControllerPhase::Leased;
            else if (phase == "connected") controller.phase = MdkrNativePartyControllerPhase::Connected;
            else return false;
            uint64_t seat = 0u;
            if (item.contains("seat") && !item["seat"].is_null() &&
                !uintValue(item, "seat", seat, 4u)) return false;
            uint64_t lease = 0u;
            uint64_t connection = 0u;
            if (!uintValue(item, "leaseGeneration", lease,
                    std::numeric_limits<uint32_t>::max()) ||
                !uintValue(item, "connectionSequence", connection,
                    std::numeric_limits<uint32_t>::max())) return false;
            controller.seat = static_cast<unsigned>(seat);
            controller.leaseGeneration = static_cast<uint32_t>(lease);
            controller.connectionSequence = static_cast<uint32_t>(connection);
            std::array<uint8_t, 65> keyBytes{};
            if (!decodePublicKey(controller.publicKey, keyBytes)) return false;
            room.controllers.push_back(std::move(controller));
        }
        room.inviteExpiresInMs = expiresAt > now ? expiresAt - now : 0u;
        return true;
    }

    void handleRoomState(const Json &value) {
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::RoomState;
        if (!parseRoom(value, event.room)) {
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "The controller room sent an invalid room update.";
            enqueue(std::move(event));
            return;
        }
        std::vector<std::shared_ptr<Peer>> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            controllers_.clear();
            for (const auto &controller : event.room.controllers) {
                controllers_[controller.id] = controller;
                const auto found = peers_.find(controller.id);
                if (found != peers_.end()) {
                    if (found->second->seat != controller.seat ||
                        found->second->leaseGeneration != controller.leaseGeneration ||
                        found->second->connectionSequence != controller.connectionSequence) {
                        retired.push_back(found->second);
                        peers_.erase(found);
                    }
                }
            }
            for (auto iterator = peers_.begin(); iterator != peers_.end();) {
                if (controllers_.count(iterator->first) == 0u) {
                    retired.push_back(iterator->second);
                    iterator = peers_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            for (auto iterator = signaled_.begin(); iterator != signaled_.end();) {
                if (controllers_.count(*iterator) == 0u) iterator = signaled_.erase(iterator);
                else ++iterator;
            }
        }
        for (const auto &peer : retired) {
            if (peer->connection) peer->connection->close();
        }
        const std::vector<MdkrNativePartyController> controllers = event.room.controllers;
        enqueue(std::move(event));
        for (const auto &controller : controllers) {
            bool shouldCreate = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldCreate = signaled_.count(controller.id) != 0u &&
                    controller.phase != MdkrNativePartyControllerPhase::Pending &&
                    peers_.count(controller.id) == 0u;
            }
            if (shouldCreate) createPeer(controller);
        }
    }

    void handleHello(const Json &value) {
        std::string id;
        if (!safeString(value, "controllerId", id, 64u) || id.empty()) return;
        MdkrNativePartyController controller;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signaled_.insert(id);
            const auto known = controllers_.find(id);
            if (known != controllers_.end()) {
                controller = known->second;
                found = true;
            }
        }
        if (found && controller.phase != MdkrNativePartyControllerPhase::Pending) {
            createPeer(controller);
        }
    }

    /* No STUN: a LAN pairing must reach the phone with zero internet, so only
     * host candidates (loopback + LAN addresses) are gathered. */
    void createPeer(const MdkrNativePartyController &controller) {
        uint32_t peerGeneration = 0u;
        std::shared_ptr<rtc::PeerConnection> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(controller.id);
            if (found != peers_.end() && !found->second->failed) return;
            if (found != peers_.end()) peers_.erase(found);
            peerGeneration = ++peerGeneration_;
            if (peerGeneration == 0u) peerGeneration = ++peerGeneration_;
        }
        if (stale) stale->close();
        rtc::Configuration configuration;
        configuration.maxMessageSize = kMaxSignalBytes;
        auto peer = std::make_shared<Peer>();
        peer->id = controller.id;
        peer->seat = controller.seat;
        peer->leaseGeneration = controller.leaseGeneration;
        peer->connectionSequence = controller.connectionSequence;
        peer->peerGeneration = peerGeneration;
        try {
            peer->connection = std::make_shared<rtc::PeerConnection>(configuration);
        } catch (...) { return; }
        const std::weak_ptr<LanTransportState> weak = shared_from_this();
        const std::weak_ptr<Peer> weakPeer = peer;
        peer->connection->onLocalDescription([weak, weakPeer](rtc::Description description) {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->sendPeerSignal(current,
                    Json{{"type", "webrtc_offer"}, {"to", current->id},
                        {"peerGeneration", current->peerGeneration},
                        {"sdp", {{"type", description.typeString()},
                            {"sdp", std::string(description)}}}});
            }
        });
        peer->connection->onLocalCandidate([weak, weakPeer](rtc::Candidate candidate) {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->sendPeerSignal(current,
                    Json{{"type", "webrtc_ice"}, {"to", current->id},
                        {"peerGeneration", current->peerGeneration},
                        {"candidate", {{"candidate", std::string(candidate)},
                            {"sdpMid", candidate.mid()}}}});
            }
        });
        peer->connection->onStateChange([weak, weakPeer](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                if (auto self = weak.lock()) {
                    if (auto current = weakPeer.lock()) self->peerDisconnected(current,
                        state == rtc::PeerConnection::State::Failed);
                }
            }
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) {
                peer->connection->close();
                return;
            }
            peers_[peer->id] = peer;
        }
        rtc::DataChannelInit stateConfiguration;
        stateConfiguration.reliability.unordered = true;
        stateConfiguration.reliability.maxRetransmits = 0u;
        try {
            peer->state = peer->connection->createDataChannel(
                "mdkr-pad-state-v1", stateConfiguration);
            peer->control = peer->connection->createDataChannel("mdkr-pad-control-v1");
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = peers_.find(peer->id);
                if (found != peers_.end() && found->second == peer) peers_.erase(found);
            }
            peer->connection->close();
            return;
        }
        peer->state->onMessage([weak, weakPeer](rtc::message_variant message) {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->stateMessage(current, message);
            }
        });
        peer->state->onClosed([weak, weakPeer]() {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->peerDisconnected(current, true);
            }
        });
        peer->control->onOpen([weak, weakPeer]() {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->controlOpened(current);
            }
        });
        peer->control->onMessage([weak, weakPeer](rtc::message_variant message) {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->controlMessage(current, message);
            }
        });
        peer->control->onClosed([weak, weakPeer]() {
            if (auto self = weak.lock()) {
                if (auto current = weakPeer.lock()) self->peerDisconnected(current, true);
            }
        });
    }

    void peerDisconnected(const std::shared_ptr<Peer> &peer, bool failed) {
        MdkrNativePartyController controller;
        bool recover = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(peer->id);
            if (found == peers_.end() || found->second != peer) return;
            peer->failed = peer->failed || failed;
            const auto known = controllers_.find(peer->id);
            if (failed && known != controllers_.end() &&
                known->second.phase != MdkrNativePartyControllerPhase::Pending) {
                controller = known->second;
                recover = true;
            }
        }
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerDisconnected;
        event.controllerId = peer->id;
        enqueue(std::move(event));
        if (recover) createPeer(controller);
    }

    void stateMessage(const std::shared_ptr<Peer> &peer,
                      const rtc::message_variant &message) {
        if (!std::holds_alternative<rtc::binary>(message)) return;
        const rtc::binary &bytes = std::get<rtc::binary>(message);
        if (bytes.size() < 24u || bytes.size() > 64u) return;
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerPacket;
        event.controllerId = peer->id;
        const auto *first = reinterpret_cast<const uint8_t *>(bytes.data());
        event.packet.assign(first, first + bytes.size());
        enqueue(std::move(event));
    }

    void controlOpened(const std::shared_ptr<Peer> &peer) {
        try {
            peer->control->send(Json{{"type", "host_ready"}, {"protocol", kProtocol},
                {"seat", peer->seat}, {"leaseGeneration", peer->leaseGeneration},
                {"connectionSequence", peer->connectionSequence}}.dump());
        } catch (...) { peerDisconnected(peer, true); }
    }

    void controlMessage(const std::shared_ptr<Peer> &peer,
                        const rtc::message_variant &message) {
        if (!std::holds_alternative<std::string>(message)) return;
        const std::string &text = std::get<std::string>(message);
        if (text.size() > 4096u) return;
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            MdkrPartyTransportEvent ready;
            if (controllerReadyEventFromControl(
                    value, peer->id, peer->connectionSequence, ready)) {
                const bool matched = ready.type ==
                    MdkrPartyTransportEventType::ControllerConnected;
                enqueue(std::move(ready));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto found = peers_.find(peer->id);
                    if (found != peers_.end() && found->second == peer) {
                        peer->protocolMismatch = !matched;
                        if (matched) {
                            peer->authenticated = true;
                            peer->pingOutstandingAt = Clock::time_point{};
                            peer->nextPingAt = Clock::now() + std::chrono::seconds(5);
                        }
                    }
                }
                if (matched) {
                    peer->control->send(Json{{"type", "controller_ready_ack"}}.dump());
                }
            } else if (value.value("type", std::string{}) == "input_test" &&
                       value.contains("nonce") &&
                       value["nonce"].is_number_unsigned()) {
                peer->control->send(Json{{"type", "input_test_ack"},
                    {"nonce", value["nonce"]}}.dump());
            } else if (value.value("type", std::string{}) == "pong" &&
                       value.value("protocol", 0u) == kProtocol &&
                       value.contains("nonce") &&
                       value["nonce"].is_number_unsigned()) {
                const uint64_t nonce = value["nonce"].get<uint64_t>();
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = peers_.find(peer->id);
                if (nonce <= std::numeric_limits<uint32_t>::max() &&
                    found != peers_.end() && found->second == peer &&
                    peer->pingOutstandingAt != Clock::time_point{} &&
                    static_cast<uint32_t>(nonce) == peer->pingNonce) {
                    peer->pingOutstandingAt = Clock::time_point{};
                    peer->nextPingAt = Clock::now() + std::chrono::seconds(5);
                }
            }
        } catch (...) { /* Malformed peer control cannot escape its callback. */ }
    }

    void handleAnswer(const Json &value) {
        std::string id;
        uint64_t peerGeneration = 0u;
        if (!safeString(value, "controllerId", id, 64u) ||
            !uintValue(value, "peerGeneration", peerGeneration,
                std::numeric_limits<uint32_t>::max()) || peerGeneration == 0u ||
            !value.contains("sdp") || !value["sdp"].is_object()) return;
        std::string sdp;
        std::string type;
        if (!safeString(value["sdp"], "sdp", sdp, 60u * 1024u) ||
            !safeString(value["sdp"], "type", type, 16u) || type != "answer") return;
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(id);
            if (found == peers_.end() ||
                found->second->peerGeneration != static_cast<uint32_t>(peerGeneration)) return;
            peer = found->second;
        }
        try { peer->connection->setRemoteDescription(rtc::Description(sdp, type)); }
        catch (...) {
            peerDisconnected(peer, true);
            return;
        }
        emitPhrase(peer, sdp);
    }

    /* SAS v2 derivation site (mirrors the cloud's emitPhrase). Both DTLS
     * fingerprints exist by now: the offer went out at createDataChannel time,
     * this answer was just applied. A missing/ambiguous piece emits no phrase
     * (fail closed). */
    void emitPhrase(const std::shared_ptr<Peer> &peer,
                    const std::string &answerSdp) {
        const std::string controllerFingerprint =
            mdkr_party::canonicalSdpFingerprint(answerSdp);
        std::string hostSdp;
        try {
            const std::optional<rtc::Description> description =
                peer->connection->localDescription();
            if (description) hostSdp = std::string(*description);
        } catch (...) { return; }
        const std::string hostFingerprint =
            mdkr_party::canonicalSdpFingerprint(hostSdp);
        std::string controllerKey;
        std::string room;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = controllers_.find(peer->id);
            if (found != controllers_.end()) controllerKey = found->second.publicKey;
            room = roomId_;
        }
        std::string phrase;
        if (controllerFingerprint.empty() || hostFingerprint.empty() ||
            controllerKey.empty() || room.empty() ||
            !identity_.phrase(controllerKey, room, hostFingerprint,
                              controllerFingerprint, phrase)) {
            return;
        }
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::ControllerPhrase;
        event.controllerId = peer->id;
        event.message = std::move(phrase);
        enqueue(std::move(event));
    }

    void handleIce(const Json &value) {
        std::string id;
        uint64_t peerGeneration = 0u;
        if (!safeString(value, "controllerId", id, 64u) ||
            !uintValue(value, "peerGeneration", peerGeneration,
                std::numeric_limits<uint32_t>::max()) || peerGeneration == 0u ||
            !value.contains("candidate") || !value["candidate"].is_object()) return;
        std::string candidate;
        std::string mid;
        if (!safeString(value["candidate"], "candidate", candidate, 4096u) ||
            !safeString(value["candidate"], "sdpMid", mid, 64u)) return;
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(id);
            if (found == peers_.end() ||
                found->second->peerGeneration != static_cast<uint32_t>(peerGeneration)) return;
            peer = found->second;
        }
        try { peer->connection->addRemoteCandidate(rtc::Candidate(candidate, mid)); }
        catch (...) { /* One malformed candidate cannot tear down a healthy peer. */ }
    }

    void sendPeerSignal(const std::shared_ptr<Peer> &peer, const Json &value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(peer->id);
            if (found == peers_.end() || found->second != peer || peer->failed) return;
        }
        (void)command(value);
    }

    void tick() {
        std::vector<std::shared_ptr<Peer>> ping;
        std::vector<std::shared_ptr<Peer>> expired;
        const Clock::time_point now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return;
            for (const auto &entry : peers_) {
                const std::shared_ptr<Peer> &peer = entry.second;
                if (peer->failed || !peer->authenticated || !peer->control) continue;
                if (peer->pingOutstandingAt != Clock::time_point{} &&
                    now - peer->pingOutstandingAt >= std::chrono::seconds(15)) {
                    expired.push_back(peer);
                } else if (peer->pingOutstandingAt == Clock::time_point{} &&
                           peer->nextPingAt != Clock::time_point{} &&
                           now >= peer->nextPingAt) {
                    peer->pingNonce++;
                    peer->pingOutstandingAt = now;
                    peer->nextPingAt = now + std::chrono::seconds(5);
                    ping.push_back(peer);
                }
            }
        }
        for (const auto &peer : expired) peerDisconnected(peer, true);
        for (const auto &peer : ping) {
            try {
                if (!peer->control->isOpen() || !peer->control->send(
                        Json{{"type", "ping"}, {"protocol", kProtocol},
                            {"nonce", peer->pingNonce}}.dump())) {
                    peerDisconnected(peer, true);
                }
            } catch (...) { peerDisconnected(peer, true); }
        }
    }

    std::mutex mutex_;
    MdkrPartyEventQueue queue_{kMaxQueuedEvents};
    std::vector<MdkrPartyTransportEvent> drained_;
    size_t drainedIndex_ = 0u;
    uint64_t loggedDroppedPadPackets_ = 0u;

    MdkrLanPartyTransportConfig config_;
    PartyIdentity identity_;
    MdkrLanPartyRoom room_;
    std::unique_ptr<MdkrLanPartyServer> server_;

    std::map<std::string, std::shared_ptr<Peer>> peers_;
    std::map<std::string, MdkrNativePartyController> controllers_;
    std::set<std::string> signaled_;
    uint32_t peerGeneration_ = 0u;

    /* Invite cache seeded from room_.open() and refreshed on rotate -- the LAN
     * analogue of the cloud's native_bootstrap cache. */
    std::string roomId_;
    std::string inviteUrl_;
    std::string fallbackCode_;
    unsigned inviteGeneration_ = 0u;
    bool bootstrapped_ = false;
    std::string initialRoomState_;

    bool shuttingDown_ = false;
};

class LanPartyTransport final : public MdkrPartyTransport {
public:
    LanPartyTransport(MdkrLanPartyTransportConfig config, bool useServer)
        : config_(std::move(config)), useServer_(useServer) {}

    bool available() const override { return true; }
    const char *unavailableReason() const override { return ""; }
    bool requiresSecureOrigin() const override { return false; }

    bool open(const std::string & /*serviceOrigin*/) override {
        /* The LAN transport is configured at construction (manifest, advertised
         * host, port); the origin argument the host passes is unused here. */
        if (state_) return false;
        state_ = std::make_shared<LanTransportState>(std::move(config_));
        if (!state_->initialize(useServer_)) {
            state_->shutdown();
            state_.reset();
            return false;
        }
        return true;
    }

    bool approve(const std::string &id, unsigned seat) override {
        return sendHostCommand("approve", id, Json{{"seat", seat}});
    }
    bool reject(const std::string &id) override { return sendHostCommand("reject", id); }
    bool remove(const std::string &id) override { return sendHostCommand("remove", id); }
    bool rotateInvite(unsigned generation) override {
        return state_ && state_->command(Json{{"type", "host_command"},
            {"action", "rotate"}, {"expectedInviteGeneration", generation}});
    }
    bool revokeInvite() override { return sendHostCommand("revoke", ""); }
    bool closeRoom() override { return sendHostCommand("close", ""); }
    bool sendRumble(const std::string &id, uint16_t strength) override {
        return state_ && state_->sendRumble(id, strength);
    }
    bool poll(MdkrPartyTransportEvent &event) override {
        return state_ && state_->poll(event);
    }
    void shutdown() override {
        if (state_) state_->shutdown();
        state_.reset();
    }

    void attachTestController(std::shared_ptr<MdkrLanPartyControllerSocket> socket) {
        if (state_) state_->attachTestController(std::move(socket));
    }

private:
    bool sendHostCommand(const char *action, const std::string &id,
                         Json extra = Json::object()) {
        if (!state_) return false;
        Json message = {{"type", "host_command"}, {"action", action}};
        if (!id.empty()) message["controllerId"] = id;
        message.update(extra);
        return state_->command(message);
    }

    MdkrLanPartyTransportConfig config_;
    bool useServer_;
    std::shared_ptr<LanTransportState> state_;
};

}  // namespace

std::unique_ptr<MdkrPartyTransport> mdkr_create_lan_party_transport(
    MdkrLanPartyTransportConfig config) {
    return std::make_unique<LanPartyTransport>(std::move(config), /*useServer=*/true);
}

#ifdef MDKR_LAN_PARTY_TESTING
std::unique_ptr<MdkrPartyTransport> mdkr_create_lan_party_transport_for_test(
    MdkrLanPartyTransportConfig config) {
    return std::make_unique<LanPartyTransport>(std::move(config), /*useServer=*/false);
}

void mdkr_lan_party_transport_attach_test_controller(
    MdkrPartyTransport &transport,
    std::shared_ptr<MdkrLanPartyControllerSocket> socket) {
    static_cast<LanPartyTransport &>(transport).attachTestController(std::move(socket));
}
#endif
