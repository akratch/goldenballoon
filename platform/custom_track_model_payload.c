#include "custom_track_model_payload.h"
#include "custom_track_model.h"
#include "custom_track_decode.h"
#include "miniz.h"
#include <limits.h>

int mdkr_track_model_repack(const void *input, size_t input_size,
    void *scratch, size_t scratch_capacity, void *output, size_t output_capacity,
    const uint32_t *texture_ids, size_t texture_count,
    uint32_t retail_count, uint32_t final_count, size_t engine_arena_bytes,
    size_t *written, size_t *decoded_size) {
    size_t decoded = 0, collision_required = 0;
    if (written) *written = 0;
    if (decoded_size) *decoded_size = 0;
    if (!written || !decoded_size) return 0;
    if (!output || output_capacity <= 5 || engine_arena_bytes > INT32_MAX ||
        !mdkr_custom_track_decode(input, input_size, scratch, scratch_capacity, &decoded) ||
        decoded > engine_arena_bytes ||
        !mdkr_track_model_collision_budget(scratch, decoded, &collision_required) ||
        collision_required > engine_arena_bytes ||
        !mdkr_track_model_relocate_textures(scratch, decoded, texture_ids,
            texture_count, retail_count, final_count)) return 0;
    unsigned char *bytes = output;
    size_t capacity = output_capacity < engine_arena_bytes ? output_capacity : engine_arena_bytes;
    if (capacity <= 5) return 0;
    const size_t compressed = tdefl_compress_mem_to_mem(bytes + 5, capacity - 5,
        scratch, decoded, TDEFL_DEFAULT_MAX_PROBES);
    if (!compressed) return 0;
    const size_t total = compressed + 5;
    /* Mirrors track_init_level_model's aligned tail staging location. */
    if (decoded > ((engine_arena_bytes - total) & ~(size_t)15)) return 0;
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(decoded >> (8 * i));
    bytes[4] = ((const unsigned char *)input)[4];
    *written = total;
    *decoded_size = decoded;
    return 1;
}
