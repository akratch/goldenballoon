/* Launcher-owned native Phone Party model and replaceable transport seam. */
#ifndef MDKR_NATIVE_PARTY_HOST_H
#define MDKR_NATIVE_PARTY_HOST_H

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

enum class MdkrNativePartyPhase {
    Closed,
    Opening,
    Open,
    InviteRevoked,
    Recovering,
    Error,
};

enum class MdkrNativePartyControllerPhase {
    Pending,
    Approved,
    Leased,
    Connected,
};

struct MdkrNativePartyController {
    std::string id;
    std::string name;
    /* Base64url-encoded uncompressed P-256 key used for the verified SAS. */
    std::string publicKey;
    std::string pairingPhrase;
    MdkrNativePartyControllerPhase phase =
        MdkrNativePartyControllerPhase::Pending;
    unsigned seat = 0u;  // 1..4 when approved; zero while pending.
    uint32_t leaseGeneration = 0u;
    uint32_t connectionSequence = 0u;
    bool direct = false;
    bool haptics = false;
    bool commandPending = false;
    /* I2: the phone's controller page completed the WebRTC handshake but
     * spoke a different pairing-protocol version, so its input can never be
     * trusted. The seat keeps its lease (the room is not torn down) but must
     * show this honestly instead of an indistinguishable "Reconnecting".
     * Cleared by a genuine ControllerConnected -- the phone reloading into a
     * matching page version is the recovery path. */
    bool protocolMismatch = false;
    /* C1 self-heal: set when an ingress push failed (queue overflow revoked
     * the seat's custody) while otherwise healthy. service() clears it once
     * the seat has a fresh bind; lastRebindMs rate-limits repeated attempts
     * so a wedged phone cannot spin the rebind every tick. */
    bool needsRebind = false;
    uint64_t lastRebindMs = 0u;
};

/* I2: the one sentence every mismatch surface shows -- the room message
 * MdkrNativePartyHost::applyEvent sets and the seat row ui_phone_party.cpp
 * derives from protocolMismatch. Both screens were designed to give the
 * same remedy, so the copy lives once, here, where the flag it narrates is
 * declared; tests/test_native_party_host.cpp pins the sentence itself. */
inline constexpr char kMdkrPartyProtocolMismatchCopy[] =
    "This phone's controller page is a different version. "
    "Refresh the page on the phone.";

struct MdkrNativePartyView {
    MdkrNativePartyPhase phase = MdkrNativePartyPhase::Closed;
    std::string controllerUrl;
    std::string fallbackCode;
    std::string message;
    uint64_t inviteExpiresAtMs = 0u;
    uint64_t transitionId = 0u;
    unsigned inviteGeneration = 0u;
    std::vector<MdkrNativePartyController> controllers;
    bool inviteVisible = false;
    bool busy = false;
};

enum class MdkrPartyTransportEventType {
    RoomState,
    ControllerConnected,
    ControllerDisconnected,
    ControllerPacket,
    ControllerPhrase,
    ControllerProtocolMismatch,
    CommandRejected,
    Recovering,
    Error,
    Closed,
};

struct MdkrPartyTransportRoomState {
    uint64_t transitionId = 0u;
    unsigned inviteGeneration = 0u;
    /* Relative milliseconds remaining as of when the transport parsed this
     * room update, NOT an absolute instant. The transport and the host each
     * run their own clock (transport: std::chrono::steady_clock since boot;
     * host: SDL_GetTicks64 since SDL init) -- an absolute value here would
     * cross domains and be meaningless once compared against the host's own
     * nowMs. The host latches nowMs + inviteExpiresInMs in its own clock at
     * the event-application site instead. */
    uint64_t inviteExpiresInMs = 0u;
    std::string controllerUrl;
    std::string fallbackCode;
    bool inviteActive = false;
    std::vector<MdkrNativePartyController> controllers;
};

struct MdkrPartyTransportEvent {
    MdkrPartyTransportEventType type = MdkrPartyTransportEventType::Error;
    MdkrPartyTransportRoomState room;
    std::string controllerId;
    std::string message;
    /* CommandRejected only: the worker's typed host_command_result error
     * code, verbatim (services/party/src/party-room.ts commandError).
     * Empty means unknown; the host then keeps its generic copy. */
    std::string errorCode;
    /* ControllerProtocolMismatch only: the protocol version the phone's
     * controller_ready declared. Zero means it declared none at all. */
    unsigned theirProtocol = 0u;
    std::vector<uint8_t> packet;
    bool haptics = false;
};

