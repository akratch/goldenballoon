/* Production native WSS/WebRTC transport factory. */
#ifndef MDKR_LIBDATACHANNEL_PARTY_TRANSPORT_H
#define MDKR_LIBDATACHANNEL_PARTY_TRANSPORT_H

#include "native_party_host.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

std::unique_ptr<MdkrPartyTransport> mdkr_create_native_party_transport();

/* M4: quitting must tell the phones goodbye without hanging the app.
 * closeRoom() sends the worker the `close` command (relayed to every
 * controller as host_closed, services/party/src/party-room.ts) and then
 * waits -- bounded by this deadline -- for the socket write to actually
 * flush before the caller tears the socket down. Before this, the frame
 * was only queued when shutdown() hard-closed the socket, so the phones
 * often met a silent drop they misread as a network fault. */
inline constexpr uint64_t kMdkrPartyCloseFlushDeadlineMs = 250u;

/* Close-flush wait seam: runs the transport's exact bounded wait loop
 * against an injectable clock, buffered-byte probe, and sleep, so tests
 * can prove the deadline cap (and that a drained socket costs nothing)
 * without a live socket. Returns the total milliseconds of sleep the loop
 * requested. */
uint64_t mdkr_party_close_flush_wait_for_test(
    const std::function<uint64_t()> &nowMs,
    const std::function<size_t()> &bufferedBytes,
    const std::function<void(uint64_t)> &sleepMs);

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
