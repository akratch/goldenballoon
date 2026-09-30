#ifndef MDKR_CUSTOM_TRACK_MODEL_PAYLOAD_H
#define MDKR_CUSTOM_TRACK_MODEL_PAYLOAD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private staging operation, not full geometry admission or publication.
 * Decode an rzip model, relocate its texture IDs, and recompress with raw
 * DEFLATE. Structural, index, visibility and overlap checks plus the conservative
 * collision extent must pass first. Separate collision-facet semantics,
 * material payloads and wave/render allocations still need admission.
 * The independent caller capacities bound both byte buffers. The
 * engine arena budget must leave decoded bytes below the 16-byte-aligned
 * compressed staging region; overlapping engine inflate regions are refused.
 * Input, scratch and output must be disjoint and stable during the call.
 * Both result pointers are required and must be disjoint from all buffers and
 * each other. Success returns the exact relocated model extent in scratch via
 * *decoded_size, not scratch capacity. On failure both sizes are zero;
 * scratch/output may be modified and MUST NOT
 * be published. No ownership transfer or global state changes occur. */
int mdkr_track_model_repack(const void *input, size_t input_size,
    void *scratch, size_t scratch_capacity, void *output, size_t output_capacity,
    const uint32_t *texture_ids, size_t texture_count,
    uint32_t retail_count, uint32_t final_count, size_t engine_arena_bytes,
    size_t *written, size_t *decoded_size);
#ifdef __cplusplus
}
#endif
#endif
