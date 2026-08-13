#include "match_peer_crypto.h"

#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_util.h>

#include <array>
#include <cstring>
#include <new>

namespace {
constexpr uint8_t kMagic[4] = {'M', 'P', 'E', '1'};
constexpr uint8_t kVersion = 1u;
constexpr char kKeyDomain[] = "golden-balloon-match-input-key-v1";

void store32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24u);
    out[1] = static_cast<uint8_t>(value >> 16u);
    out[2] = static_cast<uint8_t>(value >> 8u);
    out[3] = static_cast<uint8_t>(value);
}

void store64(uint8_t *out, uint64_t value) {
    for (unsigned index = 0u; index < 8u; ++index)
        out[index] = static_cast<uint8_t>(value >> (56u - index * 8u));
}

uint32_t load32(const uint8_t *in) {
    return static_cast<uint32_t>(in[0]) << 24u |
        static_cast<uint32_t>(in[1]) << 16u |
        static_cast<uint32_t>(in[2]) << 8u | in[3];
}

uint64_t load64(const uint8_t *in) {
    uint64_t value = 0u;
    for (unsigned index = 0u; index < 8u; ++index)
        value = value << 8u | in[index];
    return value;
}

bool keyContextValid(const MdkrMatchPeerKeyContext *context) {
    return context != nullptr && context->match_epoch != 0u &&
        context->source_endpoint_id != 0u && context->source_generation != 0u &&
        context->destination_endpoint_id != 0u &&
        context->destination_generation != 0u &&
        context->source_endpoint_id != context->destination_endpoint_id;
}

bool keyContextEqual(const MdkrMatchPeerKeyContext &left,
                     const MdkrMatchPeerKeyContext &right) {
    return left.match_epoch == right.match_epoch &&
        left.source_endpoint_id == right.source_endpoint_id &&
        left.source_generation == right.source_generation &&
        left.destination_endpoint_id == right.destination_endpoint_id &&
        left.destination_generation == right.destination_generation;
}

bool sendContextValid(const MdkrMatchPeerSendContext *context) {
    return context != nullptr && keyContextValid(&context->key) &&
        context->payload_type <= MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX &&
        (context->intermediate_endpoint_id == 0u ||
         (context->intermediate_endpoint_id != context->key.source_endpoint_id &&
          context->intermediate_endpoint_id !=
              context->key.destination_endpoint_id));
}

bool sealWindowValid(const MdkrMatchPeerSealWindow *window) {
    return window != nullptr && keyContextValid(&window->direction) &&
        ((window->ready && window->next_sequence != 0u) ||
         (!window->ready && window->next_sequence == 0u));
}

bool sealWindowEmpty(const MdkrMatchPeerSealWindow *window) {
    return window != nullptr && window->direction.match_epoch == 0u &&
        window->direction.source_endpoint_id == 0u &&
        window->direction.source_generation == 0u &&
        window->direction.destination_endpoint_id == 0u &&
        window->direction.destination_generation == 0u &&
        window->next_sequence == 0u && !window->ready;
}

bool envelopeContextValid(const MdkrMatchPeerEnvelopeContext *context) {
    return context != nullptr && keyContextValid(&context->key) &&
        context->sequence != 0u &&
        context->payload_type <= MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX &&
        (context->intermediate_endpoint_id == 0u ||
         (context->intermediate_endpoint_id != context->key.source_endpoint_id &&
          context->intermediate_endpoint_id != context->key.destination_endpoint_id));
}

void writeHeader(const MdkrMatchPeerEnvelopeContext &context, uint8_t *out) {
    std::memset(out, 0, MDKR_MATCH_PEER_HEADER_BYTES);
    std::memcpy(out, kMagic, sizeof(kMagic));
    out[4] = kVersion;
    out[5] = context.intermediate_endpoint_id == 0u ? 0u : 1u;
    out[6] = context.payload_type;
    store32(out + 8u, context.key.match_epoch);
    store64(out + 12u, context.key.source_endpoint_id);
    store32(out + 20u, context.key.source_generation);
    store64(out + 24u, context.key.destination_endpoint_id);
    store32(out + 32u, context.key.destination_generation);
    store64(out + 36u, context.intermediate_endpoint_id);
    store64(out + 44u, context.sequence);
}

