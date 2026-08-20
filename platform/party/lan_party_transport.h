/*
 * LanPartyTransport: the MdkrPartyTransport for zero-internet local play.
 *
 * It is the keystone that lets native_party_host drive a full LAN party
 * through the SAME seam it uses for the cloud transport. Where the cloud
 * transport (libdatachannel_party_transport.cpp) relays signaling through the
 * Cloudflare Worker over a WebSocket, this one is its own signaling relay: it
 * owns an in-process MdkrLanPartyServer (Task 1, serves the controller page
 * and the /party-ws socket on one LAN port) and an MdkrLanPartyRoom (Task 2,
 * the authentication boundary + protocol relay), and stitches host and phones
 * together with the very same libdatachannel WebRTC peers the cloud transport
 * uses. All of Phase 0/1's host hardening then applies unchanged.
 */
#ifndef MDKR_LAN_PARTY_TRANSPORT_H
#define MDKR_LAN_PARTY_TRANSPORT_H

#include "lan_party_server.h"
#include "native_party_host.h"

#include <cstdint>
#include <functional>
#include <memory>

/*
 * Construction inputs. The manifest is the packaged controller assets the
 * embedded server hands out (built by the caller from dist/web/controller;
 * empty is legal -- the server simply 404s asset GETs, and pairing still
 * works over /party-ws). The advertised host is what goes into the invite
 * QR/URL the phones scan (a reachable LAN address, e.g. "192.168.1.5"); when
 * empty the invite carries only the bare "/controller/#<cap>" path and the
 * caller prefixes it. bindPort 0 takes an ephemeral port.
 */
struct MdkrLanPartyTransportConfig {
    MdkrLanPartyManifest manifest;
    std::string advertisedHost;
    uint16_t bindPort = 0u;
    /* Test seams (production leaves these empty -> platform secure RNG +
     * steady_clock). They drive the room's invite/throttle clock and its
     * capability/code minting deterministically. */
    std::function<void(uint8_t *, size_t)> roomRandomBytes;
    std::function<uint64_t()> roomNowMs;
};

std::unique_ptr<MdkrPartyTransport> mdkr_create_lan_party_transport(
    MdkrLanPartyTransportConfig config = {});

#ifdef MDKR_LAN_PARTY_TESTING
class MdkrLanPartyControllerSocket;

/*
 * Test-only seams, compiled solely into the unit test (cmake/tests.cmake
 * defines MDKR_LAN_PARTY_TESTING there and nowhere else -- the
 * MDKR_A11Y_SPEECH_TESTING pattern). They let a test drive a full pairing
 * lifecycle against a fake phone socket + a real WebRTC peer WITHOUT binding
 * a TCP port or standing up the accept threads: attach a fake controller
 * straight into the transport's room, and read the STUN-free ICE config the
 * peers use.
 */
std::unique_ptr<MdkrPartyTransport> mdkr_create_lan_party_transport_for_test(
    MdkrLanPartyTransportConfig config);
void mdkr_lan_party_transport_attach_test_controller(
    MdkrPartyTransport &transport,
    std::shared_ptr<MdkrLanPartyControllerSocket> socket);
#endif

#endif /* MDKR_LAN_PARTY_TRANSPORT_H */
