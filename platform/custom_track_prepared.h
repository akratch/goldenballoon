#ifndef MDKR_CUSTOM_TRACK_PREPARED_H
#define MDKR_CUSTOM_TRACK_PREPARED_H
#include "custom_track_package_admission.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MdkrPreparedCourse MdkrPreparedCourse;
typedef struct MdkrTrackPreparation {
    uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    uint32_t retail_misc_count, retail_2d_count;
    size_t object_id_count;
    size_t engine_arena_bytes, wave_storage_limit, texture_runtime_limit;
    size_t decoded_map_limit, owned_payload_limit;
    void *scratch;
    size_t scratch_capacity;
    MdkrTrackMaterialAdmission materials;
    MdkrTrackMiscRead read_misc;
    MdkrTrackModelRead read_model;
    MdkrTrackObjectMapRead read_map;
    void *read_context;
    MdkrTrackObjectMapCheck check_map;
    void *check_context;
} MdkrTrackPreparation;
typedef struct MdkrPreparedCourseEntry {
    MdkrCustomTrackSectionKind section;
    uint32_t id;
    MdkrCustomTrackPayload payload;
} MdkrPreparedCourseEntry;
/* All-or-nothing owned staging. Inputs/callback payloads and the two scratch
 * regions must be disjoint and stable during the call. The private map-check
 * context may change on failure and must be discarded with the candidate.
 * Result retains no package, callbacks or scratch pointers. Payload allocation
 * capacities (not allocator metadata) are bounded by owned_payload_limit.
 * No session publication or disk writes. Existing semantic admission is
 * composed here; this is NOT complete gameplay/renderer/netplay qualification. */
MdkrPreparedCourse *mdkr_track_prepare_course(const MdkrTrackPackage *package,
    const MdkrTrackPreparation *preparation);
void mdkr_track_prepared_destroy(MdkrPreparedCourse *course);
const MdkrTrackCourseLayout *mdkr_track_prepared_layout(const MdkrPreparedCourse *course);
size_t mdkr_track_prepared_entry_count(const MdkrPreparedCourse *course);
/* Borrowed immutable bytes live until destruction; output unchanged on failure. */
int mdkr_track_prepared_entry(const MdkrPreparedCourse *course, size_t index,
    MdkrPreparedCourseEntry *out);
int mdkr_track_prepared_digest(const MdkrPreparedCourse *course, unsigned char out[32]);
typedef struct MdkrTrackRetailTable {
    const uint32_t *words;
    size_t count; /* Exact normalized offsets, end offset and sentinel. */
} MdkrTrackRetailTable;
/* Courses must be prepared consecutively against this retail inventory, in
 * the supplied order. Rejects stale IDs, duplicate package IDs/content keys,
 * and mismatched inventories. Copies all bytes into a new immutable session;
 * callers retain course ownership and may destroy them after success.
 * Limits cover session payload bytes/table words, not still-live courses,
 * temporary descriptors or allocator overhead. No engine publication occurs.
 * Matching inventory sizes is NOT proof of ROM-content identity; the engine
 * must also verify the actual active-ROM tables before attaching the result. */
MdkrCustomTrackSession *mdkr_track_prepared_session_create(
    const MdkrPreparedCourse *const *courses, size_t count,
    const MdkrTrackRetailTable retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    size_t max_payload_bytes, size_t max_table_words);
#ifdef __cplusplus
}
#endif
#endif
