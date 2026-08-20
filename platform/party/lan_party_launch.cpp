/*
 * Pure launch helpers: advertised-host selection and controller-manifest build.
 * No getifaddrs, no resource bundle, no SDL -- see lan_party_launch.h for why
 * these two live apart from the production glue.
 */
#include "lan_party_launch.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <ios>

namespace {

/* Parse dotted-quad IPv4 into four octets. False for anything that is not
 * exactly four decimal octets in 0..255 -- getifaddrs never hands us anything
 * else, but a caller could, and a malformed string must classify as excluded
 * rather than sneak past the range checks below. */
bool parseIpv4(const std::string &text, std::array<int, 4> &octets) {
    int index = 0;
    int value = 0;
    int digits = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char byte = i < text.size() ? text[i] : '.';
        if (byte == '.') {
            if (digits == 0 || index > 3) return false;
            if (index < 4) octets[static_cast<size_t>(index)] = value;
            ++index;
            value = 0;
            digits = 0;
        } else if (byte >= '0' && byte <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (byte - '0');
            if (value > 255) return false;
        } else {
            return false;
        }
    }
    return index == 4;
}

/* Higher is better. 0 = never advertise (loopback, link-local, malformed);
 * 1 = routable but not a private LAN address; 2 = a private LAN address, the
 * kind a phone on the same Wi-Fi reaches. */
int addressScore(const std::string &text) {
    std::array<int, 4> ip{};
    if (!parseIpv4(text, ip)) return 0;
    if (ip[0] == 127) return 0;                       /* loopback 127/8 */
    if (ip[0] == 169 && ip[1] == 254) return 0;       /* link-local 169.254/16 */
    if (ip[0] == 10) return 2;                        /* 10/8 */
    if (ip[0] == 192 && ip[1] == 168) return 2;       /* 192.168/16 */
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return 2; /* 172.16/12 */
    return 1;                                         /* other routable */
}

struct ManifestEntry {
    const char *relative;   /* path under webRoot */
    const char *served;     /* request path the browser asks for */
};

/* Exactly the files the controller page loads (dist/web/controller/index.html
 * <link>/<script> set), each at the absolute path the page requests. The
 * "/controller/" alias is what the QR's http://<host>:<port>/controller/#<cap>
 * resolves to after the fragment is stripped. */
constexpr ManifestEntry kControllerAssets[] = {
    {"controller/index.html", "/controller/"},
    {"controller/index.html", "/controller/index.html"},
    {"controller/controller.css", "/controller/controller.css"},
    {"controller/controller.js", "/controller/controller.js"},
    {"party/party-protocol.js", "/party/party-protocol.js"},
    {"party/party-sas.js", "/party/party-sas.js"},
    {"input/touch-surface.css", "/input/touch-surface.css"},
    {"input/touch-surface.js", "/input/touch-surface.js"},
};

bool readFile(const std::string &path, std::vector<uint8_t> &bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size)) {
        return false;
    }
    return true;
}

std::string joinPath(const std::string &root, const char *relative) {
    if (root.empty()) return relative;
    std::string joined = root;
    if (joined.back() != '/' && joined.back() != '\\') joined.push_back('/');
    joined += relative;
    return joined;
}

}  // namespace

std::string mdkr_lan_party_select_advertised_host(
    const std::vector<std::string> &candidates) {
    std::string best;
    int bestScore = 0;
    for (const std::string &candidate : candidates) {
        const int score = addressScore(candidate);
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

bool mdkr_lan_party_can_start(const std::string &advertisedHost,
                              const std::string &webRoot) {
    if (advertisedHost.empty()) return false;
    MdkrLanPartyManifest manifest;
    return mdkr_lan_party_build_manifest(webRoot, manifest);
}

bool mdkr_lan_party_build_manifest(const std::string &webRoot,
                                   MdkrLanPartyManifest &out) {
    out.clear();
    MdkrLanPartyManifest built;
    for (const ManifestEntry &entry : kControllerAssets) {
        std::vector<uint8_t> bytes;
        if (!readFile(joinPath(webRoot, entry.relative), bytes)) return false;
        MdkrLanPartyAsset asset;
        asset.bytes = std::move(bytes);
        /* Content type from the file's own name, not the served path: the
         * "/controller/" directory alias has no suffix, but it is still the
         * html the phone must render. */
        asset.contentType = mdkr_lan_party_content_type(entry.relative);
        built[entry.served] = std::move(asset);
    }
    out = std::move(built);
    return true;
}
