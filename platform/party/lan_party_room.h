/*
 * In-process local room for zero-internet Phone Party.
 *
 * This is the LAN equivalent of the Cloudflare Party room Durable Object
 * (services/party/src/party-room.ts + room-model.ts): it mints the invite
 * capability and fallback code, holds seat leases, and relays the pairing
 * protocol between two ends -- the phones (real WebSocket connections handed
 * over by MdkrLanPartyServer) and the native host (an in-process seam that
 * Task 3's LanPartyTransport drives). It speaks the EXACT wire protocol the
 * shipped controller page already talks, so the page needs no LAN-specific
 * message variant beyond its transport URL.
 *
 * SECURITY BOUNDARY -- anonymous until authenticated. The Task 1 server
 * validates the Host header but NOT Origin, so a cross-origin browser page
 * that knows this host's LAN ip:port can still complete the ws upgrade. The
 * upgraded socket therefore proves NOTHING. This room is the authentication
 * boundary: every freshly-attached controller socket is treated as
 * anonymous and hostile and does nothing -- it is not registered as a
 * controller, receives no room state, and cannot relay a single signal --
 * until it presents a valid capability or fallback code via a `redeem`
 * message. Anything else from an un-redeemed socket closes it (4001).
 *
 * THREADING. Controller socket callbacks (onMessage / onClosed) fire on the
 * server's connection threads; open()/deliverFromHost()/close()/invite()
 * are launcher-thread calls driven by the transport. All room state lives
 * under one mutex. Outbound work (socket sends, host-sink deliveries) is
 * collected under the lock and performed after releasing it, so no I/O runs
 * while the lock is held and a peer can never stall another delivery. The
 * host sink must follow the transport contract: copy into a bounded queue
 * and return, never call back into the launcher model synchronously.
 */
#ifndef MDKR_LAN_PARTY_ROOM_H
#define MDKR_LAN_PARTY_ROOM_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/* Mirrors of services/party/src/types.ts LIMITS and room-model timings.
 * Kept here so the local room and the worker cannot silently drift. */
inline constexpr uint64_t kMdkrLanPartyInviteTtlMs = 2u * 60u * 1000u;
inline constexpr uint64_t kMdkrLanPartyRoomTtlMs = 24u * 60u * 60u * 1000u;
inline constexpr unsigned kMdkrLanPartyMaxPending = 8u;
inline constexpr unsigned kMdkrLanPartyMaxSeats = 4u;
inline constexpr unsigned kMdkrLanPartyMaxNameCodePoints = 24u;
inline constexpr uint64_t kMdkrLanPartyMaxTransitions = 4096u;
inline constexpr size_t kMdkrLanPartyMaxSignalBytes = 64u * 1024u;

/* Code-redemption throttle. ONE shared bucket across ALL sources is correct
 * on a LAN: unlike the cloud code directory (which buckets per connecting
 * IP), a LAN has no per-IP identity to trust -- any device on the segment
 * can spoof any source address -- so the only meaningful defence against
 * brute-forcing the six-digit code is a single global budget. 12 attempts
 * per 10 minutes bounds a guesser to a negligible fraction of the million
 * codes before the invite's own 2-minute TTL rotates it out of reach. */
inline constexpr unsigned kMdkrLanPartyCodeAttemptsPerWindow = 12u;
inline constexpr uint64_t kMdkrLanPartyCodeWindowMs = 10u * 60u * 1000u;

/* Post-auth signaling rate limit, ported verbatim from the worker's
 * admitSignalMessage (services/party/src/party-room.ts): a fixed 10-second
 * window of at most 120 frames plus a hard 512-frame lifetime cap per socket.
 * A redeemed controller that breaches either bound is closed 4008
 * (rate_limited) -- the Task 1 server bounds a frame's SIZE, never its rate,
 * so this is the only thing standing between an authenticated phone and a
 * flood of the host sink. */
inline constexpr uint64_t kMdkrLanPartySignalWindowMs = 10u * 1000u;
inline constexpr unsigned kMdkrLanPartySignalWindowMessages = 120u;
inline constexpr unsigned kMdkrLanPartySignalLifetimeMessages = 512u;

