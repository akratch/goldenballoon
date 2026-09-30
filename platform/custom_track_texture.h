#ifndef MDKR_CUSTOM_TRACK_TEXTURE_H
#define MDKR_CUSTOM_TRACK_TEXTURE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MdkrTrackTextureInfo {
    unsigned frames;
    unsigned translucent_frames;
    size_t meaningful_bytes;
    /* Native data+alignment+display-list allocation, not GPU residency/cache
     * overhead or temporary compressed-input/scratch storage. */
    size_t runtime_allocation_bytes;
} MdkrTrackTextureInfo;
typedef enum MdkrTrackTextureResult {
    MDKR_TRACK_TEXTURE_OK,
    MDKR_TRACK_TEXTURE_INVALID,
    MDKR_TRACK_TEXTURE_PALETTE_REQUIRED,
    MDKR_TRACK_TEXTURE_MATERIAL_LIMIT
} MdkrTrackTextureResult;
/* Decoded BE frame records only. expected_frames comes from the outer asset
 * header for compressed assets. Reuses the native frame-boundary contract,
 * checks actual texel bytes and fixed-stride animation addressability.
 * Palette-backed formats need a separate palette admission path; they are not
 * accepted as self-contained artwork. Output is unchanged unless successful.
 * This is not compressed-stream validation or a renderer allocation budget. */
MdkrTrackTextureResult mdkr_track_texture_frames(const void *bytes, size_t size,
    unsigned expected_frames, MdkrTrackTextureInfo *out);
/* Complete serialized texture entry. Compressed entries decode into disjoint
 * caller-owned scratch with an independent capacity, then validate frame data.
 * Raw entries require no scratch. The native 40-byte header peek must fit.
 * Output is unchanged on failure; scratch may change. No publication occurs. */
MdkrTrackTextureResult mdkr_track_texture_payload(const void *bytes, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out);
/* Authored level-material profile on top of complete payload validation:
 * each frame has power-of-two sides <=64 and at most 4096 texel bytes.
 * Matches the pinned beta.11 exporter's single-block texture/wrap limits,
 * extended to validate every animated frame. Does not restrict generic retail
 * validation or prove renderer output/HD replacement identity. Output remains
 * unchanged on failure; compressed scratch may change. */
MdkrTrackTextureResult mdkr_track_texture_authored_payload(const void *bytes, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out);
/* Specialized blinking-light material: every frame contains equal RGBA16
 * halves, whole 64-bit rows, and at most 4096 texel bytes. Same contracts as
 * authored_payload. Call only for textures selected by pulsing batches; this
 * does not establish the model-to-texture relationship itself. */
MdkrTrackTextureResult mdkr_track_texture_pulse_payload(const void *bytes, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out);
#ifdef __cplusplus
}
#endif
#endif