/*
 * Implementations own HTTPS/WSS/WebRTC and their callback queue. Every method
 * below is called on the launcher thread. poll() copies one bounded event out;
 * no implementation callback may call the host model directly.
 */
class MdkrPartyTransport {
public:
    virtual ~MdkrPartyTransport() = default;
    virtual bool available() const = 0;
    virtual const char *unavailableReason() const = 0;
    virtual bool open(const std::string &serviceOrigin) = 0;
    virtual bool approve(const std::string &controllerId, unsigned seat) = 0;
    virtual bool reject(const std::string &controllerId) = 0;
    virtual bool remove(const std::string &controllerId) = 0;
    virtual bool rotateInvite(unsigned expectedGeneration) = 0;
    virtual bool revokeInvite() = 0;
    virtual bool closeRoom() = 0;
    virtual bool sendRumble(
        const std::string &controllerId, uint16_t strength) = 0;
    virtual bool poll(MdkrPartyTransportEvent &event) = 0;
    virtual void shutdown() = 0;
};

/*
 * Loopback test gate for the end-to-end lane (tests/check_party_native_e2e.py).
 * True only when BOTH hold: `url` is a plain-HTTP loopback URL — it starts
 * with `http://127.0.0.1` or `http://localhost` followed by nothing, `:` or
 * `/` (so `http://127.0.0.1.evil.example` never matches) — AND the process
 * carries MDKR_INTERNAL_TEST_TOKEN=mdkr64-party-e2e-v1. Without the token
 * every HTTP origin, loopback included, stays refused exactly as before, so
 * the shipped fail-closed HTTPS-only posture is unchanged. This mirrors the
 * Party service itself, which accepts plain HTTP solely for
 * standards-defined loopback development (services/party/src/security.ts).
 * Inline because the host model and the transport check it independently
 * and are linked in different combinations by different test binaries.
 */
inline bool mdkr_party_loopback_test_url_allowed(const std::string &url) {
    const char *token = std::getenv("MDKR_INTERNAL_TEST_TOKEN");
    if (token == nullptr ||
        std::strcmp(token, "mdkr64-party-e2e-v1") != 0) {
        return false;
    }
    for (const char *prefix : {"http://127.0.0.1", "http://localhost"}) {
        const size_t length = std::strlen(prefix);
        if (url.compare(0u, length, prefix) != 0) continue;
        /* Host boundary: the loopback name must end here, at a port, or at
         * a path — never continue into a longer hostname. */
        if (url.size() == length) return true;
        const char next = url[length];
        if (next != ':' && next != '/') continue;
        /* The rest of the authority must not smuggle a real host behind
         * userinfo: RFC 3986 reads "http://127.0.0.1:@evil.example/..." as
         * userinfo "127.0.0.1:" at host evil.example. Allow the one port
         * colon consumed above and refuse any '@' or further ':' before
         * the path begins. */
        for (size_t index = length + (next == ':' ? 1u : 0u);
             index < url.size(); index++) {
            const char byte = url[index];
            if (byte == '/') break;
            if (byte == '@' || byte == ':') return false;
        }
        return true;
    }
    return false;
}

class MdkrNativePartyHost {
public:
    explicit MdkrNativePartyHost(MdkrPartyTransport &transport);
    ~MdkrNativePartyHost();

    MdkrNativePartyHost(const MdkrNativePartyHost &) = delete;
    MdkrNativePartyHost &operator=(const MdkrNativePartyHost &) = delete;

    bool open(const std::string &serviceOrigin);
    bool approve(const std::string &controllerId, unsigned seat);
    bool reject(const std::string &controllerId);
    bool rotateInvite();
    bool dismissInvite();
    bool closeRoom();

    /* Drain bounded network events and newest engine rumble requests. */
    void service(uint64_t nowMs);
    const MdkrNativePartyView &view() const { return view_; }
    bool transportAvailable() const { return transport_.available(); }

private:
    MdkrNativePartyController *controller(const std::string &id);
    const MdkrNativePartyController *controller(const std::string &id) const;
    bool roomStateValid(const MdkrPartyTransportRoomState &room) const;
    void applyRoomState(const MdkrPartyTransportRoomState &room, uint64_t nowMs);
    void applyEvent(const MdkrPartyTransportEvent &event, uint64_t nowMs);
    void releaseController(const MdkrNativePartyController &controller);
    void releaseAll();
    void setError(const std::string &message);

    MdkrPartyTransport &transport_;
    MdkrNativePartyView view_;
    std::array<uint16_t, 4> lastRumble_{};
};

#endif /* MDKR_NATIVE_PARTY_HOST_H */
