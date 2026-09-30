#ifndef MDKR_CUSTOM_TRACK_PACKAGE_ADMISSION_H
#define MDKR_CUSTOM_TRACK_PACKAGE_ADMISSION_H
#include "custom_track_package.h"
#include "custom_track_texture.h"
#include "custom_track_header.h"
#include "custom_track_model.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MdkrTrackTextureBudget {
    size_t entries;
    size_t runtime_bytes;
} MdkrTrackTextureBudget;
/* Return nonzero with a borrowed, still-BE retail misc record. Bytes remain
 * readable until the next callback; caller retains ownership. No callback may
 * mutate package, inventory, or output storage during preparation. */
typedef int (*MdkrTrackMiscRead)(void *context, uint32_t id, MdkrCustomTrackPayload *out);
/* Borrow one compressed retail model by immutable retail index. Bytes must
 * remain stable during this call; never resolve an appended/custom model ID. */
typedef int (*MdkrTrackModelRead)(void *context, uint32_t id, MdkrCustomTrackPayload *out);
/* Borrow a complete retail 3D texture entry. Package textures are resolved
 * internally by their newly assigned IDs, never through this callback. */
typedef int (*MdkrTrackTextureRead)(void *context, uint32_t id, MdkrCustomTrackPayload *out);
typedef struct MdkrTrackMaterialAdmission {
    MdkrTrackTextureRead read_retail;
    void *context;
    /* Disjoint from model scratch, output, package and borrowed texture data.
     * May change on failure. Raw textures do not require decode scratch. */
    void *scratch;
    size_t scratch_capacity;
} MdkrTrackMaterialAdmission;
typedef struct MdkrTrackPackageLayout {
    uint32_t counts[MDKR_CUSTOM_TRACK_SECTION_COUNT];
    uint32_t addition_ids[MDKR_TRACK_ADDS_MAX];
    uint32_t texture_ids[MDKR_TRACK_MODEL_TEXTURES_MAX];
    size_t texture_count;
    MdkrTrackHeaderRelocation header;
} MdkrTrackPackageLayout;
/* Plan physical additions against a caller's current inventories. Limits are
 * maximum entry counts, not maximum IDs; the caller supplies engine/menu
 * limits (this helper additionally enforces signed-16 model/map references
 * and 15-bit texture IDs). Keep retail counts fixed across sequential plans.
 * Does not synthesize names, align header/name inventories, detect duplicate
 * course identities or validate payloads. Missing replacement roles retain
 * authored retail references. No allocation/publication; failure preserves out.
 * Input arrays may alias out: all inputs are consumed before output is stored. */
int mdkr_track_package_layout(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    MdkrTrackPackageLayout *out);
typedef struct MdkrTrackCourseLayout {
    MdkrTrackPackageLayout assets;
    uint32_t course_id;
    int synthesize_name;
} MdkrTrackCourseLayout;
/* Course-level planning requires one header and aligned retail/current header
 * and name inventories. Reserves a synthesized name when absent, using the
 * same course ID. Does not validate payloads or repair already skewed tables.
 * Failure preserves out; consume prepare_name for the reserved name entry. */
int mdkr_track_package_course_layout(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    MdkrTrackCourseLayout *out);
/* Stage the package's sole header with freshly planned model/map references.
 * Lighting/pulse-light IDs must be absent (-1) or inside retail_misc_count;
 * each referenced record must also pass its colour/pulse admission check.
 * Package bytes stay immutable; trailing exporter bytes are preserved. No
 * allocation or publication. Failure sets *written to zero and preserves the
 * output buffer. Output must be disjoint from package and argument storage.
 * This is reference preparation, NOT semantic header/course admission. */
int mdkr_track_package_prepare_header(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    uint32_t retail_misc_count, uint32_t retail_2d_count,
    MdkrTrackMiscRead read_misc, void *misc_context,
    void *output, size_t capacity, size_t *written);
/* Prepare the sole model using fresh manifest-order texture IDs. Reuses the
 * bounded model decoder/admission/repacker and its engine-arena budget.
 * Package, scratch, output and result storage must be disjoint. On failure
 * *written is zero; scratch/output may change and must not be published.
 * Wave-bearing models require a supplied header. Peak requested wave pool bytes
 * (base, grid, generators, height map and concurrent setup scratch) must fit
 * wave_storage_limit in both 1P and 2P (alternative modes, not summed).
 * Their separate wave texture must address the fixed retail 2D/3D inventory.
 * Visible nonempty pulsing batches require materials and validated selected
 * texture payloads; appended IDs resolve only to this package's own additions.
 * Ordinary texture payloads, aggregate texture budgets, allocator metadata,
 * renderer storage and full course semantics remain separate obligations.
 * Uses the target ABI's real runtime structure sizes. */
int mdkr_track_package_prepare_model(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    uint32_t retail_2d_count, const MdkrTrackMaterialAdmission *materials,
    void *scratch, size_t scratch_capacity, void *output, size_t capacity,
    size_t engine_arena_bytes, size_t wave_storage_limit, size_t *written);
/* Header-only course geometry admission: resolve its retained retail model,
 * bounded-decode and validate geometry/retail texture references, compressed
 * staging/collision arena and composed header lighting/wave requirements.
 * Requires a header and no supplied model. Does not rewrite the retail asset
 * or publish anything. Package, callback payload and scratch must be disjoint
 * and stable throughout the call. Scratch may change on failure. Header reference/raw
 * misc admission, ordinary material payloads and complete course semantics are
 * separate obligations. Pulsing batches require materials and valid selected
 * retail payloads; success alone does not authorize execution. */
