/*
 * Unit tests for the pure local-play launch helpers (Task 5):
 * advertised-host selection and the controller-asset manifest build. These are
 * the parts of local play worth pinning without a network or the packaged
 * bundle -- the getifaddrs/resource-path glue (lan_party_launch_config.cpp) has
 * no unit harness, exactly as ui_*.cpp does not.
 */
#include "party/lan_party_launch.h"

/* Assert-driven: NDEBUG would compile every check away. */
#undef NDEBUG

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

/* ---- advertised-host selection ---------------------------------------- */

void loopbackAloneYieldsNoHost() {
    /* getifaddrs always hands loopback; it is never a reachable LAN address, so
     * a machine with only loopback fails closed (empty -> "no card"). */
    assert(mdkr_lan_party_select_advertised_host({"127.0.0.1"}).empty());
    assert(mdkr_lan_party_select_advertised_host({}).empty());
    std::printf("loopbackAloneYieldsNoHost: ok\n");
}

void linkLocalIsExcluded() {
    /* 169.254/16 is APIPA -- a phone on the LAN cannot reach it. */
    assert(mdkr_lan_party_select_advertised_host(
        {"127.0.0.1", "169.254.13.7"}).empty());
    std::printf("linkLocalIsExcluded: ok\n");
}

void privateLanAddressesAreChosen() {
    assert(mdkr_lan_party_select_advertised_host(
        {"127.0.0.1", "192.168.1.5"}) == "192.168.1.5");
    assert(mdkr_lan_party_select_advertised_host({"10.0.0.4"}) == "10.0.0.4");
    assert(mdkr_lan_party_select_advertised_host({"172.16.5.9"}) == "172.16.5.9");
    assert(mdkr_lan_party_select_advertised_host({"172.31.255.1"}) ==
           "172.31.255.1");
    /* 172.15/172.32 are NOT in 172.16/12 -- routable, still usable, but not the
     * preferred private class. With no private option they are chosen anyway. */
    assert(mdkr_lan_party_select_advertised_host({"172.15.0.1"}) == "172.15.0.1");
    std::printf("privateLanAddressesAreChosen: ok\n");
}

void privateBeatsRoutableRegardlessOfOrder() {
    /* A VPN/public address alongside the real LAN address: the private LAN one
     * wins no matter which the OS enumerated first. */
    assert(mdkr_lan_party_select_advertised_host(
        {"203.0.113.9", "192.168.0.20"}) == "192.168.0.20");
    assert(mdkr_lan_party_select_advertised_host(
        {"192.168.0.20", "203.0.113.9"}) == "192.168.0.20");
    std::printf("privateBeatsRoutableRegardlessOfOrder: ok\n");
}

void selectedHostIsAlwaysAllowlisted() {
    /* The server's Host allowlist is {localhost, 127.0.0.1} + these candidates
     * (mdkr_lan_party_machine_ipv4_addresses). Whatever we advertise must be a
     * member of the candidate set, so the socket it names always answers. */
    const std::vector<std::string> candidates = {
        "127.0.0.1", "169.254.1.1", "10.1.2.3", "192.168.4.5"};
    const std::string host = mdkr_lan_party_select_advertised_host(candidates);
    assert(!host.empty());
    bool member = false;
    for (const std::string &candidate : candidates) {
        if (candidate == host) member = true;
    }
    assert(member);
    std::printf("selectedHostIsAlwaysAllowlisted: ok (host=%s)\n", host.c_str());
}

void malformedAddressesAreIgnored() {
    assert(mdkr_lan_party_select_advertised_host({"not.an.ip", "999.1.1.1"})
               .empty());
    assert(mdkr_lan_party_select_advertised_host(
        {"192.168.1.256"}).empty());       /* octet out of range */
    assert(mdkr_lan_party_select_advertised_host(
        {"192.168.1"}).empty());           /* too few octets */
    std::printf("malformedAddressesAreIgnored: ok\n");
}

/* ---- manifest build ---------------------------------------------------- */

std::string g_tempRoot;
std::vector<std::string> g_createdRoots;

std::string tempRoot() {
    if (!g_tempRoot.empty()) return g_tempRoot;
    const char *base = std::getenv("TMPDIR");
    std::string dir = (base != nullptr && base[0] != '\0') ? base : "/tmp";
    if (dir.back() != '/') dir.push_back('/');
    dir += "mdkr-lan-launch-" + std::to_string(static_cast<long>(std::rand()));
    g_tempRoot = dir;
    return dir;
}

/* Remember a root so cleanupRoots() removes it -- a local test run must not
 * litter $TMPDIR with a tree per invocation. */
void rememberRoot(const std::string &root) { g_createdRoots.push_back(root); }

void cleanupRoots() {
    for (const std::string &root : g_createdRoots) {
        (void)std::system(("rm -rf '" + root + "'").c_str());
    }
    g_createdRoots.clear();
}

