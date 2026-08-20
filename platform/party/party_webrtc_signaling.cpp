#include "party_webrtc_signaling.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace mdkr_party {

namespace {

const std::array<const char *, 32> kLeft = {{
    "Amber", "Brave", "Bright", "Calm", "Coral", "Cosmic", "Daring",
    "Flying", "Gentle", "Golden", "Happy", "Icy", "Jolly", "Lucky",
    "Mighty", "Neon", "Nimble", "Orange", "Rapid", "Royal", "Silver",
    "Solar", "Sunny", "Swift", "Teal", "Tiny", "Turbo", "Velvet",
    "Violet", "Warm", "Wild", "Zippy",
}};
const std::array<const char *, 32> kRight = {{
    "Balloon", "Comet", "Drum", "Falcon", "Fox", "Glider", "Kite", "Lion",
    "Moon", "Otter", "Panda", "Parrot", "Pebble", "Pilot", "Rocket", "Sail",
    "Sparrow", "Star", "Tiger", "Toucan", "Turtle", "Whale", "Wing", "Cloud",
    "Dolphin", "Lantern", "Meteor", "Penguin", "Planet", "Raven", "Sunrise",
    "Thunder",
}};

/*
 * SAS v2 fingerprint capture (I-1). Canonical form, agreed byte-for-byte
 * with the controller page: the value string after "a=fingerprint:" with
 * single spaces, algorithm token verbatim, hex uppercased -- e.g.
 * "sha-256 AB:CD:...". Only lines that BEGIN with the attribute count; a
 * line that begins with it but does not parse, or two lines that disagree,
 * refuse the whole description.
 */
bool canonicalFingerprintValue(const std::string &line, std::string &output) {
    std::vector<std::string> tokens;
    std::string token;
    for (const char byte : line) {
        if (byte == ' ' || byte == '\t') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token.push_back(byte);
        }
    }
    if (!token.empty()) tokens.push_back(token);
    if (tokens.size() != 2u) return false;
    const std::string &algorithm = tokens[0];
    std::string value = tokens[1];
    if (algorithm.size() > 32u || value.size() > 512u) return false;
    for (const char byte : algorithm) {
        if (std::isalnum(static_cast<unsigned char>(byte)) == 0 &&
            byte != '-') {
            return false;
        }
    }
    /* The fingerprint itself: hex pairs separated by single colons. */
    if (value.size() < 2u || (value.size() + 1u) % 3u != 0u) return false;
    for (size_t index = 0u; index < value.size(); index++) {
        if ((index + 1u) % 3u == 0u) {
            if (value[index] != ':') return false;
        } else {
            const unsigned char byte = static_cast<unsigned char>(value[index]);
            if (std::isxdigit(byte) == 0) return false;
            value[index] = static_cast<char>(std::toupper(byte));
        }
    }
    output = algorithm + ' ' + value;
    return true;
}

}  // namespace

std::string base64Url(const uint8_t *bytes, std::size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((length * 4u + 2u) / 3u);
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    for (std::size_t index = 0u; index < length; index++) {
        accumulator = (accumulator << 8u) | bytes[index];
        bits += 8u;
        while (bits >= 6u) {
            bits -= 6u;
            result.push_back(kAlphabet[(accumulator >> bits) & 63u]);
        }
    }
    if (bits != 0u) result.push_back(kAlphabet[(accumulator << (6u - bits)) & 63u]);
    return result;
}

bool decodePublicKey(const std::string &value, std::array<uint8_t, 65> &bytes) {
    if (value.size() != 87u) return false;
    std::array<int8_t, 256> decode{};
    decode.fill(-1);
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (std::size_t index = 0u; index < alphabet.size(); index++) {
        decode[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int8_t>(index);
    }
    uint32_t accumulator = 0u;
    unsigned bits = 0u;
    std::size_t output = 0u;
    for (unsigned char byte : value) {
        const int8_t digit = decode[byte];
        if (digit < 0) return false;
        accumulator = (accumulator << 6u) | static_cast<uint32_t>(digit);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (output >= bytes.size()) return false;
            bytes[output++] = static_cast<uint8_t>((accumulator >> bits) & 0xffu);
        }
    }
    return output == bytes.size() && bits == 2u &&
        (accumulator & 3u) == 0u && bytes[0] == 4u;
}

