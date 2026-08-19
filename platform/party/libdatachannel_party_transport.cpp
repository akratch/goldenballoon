#include "libdatachannel_party_transport.h"
#include "mozilla_ca_bundle.h"
#include "party_event_queue.h"
#include "party_retry_policy.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
constexpr size_t kMaxControlBytes = 4096u;
constexpr unsigned kProtocol = 1u;

const std::array<const char *, 32> kLeft = {{
    "Amber", "Brave", "Bright", "Calm", "Coral", "Cosmic", "Daring",
    "Flying", "Gentle", "Golden", "Happy", "Icy", "Jolly", "Lucky",
    "Mighty", "Neon", "Nimble", "Orange", "Rapid", "Royal", "Silver",
    "Solar", "Sunny", "Swift", "Teal", "Tiny", "Turbo", "Velvet",
    "Violet", "Warm", "Wild", "Zippy",
}};
const std::array<const char *, 32> kRight = {{
    "Balloon", "Comet", "Drum", "Falcon", "Fox", "Glider", "Kite", "Lion",
    "Moon", "Otter", "Panda", "Parrot", "Pebble", "Pilot", "Rocket", "Sail",
    "Sparrow", "Star", "Tiger", "Toucan", "Turtle", "Whale", "Wing", "Cloud",
    "Dolphin", "Lantern", "Meteor", "Penguin", "Planet", "Raven", "Sunrise",
    "Thunder",
}};

/*
 * Signaling URL construction, shared by the create and reconnect sockets and
 * pinned by tests/test_native_party_sas.cpp through
 * mdkr_party_signaling_url_for_test. An https origin becomes wss and the
 * loopback test origin (gated in initialize() below) becomes plain ws. The
 * previous in-place rewrite here replaced the first five characters with
 * "wss:", which turned every https origin into "wss:://host/..." — a URL
 * libdatachannel's RFC 3986 parser rejects outright, so the production
 * socket could never even begin its handshake. Splitting on the exact
 * scheme keeps both forms honest.
 */
std::string signalingUrl(const std::string &origin, const std::string &path) {
    std::string url = origin;
    if (url.rfind("https://", 0u) == 0u) {
        url.replace(0u, 8u, "wss://");
    } else {
        url.replace(0u, 7u, "ws://");
    }
    return url + path;
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

uint64_t wallExpiryToSteady(uint64_t expiresAtMs) {
    const uint64_t wall = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t now = steadyNowMs();
    return expiresAtMs > wall ? now + (expiresAtMs - wall) : now;
}

bool safeString(const Json &object, const char *key, std::string &output,
                size_t maximum, bool required = true) {
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (!found->is_string()) return false;
    output = found->get<std::string>();
    return output.size() <= maximum;
}

bool uintValue(const Json &object, const char *key, uint64_t &output,
               uint64_t maximum = std::numeric_limits<uint64_t>::max()) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) return false;
    output = found->get<uint64_t>();
    return output <= maximum;
}

/* I3: bound for the worker's typed host_command_result error code. The
 * longest real code today (services/party/src/types.ts) is well under this;
 * anything larger is not a code the host could ever map to copy anyway. */
constexpr size_t kMaxErrorCodeBytes = 64u;

/*
 * host_command_result{ok:false} -> CommandRejected. Returns false when the
 * message is not a failed host_command_result at all (successes never send
 * a result event; the room_state transition is the acknowledgement). The
 * worker's `error` field is a typed code (party-room.ts commandError), not
 * prose: it rides errorCode VERBATIM for the host's per-code copy table. A
 * missing, non-string, or oversized code degrades to the empty "unknown"
 * code rather than rejecting the event -- an unrecognized future code must
 * never hide the failure itself -- and the generic prose stays on the
 * event as exactly that fallback.
 */
bool commandRejectionFromSignal(const Json &value,
                                MdkrPartyTransportEvent &event) {
    if (value.value("type", std::string{}) != "host_command_result" ||
        value.value("ok", true) != false) {
        return false;
    }
    event.type = MdkrPartyTransportEventType::CommandRejected;
    event.message = "That controller action did not complete. Try again.";
    std::string code;
    if (safeString(value, "error", code, kMaxErrorCodeBytes,
                   /*required=*/false)) {
        event.errorCode = std::move(code);
    }
    return true;
}

std::string base64Url(const uint8_t *bytes, size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((length * 4u + 2u) / 3u);
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    for (size_t index = 0u; index < length; index++) {
        accumulator = (accumulator << 8u) | bytes[index];
        bits += 8u;
        while (bits >= 6u) {
            bits -= 6u;
            result.push_back(kAlphabet[(accumulator >> bits) & 63u]);
        }
    }
    if (bits != 0u) result.push_back(kAlphabet[(accumulator << (6u - bits)) & 63u]);
    return result;
}

