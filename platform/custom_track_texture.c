#include "custom_track_texture.h"
#include "runtime_contracts.h"
#include "custom_track_decode.h"

static unsigned be16(const unsigned char *p) {
    return (unsigned)p[0] << 8 | p[1];
}

static const unsigned format_bits[] = {32, 16, 8, 4, 16, 8, 4};

MdkrTrackTextureResult mdkr_track_texture_frames(const void *data, size_t size,
    unsigned expected_frames, MdkrTrackTextureInfo *out) {
    const unsigned char *bytes = data;
    if (!bytes || !out || size < 32 || !expected_frames || expected_frames > 255 ||
        bytes[0x12] != expected_frames) return MDKR_TRACK_TEXTURE_INVALID;
    const unsigned stride = be16(bytes + 0x16);
    if (stride > 0x7FFF || (expected_frames > 1 && (stride < 32 || stride % 16)))
        return MDKR_TRACK_TEXTURE_INVALID;
    MdkrTrackTextureInfo result = {expected_frames, 0, 0, 0};
    for (unsigned frame = 0; frame < expected_frames; ++frame) {
        const size_t offset = (size_t)frame * stride;
        size_t next = 0;
        if (!mdkr_texture_frame_advance(size, offset, 32, 32, 0, &next))
            return MDKR_TRACK_TEXTURE_INVALID;
        const unsigned char *header = bytes + offset;
        const unsigned format = header[2] & 15, mode = header[2] >> 4;
        const unsigned span = be16(header + 0x16);
        if (!header[0] || !header[1] || format > 8 || mode > 3 || span > 0x7FFF ||
            (frame + 1 < expected_frames && span != stride) ||
            !mdkr_texture_frame_advance(size, offset, 32, span,
                frame + 1 == expected_frames, &next)) return MDKR_TRACK_TEXTURE_INVALID;
        if (format >= 7) return MDKR_TRACK_TEXTURE_PALETTE_REQUIRED;
        const size_t row = ((size_t)header[0] * format_bits[format] + 7) / 8;
        const size_t texels = row * header[1];
        if (texels > size - offset - 32 || (span && texels > span - 32))
            return MDKR_TRACK_TEXTURE_INVALID;
        result.meaningful_bytes = offset + 32 + texels;
        if ((format <= 1 && (mode == 0 || mode == 2)) || (format >= 4 && format <= 6))
            ++result.translucent_frames;
    }
    if (!mdkr_texture_allocation_size(size, expected_frames,
            MDKR_TEXTURE_COMMAND_BYTES, 0, &result.runtime_allocation_bytes))
        return MDKR_TRACK_TEXTURE_INVALID;
    *out = result;
    return MDKR_TRACK_TEXTURE_OK;
}

static MdkrTrackTextureResult material_payload(const void *data, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out, int pulsing) {
    if (!out) return MDKR_TRACK_TEXTURE_INVALID;
    MdkrTrackTextureInfo result;
    const MdkrTrackTextureResult status = mdkr_track_texture_payload(
        data, size, scratch, scratch_capacity, &result);
    if (status != MDKR_TRACK_TEXTURE_OK) return status;
    const unsigned char *outer = data;
    const unsigned char *bytes = outer[0x1D] ? scratch : data;
    const unsigned stride = be16(bytes + 0x16);
    /* Generic admission already established every frame boundary and format;
     * inspect the exact decoded stream without inflating it a second time. */
    for (unsigned frame = 0; frame < result.frames; ++frame) {
        const unsigned char *header = bytes + (size_t)frame * stride;
        const unsigned width = header[0], height = header[1];
        const size_t row = ((size_t)width * format_bits[header[2] & 15] + 7) / 8;
        if (width > 64 || height > 64 || (width & (width - 1)) ||
            (height & (height - 1)) || row * height > 4096)
            return MDKR_TRACK_TEXTURE_MATERIAL_LIMIT;
        MdkrPulseUpload pulse_upload;
        if (pulsing && !mdkr_texture_pulse_upload(width, height,
                header[2] & 15, &pulse_upload))
            return MDKR_TRACK_TEXTURE_MATERIAL_LIMIT;
    }
    *out = result;
    return MDKR_TRACK_TEXTURE_OK;
}

MdkrTrackTextureResult mdkr_track_texture_authored_payload(const void *data, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out) {
    return material_payload(data, size, scratch, scratch_capacity, out, 0);
}

MdkrTrackTextureResult mdkr_track_texture_pulse_payload(const void *data, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out) {
    return material_payload(data, size, scratch, scratch_capacity, out, 1);
}

MdkrTrackTextureResult mdkr_track_texture_payload(const void *data, size_t size,
    void *scratch, size_t scratch_capacity, MdkrTrackTextureInfo *out) {
    const unsigned char *bytes = data;
    if (!bytes || !out || size < 40) return MDKR_TRACK_TEXTURE_INVALID;
    if (!bytes[0x1D]) return mdkr_track_texture_frames(bytes, size, bytes[0x12], out);
    size_t decoded_size = 0;
    if (!mdkr_custom_track_decode(bytes + 32, size - 32,
            scratch, scratch_capacity, &decoded_size) || decoded_size < 32)
        return MDKR_TRACK_TEXTURE_INVALID;
    const unsigned char *decoded = scratch;
    /* Allocation/animation decisions use the outer header while material
     * commands use the decoded one. Require their first-frame identity. */
    if (bytes[0] != decoded[0] || bytes[1] != decoded[1] || bytes[2] != decoded[2])
        return MDKR_TRACK_TEXTURE_INVALID;
    MdkrTrackTextureInfo result;
    const MdkrTrackTextureResult status = mdkr_track_texture_frames(
        decoded, decoded_size, bytes[0x12], &result);
    if (status != MDKR_TRACK_TEXTURE_OK) return status;
    /* load_texture reserves the decoded stream plus one outer-header-sized
     * allowance for compressed inputs before adding commands/alignment. */
    if (decoded_size > SIZE_MAX - 32 || !mdkr_texture_allocation_size(
            decoded_size + 32, result.frames, MDKR_TEXTURE_COMMAND_BYTES, 0,
            &result.runtime_allocation_bytes)) return MDKR_TRACK_TEXTURE_INVALID;
    *out = result;
    return MDKR_TRACK_TEXTURE_OK;
}
