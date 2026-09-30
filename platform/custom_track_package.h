#ifndef MDKR_CUSTOM_TRACK_PACKAGE_H
#define MDKR_CUSTOM_TRACK_PACKAGE_H
#include "custom_track_manifest.h"
#include "mod_source.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MdkrTrackPackage MdkrTrackPackage;
typedef struct MdkrTrackPackageLimits {
    size_t per_payload_bytes;
    size_t total_payload_bytes;
} MdkrTrackPackageLimits;
typedef enum MdkrTrackPackageError {
    MDKR_TRACK_PACKAGE_OK,
    MDKR_TRACK_PACKAGE_ARGUMENT,
    MDKR_TRACK_PACKAGE_MANIFEST,
    MDKR_TRACK_PACKAGE_READ,
    MDKR_TRACK_PACKAGE_BUDGET,
    MDKR_TRACK_PACKAGE_MEMORY
} MdkrTrackPackageError;
/* Same two-pass contract as mdkr_mod_source_read. A reader must honor capacity.
 * Useful for controlled failure/size-change fixtures; production uses a pinned
 * MdkrModSource. No callbacks occur after construction returns. */
typedef int (*MdkrTrackPackageRead)(void *context, const char *path,
    void *bytes, size_t capacity, size_t *size);
MdkrTrackPackage *mdkr_track_package_read(MdkrTrackPackageRead read, void *context,
    MdkrTrackPackageLimits limits, MdkrTrackPackageError *error);
MdkrTrackPackage *mdkr_track_package_from_source(MdkrModSource *source,
    MdkrTrackPackageLimits limits, MdkrTrackPackageError *error);
/* This owns bytes, not semantic admission. Do not install a loader session or
 * execute these assets until geometry, references and gameplay are validated.
 * Changes in size between measurement and reading fail. Same-size mutations
 * and changes across files are not detected: no atomic source snapshot claim.
 * Companion HD metadata is retained but never authorizes sibling file reads. */
const MdkrTrackManifest *mdkr_track_package_manifest(const MdkrTrackPackage *package);
/* Versioned SHA-256 of parsed metadata and exact owned payloads in manifest
 * order. Independent of JSON whitespace/key order and container path/encoding.
 * Includes all recognized metadata and addition paths/roles. Compressed bytes
 * are hashed as supplied, not normalized. This is a package content key, NOT
 * authenticity, semantic admission or a complete netplay compatibility key
 * (retail ROM, engine schema and simulation configuration remain separate).
 * Copies 32 bytes; invalid arguments leave output unchanged. */
int mdkr_track_package_digest(const MdkrTrackPackage *package, unsigned char out[32]);
int mdkr_track_package_payload(const MdkrTrackPackage *package, size_t index,
    MdkrCustomTrackPayload *out);
void mdkr_track_package_destroy(MdkrTrackPackage *package);
#ifdef __cplusplus
}
#endif
#endif