bool readHeader(const uint8_t *in, MdkrMatchPeerEnvelopeContext &context) {
    if (std::memcmp(in, kMagic, sizeof(kMagic)) != 0 || in[4] != kVersion ||
        in[5] > 1u || in[6] > MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX ||
        in[7] != 0u) return false;
    std::memset(&context, 0, sizeof(context));
    context.key.match_epoch = load32(in + 8u);
    context.key.source_endpoint_id = load64(in + 12u);
    context.key.source_generation = load32(in + 20u);
    context.key.destination_endpoint_id = load64(in + 24u);
    context.key.destination_generation = load32(in + 32u);
    context.intermediate_endpoint_id = load64(in + 36u);
    context.sequence = load64(in + 44u);
    context.payload_type = in[6];
    return envelopeContextValid(&context) &&
        ((in[5] == 0u) == (context.intermediate_endpoint_id == 0u));
}

void nonce(const MdkrMatchPeerEnvelopeContext &context, uint8_t out[12]) {
    store32(out, context.key.match_epoch);
    store64(out + 4u, context.sequence);
}

bool replayAccept(MdkrMatchPeerReplayWindow &window, uint64_t sequence) {
    if (!window.initialized) {
        window.initialized = true;
        window.greatest_sequence = sequence;
        window.seen_bitmap = 1u;
        return true;
    }
    if (sequence > window.greatest_sequence) {
        const uint64_t distance = sequence - window.greatest_sequence;
        window.seen_bitmap = distance >= 64u ? 1u :
            (window.seen_bitmap << static_cast<unsigned>(distance)) | 1u;
        window.greatest_sequence = sequence;
        return true;
    }
    const uint64_t distance = window.greatest_sequence - sequence;
    if (distance >= 64u ||
        (window.seen_bitmap & (UINT64_C(1) << static_cast<unsigned>(distance))) != 0u)
        return false;
    window.seen_bitmap |= UINT64_C(1) << static_cast<unsigned>(distance);
    return true;
}

bool replayWindowValid(const MdkrMatchPeerReplayWindow &window) {
    if (!window.initialized)
        return window.greatest_sequence == 0u && window.seen_bitmap == 0u;
    return window.greatest_sequence != 0u &&
        (window.seen_bitmap & UINT64_C(1)) != 0u;
}
} // namespace

struct MdkrMatchPeerIdentity {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_ecp_group group;
    mbedtls_mpi privateKey;
    mbedtls_ecp_point publicKey;
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> encoded{};

    MdkrMatchPeerIdentity() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&privateKey);
        mbedtls_ecp_point_init(&publicKey);
    }

    ~MdkrMatchPeerIdentity() {
        mbedtls_ecp_point_free(&publicKey);
        mbedtls_mpi_free(&privateKey);
        mbedtls_ecp_group_free(&group);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
        mbedtls_platform_zeroize(encoded.data(), encoded.size());
    }

    bool generate() {
        static constexpr char personalization[] = "mdkr-match-peer-key-v1";
        size_t written = 0u;
        return mbedtls_ctr_drbg_seed(
                   &random, mbedtls_entropy_func, &entropy,
                   reinterpret_cast<const unsigned char *>(personalization),
                   sizeof(personalization) - 1u) == 0 &&
            mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_gen_keypair(&group, &privateKey, &publicKey,
                                    mbedtls_ctr_drbg_random, &random) == 0 &&
            mbedtls_ecp_point_write_binary(
                &group, &publicKey, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
                encoded.data(), encoded.size()) == 0 && written == encoded.size();
    }

    bool shared(const uint8_t peerBytes[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
                uint8_t secret[MDKR_MATCH_PEER_SECRET_BYTES]) {
        mbedtls_ecp_point peer;
        mbedtls_ecp_point point;
        mbedtls_ecp_point_init(&peer);
        mbedtls_ecp_point_init(&point);
        const bool ok = peerBytes != nullptr && secret != nullptr &&
            mbedtls_ecp_point_read_binary(
                &group, &peer, peerBytes, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES) == 0 &&
            mbedtls_ecp_check_pubkey(&group, &peer) == 0 &&
            mbedtls_ecp_mul(&group, &point, &privateKey, &peer,
                            mbedtls_ctr_drbg_random, &random) == 0 &&
            mbedtls_mpi_write_binary(&point.MBEDTLS_PRIVATE(X), secret,
                                     MDKR_MATCH_PEER_SECRET_BYTES) == 0;
        mbedtls_ecp_point_free(&point);
        mbedtls_ecp_point_free(&peer);
        return ok;
    }
};

extern "C" MdkrMatchPeerIdentity *mdkr_match_peer_identity_create(void) {
    MdkrMatchPeerIdentity *identity = new (std::nothrow) MdkrMatchPeerIdentity();
    if (identity == nullptr || !identity->generate()) {
        delete identity;
        return nullptr;
    }
    return identity;
}