bool decodePublicKey(const std::string &value, std::array<uint8_t, 65> &bytes) {
    if (value.size() != 87u) return false;
    std::array<int8_t, 256> decode{};
    decode.fill(-1);
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (size_t index = 0u; index < alphabet.size(); index++) {
        decode[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int8_t>(index);
    }
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    size_t output = 0u;
    for (unsigned char byte : value) {
        const int8_t digit = decode[byte];
        if (digit < 0) return false;
        accumulator = (accumulator << 6u) | static_cast<uint32_t>(digit);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (output >= bytes.size()) return false;
            bytes[output++] = static_cast<uint8_t>((accumulator >> bits) & 0xffu);
        }
    }
    return output == bytes.size() && bits == 2u &&
        (accumulator & 3u) == 0u && bytes[0] == 4u;
}

class PartyIdentity {
public:
    PartyIdentity() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_ecp_group_init(&group_);
        mbedtls_mpi_init(&privateKey_);
        mbedtls_ecp_point_init(&publicKey_);
    }

    ~PartyIdentity() {
        mbedtls_ecp_point_free(&publicKey_);
        mbedtls_mpi_free(&privateKey_);
        mbedtls_ecp_group_free(&group_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
    }

    PartyIdentity(const PartyIdentity &) = delete;
    PartyIdentity &operator=(const PartyIdentity &) = delete;

    bool generate() {
        if (!initializeCurve() ||
            mbedtls_ecp_gen_keypair(
                &group_, &privateKey_, &publicKey_, mbedtls_ctr_drbg_random,
                &random_) != 0) {
            return false;
        }
        return encodePublic();
    }

    bool loadPrivateForTest(const uint8_t scalar[32]) {
        if (scalar == nullptr || !initializeCurve() ||
            mbedtls_mpi_read_binary(&privateKey_, scalar, 32u) != 0 ||
            mbedtls_ecp_check_privkey(&group_, &privateKey_) != 0 ||
            mbedtls_ecp_mul(&group_, &publicKey_, &privateKey_,
                            &group_.G,
                            mbedtls_ctr_drbg_random, &random_) != 0) {
            return false;
        }
        return encodePublic();
    }

private:
    bool initializeCurve() {
        static constexpr char kPersonalization[] = "mdkr-native-party-sas-v1";
        return mbedtls_ctr_drbg_seed(
                   &random_, mbedtls_entropy_func, &entropy_,
                   reinterpret_cast<const unsigned char *>(kPersonalization),
                   sizeof(kPersonalization) - 1u) == 0 &&
            mbedtls_ecp_group_load(&group_, MBEDTLS_ECP_DP_SECP256R1) == 0;
    }

    bool encodePublic() {
        std::array<uint8_t, 65> raw{};
        size_t written = 0u;
        if (mbedtls_ecp_point_write_binary(
                &group_, &publicKey_, MBEDTLS_ECP_PF_UNCOMPRESSED,
                &written, raw.data(), raw.size()) != 0 ||
            written != raw.size()) {
            return false;
        }
        encodedPublicKey_ = base64Url(raw.data(), raw.size());
        return encodedPublicKey_.size() == 87u;
    }

public:

    const std::string &publicKey() const { return encodedPublicKey_; }

    bool phrase(const std::string &peerEncoded, const std::string &roomId,
                std::string &result) {
        std::array<uint8_t, 65> peerBytes{};
        if (!decodePublicKey(peerEncoded, peerBytes)) return false;
        mbedtls_ecp_point peer;
        mbedtls_ecp_point shared;
        mbedtls_ecp_point_init(&peer);
        mbedtls_ecp_point_init(&shared);
        const bool ok = mbedtls_ecp_point_read_binary(
                            &group_, &peer, peerBytes.data(), peerBytes.size()) == 0 &&
            mbedtls_ecp_check_pubkey(&group_, &peer) == 0 &&
            mbedtls_ecp_mul(&group_, &shared, &privateKey_, &peer,
                            mbedtls_ctr_drbg_random, &random_) == 0;
        std::array<uint8_t, 32> secret{};
        const bool wrote = ok && mbedtls_mpi_write_binary(
            &shared.MBEDTLS_PRIVATE(X), secret.data(), secret.size()) == 0;
        mbedtls_ecp_point_free(&shared);
        mbedtls_ecp_point_free(&peer);
        if (!wrote) return false;

        const std::string context = std::string("golden-balloon-party-sas-v1") +
            '\0' + roomId + '\0' + encodedPublicKey_ + '\0' + peerEncoded;
        std::array<uint8_t, 32> digest{};
        mbedtls_sha256_context hash;
        mbedtls_sha256_init(&hash);
        const bool hashed = mbedtls_sha256_starts(&hash, 0) == 0 &&
            mbedtls_sha256_update(&hash, secret.data(), secret.size()) == 0 &&
            mbedtls_sha256_update(&hash,
                reinterpret_cast<const unsigned char *>(context.data()),
                context.size()) == 0 &&
            mbedtls_sha256_finish(&hash, digest.data()) == 0;
        mbedtls_sha256_free(&hash);
        mbedtls_platform_zeroize(secret.data(), secret.size());
        if (!hashed) {
            return false;
        }
        const uint32_t value = (static_cast<uint32_t>(digest[0]) << 12u) |
            (static_cast<uint32_t>(digest[1]) << 4u) |
            (static_cast<uint32_t>(digest[2]) >> 4u);
        result = std::string(kLeft[(value >> 15u) & 31u]) + "-" +
            kRight[(value >> 10u) & 31u] + " " +
            kLeft[(value >> 5u) & 31u] + "-" + kRight[value & 31u];
        return true;
    }

private:
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context random_;
    mbedtls_ecp_group group_;
    mbedtls_mpi privateKey_;
    mbedtls_ecp_point publicKey_;
    std::string encodedPublicKey_;
};

