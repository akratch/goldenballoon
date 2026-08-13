#include "compatibility_identity.h"

#include <string.h>

#include "sha256.h"

static bool decimal_component(const char **cursor) {
    const char *value = *cursor;
    if (*value < '0' || *value > '9')
        return false;
    do {
        value++;
    } while (*value >= '0' && *value <= '9');
    *cursor = value;
    return true;
}

static size_t bounded_length(const char *value, size_t maximum) {
    size_t length;
    if (value == NULL)
        return maximum + 1u;
    for (length = 0u; length <= maximum; length++) {
        if (value[length] == '\0')
            return length;
    }
    return maximum + 1u;
}

static bool valid_version(const char *version) {
    const char *cursor = version;
    size_t length;
    if (version == NULL)
        return false;
    length = bounded_length(version, 32u);
    if (length == 0u || length > 32u || !decimal_component(&cursor) ||
        *cursor++ != '.' || !decimal_component(&cursor) || *cursor++ != '.' ||
        !decimal_component(&cursor))
        return false;
    return *cursor == '\0';
}

static bool valid_commit(const char *commit) {
    unsigned index;
    if (bounded_length(commit, 40u) != 40u)
        return false;
    for (index = 0u; index < 40u; index++) {
        const char value = commit[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

static void hash_prefix(MdkrSha256 *hash, const char *label) {
    static const char product[] = "golden-balloon\n";
    static const char protocol[] = "\nv1\n";
    mdkr_sha256_init(hash);
    mdkr_sha256_update(hash, product, sizeof(product) - 1u);
    mdkr_sha256_update(hash, label, strlen(label));
    mdkr_sha256_update(hash, protocol, sizeof(protocol) - 1u);
}

bool mdkr_online_compatibility_from_provenance(
    const char *version, const char *source_commit, bool source_dirty,
    uint8_t rom_revision, MdkrOnlineCompatibilityV1 *output) {
    static const char build_label[] = "online-build";
    static const char gameplay_label[] = "gameplay-contract";
    static const char gameplay_contract[] =
        "\nprotocol=1\nrules=standard-race\nrollback=bounded-v1";
    MdkrOnlineCompatibilityV1 next;
    MdkrSha256 hash;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    if (output == NULL || source_dirty || !valid_version(version) ||
        !valid_commit(source_commit) ||
        (rom_revision != 1u && rom_revision != 2u))
        return false;

    memset(&next, 0, sizeof(next));
    next.protocol_version = MDKR_ONLINE_PROTOCOL_VERSION;
    hash_prefix(&hash, build_label);
    mdkr_sha256_update(&hash, version, strlen(version));
    mdkr_sha256_update(&hash, "\n", 1u);
    mdkr_sha256_update(&hash, source_commit, 40u);
    mdkr_sha256_final(&hash, digest);
    memcpy(next.build_id, digest, sizeof(next.build_id));

    hash_prefix(&hash, gameplay_label);
    mdkr_sha256_update(&hash, source_commit, 40u);
    mdkr_sha256_update(&hash, gameplay_contract,
                       sizeof(gameplay_contract) - 1u);
    mdkr_sha256_final(&hash, next.gameplay_digest);
    next.rom_revision = rom_revision;
    next.cadence_hz = rom_revision == 1u ? 30u : 25u;
    if (!mdkr_online_compatibility_valid(&next))
        return false;
    *output = next;
    return true;
}
