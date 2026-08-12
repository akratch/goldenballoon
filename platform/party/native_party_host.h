/* Launcher-owned native Phone Party model and replaceable transport seam. */
#ifndef MDKR_NATIVE_PARTY_HOST_H
#define MDKR_NATIVE_PARTY_HOST_H

#include <array>
#include <cstdint>
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
};

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
    CommandRejected,
    Recovering,
    Error,
    Closed,
};

struct MdkrPartyTransportRoomState {
    uint64_t transitionId = 0u;
    unsigned inviteGeneration = 0u;
    uint64_t inviteExpiresAtMs = 0u;
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