void writeAsset(const std::string &root, const std::string &relative,
                const std::string &contents) {
    /* Create the parent directory (controller/, party/ or input/). */
    const size_t slash = relative.find('/');
    if (slash != std::string::npos) {
        std::string command = "mkdir -p '" + root + "/" +
                              relative.substr(0, slash) + "'";
        assert(std::system(command.c_str()) == 0);
    }
    std::ofstream file(root + "/" + relative, std::ios::binary);
    assert(static_cast<bool>(file));
    file << contents;
}

void seedFullTree(const std::string &root) {
    assert(std::system(("mkdir -p '" + root + "'").c_str()) == 0);
    rememberRoot(root);
    writeAsset(root, "controller/index.html", "<!doctype html><html></html>");
    writeAsset(root, "controller/controller.css", ".x{color:red}");
    writeAsset(root, "controller/controller.js", "console.log(1)");
    writeAsset(root, "party/party-mode.js", "window.__MDKR_LAN_PARTY__ = false;\n");
    writeAsset(root, "party/party-protocol.js", "export const P=2");
    writeAsset(root, "party/party-sas.js", "export function sas(){}");
    writeAsset(root, "input/touch-surface.css", ".t{}");
    writeAsset(root, "input/touch-surface.js", "function t(){}");
}

void manifestBuildsFromPackagedTree() {
    const std::string root = tempRoot();
    seedFullTree(root);
    MdkrLanPartyManifest manifest;
    assert(mdkr_lan_party_build_manifest(root, manifest));

    /* The QR resolves to /controller/ after its fragment is stripped. */
    assert(manifest.count("/controller/") == 1u);
    assert(manifest.at("/controller/").contentType == "text/html; charset=utf-8");
    assert(manifest.at("/controller/").bytes ==
           std::vector<uint8_t>({'<', '!', 'd', 'o', 'c', 't', 'y', 'p', 'e',
                                 ' ', 'h', 't', 'm', 'l', '>', '<', 'h', 't',
                                 'm', 'l', '>', '<', '/', 'h', 't', 'm', 'l',
                                 '>'}));

    /* Every asset the controller page loads is present at its request path with
     * the right content type. */
    assert(manifest.at("/controller/controller.css").contentType ==
           "text/css; charset=utf-8");
    assert(manifest.at("/controller/controller.js").contentType ==
           "text/javascript; charset=utf-8");
    assert(manifest.count("/party/party-protocol.js") == 1u);
    assert(manifest.at("/party/party-sas.js").contentType ==
           "text/javascript; charset=utf-8");
    assert(manifest.count("/input/touch-surface.css") == 1u);
    assert(manifest.count("/input/touch-surface.js") == 1u);

    /* The embedded server declares local-play mode by OVERRIDING this one asset:
     * the packaged file ships cloud mode (false), but a page served from here
     * must read true so the controller picks ws-redeem over POST. The tree's
     * false body must not survive into the served manifest. */
    const std::string lanMode = "window.__MDKR_LAN_PARTY__ = true;\n";
    assert(manifest.count("/party/party-mode.js") == 1u);
    assert(manifest.at("/party/party-mode.js").contentType ==
           "text/javascript; charset=utf-8");
    assert((manifest.at("/party/party-mode.js").bytes ==
            std::vector<uint8_t>(lanMode.begin(), lanMode.end())));
    std::printf("manifestBuildsFromPackagedTree: ok (%zu entries)\n",
                manifest.size());
}

void canStartRequiresBothHostAndAssets() {
    /* The availability predicate the launcher card gates on. Assets-present +
     * host -> can start; assets-missing -> cannot (the dead-button fix); no
     * host -> cannot regardless of assets. */
    const std::string root = tempRoot() + "-canstart";
    seedFullTree(root);
    const std::string missing = root + "-missing";  /* never created */
    assert(mdkr_lan_party_can_start("192.168.1.5", root));
    assert(!mdkr_lan_party_can_start("192.168.1.5", missing));
    assert(!mdkr_lan_party_can_start("", root));
    assert(!mdkr_lan_party_can_start("", missing));
    std::printf("canStartRequiresBothHostAndAssets: ok\n");
}

void missingControllerPageFailsClosed() {
    const std::string root = tempRoot() + "-partial";
    assert(std::system(("mkdir -p '" + root + "/party'").c_str()) == 0);
    rememberRoot(root);
    writeAsset(root, "party/party-sas.js", "x");
    /* No controller/index.html: a page that would load broken is no page.
     * build must fail and leave the manifest empty. */
    MdkrLanPartyManifest manifest;
    manifest["/stale"].contentType = "text/plain";
    assert(!mdkr_lan_party_build_manifest(root, manifest));
    assert(manifest.empty());
    std::printf("missingControllerPageFailsClosed: ok\n");
}

}  // namespace

int main() {
    loopbackAloneYieldsNoHost();
    linkLocalIsExcluded();
    privateLanAddressesAreChosen();
    privateBeatsRoutableRegardlessOfOrder();
    selectedHostIsAlwaysAllowlisted();
    malformedAddressesAreIgnored();
    manifestBuildsFromPackagedTree();
    canStartRequiresBothHostAndAssets();
    missingControllerPageFailsClosed();
    cleanupRoots();
    std::printf("all lan_party_launch cases passed\n");
    return 0;
}