extern "C" void mdkr_match_peer_identity_destroy(MdkrMatchPeerIdentity *identity) {
    delete identity;
}

extern "C" bool mdkr_match_peer_identity_public_key(
    const MdkrMatchPeerIdentity *identity,
    uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES]) {
    if (identity == nullptr || public_key == nullptr) return false;
    std::memcpy(public_key, identity->encoded.data(), identity->encoded.size());
    return true;
}

extern "C" bool mdkr_match_peer_identity_derive_key(
    MdkrMatchPeerIdentity *identity,
    const uint8_t peer_public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context,
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]) {
    std::array<uint8_t, MDKR_MATCH_PEER_SECRET_BYTES> secret{};
    if (identity == nullptr || peer_public_key == nullptr ||
        transcript_digest == nullptr || key == nullptr ||
        !keyContextValid(context)) return false;
    const bool shared = identity->shared(peer_public_key, secret.data());
    const bool derived = shared && mdkr_match_peer_derive_key(
        secret.data(), transcript_digest, context, key);
    /* The ECDH adapter may have written part of X before reporting a library
     * failure. Retire the temporary on every attempted exchange, not only the
     * successful path. */
    mbedtls_platform_zeroize(secret.data(), secret.size());
    return derived;
}

extern "C" bool mdkr_match_peer_derive_key(
    const uint8_t shared_secret[MDKR_MATCH_PEER_SECRET_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context,
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]) {
    std::array<uint8_t, sizeof(kKeyDomain) - 1u + 28u> info{};
    std::array<uint8_t, MDKR_MATCH_PEER_KEY_BYTES> next{};
    size_t offset = sizeof(kKeyDomain) - 1u;
    if (shared_secret == nullptr || transcript_digest == nullptr || key == nullptr ||
        !keyContextValid(context)) return false;
    std::memcpy(info.data(), kKeyDomain, offset);
    store32(info.data() + offset, context->match_epoch); offset += 4u;
    store64(info.data() + offset, context->source_endpoint_id); offset += 8u;
    store32(info.data() + offset, context->source_generation); offset += 4u;
    store64(info.data() + offset, context->destination_endpoint_id); offset += 8u;
    store32(info.data() + offset, context->destination_generation);
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const bool derived = sha256 != nullptr && mbedtls_hkdf(
        sha256, transcript_digest, MDKR_MATCH_PEER_TRANSCRIPT_BYTES,
        shared_secret, MDKR_MATCH_PEER_SECRET_BYTES, info.data(), info.size(),
        next.data(), next.size()) == 0;
    if (derived) std::memcpy(key, next.data(), next.size());
    mbedtls_platform_zeroize(next.data(), next.size());
    return derived;
}

extern "C" void mdkr_match_peer_forget_key(
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]) {
    if (key != nullptr) mbedtls_platform_zeroize(key, MDKR_MATCH_PEER_KEY_BYTES);
}

extern "C" bool mdkr_match_peer_inspect(
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context) {
    MdkrMatchPeerEnvelopeContext parsed;
    if (envelope == nullptr || context == nullptr || !readHeader(envelope, parsed))
        return false;
    *context = parsed;
    return true;
}

extern "C" bool mdkr_match_peer_seal_window_init(
    MdkrMatchPeerSealWindow *window,
    const MdkrMatchPeerKeyContext *direction) {
    MdkrMatchPeerSealWindow next{};
    if (!sealWindowEmpty(window) || !keyContextValid(direction)) return false;
    next.direction = *direction;
    next.next_sequence = 1u;
    next.ready = true;
    *window = next;
    return true;
}

