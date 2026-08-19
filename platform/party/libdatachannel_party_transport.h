/* Production native WSS/WebRTC transport factory. */
#ifndef MDKR_LIBDATACHANNEL_PARTY_TRANSPORT_H
#define MDKR_LIBDATACHANNEL_PARTY_TRANSPORT_H

#include "native_party_host.h"

#include <memory>

std::unique_ptr<MdkrPartyTransport> mdkr_create_native_party_transport();

/* Deterministic interoperability seam; production identities remain random. */
bool mdkr_party_sas_phrase_for_test(
    const uint8_t privateScalar[32], const std::string &roomId,
    const std::string &controllerPublicKey, std::string &hostPublicKey,
    std::string &phrase);

/* Signaling-URL seam: pins the https->wss and loopback http->ws scheme
 * rewrites the sockets are actually opened with. */
std::string mdkr_party_signaling_url_for_test(
    const std::string &origin, const std::string &path);

/* host_command_result parse seam: feeds one raw signaling text through the
 * same parser the socket path uses and copies out the CommandRejected event
 * it would enqueue. False when the text is not a failed host_command_result
 * (successes never produce a result event). */
bool mdkr_party_host_command_rejection_for_test(
    const std::string &text, MdkrPartyTransportEvent &event);

/* controller_ready parse seam: feeds one raw control-channel text through
 * the same classifier the live data channel uses, addressed as if the peer
 * were `controllerId`/`connectionSequence`. Copies out the event the channel
 * would enqueue -- ControllerConnected for this build's protocol,
 * ControllerProtocolMismatch for any other declared version -- and returns
 * false when the text produces no event at all (not controller_ready, or
 * addressed to some other peer). */
bool mdkr_party_controller_ready_event_for_test(
    const std::string &text, const std::string &controllerId,
    uint32_t connectionSequence, MdkrPartyTransportEvent &event);

#endif /* MDKR_LIBDATACHANNEL_PARTY_TRANSPORT_H */
