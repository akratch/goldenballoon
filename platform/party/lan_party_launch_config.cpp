/*
 * Production glue for local play: reach the real machine addresses and the
 * packaged web bundle. App-only (no unit harness) -- the logic worth pinning is
 * in lan_party_launch.cpp; this file is the thin wiring to getifaddrs and the
 * resource path.
 */
#include "lan_party_launch.h"
#include "lan_party_server.h"       /* mdkr_lan_party_machine_ipv4_addresses */
#include "user_paths.h"             /* mdkr_user_resource_path (self-guards C) */

#include <string>

std::string mdkr_lan_party_advertised_host() {
    return mdkr_lan_party_select_advertised_host(
        mdkr_lan_party_machine_ipv4_addresses());
}

bool mdkr_lan_party_build_launch_config(MdkrLanPartyTransportConfig &out,
                                        std::string &reason) {
    const std::string host = mdkr_lan_party_advertised_host();
    if (host.empty()) {
        reason = "Connect this computer to Wi-Fi or a wired network, then try "
                 "local play again.";
        return false;
    }
    /* The packaged controller assets live beside the app's other resources; a
     * command-line build resolves the same tree relative to the working
     * directory (mdkr_user_resource_path). */
    char webRoot[4096] = {0};
    if (!mdkr_user_resource_path("dist/web", webRoot, sizeof(webRoot))) {
        reason = "The controller page is missing from this build, so local play "
                 "cannot start.";
        return false;
    }
    MdkrLanPartyManifest manifest;
    if (!mdkr_lan_party_build_manifest(webRoot, manifest)) {
        reason = "The controller page is missing from this build, so local play "
                 "cannot start.";
        return false;
    }
    out = MdkrLanPartyTransportConfig{};
    out.manifest = std::move(manifest);
    out.advertisedHost = host;
    out.bindPort = 0u;   /* ephemeral: the OS picks a free port, stable for the
                          * session (the server reports it back for the QR). */
    reason.clear();
    return true;
}