struct Peer {
    std::string id;
    unsigned seat = 0u;
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
    uint32_t peerGeneration = 0u;
    bool failed = false;
    bool authenticated = false;
    uint32_t pingNonce = 0u;
    Clock::time_point nextPingAt{};
    Clock::time_point pingOutstandingAt{};
    /* C3 signaling retry (party_retry_policy.h): steadyNowMs() the current
     * offer was last (re)sent, 0 meaning none sent yet; how many distinct
     * offers (not resends) have been sent so far; and whether this peer has
     * already been reported connect_timeout so tick() stops re-evaluating
     * and re-emitting it every pass. */
    uint64_t offerSentMs = 0u;
    unsigned offerAttempts = 0u;
    bool gaveUp = false;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> state;
    std::shared_ptr<rtc::DataChannel> control;
};

class TransportState final : public std::enable_shared_from_this<TransportState> {
public:
    bool initialize(const std::string &serviceOrigin) {
        if (!identity_.generate()) return false;
        origin_ = serviceOrigin;
        while (!origin_.empty() && origin_.back() == '/') origin_.pop_back();
        /* The host has already refused anything that is not HTTPS or the
         * token-gated loopback test origin; re-checking here keeps this
         * transport fail-closed even if it is ever driven directly. */
        if (origin_.rfind("https://", 0u) != 0u &&
            !mdkr_party_loopback_test_url_allowed(origin_)) {
            return false;
        }
        return connect(true);
    }

    bool command(const Json &message) {
        std::shared_ptr<rtc::WebSocket> socket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_ || !socket_ || !socket_->isOpen()) return false;
            socket = socket_;
        }
        const std::string encoded = message.dump();
        if (encoded.size() > kMaxControlBytes) return false;
        try { return socket->send(encoded); }
        catch (...) { return false; }
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
            /* Log a burst only when its size changes, not once per drained
             * event: a sustained flood must not itself become a stderr
             * flood. */
            const uint64_t dropped = queue_.droppedPadPackets();
            if (dropped != loggedDroppedPadPackets_) {
                std::fprintf(stderr, "[PARTY-QUEUE] dropped=%llu\n",
                    static_cast<unsigned long long>(dropped));
                loggedDroppedPadPackets_ = dropped;
            }
        }
        if (drainedIndex_ >= drained_.size()) return false;
        event = std::move(drained_[drainedIndex_++]);
        return true;
    }

    void shutdown() {
        std::shared_ptr<rtc::WebSocket> socket;
        std::map<std::string, std::shared_ptr<Peer>> peers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return;
            shuttingDown_ = true;
            generation_++;
            socket = std::move(socket_);
            peers.swap(peers_);
            signaled_.clear();
        }
        if (socket) socket->close();
        for (auto &entry : peers) {
            if (entry.second->connection) entry.second->connection->close();
        }
    }

