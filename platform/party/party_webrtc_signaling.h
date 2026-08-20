/*
 * Shared native Phone Party WebRTC signaling primitives.
 *
 * These are the self-contained, socket-agnostic helpers the two native party
 * transports both need, byte-for-byte: the cloud transport
 * (libdatachannel_party_transport.cpp, which relays through the Cloudflare
 * Worker) and the LAN transport (lan_party_transport.cpp, which relays
 * through the in-process MdkrLanPartyRoom). Extracting them here keeps the
 * SAS derivation, the DTLS-fingerprint capture, the base64url codec and the
 * defensive JSON readers identical on both paths -- a divergence between the
 * two would be a security bug, not a style one.
 *
 * Only the PURE, per-transport-state-free helpers live here. The peer
 * lifecycle machinery (rtc::PeerConnection creation, data channels, the
 * room-state roster, the event queue) stays inside each transport because it
 * is inseparable from that transport's own signaling channel and teardown
 * discipline; see lan_party_transport.cpp's header comment for the
 * extraction-vs-duplication rationale.
 */
#ifndef MDKR_PARTY_WEBRTC_SIGNALING_H
#define MDKR_PARTY_WEBRTC_SIGNALING_H

#include "native_party_host.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace mdkr_party {

using Json = nlohmann::json;

/*
 * Channel protocol: the version tag inside the direct data-channel messages
 * (controller_ready/host_ready/ping/pong/rumble). This is a SEPARATE
 * namespace from the HTTP/redeem "pairing protocol" (room-model.ts, now 2 =
 * SAS v2). A channel mismatch sets the protocolMismatch latch and shows
 * kMdkrPartyProtocolMismatchCopy (native_party_host.h). SAS v2 changed no
 * channel message shape, so this stays 1.
 */
inline constexpr unsigned kChannelProtocol = 1u;

/* Bound for the worker's typed host_command_result error code. */
inline constexpr std::size_t kMaxErrorCodeBytes = 64u;

/* URL-safe base64 (no padding), the alphabet the browser and worker use. */
std::string base64Url(const uint8_t *bytes, std::size_t length);

/* Decode an 87-char base64url uncompressed P-256 public key into its 65
 * raw bytes; false on any malformed input (wrong length, bad char, not an
 * uncompressed point). */
bool decodePublicKey(const std::string &value, std::array<uint8_t, 65> &bytes);

/*
 * SAS v2 DTLS-fingerprint capture. Canonical form agreed byte-for-byte with
 * the controller page: the value after "a=fingerprint:" with single spaces,
 * algorithm token verbatim, hex uppercased ("sha-256 AB:CD:..."). Returns
 * the empty string -- read everywhere as "no phrase" -- for a description
 * with no fingerprint, a malformed one, or two that disagree.
 */
std::string canonicalSdpFingerprint(const std::string &sdp);

/* nlohmann helpers: a string field within a byte cap (absent is allowed when
 * !required), and an unsigned integer within a value cap. */
bool safeString(const Json &object, const char *key, std::string &output,
                std::size_t maximum, bool required = true);
bool uintValue(const Json &object, const char *key, uint64_t &output,
               uint64_t maximum = std::numeric_limits<uint64_t>::max());

/*
 * host_command_result{ok:false} -> CommandRejected. False when the message
 * is not a failed host_command_result (successes never send a result event).
 * The worker's typed `error` code, echoed `command` name and targeted
 * `controllerId` ride the event verbatim; each degrades to absent rather
 * than hiding the failure. May throw on malformed field types; callers guard.
 */
bool commandRejectionFromSignal(const Json &value,
                                MdkrPartyTransportEvent &event);

/*
 * controller_ready classification for a live control channel. Only a
 * controller_ready addressed to this exact peer (matching controllerId AND
 * connectionSequence) produces an event; a matching peer declaring any
 * channel version but kChannelProtocol becomes ControllerProtocolMismatch,
 * otherwise ControllerConnected (with the haptics capability). May throw on
 * malformed field types; callers guard.
 */
bool controllerReadyEventFromControl(const Json &value,
                                     const std::string &peerId,
                                     uint32_t connectionSequence,
                                     MdkrPartyTransportEvent &event);

/*
 * The host's SAS identity: a P-256 keypair plus the SAS v2 phrase
 * derivation, which commits to both DTLS fingerprints so a relay that
 * swapped either certificate moves the words on one screen. mbedtls is
 * confined to the implementation; this class exposes only what the
 * transports call.
 */
class Identity {
public:
    Identity();
    ~Identity();

    Identity(const Identity &) = delete;
    Identity &operator=(const Identity &) = delete;

    bool generate();
    bool loadPrivateForTest(const uint8_t scalar[32]);
    const std::string &publicKey() const;
    bool phrase(const std::string &peerEncoded, const std::string &roomId,
                const std::string &hostFingerprint,
                const std::string &controllerFingerprint, std::string &result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mdkr_party

#endif /* MDKR_PARTY_WEBRTC_SIGNALING_H */