int mdkr_track_package_retained_model_valid(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT], uint32_t retail_2d_count,
    const MdkrTrackMaterialAdmission *materials,
    MdkrTrackModelRead read_model, void *context, void *scratch, size_t scratch_capacity,
    size_t engine_arena_bytes, size_t wave_storage_limit);
/* Validate every supplied LEVEL_NAMES entry with the native four-language
 * bounded lookup. Missing name additions are not synthesized here and do not
 * establish header/name inventory alignment. Legacy byte encoding is preserved. */
int mdkr_track_package_names_valid(const MdkrTrackPackage *package);
typedef enum MdkrTrackNameSource {
    MDKR_TRACK_NAME_REFUSED,
    MDKR_TRACK_NAME_PAYLOAD,
    MDKR_TRACK_NAME_METADATA,
    MDKR_TRACK_NAME_IDENTIFIER
} MdkrTrackNameSource;
/* Prepare one native four-NUL-terminated-string entry. Supplied localized bytes
 * take precedence and are preserved exactly (including trailing bytes); invalid
 * supplied data refuses rather than falling back. Without a payload, repeat a
 * nonempty printable-ASCII metadata name in all four languages. For empty,
 * control-containing or non-ASCII metadata, use the ASCII package identifier;
 * UTF-8 metadata remains intact for the library UI, not interpreted as legacy
 * font bytes. The return value exposes this fallback to the caller.
 * Output/result/package storage must be disjoint. Failure preserves output and
 * zeroes *written. This does not align inventories or publish a course. */
MdkrTrackNameSource mdkr_track_package_prepare_name(const MdkrTrackPackage *package,
    void *output, size_t capacity, size_t *written);
typedef struct MdkrTrackObjectMapBudget {
    size_t entries, records, decoded_bytes;
} MdkrTrackObjectMapBudget;
/* Borrowed still-BE entry stream (without the map header). Called only after
 * structural validation. Must not mutate package, scratch or result storage.
 * Production adapters must use initialized game-thread behavior tables. */
typedef int (*MdkrTrackObjectMapCheck)(void *context, const void *entries, size_t size);
typedef int (*MdkrTrackObjectMapRead)(void *context, uint32_t id, MdkrCustomTrackPayload *out);
/* Validate both final header-selected maps, structure then collectables, just
 * like init_track. The reader must resolve IDs against the private prepared
 * course inventory, with retail provenance already checked by relocation.
 * Repeated IDs are intentionally visited twice: the engine spawns twice.
 * Budget/output are atomic; private check context may change before failure
 * and must be discarded with the candidate. Borrowed payloads, header, scratch
 * and output must be disjoint and stable for each decode. No publication. */
int mdkr_track_course_object_maps_admit(const void *header, size_t header_size,
    uint32_t map_count, MdkrTrackObjectMapRead read_map, void *read_context,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *check_context,
    MdkrTrackObjectMapBudget *out);
/* Package-aware final map-pair admission: privately relocate the supplied
 * header and resolve each selected map either from this package's additions
 * or the immutable retail prefix. Prior custom-course IDs are never accepted
 * as retained references. Header scalar/misc and full gameplay admission are
 * separate. Same scratch/context/output contracts as the course-level check. */
int mdkr_track_package_course_maps_admit(const MdkrTrackPackage *package,
    const uint32_t retail[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t current[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    const uint32_t limits[MDKR_CUSTOM_TRACK_SECTION_COUNT],
    MdkrTrackObjectMapRead read_retail, void *read_context,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *check_context,
    MdkrTrackObjectMapBudget *out);
/* Decode every object map into reusable caller scratch and validate record
 * boundaries/translation IDs. Atomic output, bounded aggregate decoded bytes.
 * Package, scratch and result must be disjoint; scratch may change on failure.
 * Each decode is capped by both scratch capacity and remaining aggregate budget.
 * Does not prove behavior-specific sizes, checkpoints, AI or spawn semantics. */
int mdkr_track_package_object_maps_valid(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapBudget *out);
/* Additionally requires the caller's behavior validator for each map, including
 * empty maps. Missing/refusing callback fails without publishing result counts.
 * Callback context may retain earlier successful calls if a later map fails;
 * use private course staging state and discard it on package refusal. This
 * visits supplied maps only: retained retail maps must also enter any shared
 * course-wide identity/topology validator before publication.
 * Does not imply checkpoint/AI/spawn-field semantic acceptance. */
int mdkr_track_package_object_maps_admit(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t object_id_count,
    size_t decoded_limit, MdkrTrackObjectMapCheck check, void *context,
    MdkrTrackObjectMapBudget *out);
/* Authored level-material texture admission (including wrap/TMEM profile),
 * not authorization to publish the entire package.
 * Reuses caller-owned decode scratch sequentially. Every texture is inspected
 * in manifest order and counted even when payload paths repeat. Output is
 * unchanged on refusal. Budget excludes GPU/cache overhead and decode scratch.
 * Return 1 on success, 0 on invalid/unsupported artwork or aggregate overflow. */
int mdkr_track_package_texture_budget(const MdkrTrackPackage *package,
    void *scratch, size_t scratch_capacity, size_t runtime_limit,
    MdkrTrackTextureBudget *out);
#ifdef __cplusplus
}
#endif
#endif
