/*
 * Launcher-side configuration for zero-internet local play (Task 5).
 *
 * The LanPartyTransport (lan_party_transport.h) is configured at construction
 * with three things the launcher must supply: the LAN address the invite QR
 * encodes, the packaged controller assets the embedded server hands out, and
 * an ephemeral port. This module builds all three, and it fails closed: no LAN
 * address or no controller page means no local-play card.
 *
 * The two pure functions below (select-advertised-host and build-manifest) carry
 * the logic worth pinning and live in lan_party_launch.cpp with no dependency on
 * getifaddrs, the resource bundle, or SDL, so a unit test can exercise them
 * directly (tests/test_lan_party_launch.cpp). The two production wrappers
 * (advertised-host and build-launch-config) that reach the real machine
 * addresses and the packaged bundle live in lan_party_launch_config.cpp, which
 * only the app links -- ui_*.cpp has no unit harness, and neither does the glue
 * that resolves them.
 */
#ifndef MDKR_LAN_PARTY_LAUNCH_H
#define MDKR_LAN_PARTY_LAUNCH_H

#include "lan_party_server.h"       /* MdkrLanPartyManifest */
#include "lan_party_transport.h"    /* MdkrLanPartyTransportConfig */

#include <string>
#include <vector>

/*
 * Choose the LAN IPv4 the invite QR will advertise from a set of this
 * machine's addresses (the SAME set the server freezes into its Host allowlist,
 * so whatever this returns is always allowlisted). Loopback (127/8) and
 * link-local (169.254/16) are never a reachable LAN address and are excluded; a
 * private LAN address (10/8, 172.16/12, 192.168/16) is preferred over any other
 * routable one. Returns the empty string when no candidate qualifies -- the
 * fail-closed signal the caller turns into "no card".
 */
std::string mdkr_lan_party_select_advertised_host(
    const std::vector<std::string> &candidates);

/*
 * Build the served-asset manifest for the controller page from the packaged web
 * tree rooted at `webRoot` (a directory containing controller/, party/ and
 * input/). Reads exactly the files the controller page loads and maps each to
 * its request path, plus the "/controller/" directory alias the QR resolves to.
 * Returns false and leaves `out` empty when any required file is missing or
 * unreadable -- a controller page that would load broken is no page at all.
 */
bool mdkr_lan_party_build_manifest(const std::string &webRoot,
                                   MdkrLanPartyManifest &out);

/*
 * Production advertised-host: select over this machine's real IPv4 addresses
 * (mdkr_lan_party_machine_ipv4_addresses, the allowlist source). Empty when the
 * machine is on no network.
 */
std::string mdkr_lan_party_advertised_host();

/*
 * Assemble a full LanPartyTransport config from the live machine: advertised
 * host, controller manifest from the packaged bundle, and an ephemeral port
 * (bindPort 0). Returns false with a player-facing `reason` when local play
 * cannot start (no LAN address, or the controller assets are not present in this
 * build); the caller must not start a transport in that case.
 */
bool mdkr_lan_party_build_launch_config(MdkrLanPartyTransportConfig &out,
                                        std::string &reason);

#endif /* MDKR_LAN_PARTY_LAUNCH_H */
