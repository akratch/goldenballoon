#ifndef MDKR_CUSTOM_TRACK_SECTION_H
#define MDKR_CUSTOM_TRACK_SECTION_H
#include "custom_track_table.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrCustomTrackSection MdkrCustomTrackSection;
typedef struct MdkrCustomTrackSession MdkrCustomTrackSession;
typedef enum MdkrCustomTrackSectionKind {
    MDKR_CUSTOM_TRACK_HEADERS,
    MDKR_CUSTOM_TRACK_OBJECT_MAPS,
    MDKR_CUSTOM_TRACK_NAMES,
    MDKR_CUSTOM_TRACK_MODELS,
    MDKR_CUSTOM_TRACK_TEXTURES,
    MDKR_CUSTOM_TRACK_SECTION_COUNT
} MdkrCustomTrackSectionKind;
typedef struct MdkrCustomTrackPayload {
    const void *bytes;
    uint32_t size;
} MdkrCustomTrackPayload;

typedef struct MdkrCustomTrackSectionInput {
    MdkrCustomTrackSectionKind kind;
    const uint32_t *retail;
    size_t retail_words;
    const MdkrCustomTrackPayload *entries;
    size_t entry_count;
} MdkrCustomTrackSectionInput;

/* All-or-nothing private construction; limits are aggregate across sections.
 * Duplicate/unknown kinds are invalid. This does not install a global session
 * or synchronize readers: the engine must publish at a quiescent boundary. */
MdkrCustomTrackSession *mdkr_custom_track_session_create(
    const MdkrCustomTrackSectionInput *inputs, size_t count,
    size_t max_payload_bytes, size_t max_table_words);
void mdkr_custom_track_session_destroy(MdkrCustomTrackSession *session);
const MdkrCustomTrackSection *mdkr_custom_track_session_section(
    const MdkrCustomTrackSession *session, MdkrCustomTrackSectionKind kind);

/* Copies inputs; caller retains their ownership. Inputs must remain stable
 * during creation. Limits cover payload bytes and total output table words.
 * Failure publishes nothing. This validates ranges, NOT game asset formats. */
MdkrCustomTrackSection *mdkr_custom_track_section_create(
    const uint32_t *retail, size_t words,
    const MdkrCustomTrackPayload *entries, size_t count,
    size_t max_payload_bytes, size_t max_table_words);
void mdkr_custom_track_section_destroy(MdkrCustomTrackSection *section);
/* Logical byte extent, including the retail prefix. Fits the signed engine
 * loader range. NULL returns zero; this is not the resident allocation size. */
uint32_t mdkr_custom_track_section_size(const MdkrCustomTrackSection *section);
/* Exact normalized retail prefix identity, including original entry count.
 * The supplied table includes its logical end offset and sentinel, no padding. */
int mdkr_custom_track_section_matches_retail(const MdkrCustomTrackSection *section,
                                            const uint32_t *table, size_t words);
/* Reader returns nonzero only after the complete retail prefix was read.
 * Offsets are relative to the retail data section, never ROM addresses. */
typedef int (*MdkrCustomTrackRetailRead)(void *context, uint32_t offset,
                                       void *destination, uint32_t size);
/* Returns 1 for a complete logical read, 0 for invalid input or reader failure.
 * Invalid input never invokes reader or writes destination. Reader failure may
 * leave its prefix modified, but the custom suffix is not copied. No allocation.
 * The caller must retain the immutable section throughout callback execution. */
int mdkr_custom_track_section_load(const MdkrCustomTrackSection *section,
    int32_t offset, int32_t size, void *destination, size_t capacity,
    MdkrCustomTrackRetailRead reader, void *context);
/* Borrowed immutable table; valid until section destruction. */
const uint32_t *mdkr_custom_track_section_table(const MdkrCustomTrackSection *section,
                                               size_t *words);
/* RETAIL/INVALID leave destination untouched. Only RETAIL permits ROM dispatch.
 * Caller synchronizes destruction with all readers. */
MdkrCustomTrackSpan mdkr_custom_track_section_read(
    const MdkrCustomTrackSection *section, int32_t offset, int32_t size,
    void *destination, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
