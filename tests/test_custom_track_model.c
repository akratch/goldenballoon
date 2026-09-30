#include "custom_track_model.h"
#include "custom_track_model_payload.h"
#include "custom_track_decode.h"
#include "gzip.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "model relocation line %d\n", __LINE__); return 1; } } while (0)
/* Link the actual native game's block decoder. Only its shared-state storage
 * is supplied here; no replacement decompression logic is used. */
u8 *gzip_inflate_input, *gzip_inflate_output;
u8 *gzip_inflate_output_start, *gzip_inflate_output_end, *gzip_inflate_input_end;
u32 gzip_bit_buffer, gzip_num_bits;
static int native_decode(unsigned char *input, size_t size,
                         unsigned char *out, size_t expected) {
    if (size <= 5) return 0;
    gzip_inflate_input = input + 5; gzip_inflate_input_end = input + size;
    gzip_inflate_output = gzip_inflate_output_start = out;
    gzip_inflate_output_end = out + expected;
    gzip_bit_buffer = gzip_num_bits = 0;
    int status;
    do { status = gzip_inflate_block(); } while (status > 0);
    return status == 0 && gzip_inflate_output == gzip_inflate_output_end;
}
typedef struct PulseCheck { unsigned calls; uint32_t expected; int accept; } PulseCheck;
static int pulse_check(void *context, uint32_t id) {
    PulseCheck *state = context;
    ++state->calls;
    return state->accept && id == state->expected;
}
static void put(unsigned char *p, uint32_t n) {
    p[0] = (unsigned char)(n >> 24); p[1] = (unsigned char)(n >> 16);
    p[2] = (unsigned char)(n >> 8); p[3] = (unsigned char)n;
}
static void put16(unsigned char *p, int32_t n) {
    const uint16_t value = (uint16_t)n;
    p[0] = (unsigned char)(value >> 8); p[1] = (unsigned char)value;
}
int main(void) {
    unsigned char draw_model[0x400] = {0};
    put(draw_model + 4, 0x4C); put(draw_model + 8, 0x90);
    put(draw_model + 0x48, sizeof draw_model); put16(draw_model + 0x1A, 1);
    put(draw_model + 0x4C, 0xC0); put(draw_model + 0x50, 0x210);
    put(draw_model + 0x60, 0x320); /* Complete collision-facet span. */
    put(draw_model + 0x58, 0x9C); /* Batch plus sentinel. */
    put16(draw_model + 0x6C, 1); draw_model[0x9C] = 0xFF;
    for (unsigned vertices = 1; vertices <= 33; ++vertices)
        for (unsigned faces = 0; faces <= 17; ++faces) {
            put16(draw_model + 0x68, vertices); put16(draw_model + 0x6A, faces);
            put16(draw_model + 0xAA, vertices); put16(draw_model + 0xAC, faces);
            CHECK(mdkr_track_model_validate_indices(draw_model, sizeof draw_model));
            CHECK(mdkr_track_model_validate_draw_batches(draw_model, sizeof draw_model) ==
                (!faces || (vertices <= 32 && faces <= 16)));
        }
    unsigned char geometry[0xA8] = {0}, changed[sizeof geometry];
    unsigned char visibility[0x120] = {0};
    put(visibility + 4, 0x4C); put(visibility + 8, 0xD4);
    put(visibility + 0x10, 0xF4); put(visibility + 0x14, 0xEC);
    put(visibility + 0x48, sizeof visibility); visibility[0x1B] = 2;
    put(visibility + 0x4C + 0x0C, 0xF8); put(visibility + 0x90 + 0x0C, 0xF8);
    visibility[0x90 + 0x29] = 1;
    memset(visibility + 0xEC, 0xFF, 4);
    visibility[0xF0] = 0; visibility[0xF1] = 1;
    CHECK(mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xED] = 0; visibility[0xEC] = 0; /* Self cycle. */
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xED] = 1; /* Out-of-range child. */
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xEC] = visibility[0xED] = 0xFF;
    visibility[0xF1] = 0;
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xF1] = 2;
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xF1] = 1; visibility[0xF0] = 3;
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0xF0] = 0; put(visibility + 0x10, sizeof visibility);
    CHECK(!mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    visibility[0x4C + 0x28] = visibility[0x4C + 0x29] = 0xFF;
    visibility[0x90 + 0x28] = visibility[0x90 + 0x29] = 0xFF;
    CHECK(mdkr_track_model_validate_visibility(visibility, sizeof visibility));
    put(geometry + 4, 0x4C); put(geometry + 8, 0x90);
    geometry[0x1B] = 1; put(geometry + 0x48, sizeof geometry);
    put(geometry + 0x4C + 0x0C, 0x9C);
    CHECK(mdkr_track_model_validate_spans(geometry, sizeof geometry));
    uint32_t wave_count = 99;
    CHECK(mdkr_track_model_wave_count(geometry, sizeof geometry, &wave_count) && wave_count == 0);
    for (unsigned flag = 1; flag <= 255; ++flag) {
        geometry[0x4C + 0x2B] = (unsigned char)flag;
        CHECK(mdkr_track_model_wave_count(geometry, sizeof geometry, &wave_count) && wave_count == 1);
        CHECK(geometry[0x4C + 0x2B] == flag);
    }
    geometry[0x4C + 0x2B] = 0;
    wave_count = 99;
    CHECK(!mdkr_track_model_wave_count(geometry, sizeof geometry - 1, &wave_count) && wave_count == 99);
    CHECK(!mdkr_track_model_wave_count(NULL, sizeof geometry, &wave_count) && wave_count == 99);
    CHECK(!mdkr_track_model_wave_count(geometry, sizeof geometry, NULL));
    CHECK(mdkr_track_model_validate_indices(geometry, sizeof geometry));
    unsigned char indexed[0x100] = {0};
    memcpy(indexed, geometry, sizeof geometry);
    put(indexed + 0x48, sizeof indexed);
    put(indexed + 0x4C, 0xB4); put(indexed + 0x50, 0xD2);
    put(indexed + 0x60, 0xE2);
    indexed[0x4C + 0x1D] = 3; indexed[0x4C + 0x1F] = 1;
    indexed[0x4C + 0x21] = 1; indexed[0x9C] = 0xFF;
    indexed[0xA8 + 3] = 3; indexed[0xA8 + 5] = 1;
    indexed[0xD3] = 0; indexed[0xD4] = 1; indexed[0xD5] = 2;
    CHECK(mdkr_track_model_validate_indices(indexed, sizeof indexed));
    unsigned char wave_model[0x110] = {0};
    memcpy(wave_model, indexed, sizeof indexed);
    put(wave_model + 0x48, sizeof wave_model);
    put(wave_model, 0x100); wave_model[0x19] = 1;
    wave_model[0x77] = 1; wave_model[0x9C] = 0;
    put(wave_model + 0xA4, 0x01002000);
    unsigned char lighting_header[0xC4] = {0};
    for (uint32_t slot = 1; slot < 8; ++slot) {
        put(wave_model + 0xA4, 0x01002000u | (slot << 28));
        CHECK(mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model,
            lighting_header, sizeof lighting_header));
        CHECK(!mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model, NULL, 0));
        put(lighting_header + 0x74 + 4 * (slot - 1), UINT32_MAX);
        CHECK(!mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model,
            lighting_header, sizeof lighting_header));
        put(lighting_header + 0x74 + 4 * (slot - 1), 0);
    }
    put(wave_model + 0xA4, 0x01042000);
    PulseCheck pulse = {0, 37, 1};
    put(wave_model + 0x100, 37);
    CHECK(mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, pulse_check, &pulse));
    CHECK(pulse.calls == 1);
    pulse.accept = 0;
    CHECK(!mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, pulse_check, &pulse));
    CHECK(pulse.calls == 2);
    CHECK(!mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, NULL, NULL));
    CHECK(!mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model - 1, pulse_check, &pulse));
    CHECK(pulse.calls == 2);
    put(wave_model + 0x100, 0);
    put(wave_model + 0xA4, 0x01042100); /* Hidden batches never upload. */
    CHECK(mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, NULL, NULL));
    put(wave_model + 0xA4, 0x01042000);
    put16(wave_model + 0xA0, 1); /* Empty face range, same sentinel. */
    CHECK(mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, NULL, NULL));
    put16(wave_model + 0xA0, 0);
    CHECK(mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model,
        lighting_header, sizeof lighting_header));
    wave_model[0x9C] = 0xFF;
    CHECK(!mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, pulse_check, &pulse));
    CHECK(pulse.calls == 2);
    CHECK(!mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model,
        lighting_header, sizeof lighting_header));
    wave_model[0x9C] = 0;
    put(lighting_header + 0xAC, UINT32_MAX);
    CHECK(!mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model,
        lighting_header, sizeof lighting_header));
    put(wave_model + 0xA4, 0x01002000);
    CHECK(mdkr_track_model_pulse_textures_valid(wave_model, sizeof wave_model, NULL, NULL));
    CHECK(mdkr_track_model_lighting_links_valid(wave_model, sizeof wave_model, NULL, 0));
    wave_model[0x97] = wave_model[0x9B] = 100;
    MdkrTrackWaveGrid measured_grid = {99, 99, 99};
    CHECK(mdkr_track_model_wave_grid(wave_model, sizeof wave_model, &measured_grid));
    CHECK(measured_grid.segments == 1 && measured_grid.columns == 2 && measured_grid.rows == 2);
    CHECK(!mdkr_track_model_wave_grid(wave_model, sizeof wave_model - 1, &measured_grid));
    CHECK(measured_grid.segments == 1 && measured_grid.columns == 2 && measured_grid.rows == 2);
    CHECK(!mdkr_track_model_wave_grid(wave_model, sizeof wave_model, NULL));
    for (uint32_t slot = 0; slot < 8; ++slot) {
        put(wave_model + 0xA4, 0x01002000u | (slot << 28));
        CHECK(mdkr_track_model_wave_grid(wave_model, sizeof wave_model, &measured_grid));
        CHECK(measured_grid.colour_slot == slot);
    }
    put(wave_model + 0xA4, 0x01002000);
    CHECK(mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    put(wave_model + 0xA4, 0x01402000);
    CHECK(mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    /* Empty batch at the legal sentinel offset is structurally valid, but
     * wave height sampling dereferences that offset rather than its faces. */
    wave_model[0x9F] = 3; wave_model[0xA1] = 1;
    CHECK(mdkr_track_model_validate_indices(wave_model, sizeof wave_model));
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x9F] = 2;
    CHECK(mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x9F] = wave_model[0xA1] = 0;
    put(wave_model + 0xA4, 0x01002000);
    unsigned char later_water[0x140] = {0};
    memcpy(later_water, wave_model, sizeof wave_model);
    put(later_water + 0x48, sizeof later_water);
    put(later_water + 0x58, 0x110);
    later_water[0x6D] = 2;
    memcpy(later_water + 0x110, wave_model + 0x9C, 12);
    memcpy(later_water + 0x11C, wave_model + 0xA8, 12);
    memcpy(later_water + 0x128, wave_model + 0xA8, 12);
    put(later_water + 0x124, 0x00402000);
    CHECK(mdkr_track_model_validate_indices(later_water, sizeof later_water));
    CHECK(!mdkr_track_model_wave_basis_valid(later_water, sizeof later_water));
    put(later_water + 0x124, 0);
    CHECK(mdkr_track_model_wave_basis_valid(later_water, sizeof later_water));
    unsigned char grid_model[0x200] = {0};
    memcpy(grid_model, wave_model, sizeof wave_model);
    put(grid_model + 0x48, sizeof grid_model);
    put(grid_model + 4, 0x140); put(grid_model + 8, 0x1C8);
    put(grid_model + 0x14, 0x1E0); /* Required two-segment BSP storage. */
    grid_model[0x1B] = 2;
    memcpy(grid_model + 0x140, wave_model + 0x4C, 0x44);
    memcpy(grid_model + 0x184, wave_model + 0x4C, 0x44);
    grid_model[0x1AF] = 0; /* Unflagged segments still read the row mask. */
    memcpy(grid_model + 0x1C8, wave_model + 0x90, 12);
    for (unsigned axis = 0; axis < 2; ++axis) {
        for (unsigned cell = 0; cell <= 65; ++cell) {
            unsigned char *box = grid_model + 0x1D4;
            memcpy(box, wave_model + 0x90, 12);
            const unsigned offset = axis ? 4 : 0;
            const unsigned low = cell * 100, high = low + 100;
            box[offset] = low >> 8; box[offset + 1] = low;
            box[offset + 6] = high >> 8; box[offset + 7] = high;
            CHECK(mdkr_track_model_validate_indices(grid_model, sizeof grid_model));
            CHECK(mdkr_track_model_wave_basis_valid(grid_model, sizeof grid_model) ==
                (cell < (axis ? 64u : 32u)));
        }
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
        unsigned char *basis_box = grid_model + 0x1C8;
        unsigned char *other_box = grid_model + 0x1D4;
        memcpy(basis_box, wave_model + 0x90, 12);
        memcpy(other_box, wave_model + 0x90, 12);
        const unsigned offset = axis ? 4 : 0;
        put16(basis_box + offset, -32700); put16(basis_box + offset + 6, -32600);
        put16(other_box + offset, -32768); put16(other_box + offset + 6, -32668);
        CHECK(mdkr_track_model_validate_indices(grid_model, sizeof grid_model));
        CHECK(!mdkr_track_model_wave_basis_valid(grid_model, sizeof grid_model));
        put16(other_box + offset, -32708); put16(other_box + offset + 6, -32608);
        CHECK(mdkr_track_model_wave_basis_valid(grid_model, sizeof grid_model));
    }
    wave_model[0x97] = 0;
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x96] = wave_model[0x97] = 0xFF;
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x96] = 0; wave_model[0x97] = 100; wave_model[0x9B] = 0;
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x9B] = 100; wave_model[0x9C] = 0xFF;
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x9C] = 0; put(wave_model + 0xA4, 0x01002100);
    CHECK(!mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    wave_model[0x77] = 0;
    CHECK(mdkr_track_model_wave_basis_valid(wave_model, sizeof wave_model));
    indexed[0xD5] = 3;
    CHECK(!mdkr_track_model_validate_indices(indexed, sizeof indexed));
    indexed[0xD5] = 2; indexed[0x9C] = 0;
    CHECK(!mdkr_track_model_validate_indices(indexed, sizeof indexed));
    indexed[0x9C] = 0xFF; indexed[0xA8 + 5] = 2;
    CHECK(!mdkr_track_model_validate_indices(indexed, sizeof indexed));
    indexed[0xA8 + 5] = 1; indexed[0x4C + 0x40] = 2;
    CHECK(!mdkr_track_model_validate_indices(indexed, sizeof indexed));
    indexed[0x4C + 0x40] = 0;
    indexed[0x4C + 0x28] = indexed[0x4C + 0x29] = 0xFF;
    CHECK(mdkr_track_model_validate_regions(indexed, sizeof indexed));
    put(indexed + 0x60, 0xD2); /* Collision facets alias triangle storage. */
    CHECK(mdkr_track_model_validate_visibility(indexed, sizeof indexed));
    CHECK(!mdkr_track_model_validate_regions(indexed, sizeof indexed));
    put(indexed + 0x60, 0xE2);
    indexed[0x4C + 0x28] = indexed[0x4C + 0x29] = 0;
    put(indexed + 0x10, 0xD2); /* Byte-valid PVS aliases endian-swapped UVs. */
    CHECK(!mdkr_track_model_validate_regions(indexed, sizeof indexed));
    put(indexed + 0x10, 0xEA);
    CHECK(mdkr_track_model_validate_regions(indexed, sizeof indexed));
    size_t collision_budget = 123;
    CHECK(mdkr_track_model_collision_budget(indexed, sizeof indexed, &collision_budget));
    CHECK(collision_budget == 352);
    indexed[0xE3] = 1;
    CHECK(!mdkr_track_model_validate_collision(indexed, sizeof indexed));
    indexed[0xE3] = 0; indexed[0xE5] = 1;
    CHECK(!mdkr_track_model_validate_collision(indexed, sizeof indexed));
    indexed[0xE5] = 0;
    CHECK(mdkr_track_model_validate_collision(indexed, sizeof indexed));
    CHECK(!mdkr_track_model_collision_budget(NULL, sizeof indexed, &collision_budget));
    CHECK(collision_budget == 352);
    CHECK(!mdkr_track_model_collision_budget(indexed, sizeof indexed, NULL));
    CHECK(!mdkr_track_model_validate_spans(NULL, sizeof geometry));
    for (size_t n = 0; n < sizeof geometry; ++n)
        CHECK(!mdkr_track_model_validate_spans(geometry, n));
    const unsigned bad_offsets[] = {4, 8, 0x4C + 0x0C};
    for (size_t i = 0; i < sizeof bad_offsets / sizeof bad_offsets[0]; ++i) {
        memcpy(changed, geometry, sizeof changed); put(changed + bad_offsets[i], UINT32_MAX);
        CHECK(!mdkr_track_model_validate_spans(changed, sizeof changed));
    }
    const unsigned bad_counts[] = {0x1A, 0x4C + 0x1C, 0x4C + 0x1E, 0x4C + 0x20};
    for (size_t i = 0; i < sizeof bad_counts / sizeof bad_counts[0]; ++i) {
        memcpy(changed, geometry, sizeof changed); changed[bad_counts[i]] = 0xFF;
        CHECK(!mdkr_track_model_validate_spans(changed, sizeof changed));
    }
    unsigned char original[0x70], bytes[sizeof original], before[sizeof original];
    const uint32_t map[] = {1402, 1401};
    memset(original, 0xA5, sizeof original);
    put(original, 0x4C); put(original + 0x48, 0x64);
    original[0x18] = 0; original[0x19] = 3;
    put(original + 0x4C, 12); put(original + 0x54, 0x7000); put(original + 0x5C, 0x7001);
    memcpy(bytes, original, sizeof bytes);
    CHECK(mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1403));
    put(original + 0x54, 1402); put(original + 0x5C, 1401);
    CHECK(!memcmp(bytes, original, sizeof bytes));
    put(original + 0x54, 0x7000); put(original + 0x5C, 0x7001);
    for (size_t n = 0; n < 0x64; ++n) {
        memcpy(bytes, original, sizeof bytes);
        CHECK(!mdkr_track_model_relocate_textures(bytes, n, map, 2, 1401, 1403));
        CHECK(!memcmp(bytes, original, sizeof bytes));
    }
    const uint32_t bad_ids[] = {1401, 0x70FF, 0x8000, UINT32_MAX};
    for (size_t i = 0; i < sizeof bad_ids / sizeof bad_ids[0]; ++i) {
        memcpy(bytes, original, sizeof bytes); put(bytes + 0x5C, bad_ids[i]);
        memcpy(before, bytes, sizeof bytes);
        CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1403));
        CHECK(!memcmp(bytes, before, sizeof bytes));
    }
    const uint32_t offsets[] = {0, 0x48, 0x4D, 0x64, UINT32_MAX};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        memcpy(bytes, original, sizeof bytes); put(bytes, offsets[i]);
        memcpy(before, bytes, sizeof bytes);
        CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1403));
        CHECK(!memcmp(bytes, before, sizeof bytes));
    }
    memcpy(bytes, original, sizeof bytes);
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 1, 1401, 1403));
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, NULL, 2, 1401, 1403));
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 256, 1401, 1403));
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1402));
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1403, 1401));
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 0, 0x8001));
    CHECK(!memcmp(bytes, original, sizeof bytes));
    bytes[0x18] = 0xFF; bytes[0x19] = 0xFF;
    CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1403));
    bytes[0x18] = 0; bytes[0x19] = 0;
    CHECK(mdkr_track_model_relocate_textures(bytes, sizeof bytes, NULL, 0, 0, 0));
    CHECK(!mdkr_track_model_relocate_textures(NULL, sizeof bytes, NULL, 0, 0, 0));
    /* Every local ordinal, reversed assignment, and the last 15-bit ID. */
    unsigned char large[0x4C + 255 * 8 + 8], expected[sizeof large];
    uint32_t full_map[255];
    memset(large, 0xA5, sizeof large);
    put(large, 0x4C); put(large + 0x48, sizeof large - 8);
    large[0x18] = 0; large[0x19] = 255;
    for (unsigned i = 0; i < 255; ++i) {
        full_map[i] = 0x7FFF - i;
        put(large + 0x4C + 8 * i, 0x7000 + i);
    }
    memcpy(expected, large, sizeof large);
    for (unsigned i = 0; i < 255; ++i)
        put(expected + 0x4C + 8 * i, full_map[i]);
    CHECK(mdkr_track_model_relocate_textures(large, sizeof large, full_map, 255,
                                           0x8000 - 255, 0x8000));
    CHECK(!memcmp(large, expected, sizeof large));
    /* One past the u8 batch-index domain must fail without touching bytes. */
    large[0x18] = 1; large[0x19] = 0;
    memcpy(expected, large, sizeof large);
    CHECK(!mdkr_track_model_relocate_textures(large, sizeof large, full_map, 255,
                                            0x8000 - 255, 0x8000));
    CHECK(!memcmp(large, expected, sizeof large));
    /* A native-aligned mapping may live in the same allocation as the model.
     * Character-byte access is legal for this uint32_t backing storage. */
    uint32_t backing[sizeof original / sizeof(uint32_t)];
    unsigned char *aliased = (unsigned char *)backing;
    memcpy(aliased, original, sizeof original);
    backing[0x20 / 4] = map[0]; backing[0x24 / 4] = map[1];
    memcpy(before, aliased, sizeof before);
    put(before + 0x54, map[0]); put(before + 0x5C, map[1]);
    CHECK(mdkr_track_model_relocate_textures(aliased, sizeof backing,
        backing + 0x20 / 4, 2, 1401, 1403));
    CHECK(!memcmp(aliased, before, sizeof before));
    /* Each malformed model-size claim refuses before relocation. */
    const uint32_t extents[] = {0, 0x4B, 0x63, sizeof bytes + 1, UINT32_MAX};
    for (size_t i = 0; i < sizeof extents / sizeof extents[0]; ++i) {
        memcpy(bytes, original, sizeof bytes); put(bytes + 0x48, extents[i]);
        memcpy(before, bytes, sizeof bytes);
        CHECK(!mdkr_track_model_relocate_textures(bytes, sizeof bytes, map, 2, 1401, 1403));
        CHECK(!memcmp(bytes, before, sizeof bytes));
    }
    /* Independently construct an rzip stored block, then consume the repacked
     * stream through the same bounded decoder used by package admission. */
    put(indexed, 0xF0); indexed[0x19] = 2;
    put(indexed + 0xF0, 0x7000); put(indexed + 0xF8, 0x7001);
    indexed[0x9C] = 0;
    unsigned char packed[10 + sizeof indexed], output[512], scratch[sizeof indexed + 32];
    unsigned char decoded[sizeof indexed], expected_model[sizeof indexed];
    memset(packed, 0, sizeof packed);
    packed[0] = (unsigned char)sizeof indexed; packed[1] = sizeof indexed >> 8; packed[4] = 9;
    packed[5] = 1; packed[6] = (unsigned char)sizeof indexed; packed[7] = sizeof indexed >> 8;
    packed[8] = (unsigned char)~sizeof indexed; packed[9] = (unsigned char)(~sizeof indexed >> 8);
    memcpy(packed + 10, indexed, sizeof indexed);
    size_t written = 0, decoded_size = 0, repacked_size = 99;
    memset(scratch, 0xD5, sizeof scratch);
    CHECK(mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, 1024, &written, &repacked_size));
    CHECK(repacked_size == sizeof indexed);
    for (size_t i = repacked_size; i < sizeof scratch; ++i) CHECK(scratch[i] == 0xD5);
    CHECK(written > 5 && output[4] == 9);
    CHECK(mdkr_custom_track_decode(output, written, decoded, sizeof decoded, &decoded_size));
    CHECK(decoded_size == sizeof indexed);
    memcpy(expected_model, indexed, sizeof indexed);
    put(expected_model + 0xF0, map[0]); put(expected_model + 0xF8, map[1]);
    CHECK(!memcmp(decoded, expected_model, sizeof expected_model));
    unsigned char native_output[sizeof indexed + 2];
    memset(native_output, 0xD5, sizeof native_output);
    CHECK(native_decode(output, written, native_output + 1, sizeof indexed));
    CHECK(!memcmp(native_output + 1, expected_model, sizeof expected_model));
    CHECK(native_output[0] == 0xD5 && native_output[sizeof native_output - 1] == 0xD5);
    /* A short output bound or truncated stream must not masquerade as a pass. */
    CHECK(!native_decode(output, written, native_output + 1, sizeof indexed - 1));
    CHECK(!native_decode(output, 6, native_output + 1, sizeof indexed));
    CHECK(native_output[0] == 0xD5 && native_output[sizeof native_output - 1] == 0xD5);
    const size_t encoded_size = written;
    const size_t staging_minimum = ((sizeof indexed + 15) & ~(size_t)15) + encoded_size;
    CHECK(mdkr_track_model_collision_budget(indexed, sizeof indexed, &collision_budget));
    const size_t minimum_arena = staging_minimum > collision_budget ? staging_minimum : collision_budget;
    CHECK(mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, minimum_arena, &written, &repacked_size));
    CHECK(repacked_size == sizeof indexed);
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, minimum_arena - 1, &written, &repacked_size));
    CHECK(written == 0 && repacked_size == 0);
    /* Valid compression must not bypass semantic geometry checks. */
    packed[10 + 0xD5] = 3;
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, 1024, &written, &repacked_size));
    CHECK(written == 0 && repacked_size == 0);
    packed[10 + 0xD5] = 2;
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof indexed - 1,
        output, sizeof output, map, 2, 1401, 1403, 1024, &written, &repacked_size));
    CHECK(written == 0 && repacked_size == 0);
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, 6, map, 2, 1401, 1403, 1024, &written, &repacked_size));
    CHECK(written == 0 && repacked_size == 0);
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 1, 1401, 1403, 1024, &written, &repacked_size));
    CHECK(written == 0 && repacked_size == 0);
    written = repacked_size = 99;
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, 1024, &written, NULL));
    CHECK(written == 0);
    CHECK(!mdkr_track_model_repack(packed, sizeof packed, scratch, sizeof scratch,
        output, sizeof output, map, 2, 1401, 1403, 1024, NULL, &repacked_size));
    CHECK(repacked_size == 0);
    puts("custom track model texture relocation/repack: PASS");
    return 0;
}
