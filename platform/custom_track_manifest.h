#ifndef MDKR_CUSTOM_TRACK_MANIFEST_H
#define MDKR_CUSTOM_TRACK_MANIFEST_H
#include "custom_track_section.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MDKR_TRACK_MANIFEST_BYTES_MAX (256u * 1024u)
#define MDKR_TRACK_ADDS_MAX 256u
#define MDKR_TRACK_PATH_MAX 256u
typedef enum MdkrTrackMapSlot {
    MDKR_TRACK_MAP_NONE, MDKR_TRACK_MAP_STRUCTURE, MDKR_TRACK_MAP_COLLECTABLES
} MdkrTrackMapSlot;
typedef struct MdkrTrackAddition {
    MdkrCustomTrackSectionKind section;
    MdkrTrackMapSlot slot;
    char file[MDKR_TRACK_PATH_MAX];
} MdkrTrackAddition;
typedef struct MdkrTrackManifest {
    char id[96], name[256], author[256];
    char hd_file[MDKR_TRACK_PATH_MAX], hd_digest[128];
    size_t count;
    MdkrTrackAddition adds[MDKR_TRACK_ADDS_MAX];
} MdkrTrackManifest;
/* Owned metadata only: does not read payloads, install, relocate or launch.
 * Unknown fields are syntax-validated and skipped. Known duplicates fail.
 * Returns NULL on malformed/unsupported input, budget or allocation failure. */
MdkrTrackManifest *mdkr_track_manifest_parse(const void *bytes, size_t size);
void mdkr_track_manifest_destroy(MdkrTrackManifest *manifest);
#ifdef __cplusplus
}
#endif
#endif
