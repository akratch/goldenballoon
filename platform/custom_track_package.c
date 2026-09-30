#include "custom_track_package.h"
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#ifdef MDKR_TRACK_PACKAGE_TEST_ALLOCATOR
void *mdkr_track_test_malloc(size_t size);
void *mdkr_track_test_calloc(size_t count, size_t size);
void mdkr_track_test_free(void *pointer);
#define malloc mdkr_track_test_malloc
#define calloc mdkr_track_test_calloc
#define free mdkr_track_test_free
#endif

struct MdkrTrackPackage {
    MdkrTrackManifest *manifest;
    unsigned char *bytes;
    size_t offsets[MDKR_TRACK_ADDS_MAX];
    uint32_t sizes[MDKR_TRACK_ADDS_MAX];
    unsigned char digest[MDKR_SHA256_DIGEST_SIZE];
};

static void digest_u32(MdkrSha256 *hash, uint32_t value) {
    const unsigned char bytes[4] = {value >> 24, value >> 16, value >> 8, value};
    mdkr_sha256_update(hash, bytes, sizeof bytes);
}

static void digest_string(MdkrSha256 *hash, const char *value) {
    size_t size = strlen(value);
    digest_u32(hash, (uint32_t)size);
    mdkr_sha256_update(hash, value, size);
}

static void package_digest(MdkrTrackPackage *package) {
    _Static_assert(MDKR_CUSTOM_TRACK_HEADERS == 0 && MDKR_CUSTOM_TRACK_OBJECT_MAPS == 1 &&
        MDKR_CUSTOM_TRACK_NAMES == 2 && MDKR_CUSTOM_TRACK_MODELS == 3 && MDKR_CUSTOM_TRACK_TEXTURES == 4 &&
        MDKR_TRACK_MAP_NONE == 0 && MDKR_TRACK_MAP_STRUCTURE == 1 && MDKR_TRACK_MAP_COLLECTABLES == 2,
        "package digest v1 wire tags changed; version the identity schema");
    static const char domain[] = "GoldenBalloon.TrackPackage.v1";
    const MdkrTrackManifest *manifest = package->manifest;
    MdkrSha256 hash;
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, domain, sizeof domain); /* Includes domain NUL. */
    digest_string(&hash, manifest->id);
    digest_string(&hash, manifest->name);
    digest_string(&hash, manifest->author);
    digest_string(&hash, manifest->hd_file);
    digest_string(&hash, manifest->hd_digest);
    digest_u32(&hash, (uint32_t)manifest->count);
    for (size_t i = 0; i < manifest->count; ++i) {
        digest_u32(&hash, (uint32_t)manifest->adds[i].section);
        digest_u32(&hash, (uint32_t)manifest->adds[i].slot);
        digest_string(&hash, manifest->adds[i].file);
        digest_u32(&hash, package->sizes[i]);
        mdkr_sha256_update(&hash, package->bytes + package->offsets[i], package->sizes[i]);
    }
    mdkr_sha256_final(&hash, package->digest);
}

int mdkr_track_package_digest(const MdkrTrackPackage *package, unsigned char out[32]) {
    if (!package || !out) return 0;
    memcpy(out, package->digest, sizeof package->digest);
    return 1;
}

void mdkr_track_package_destroy(MdkrTrackPackage *package) {
    if (!package) return;
    mdkr_track_manifest_destroy(package->manifest);
    free(package->bytes);
    free(package);
}

const MdkrTrackManifest *mdkr_track_package_manifest(const MdkrTrackPackage *package) {
    return package ? package->manifest : NULL;
}

int mdkr_track_package_payload(const MdkrTrackPackage *package, size_t index,
    MdkrCustomTrackPayload *out) {
    if (!package || !out || index >= package->manifest->count) return 0;
    *out = (MdkrCustomTrackPayload){package->bytes + package->offsets[index], package->sizes[index]};
    return 1;
}

MdkrTrackPackage *mdkr_track_package_read(MdkrTrackPackageRead read, void *context,
    MdkrTrackPackageLimits limits, MdkrTrackPackageError *error) {
    MdkrTrackPackageError result = MDKR_TRACK_PACKAGE_ARGUMENT;
    MdkrTrackPackage *package = NULL;
    void *manifest_bytes = NULL;
    size_t manifest_size = 0, actual = 0, total = 0;
    if (!read || !limits.per_payload_bytes || !limits.total_payload_bytes) goto fail;
    result = MDKR_TRACK_PACKAGE_MANIFEST;
    if (read(context, "manifest.json", NULL, 0, &manifest_size) != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL ||
        !manifest_size || manifest_size > MDKR_TRACK_MANIFEST_BYTES_MAX) goto fail;
    result = MDKR_TRACK_PACKAGE_MEMORY;
    manifest_bytes = malloc(manifest_size);
    package = calloc(1, sizeof *package);
    if (!manifest_bytes || !package) goto fail;
    result = MDKR_TRACK_PACKAGE_READ;
    if (read(context, "manifest.json", manifest_bytes, manifest_size, &actual) != MDKR_MOD_SOURCE_OK ||
        actual != manifest_size) goto fail;
    result = MDKR_TRACK_PACKAGE_MANIFEST;
    package->manifest = mdkr_track_manifest_parse(manifest_bytes, actual);
    if (!package->manifest) goto fail;
    free(manifest_bytes);
    manifest_bytes = NULL;
    /* Measure every entry before allocating or reading payload data. */
    for (size_t i = 0; i < package->manifest->count; ++i) {
        size_t size = 0;
        result = MDKR_TRACK_PACKAGE_READ;
        if (read(context, package->manifest->adds[i].file, NULL, 0, &size) != MDKR_MOD_SOURCE_BUFFER_TOO_SMALL)
            goto fail;
        result = MDKR_TRACK_PACKAGE_BUDGET;
        if (!size || size > INT32_MAX || size > limits.per_payload_bytes ||
            size > limits.total_payload_bytes - total) goto fail;
        package->offsets[i] = total;
        package->sizes[i] = (uint32_t)size;
        total += size;
    }
    result = MDKR_TRACK_PACKAGE_MEMORY;
    package->bytes = malloc(total);
    if (!package->bytes) goto fail;
    for (size_t i = 0; i < package->manifest->count; ++i) {
        actual = 0;
        result = MDKR_TRACK_PACKAGE_READ;
        if (read(context, package->manifest->adds[i].file,
                 package->bytes + package->offsets[i], package->sizes[i], &actual) != MDKR_MOD_SOURCE_OK ||
            actual != package->sizes[i]) goto fail;
    }
    package_digest(package);
    if (error) *error = MDKR_TRACK_PACKAGE_OK;
    return package;
fail:
    free(manifest_bytes);
    mdkr_track_package_destroy(package);
    if (error) *error = result;
    return NULL;
}

static int source_read(void *context, const char *path, void *bytes, size_t capacity, size_t *size) {
    return mdkr_mod_source_read(context, path, bytes, capacity, size);
}

MdkrTrackPackage *mdkr_track_package_from_source(MdkrModSource *source,
    MdkrTrackPackageLimits limits, MdkrTrackPackageError *error) {
    return mdkr_track_package_read(source ? source_read : NULL, source, limits, error);
}
