#ifndef MDKR_CUSTOM_TRACK_MODEL_H
#define MDKR_CUSTOM_TRACK_MODEL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MDKR_TRACK_TEXTURE_LOCAL_BASE 0x7000u
#define MDKR_TRACK_MODEL_TEXTURES_MAX 255u
/* Read-only BE structural spans consumed by normalization/initial loading.
 * Includes batch sentinel and collision-facet storage. Does NOT validate
 * overlap, PVS/BSP links, indices, material semantics or runtime work budgets;
 * success alone must never authorize geometry publication. */
int mdkr_track_model_validate_spans(const void *model, size_t size);
/* Count nonzero authored hasWaves flags after structural span validation.
 * Matches the native 1P/2P activation census; 3P/4P activation is suppressed
 * by the caller. Not a wave geometry/batch validation. Output is unchanged
 * on failure and no authored flags are normalized in place. */
int mdkr_track_model_wave_count(const void *model, size_t size, uint32_t *count);
/* For wave-bearing models, reproduce the native first visible water-batch
 * selection and require a texture plus positive X/Z divisors from that segment.
 * Checks height-sampling vertices and segment grid coordinates against tile
 * storage and the native 64-row/32-bit masks. Includes index validation.
 * Not full grid allocation, subdivision/render-index or material
 * admission; success alone does not authorize wave initialization. */
int mdkr_track_model_wave_basis_valid(const void *model, size_t size);
typedef struct MdkrTrackWaveGrid {
    uint32_t segments, columns, rows, colour_slot; /* 0: none; 1–7: header slot. */
} MdkrTrackWaveGrid;
/* Same validation; no active waves yields a zero grid. Output unchanged on
 * failure. Counts describe storage, not an allocation authorization. */
int mdkr_track_model_wave_grid(const void *model, size_t size, MdkrTrackWaveGrid *out);
/* Every material-selected colour/pulse slot must be present in a supplied BE
 * header. No header is needed if no batch references lighting. Raw misc record
 * validity is a separate header-preparation obligation. Pulsing materials must
 * also select a texture; this does not establish its special split-image layout. */
int mdkr_track_model_lighting_links_valid(const void *model, size_t size,
    const void *header, size_t header_size);
typedef int (*MdkrTrackPulseTextureCheck)(void *context, uint32_t texture_id);
/* Check precisely the texture table slots selected by visible nonempty pulsing batches, once
 * per slot. Includes index validation and rejects textureless pulse batches.
 * IDs are serialized table values (relocate first for final session IDs).
 * Callback must validate the selected payload's pulse layout; it must not
 * mutate the model or publish state. Its private context may change on failure.
 * No callback is required when no pulsing material is present. */
int mdkr_track_model_pulse_textures_valid(const void *model, size_t size,
    MdkrTrackPulseTextureCheck check, void *context);
/* Includes span validation, then checks batch/sentinel ranges, material-table
 * indices and batch-relative triangle vertices. Not full graph/material or
 * runtime-work admission; see the remaining restrictions above. */
int mdkr_track_model_validate_indices(const void *model, size_t size);
/* Nonempty batches must fit one native draw: <=32 loaded vertices and <=16
 * triangles. Includes index validation. Empty batches are allowed and skipped
 * by the native renderer; no aggregate per-frame command budget is implied. */
int mdkr_track_model_validate_draw_batches(const void *model, size_t size);
/* Includes index checks; validates PVS row spans and an acyclic BSP partition
 * rooted at node zero, with exactly one leaf per segment. Still not full
 * geometry/material/work-budget admission. */
int mdkr_track_model_validate_visibility(const void *model, size_t size);
/* Includes visibility checks and rejects aliased typed regions that endian
 * normalization would mutate repeatedly or reinterpret. PVS rows may share
 * storage with other PVS rows, but not typed geometry. */
int mdkr_track_model_validate_regions(const void *model, size_t size);
/* Includes region checks and verifies authored face-plane identity and initial
 * edge references against the native builder's first-pass plane inventory. */
int mdkr_track_model_validate_collision(const void *model, size_t size);
/* Conservative model-arena extent: aligned triangle indices, at most four
 * 16-byte collision planes per face, and one s16 work index per batch.
 * Includes region checks. Leaves *required unchanged on failure. This does
 * not account for separate wave/render allocations or prove facet semantics. */
int mdkr_track_model_collision_budget(const void *model, size_t size, size_t *required);
/* Decoded, still-big-endian LevelModel only; NOT a compressed payload.
 * Map exporter-local texture ordinals to final session IDs in manifest order.
 * Ordinary references must address the retail prefix; mapped references must
 * address the appended inventory and fit load_texture's 15-bit index domain.
 * Checks the model extent/table and every reference before writing anything.
 * Failure preserves all bytes. This is NOT geometry or texture admission;
 * segments, batches, collision, and referenced payloads need separate checks.
 * Caller owns stable input memory for this call; no global publication occurs.
 */
int mdkr_track_model_relocate_textures(void *model, size_t size,
    const uint32_t *texture_ids, size_t texture_count,
    uint32_t retail_count, uint32_t final_count);
#ifdef __cplusplus
}
#endif
#endif