std::string canonicalSdpFingerprint(const std::string &sdp) {
    static constexpr char kPrefix[] = "a=fingerprint:";
    constexpr std::size_t kPrefixLength = sizeof(kPrefix) - 1u;
    std::string canonical;
    std::size_t start = 0u;
    for (;;) {
        std::size_t end = sdp.find('\n', start);
        if (end == std::string::npos) end = sdp.size();
        std::size_t stop = end;
        if (stop > start && sdp[stop - 1u] == '\r') stop--;
        if (stop - start >= kPrefixLength &&
            sdp.compare(start, kPrefixLength, kPrefix) == 0) {
            std::string value;
            if (!canonicalFingerprintValue(
                    sdp.substr(start + kPrefixLength,
                               stop - start - kPrefixLength), value)) {
                return std::string{};
            }
            if (!canonical.empty() && canonical != value) return std::string{};
            canonical = std::move(value);
        }
        if (end >= sdp.size()) break;
        start = end + 1u;
    }
    return canonical;
}

bool safeString(const Json &object, const char *key, std::string &output,
                std::size_t maximum, bool required) {
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (!found->is_string()) return false;
    output = found->get<std::string>();
    return output.size() <= maximum;
}

bool uintValue(const Json &object, const char *key, uint64_t &output,
               uint64_t maximum) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) return false;
    output = found->get<uint64_t>();
    return output <= maximum;
}

bool commandRejectionFromSignal(const Json &value,
                                MdkrPartyTransportEvent &event) {
    if (value.value("type", std::string{}) != "host_command_result" ||
        value.value("ok", true) != false) {
        return false;
    }
    event.type = MdkrPartyTransportEventType::CommandRejected;
    event.message = "That controller action did not complete. Try again.";
    std::string code;
    if (safeString(value, "error", code, kMaxErrorCodeBytes,
                   /*required=*/false)) {
        event.errorCode = std::move(code);
    }
    std::string command;
    if (safeString(value, "command", command, kMaxErrorCodeBytes,
                   /*required=*/false)) {
        event.command = std::move(command);
    }
    std::string controllerId;
    if (safeString(value, "controllerId", controllerId, 64u,
                   /*required=*/false)) {
        event.controllerId = std::move(controllerId);
    }
    return true;
}

bool controllerReadyEventFromControl(const Json &value,
                                     const std::string &peerId,
                                     uint32_t connectionSequence,
                                     MdkrPartyTransportEvent &event) {
    if (value.value("type", std::string{}) != "controller_ready" ||
        value.value("controllerId", std::string{}) != peerId ||
        value.value("connectionSequence", 0u) != connectionSequence) {
        return false;
    }
    const unsigned theirProtocol = value.value("protocol", 0u);
    if (theirProtocol != kChannelProtocol) {
        event.type = MdkrPartyTransportEventType::ControllerProtocolMismatch;
        event.controllerId = peerId;
        event.theirProtocol = theirProtocol;
        return true;
    }
    event.type = MdkrPartyTransportEventType::ControllerConnected;
    event.controllerId = peerId;
    event.haptics = value.contains("capabilities") &&
        value["capabilities"].is_object() &&
        value["capabilities"].value("vibration", false);
    return true;
}

/* ---- Identity (SAS keypair + phrase) ----------------------------------- */

struct Identity::Impl {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_ecp_group group;
    mbedtls_mpi privateKey;
    mbedtls_ecp_point publicKey;
    std::string encodedPublicKey;

    Impl() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&privateKey);
        mbedtls_ecp_point_init(&publicKey);
    }

    ~Impl() {
        mbedtls_ecp_point_free(&publicKey);
        mbedtls_mpi_free(&privateKey);
        mbedtls_ecp_group_free(&group);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    bool initializeCurve() {
        static constexpr char kPersonalization[] = "mdkr-native-party-sas-v1";
        return mbedtls_ctr_drbg_seed(
                   &random, mbedtls_entropy_func, &entropy,
                   reinterpret_cast<const unsigned char *>(kPersonalization),
                   sizeof(kPersonalization) - 1u) == 0 &&
            mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0;
    }

    bool encodePublic() {
        std::array<uint8_t, 65> raw{};
        size_t written = 0u;
        if (mbedtls_ecp_point_write_binary(
                &group, &publicKey, MBEDTLS_ECP_PF_UNCOMPRESSED,
                &written, raw.data(), raw.size()) != 0 ||
            written != raw.size()) {
            return false;
        }
        encodedPublicKey = base64Url(raw.data(), raw.size());
        return encodedPublicKey.size() == 87u;
    }
};

