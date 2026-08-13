#include "platform/online/compatibility_identity.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int bytes_equal_hex(const uint8_t *bytes, size_t count,
                           const char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < count; index++) {
        if (hex[index * 2u] != digits[bytes[index] >> 4u] ||
            hex[index * 2u + 1u] != digits[bytes[index] & 15u])
            return 0;
    }
    return hex[count * 2u] == '\0';
}

int main(void) {
    static const char commit[] = "0123456789abcdef0123456789abcdef01234567";
    MdkrOnlineCompatibilityV1 compatibility;
    MdkrOnlineCompatibilityV1 untouched;

    memset(&compatibility, 0xa5, sizeof(compatibility));
    untouched = compatibility;
    expect(mdkr_online_compatibility_from_provenance("1.3.0", commit, false, 1u,
                                                     &compatibility),
           "clean semver, commit and supported ROM derive compatibility");
    expect(compatibility.protocol_version == 1u &&
               compatibility.rom_revision == 1u &&
               compatibility.cadence_hz == 30u &&
               bytes_equal_hex(compatibility.build_id,
                               sizeof(compatibility.build_id),
                               "336892f0abf0a2c25c91a7ebcb266364") &&
               bytes_equal_hex(compatibility.gameplay_digest,
                               sizeof(compatibility.gameplay_digest),
                               "fb34ef8ddfcf782a852375b8ce71d1bcd552a185cf2c18f"
                               "e1e7727996b749522"),
           "native bytes equal the browser provenance vector exactly");
    expect(mdkr_online_compatibility_from_provenance("1.3.0", commit, false, 2u,
                                                     &compatibility) &&
               compatibility.rom_revision == 2u &&
               compatibility.cadence_hz == 25u &&
               bytes_equal_hex(compatibility.build_id,
                               sizeof(compatibility.build_id),
                               "336892f0abf0a2c25c91a7ebcb266364"),
           "PAL changes only ROM identity and authored cadence");

    compatibility = untouched;
    expect(!mdkr_online_compatibility_from_provenance("1.3.0", commit, true, 1u,
                                                      &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "dirty provenance fails atomically");
    expect(!mdkr_online_compatibility_from_provenance("1.3", commit, false, 1u,
                                                      &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "v1.3.0", commit, false, 1u, &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "1.3.0-dev", commit, false, 1u, &compatibility),
           "only the published three-component version grammar is accepted");
    expect(!mdkr_online_compatibility_from_provenance(
               "12345678901234567890123456789.0.0", commit, false, 1u,
               &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "published version identity is bounded to 32 bytes cross-platform");
    expect(!mdkr_online_compatibility_from_provenance(
               "1.3.0", "0123456789ABCDEF0123456789ABCDEF01234567", false, 1u,
               &compatibility) &&
               !mdkr_online_compatibility_from_provenance(
                   "1.3.0", "01234567", false, 1u, &compatibility),
           "commit must be exact lowercase forty-character hex");
    expect(!mdkr_online_compatibility_from_provenance("1.3.0", commit, false,
                                                      3u, &compatibility) &&
               memcmp(&compatibility, &untouched, sizeof(compatibility)) == 0,
           "unsupported ROM revision fails without output mutation");

    if (failures != 0)
        return 1;
    puts("online compatibility identity: PASS (native/browser exact vector)");
    return 0;
}