/*
 * Abstract controller socket. The room depends only on this, never on the
 * concrete MdkrLanPartyWebSocket, so it is unit-testable with a fake and
 * carries no dependency on the server (or on libdatachannel). Task 3 adapts
 * a real MdkrLanPartyWebSocket to this interface. sendText()/close() must be
 * safe to call from any thread; onMessage()/onClosed() are registered once,
 * before the first frame, by attachController().
 */
class MdkrLanPartyControllerSocket {
public:
    virtual ~MdkrLanPartyControllerSocket() = default;
    virtual bool sendText(const std::string &payload) = 0;
    virtual void close(uint16_t code) = 0;
    virtual bool isOpen() const = 0;
    virtual void onMessage(std::function<void(const std::string &)> callback) = 0;
    virtual void onClosed(std::function<void()> callback) = 0;
};

struct MdkrLanPartyRoomConfig {
    /* Fill `length` bytes with cryptographically strong randomness. Default
     * (empty) uses the platform secure RNG. Tests inject a deterministic
     * source. */
    std::function<void(uint8_t *, size_t)> randomBytes;
    /* Monotonic milliseconds. Default (empty) uses steady_clock. Tests drive
     * invite TTL and the throttle window through a controllable clock. */
    std::function<uint64_t()> nowMs;
};

/* The invite the native host displays (QR + code). controllerUrl is the full
 * link when open() was given a controllerOrigin, otherwise controllerPath is
 * the "/controller/#<capability>" tail the caller prefixes. */
struct MdkrLanPartyInvite {
    std::string roomId;
    std::string capability;
    std::string controllerPath;
    std::string controllerUrl;
    std::string fallbackCode;
    std::string hostPublicKey;
    uint64_t inviteExpiresAtMs = 0u;
    unsigned inviteGeneration = 0u;
    bool active = false;
};

struct MdkrLanPartyRoomState; /* Internal; defined in lan_party_room.cpp. */

class MdkrLanPartyRoom {
public:
    /* Room -> host sink. One JSON message per call (room_state, relayed
     * controller_hello / webrtc_answer / webrtc_ice, host_command_result,
     * the rotate invite, host_closed). Fires on either thread. */
    using HostMessage = std::function<void(const std::string &json)>;

    explicit MdkrLanPartyRoom(MdkrLanPartyRoomConfig config = {});
    ~MdkrLanPartyRoom();

    MdkrLanPartyRoom(const MdkrLanPartyRoom &) = delete;
    MdkrLanPartyRoom &operator=(const MdkrLanPartyRoom &) = delete;

    /* Open the room with the native host's SAS public key (87-char base64url)
     * and the origin the phones reach it at (e.g. "http://192.168.1.5:49200",
     * empty if unknown). Mints the capability + fallback code and registers
     * the host sink. Returns the invite to display. Idempotent guard: a
     * second open() on a live room returns the current invite unchanged. */
    MdkrLanPartyInvite open(const std::string &hostPublicKey,
                            const std::string &controllerOrigin,
                            HostMessage onHostMessage);

    /* Register a freshly-upgraded, ANONYMOUS controller socket. The room
     * wires its callbacks; the socket stays anonymous until it redeems. */
    void attachController(std::shared_ptr<MdkrLanPartyControllerSocket> socket);

    /* A host-role JSON frame from the transport (host_command / webrtc_offer
     * / webrtc_ice), shaped exactly as a host WebSocket frame in the DO. */
    void deliverFromHost(const std::string &json);

    /* Host-initiated teardown: closes every controller socket with 4000
     * host_closed and notifies the host sink. Equivalent to a close command. */
    void close();

    /* A thread-safe snapshot of the current invite for redisplay. */
    MdkrLanPartyInvite invite() const;

private:
    std::shared_ptr<MdkrLanPartyRoomState> state_;
};

#endif /* MDKR_LAN_PARTY_ROOM_H */