Identity::Identity() : impl_(std::make_unique<Impl>()) {}
Identity::~Identity() = default;

bool Identity::generate() {
    if (!impl_->initializeCurve() ||
        mbedtls_ecp_gen_keypair(
            &impl_->group, &impl_->privateKey, &impl_->publicKey,
            mbedtls_ctr_drbg_random, &impl_->random) != 0) {
        return false;
    }
    return impl_->encodePublic();
}

bool Identity::loadPrivateForTest(const uint8_t scalar[32]) {
    if (scalar == nullptr || !impl_->initializeCurve() ||
        mbedtls_mpi_read_binary(&impl_->privateKey, scalar, 32u) != 0 ||
        mbedtls_ecp_check_privkey(&impl_->group, &impl_->privateKey) != 0 ||
        mbedtls_ecp_mul(&impl_->group, &impl_->publicKey, &impl_->privateKey,
                        &impl_->group.G,
                        mbedtls_ctr_drbg_random, &impl_->random) != 0) {
        return false;
    }
    return impl_->encodePublic();
}

const std::string &Identity::publicKey() const {
    return impl_->encodedPublicKey;
}

bool Identity::phrase(const std::string &peerEncoded, const std::string &roomId,
                      const std::string &hostFingerprint,
                      const std::string &controllerFingerprint,
                      std::string &result) {
    if (hostFingerprint.empty() || controllerFingerprint.empty()) {
        return false;
    }
    std::array<uint8_t, 65> peerBytes{};
    if (!decodePublicKey(peerEncoded, peerBytes)) return false;
    mbedtls_ecp_point peer;
    mbedtls_ecp_point shared;
    mbedtls_ecp_point_init(&peer);
    mbedtls_ecp_point_init(&shared);
    const bool ok = mbedtls_ecp_point_read_binary(
                        &impl_->group, &peer, peerBytes.data(),
                        peerBytes.size()) == 0 &&
        mbedtls_ecp_check_pubkey(&impl_->group, &peer) == 0 &&
        mbedtls_ecp_mul(&impl_->group, &shared, &impl_->privateKey, &peer,
                        mbedtls_ctr_drbg_random, &impl_->random) == 0;
    std::array<uint8_t, 32> secret{};
    const bool wrote = ok && mbedtls_mpi_write_binary(
        &shared.MBEDTLS_PRIVATE(X), secret.data(), secret.size()) == 0;
    mbedtls_ecp_point_free(&shared);
    mbedtls_ecp_point_free(&peer);
    if (!wrote) return false;

    const std::string context = std::string("golden-balloon-party-sas-v2") +
        '\0' + roomId + '\0' + impl_->encodedPublicKey + '\0' + peerEncoded +
        '\0' + hostFingerprint + '\0' + controllerFingerprint;
    std::array<uint8_t, 32> digest{};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    const bool hashed = mbedtls_sha256_starts(&hash, 0) == 0 &&
        mbedtls_sha256_update(&hash, secret.data(), secret.size()) == 0 &&
        mbedtls_sha256_update(&hash,
            reinterpret_cast<const unsigned char *>(context.data()),
            context.size()) == 0 &&
        mbedtls_sha256_finish(&hash, digest.data()) == 0;
    mbedtls_sha256_free(&hash);
    mbedtls_platform_zeroize(secret.data(), secret.size());
    if (!hashed) {
        return false;
    }
    const uint32_t value = (static_cast<uint32_t>(digest[0]) << 12u) |
        (static_cast<uint32_t>(digest[1]) << 4u) |
        (static_cast<uint32_t>(digest[2]) >> 4u);
    result = std::string(kLeft[(value >> 15u) & 31u]) + "-" +
        kRight[(value >> 10u) & 31u] + " " +
        kLeft[(value >> 5u) & 31u] + "-" + kRight[value & 31u];
    return true;
}

}  // namespace mdkr_party
