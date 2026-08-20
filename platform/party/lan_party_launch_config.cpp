/*
 * Production glue for local play: reach the real machine addresses and the
 * packaged web bundle. App-only (no unit harness) -- the logic worth pinning is
 * in lan_party_launch.cpp; this file is the thin wiring to getifaddrs, the
 * resource path, and SDL's executable directory.
 */
#include "lan_party_launch.h"
#include "lan_party_server.h"       /* mdkr_lan_party_machine_ipv4_addresses */
#include "user_paths.h"             /* mdkr_user_resource_path (self-guards C) */

#include "SDL.h"                    /* SDL_GetBasePath / SDL_free */

#include <string>

namespace {

/* Does the controller page's full asset set resolve under this root? Reuses the
 * one manifest builder so "root accepted" means every asset the phone loads is
 * present, never just the index. */
bool assetsResolveAt(const std::string &webRoot) {
    if (webRoot.empty()) return false;
    MdkrLanPartyManifest manifest;
    return mdkr_lan_party_build_manifest(webRoot, manifest);
}

}  // namespace

std::string mdkr_lan_party_advertised_host() {
    return mdkr_lan_party_select_advertised_host(
        mdkr_lan_party_machine_ipv4_addresses());
}

std::string mdkr_lan_party_web_root() {
    /* 1) macOS .app: mdkr_user_resource_path resolves Contents/Resources. */
    char resourceRoot[4096] = {0};
    if (mdkr_user_paths_is_packaged() &&
        mdkr_user_resource_path("dist/web", resourceRoot,
                                sizeof(resourceRoot)) &&
        assetsResolveAt(resourceRoot)) {
        return resourceRoot;
    }
    /* 2) Beside the executable (Windows zip / Linux AppImage stage dist/web
     * there), resolved independently of the caller's CWD -- the exact posture
     * the controller-mapping database already uses (platform_sdl_min.c). */
    char *base = SDL_GetBasePath();
    if (base != nullptr) {
        const std::string besideExe = std::string(base) + "dist/web";
        SDL_free(base);
        if (assetsResolveAt(besideExe)) return besideExe;
    }
    /* 3) A dev run from the repository root. */
    if (assetsResolveAt("dist/web")) return "dist/web";
    return {};
}

bool mdkr_lan_party_build_launch_config(MdkrLanPartyTransportConfig &out,
                                        std::string &reason) {
    const std::string host = mdkr_lan_party_advertised_host();
    if (host.empty()) {
        reason = kMdkrLanPartyNoNetworkReason;
        return false;
    }
    const std::string webRoot = mdkr_lan_party_web_root();
    MdkrLanPartyManifest manifest;
    if (webRoot.empty() || !mdkr_lan_party_build_manifest(webRoot, manifest)) {
        reason = kMdkrLanPartyNoAssetsReason;
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