private:
    void enqueue(MdkrPartyTransportEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) return;
        queue_.push(std::move(event));
    }

    bool connect(bool create) {
        rtc::WebSocket::Configuration configuration;
        configuration.disableTlsVerification = false;
        configuration.caCertificatePemFile = std::string(
            reinterpret_cast<const char *>(kMdkrMozillaCaBundle),
            static_cast<size_t>(kMdkrMozillaCaBundleLength));
        configuration.connectionTimeout = std::chrono::seconds(10);
        configuration.pingInterval = std::chrono::seconds(15);
        configuration.maxOutstandingPings = 2;
        configuration.maxMessageSize = kMaxSignalBytes;
        configuration.protocols = {"gb-native-host-v1", "gb-control-v1",
            create ? "gb-key." + identity_.publicKey() : "gb-host." + credential_};
        auto socket = std::make_shared<rtc::WebSocket>(configuration);
        uint64_t generation = 0u;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return false;
            generation = ++generation_;
            socket_ = socket;
            creating_ = create;
        }
        const std::weak_ptr<TransportState> weak = shared_from_this();
        socket->onOpen([weak, generation]() {
            if (auto state = weak.lock()) state->socketOpened(generation);
        });
        socket->onError([weak, generation](const std::string &reason) {
            if (auto state = weak.lock()) state->socketError(generation, reason);
        });
        socket->onClosed([weak, generation]() {
            if (auto state = weak.lock()) state->socketClosed(generation);
        });
        socket->onMessage([weak, generation](rtc::message_variant message) {
            if (auto state = weak.lock()) state->socketMessage(generation, message);
        });
        const std::string url = signalingUrl(
            origin_, create ? "/api/party/native-create"
                            : "/api/party/" + roomId_ + "/connect");
        try {
            socket->open(url);
            return true;
        } catch (...) {
            socketError(generation, "secure_socket_open_failed");
            return false;
        }
    }

    void socketOpened(uint64_t generation) {
        std::vector<std::shared_ptr<Peer>> pendingOffers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            reconnectAttempt_ = 0u;
            reconnectAt_ = Clock::time_point{};
            /* C3: the socket just (re)opened. Any still-unauthenticated
             * peer with an outstanding offer may have had that offer
             * dropped by the Worker while the socket was down -- resend it
             * now instead of waiting out the 20 s deadline. socketOpen=true
             * is edge-triggered by construction here: this runs once per
             * onOpen callback, never once per tick. */
            for (const auto &entry : peers_) {
                const std::shared_ptr<Peer> &peer = entry.second;
                if (peer->failed || peer->gaveUp) continue;
                const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
                    steadyNowMs(), peer->offerSentMs, peer->offerAttempts,
                    peer->authenticated, /*socketOpen=*/true);
                if (decision.resendOffer) pendingOffers.push_back(peer);
            }
        }
        for (const auto &peer : pendingOffers) resendOffer(peer);
    }

    void socketError(uint64_t generation, const std::string &) {
        bool fatal = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            fatal = creating_ && credential_.empty();
        }
        if (fatal) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Could not create a secure phone controller room.";
            enqueue(std::move(event));
        }
    }

    void socketClosed(uint64_t generation) {
        bool recover = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
            socket_.reset();
            recover = !credential_.empty();
            if (recover) {
                reconnectAttempt_ = std::min(reconnectAttempt_ + 1u, 6u);
                const unsigned delay = std::min(8000u,
                    300u * (1u << std::min(reconnectAttempt_ - 1u, 5u)));
                reconnectAt_ = Clock::now() + std::chrono::milliseconds(delay);
            }
        }
        MdkrPartyTransportEvent event;
        event.type = recover ? MdkrPartyTransportEventType::Recovering
                             : MdkrPartyTransportEventType::Error;
        event.message = recover ? "Controller room reconnecting."
                                : "Phone controller room closed before it was ready.";
        enqueue(std::move(event));
    }

    void tick() {
        bool reconnect = false;
        std::vector<std::shared_ptr<Peer>> ping;
        std::vector<std::shared_ptr<Peer>> expired;
        std::vector<std::shared_ptr<Peer>> timedOut;
        std::vector<std::pair<MdkrNativePartyController, unsigned>> recreations;
        const Clock::time_point now = Clock::now();
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reconnect = !shuttingDown_ && !socket_ && !credential_.empty() &&
                reconnectAt_ != Clock::time_point{} && now >= reconnectAt_;
            if (reconnect) reconnectAt_ = Clock::time_point{};
            for (const auto &entry : peers_) {
                const std::shared_ptr<Peer> &peer = entry.second;
                if (peer->failed) continue;
                if (!peer->authenticated) {
                    /* C3: an unanswered offer never makes the PeerConnection
                     * reach Failed (no ICE start without a remote
                     * description), so this is the only place a silently
                     * stranded peer is ever noticed. gaveUp latches once the
                     * host has been told, so a given-up peer is never
                     * re-evaluated or re-reported every tick. */
                    if (!peer->gaveUp) {
                        const MdkrPartyRetryDecision decision = mdkr_party_retry_decide(
                            nowMs, peer->offerSentMs, peer->offerAttempts,
                            /*authenticated=*/false, /*socketOpen=*/false);
                        if (decision.giveUp) {
                            peer->gaveUp = true;
                            timedOut.push_back(peer);
                        } else if (decision.recreatePeer) {
                            const auto known = controllers_.find(peer->id);
                            if (known != controllers_.end()) {
                                recreations.emplace_back(known->second, peer->offerAttempts);
                            }
                        }
                    }
                    continue;
                }
                if (!peer->control) continue;
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
        if (reconnect) (void)connect(false);
        for (const auto &peer : timedOut) {
            /* Fail-closed but scoped: this is an explicit per-controller
             * error, not a room-wide one -- other peers and the room itself
             * are untouched. Reused CommandRejected shape (reason:
             * connect_timeout) rather than a new event type, since the host
             * already renders CommandRejected.message without needing to
             * know the controller it came from. */
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::CommandRejected;
            event.controllerId = peer->id;
            event.message = "This phone could not connect. Remove it and pair again.";
            enqueue(std::move(event));
        }
        for (const auto &recreation : recreations) {
            createPeer(recreation.first, /*forceRecreate=*/true, recreation.second);
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

    void socketMessage(uint64_t generation, const rtc::message_variant &message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || shuttingDown_) return;
        }
        if (!std::holds_alternative<std::string>(message)) return;
        const std::string &text = std::get<std::string>(message);
        if (text.size() > kMaxSignalBytes) return;
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            const std::string type = value.value("type", std::string{});
            if (type == "native_bootstrap") handleBootstrap(value);
            else if (type == "room_state") handleRoomState(value);
            else if (type == "controller_hello") handleHello(value);
            else if (type == "webrtc_answer") handleAnswer(value);
            else if (type == "webrtc_ice") handleIce(value);
            else if (type == "host_command_result") {
                MdkrPartyTransportEvent event;
                if (commandRejectionFromSignal(value, event)) {
                    enqueue(std::move(event));
                }
            }
        } catch (...) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller service sent an invalid room update.";
            enqueue(std::move(event));
        }
    }

    void handleBootstrap(const Json &value) {
        std::string room;
        std::string credential;
        std::string url;
        std::string code;
        uint64_t generation = 0u;
        uint64_t expiresIn = 0u;
        if (!safeString(value, "roomId", room, 22u) || room.size() != 22u ||
            !safeString(value, "hostCredential", credential, 43u) ||
            credential.size() != 43u || !safeString(value, "controllerUrl", url, 2048u) ||
            (url.rfind("https://", 0u) != 0u &&
             !mdkr_party_loopback_test_url_allowed(url)) ||
            !safeString(value, "fallbackCode", code, 6u) ||
            code.size() != 6u || !uintValue(value, "inviteGeneration", generation,
                std::numeric_limits<unsigned>::max()) || generation == 0u ||
            !uintValue(value, "inviteExpiresInMs", expiresIn, 300000u) || expiresIn == 0u) {
            MdkrPartyTransportEvent event;
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller room bootstrap was invalid.";
            enqueue(std::move(event));
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!credential_.empty()) return;
        roomId_ = std::move(room);
        credential_ = std::move(credential);
        inviteUrl_ = std::move(url);
        fallbackCode_ = std::move(code);
        inviteGeneration_ = static_cast<unsigned>(generation);
        inviteExpiresAtMs_ = steadyNowMs() + expiresIn;
    }

    bool parseRoom(const Json &value, MdkrPartyTransportRoomState &room) {
        uint64_t transition = 0u;
        uint64_t generation = 0u;
        uint64_t wallExpiry = 0u;
        if (!uintValue(value, "transitionId", transition) || transition == 0u ||
            !uintValue(value, "inviteGeneration", generation,
                std::numeric_limits<unsigned>::max()) || generation == 0u ||
            !uintValue(value, "inviteExpiresAt", wallExpiry) ||
            !value.contains("controllers") || !value["controllers"].is_array() ||
            value["controllers"].size() > 8u) return false;
        room.transitionId = transition;
        room.inviteGeneration = static_cast<unsigned>(generation);
        /* I1: kept in this function's own steady-clock domain
         * (wallExpiryToSteady/steadyNowMs) throughout parsing -- only
         * translated to a cross-domain-safe *relative* value
         * (room.inviteExpiresInMs) once, right before returning, so the host
         * can anchor it in its own clock instead of this transport's. */
        uint64_t expiresAtSteadyMs = wallExpiryToSteady(wallExpiry);
        room.inviteActive = value.value("phase", std::string{}) == "open" &&
            expiresAtSteadyMs > steadyNowMs();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (room.inviteGeneration == inviteGeneration_) {
                room.controllerUrl = inviteUrl_;
                room.fallbackCode = fallbackCode_;
                expiresAtSteadyMs = inviteExpiresAtMs_;
            }
        }
        if (safeString(value, "controllerUrl", room.controllerUrl, 2048u, false) &&
            safeString(value, "fallbackCode", room.fallbackCode, 6u, false)) {
            if (value.contains("controllerUrl") && value.contains("fallbackCode")) {
                std::lock_guard<std::mutex> lock(mutex_);
                inviteUrl_ = room.controllerUrl;
                fallbackCode_ = room.fallbackCode;
                inviteGeneration_ = room.inviteGeneration;
                inviteExpiresAtMs_ = expiresAtSteadyMs;
            }
        } else return false;
        for (const Json &item : value["controllers"]) {
            if (!item.is_object()) return false;
            MdkrNativePartyController controller;
            if (!safeString(item, "controllerId", controller.id, 64u) ||
                controller.id.empty() || !safeString(item, "name", controller.name, 48u) ||
                !safeString(item, "controllerPublicKey", controller.publicKey, 87u) ||
                controller.publicKey.size() != 87u) return false;
            const std::string phase = item.value("phase", std::string{});
            if (phase == "pending") controller.phase = MdkrNativePartyControllerPhase::Pending;
            else if (phase == "approved") controller.phase = MdkrNativePartyControllerPhase::Approved;
            else if (phase == "leased") controller.phase = MdkrNativePartyControllerPhase::Leased;
            else if (phase == "connected") controller.phase = MdkrNativePartyControllerPhase::Connected;
            else return false;
            uint64_t seat = 0u;
            /* I5: a pending controller with no seat assigned yet may arrive
             * with the "seat" key absent entirely, not merely null. nlohmann's
             * const operator[] on a missing key is undefined behaviour under
             * NDEBUG (an end-iterator dereference, not a catchable
             * exception) -- contains() first, same as every other field in
             * this function. */
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
            std::string phrase;
            std::string currentRoom;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                currentRoom = roomId_;
            }
            if (!identity_.phrase(controller.publicKey, currentRoom, phrase)) return false;
            controller.pairingPhrase = std::move(phrase);
            room.controllers.push_back(std::move(controller));
        }
        const uint64_t nowSteady = steadyNowMs();
        room.inviteExpiresInMs =
            expiresAtSteadyMs > nowSteady ? expiresAtSteadyMs - nowSteady : 0u;
        return true;
    }

    void handleRoomState(const Json &value) {
        MdkrPartyTransportEvent event;
        event.type = MdkrPartyTransportEventType::RoomState;
        if (!parseRoom(value, event.room)) {
            event.type = MdkrPartyTransportEventType::Error;
            event.message = "Controller service sent an invalid signed room update.";
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
        bool foundController = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signaled_.insert(id);
            const auto found = controllers_.find(id);
            if (found != controllers_.end()) {
                controller = found->second;
                foundController = true;
            }
        }
        if (foundController && controller.phase != MdkrNativePartyControllerPhase::Pending) {
            createPeer(controller);
        }
    }

    /* forceRecreate/carriedOfferAttempts serve the C3 retry ladder only:
     * tick() sets forceRecreate when mdkr_party_retry_decide() says an
     * unauthenticated peer's offer has gone unanswered past the deadline,
     * and carries its offerAttempts forward so a peer does not get an
     * unbounded number of attempts just because each one recreates a brand
     * new Peer object. Every other call site (handleRoomState, handleHello,
     * peerDisconnected's existing reconnect-after-drop path) leaves both at
     * their defaults, exactly as before this policy existed. */
    void createPeer(const MdkrNativePartyController &controller,
                    bool forceRecreate = false, unsigned carriedOfferAttempts = 0u) {
        uint32_t peerGeneration = 0u;
        std::shared_ptr<rtc::PeerConnection> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = peers_.find(controller.id);
            if (found != peers_.end() && !found->second->failed && !forceRecreate) return;
            if (found != peers_.end()) {
                /* A retry-driven recreation targets a peer that never
                 * reached Failed -- that is the defect: an unanswered offer
                 * leaves the PeerConnection sitting healthy forever, unlike
                 * the already-failed path this early-return also serves,
                 * whose connection is already closed at the RTC layer by
                 * the time onStateChange got here. Close it explicitly
                 * before the fresh one takes its place. */
                if (!found->second->failed) stale = found->second->connection;
                peers_.erase(found);
            }
            peerGeneration = ++peerGeneration_;
            if (peerGeneration == 0u) peerGeneration = ++peerGeneration_;
        }
        if (stale) stale->close();
        rtc::Configuration configuration;
        configuration.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
        configuration.maxMessageSize = kMaxSignalBytes;
        auto peer = std::make_shared<Peer>();
        peer->id = controller.id;
        peer->seat = controller.seat;
        peer->leaseGeneration = controller.leaseGeneration;
        peer->connectionSequence = controller.connectionSequence;
        peer->peerGeneration = peerGeneration;
        peer->offerAttempts = carriedOfferAttempts;
        try {
            peer->connection = std::make_shared<rtc::PeerConnection>(configuration);
        } catch (...) { return; }
        const std::weak_ptr<TransportState> weak = shared_from_this();
        const std::weak_ptr<Peer> weakPeer = peer;
        peer->connection->onLocalDescription([weak, weakPeer](rtc::Description description) {
            if (auto state = weak.lock()) {
                if (auto current = weakPeer.lock()) {
                    /* This is the offer going out for real (the first one,
                     * or a fresh one from a retry-driven recreation) -- a
                     * new attempt, so mark it as one. Resends of this same
                     * description on socket reopen go through resendOffer()
                     * instead and do not call onLocalDescription again. */
                    state->markOfferSent(current, /*freshAttempt=*/true);
                    state->sendPeerSignal(current,
                        Json{{"type", "webrtc_offer"}, {"to", current->id},
                            {"peerGeneration", current->peerGeneration},
                            {"sdp", {{"type", description.typeString()},
                                {"sdp", std::string(description)}}}});
                }
            }
        });
        peer->connection->onLocalCandidate([weak, weakPeer](rtc::Candidate candidate) {
            if (auto state = weak.lock()) {
                if (auto current = weakPeer.lock()) state->sendPeerSignal(current,
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
                if (auto owner = weak.lock()) {
                    if (auto current = weakPeer.lock()) owner->peerDisconnected(current,
                        state == rtc::PeerConnection::State::Failed);
                }
            }
        });
        /* The peer must be registered BEFORE the first createDataChannel:
         * creating the channel is what makes libdatachannel set the local
         * description, and onLocalDescription fires as soon as it does.
         * markOfferSent and sendPeerSignal both look this peer up in peers_
         * and silently return when it is absent, so registering only after
         * the channels exist (as this function originally did) raced the
         * callback -- the first offer was discarded unsent, offerSentMs
         * stayed 0, and the retry ladder (which reads offerSentMs == 0 as
         * "no offer to retry yet") could never rescue the stranded phone. */
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
                if (found != peers_.end() && found->second == peer) {
                    peers_.erase(found);
                }
            }
            peer->connection->close();
            return;
        }
        peer->state->onMessage([weak, weakPeer](rtc::message_variant message) {
            if (auto owner = weak.lock()) {
                if (auto current = weakPeer.lock()) owner->stateMessage(current, message);
            }
        });
        peer->state->onClosed([weak, weakPeer]() {
            if (auto owner = weak.lock()) {
                if (auto current = weakPeer.lock()) owner->peerDisconnected(current, true);
            }
        });
        peer->control->onOpen([weak, weakPeer]() {
            if (auto owner = weak.lock()) {
                if (auto current = weakPeer.lock()) owner->controlOpened(current);
            }
        });
        peer->control->onMessage([weak, weakPeer](rtc::message_variant message) {
            if (auto owner = weak.lock()) {
                if (auto current = weakPeer.lock()) owner->controlMessage(current, message);
            }
        });
        peer->control->onClosed([weak, weakPeer]() {
            if (auto owner = weak.lock()) {
                if (auto current = weakPeer.lock()) owner->peerDisconnected(current, true);
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
        if (text.size() > kMaxControlBytes) return;
        Json value = Json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object()) return;
        try {
            if (value.value("type", std::string{}) == "controller_ready" &&
                value.value("protocol", 0u) == kProtocol &&
                value.value("controllerId", std::string{}) == peer->id &&
                value.value("connectionSequence", 0u) == peer->connectionSequence) {
                const bool haptics = value.contains("capabilities") &&
                    value["capabilities"].is_object() &&
                    value["capabilities"].value("vibration", false);
                MdkrPartyTransportEvent event;
                event.type = MdkrPartyTransportEventType::ControllerConnected;
                event.controllerId = peer->id;
                event.haptics = haptics;
                enqueue(std::move(event));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto found = peers_.find(peer->id);
                    if (found != peers_.end() && found->second == peer) {
                        peer->authenticated = true;
                        peer->pingOutstandingAt = Clock::time_point{};
                        peer->nextPingAt = Clock::now() + std::chrono::seconds(5);
                    }
                }
                peer->control->send(Json{{"type", "controller_ready_ack"}}.dump());
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
        catch (...) { peerDisconnected(peer, true); }
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

    /* C3 retry bookkeeping. freshAttempt=true is a genuinely new offer (the
     * first one, or one from a retry-driven recreation); false is a resend
     * of the same description after the socket reopened, which restarts
     * the 20 s deadline (it just went out again) but must not consume one
     * of the 3 attempts. offerSentMs/offerAttempts are set unconditionally,
     * even though command() inside sendPeerSignal may still fail if the
     * socket happens to be down right now -- that is exactly the case
     * resendOffer() exists to recover from once the socket comes back, so
     * the record must survive a send that never reached the wire. */
    void markOfferSent(const std::shared_ptr<Peer> &peer, bool freshAttempt) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = peers_.find(peer->id);
        if (found == peers_.end() || found->second != peer) return;
        peer->offerSentMs = steadyNowMs();
        if (freshAttempt) peer->offerAttempts++;
    }

    /* Resends the peer's existing local description verbatim -- no
     * createOffer(), no renegotiation, same peerGeneration -- because
     * C3 is a Worker-relay drop, not anything wrong with the offer. */
    void resendOffer(const std::shared_ptr<Peer> &peer) {
        std::optional<rtc::Description> description;
        try { description = peer->connection->localDescription(); }
        catch (...) { return; }
        if (!description) return;
        markOfferSent(peer, /*freshAttempt=*/false);
        sendPeerSignal(peer, Json{{"type", "webrtc_offer"}, {"to", peer->id},
            {"peerGeneration", peer->peerGeneration},
            {"sdp", {{"type", description->typeString()},
                {"sdp", std::string(*description)}}}});
    }

    std::mutex mutex_;
    MdkrPartyEventQueue queue_{kMaxQueuedEvents};
    std::vector<MdkrPartyTransportEvent> drained_;
    size_t drainedIndex_ = 0u;
    uint64_t loggedDroppedPadPackets_ = 0u;
    std::shared_ptr<rtc::WebSocket> socket_;
    std::map<std::string, std::shared_ptr<Peer>> peers_;
    std::map<std::string, MdkrNativePartyController> controllers_;
    std::set<std::string> signaled_;
    PartyIdentity identity_;
    std::string origin_;
    std::string roomId_;
    std::string credential_;
    std::string inviteUrl_;
    std::string fallbackCode_;
    unsigned inviteGeneration_ = 0u;
    uint64_t inviteExpiresAtMs_ = 0u;
    uint64_t generation_ = 0u;
    uint32_t peerGeneration_ = 0u;
    unsigned reconnectAttempt_ = 0u;
    Clock::time_point reconnectAt_{};
    bool creating_ = false;
    bool shuttingDown_ = false;
};

class LibDatachannelPartyTransport final : public MdkrPartyTransport {
public:
    bool available() const override { return true; }
    const char *unavailableReason() const override { return ""; }

    bool open(const std::string &serviceOrigin) override {
        if (state_) return false;
        state_ = std::make_shared<TransportState>();
        if (!state_->initialize(serviceOrigin)) {
            state_->shutdown();
            state_.reset();
            return false;
        }
        return true;
    }

    bool approve(const std::string &id, unsigned seat) override {
        return sendHostCommand("approve", id, Json{{"seat", seat}});
    }
    bool reject(const std::string &id) override {
        return sendHostCommand("reject", id);
    }
    bool remove(const std::string &id) override {
        return sendHostCommand("remove", id);
    }
    bool rotateInvite(unsigned generation) override {
        return state_ && state_->command(Json{{"type", "host_command"},
            {"action", "rotate"}, {"expectedInviteGeneration", generation}});
    }
    bool revokeInvite() override {
        return sendHostCommand("revoke", "");
    }
    bool closeRoom() override {
        return sendHostCommand("close", "");
    }
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

private:
    bool sendHostCommand(const char *action, const std::string &id,
                         Json extra = Json::object()) {
        if (!state_) return false;
        Json message = {{"type", "host_command"}, {"action", action}};
        if (!id.empty()) message["controllerId"] = id;
        message.update(extra);
        return state_->command(message);
    }

    std::shared_ptr<TransportState> state_;
};

}  // namespace

std::unique_ptr<MdkrPartyTransport> mdkr_create_native_party_transport() {
    return std::make_unique<LibDatachannelPartyTransport>();
}

std::string mdkr_party_signaling_url_for_test(
    const std::string &origin, const std::string &path) {
    return signalingUrl(origin, path);
}

bool mdkr_party_sas_phrase_for_test(
    const uint8_t privateScalar[32], const std::string &roomId,
    const std::string &controllerPublicKey, std::string &hostPublicKey,
    std::string &phrase) {
    PartyIdentity identity;
    if (!identity.loadPrivateForTest(privateScalar)) return false;
    hostPublicKey = identity.publicKey();
    return identity.phrase(controllerPublicKey, roomId, phrase);
}

bool mdkr_party_host_command_rejection_for_test(
    const std::string &text, MdkrPartyTransportEvent &event) {
    const Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return false;
    /* Same guard the socket path's outer try gives the shared parser. */
    try { return commandRejectionFromSignal(value, event); }
    catch (...) { return false; }
}