extern "C" bool mdkr_match_peer_seal(
    const uint8_t key[MDKR_MATCH_PEER_KEY_BYTES],
    MdkrMatchPeerSealWindow *window,
    const MdkrMatchPeerSendContext *context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES]) {
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> next{};
    MdkrMatchPeerSealWindow nextWindow;
    MdkrMatchPeerEnvelopeContext sealed{};
    uint8_t iv[12];
    mbedtls_gcm_context cipher;
    int result;
    if (key == nullptr || payload == nullptr || envelope == nullptr ||
        !sealWindowValid(window) || !window->ready ||
        !sendContextValid(context) ||
        !keyContextEqual(window->direction, context->key)) return false;
    sealed.key = context->key;
    sealed.intermediate_endpoint_id = context->intermediate_endpoint_id;
    sealed.sequence = window->next_sequence;
    sealed.payload_type = context->payload_type;
    writeHeader(sealed, next.data());
    nonce(sealed, iv);
    mbedtls_gcm_init(&cipher);
    result = mbedtls_gcm_setkey(
        &cipher, MBEDTLS_CIPHER_ID_AES, key, MDKR_MATCH_PEER_KEY_BYTES * 8u);
    if (result == 0) result = mbedtls_gcm_crypt_and_tag(
        &cipher, MBEDTLS_GCM_ENCRYPT, MDKR_MATCH_PEER_PAYLOAD_BYTES,
        iv, sizeof(iv), next.data(), MDKR_MATCH_PEER_HEADER_BYTES, payload,
        next.data() + MDKR_MATCH_PEER_HEADER_BYTES, MDKR_MATCH_PEER_TAG_BYTES,
        next.data() + MDKR_MATCH_PEER_HEADER_BYTES + MDKR_MATCH_PEER_PAYLOAD_BYTES);
    mbedtls_gcm_free(&cipher);
    mbedtls_platform_zeroize(iv, sizeof(iv));
    if (result != 0) return false;
    nextWindow = *window;
    if (nextWindow.next_sequence == UINT64_MAX) {
        nextWindow.next_sequence = 0u;
        nextWindow.ready = false;
    } else {
        nextWindow.next_sequence++;
    }
    std::memcpy(envelope, next.data(), next.size());
    *window = nextWindow;
    return true;
}

extern "C" MdkrMatchPeerCryptoResult mdkr_match_peer_open(
    const uint8_t key[MDKR_MATCH_PEER_KEY_BYTES],
    const MdkrMatchPeerKeyContext *expected_context,
    MdkrMatchPeerReplayWindow *replay,
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]) {
    MdkrMatchPeerEnvelopeContext parsed;
    MdkrMatchPeerReplayWindow nextReplay;
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> plaintext{};
    uint8_t iv[12];
    mbedtls_gcm_context cipher;
    int result;
    if (key == nullptr || replay == nullptr || envelope == nullptr ||
        context == nullptr || payload == nullptr ||
        !keyContextValid(expected_context) || !replayWindowValid(*replay) ||
        !readHeader(envelope, parsed))
        return MDKR_MATCH_PEER_CRYPTO_INVALID;
    if (parsed.key.match_epoch != expected_context->match_epoch)
        return MDKR_MATCH_PEER_CRYPTO_STALE_EPOCH;
    if (parsed.key.source_endpoint_id != expected_context->source_endpoint_id)
        return MDKR_MATCH_PEER_CRYPTO_WRONG_SOURCE;
    if (parsed.key.destination_endpoint_id !=
        expected_context->destination_endpoint_id)
        return MDKR_MATCH_PEER_CRYPTO_WRONG_RECIPIENT;
    if (parsed.key.source_generation != expected_context->source_generation ||
        parsed.key.destination_generation !=
            expected_context->destination_generation)
        return MDKR_MATCH_PEER_CRYPTO_STALE_GENERATION;
    nonce(parsed, iv);
    mbedtls_gcm_init(&cipher);
    result = mbedtls_gcm_setkey(
        &cipher, MBEDTLS_CIPHER_ID_AES, key, MDKR_MATCH_PEER_KEY_BYTES * 8u);
    if (result == 0) result = mbedtls_gcm_auth_decrypt(
        &cipher, MDKR_MATCH_PEER_PAYLOAD_BYTES, iv, sizeof(iv), envelope,
        MDKR_MATCH_PEER_HEADER_BYTES,
        envelope + MDKR_MATCH_PEER_HEADER_BYTES + MDKR_MATCH_PEER_PAYLOAD_BYTES,
        MDKR_MATCH_PEER_TAG_BYTES, envelope + MDKR_MATCH_PEER_HEADER_BYTES,
        plaintext.data());
    mbedtls_gcm_free(&cipher);
    mbedtls_platform_zeroize(iv, sizeof(iv));
    if (result != 0) {
        mbedtls_platform_zeroize(plaintext.data(), plaintext.size());
        return MDKR_MATCH_PEER_CRYPTO_AUTHENTICATION;
    }
    nextReplay = *replay;
    if (!replayAccept(nextReplay, parsed.sequence)) {
        mbedtls_platform_zeroize(plaintext.data(), plaintext.size());
        return MDKR_MATCH_PEER_CRYPTO_REPLAY;
    }
    *replay = nextReplay;
    *context = parsed;
    std::memcpy(payload, plaintext.data(), plaintext.size());
    mbedtls_platform_zeroize(plaintext.data(), plaintext.size());
    return MDKR_MATCH_PEER_CRYPTO_OK;
}
