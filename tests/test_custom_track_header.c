#include "custom_track_header.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "header relocation line %d\n", __LINE__); return 1; } } while (0)

int main(void) {
    unsigned char scalar[MDKR_TRACK_HEADER_BYTES] = {0};
    CHECK(mdkr_track_header_scalars_valid(scalar, sizeof scalar));
    CHECK(!mdkr_track_header_scalars_valid(NULL, sizeof scalar));
    for (size_t size = 0; size < sizeof scalar; ++size)
        CHECK(!mdkr_track_header_scalars_valid(scalar, size));
    const unsigned fields[] = {0, 0x4C, 0x4D, 0x4E, 0x9C};
    for (unsigned field = 0; field < sizeof fields / sizeof fields[0]; ++field) {
        for (unsigned value = 0; value < 256; ++value) {
            scalar[fields[field]] = value;
            const int valid = field == 0 ? value <= 6 || value == 255 :
                field == 1 ? value == 0 || value == 1 || value == 3 || value == 5 ||
                    value == 6 || value == 7 || value == 8 || value == 64 || value == 65 || value == 66 :
                field == 2 ? value <= 2 : field == 3 ? value <= 7 : value <= 90;
            CHECK(mdkr_track_header_scalars_valid(scalar, sizeof scalar) == valid);
        }
        scalar[fields[field]] = 0;
    }
    /* Both signs, every exponent, zero/nonzero mantissa. Reject all NaNs and
     * infinities without converting potentially signaling IEEE values. */
    for (unsigned sign = 0; sign < 2; ++sign)
        for (unsigned exponent = 0; exponent < 256; ++exponent)
            for (unsigned mantissa = 0; mantissa < 2; ++mantissa) {
                scalar[8] = (sign << 7) | (exponent >> 1);
                scalar[9] = exponent << 7;
                scalar[11] = mantissa;
                CHECK(mdkr_track_header_scalars_valid(scalar, sizeof scalar) == (exponent != 255));
            }
    unsigned char wave_texture[MDKR_TRACK_HEADER_BYTES] = {0};
    for (unsigned id = 0; id <= 0xFFFFu; ++id) {
        wave_texture[0x68] = id >> 8; wave_texture[0x69] = id;
        CHECK(mdkr_track_header_wave_texture_valid(wave_texture, sizeof wave_texture,
            63, 1416) == ((id & 0x7FFFu) < (id & 0x8000u ? 1416u : 63u)));
        CHECK(mdkr_track_header_wave_texture_valid(wave_texture, sizeof wave_texture,
            32768, 32768));
    }
    CHECK(!mdkr_track_header_wave_texture_valid(wave_texture, sizeof wave_texture, 32769, 32768));
    CHECK(!mdkr_track_header_wave_texture_valid(NULL, sizeof wave_texture, 1, 1));
    for (size_t n = 0; n < sizeof wave_texture; ++n)
        CHECK(!mdkr_track_header_wave_texture_valid(wave_texture, n, 32768, 32768));
    unsigned char wave[MDKR_TRACK_HEADER_BYTES] = {0};
    for (unsigned subdivisions = 0; subdivisions < 256; ++subdivisions) {
        wave[0x56] = subdivisions;
        for (unsigned density = 0; density <= 1; ++density) {
            wave[0x71] = density;
            CHECK(mdkr_track_header_wave_render_valid(wave, sizeof wave, 1) ==
                (subdivisions >= 1 && subdivisions <= 8));
            CHECK(mdkr_track_header_wave_render_valid(wave, sizeof wave, 2));
        }
    }
    wave[0x71] = 0;
    CHECK(!mdkr_track_header_wave_render_valid(NULL, sizeof wave, 1));
    CHECK(!mdkr_track_header_wave_render_valid(wave, sizeof wave - 1, 1));
    CHECK(!mdkr_track_header_wave_render_valid(wave, sizeof wave, 0));
    CHECK(!mdkr_track_header_wave_render_valid(wave, sizeof wave, 3));
    wave[0x56] = 4; wave[0x57] = 8; wave[0x61] = 120;
    size_t wave_bytes = 99;
    CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 1, 15260, &wave_bytes));
    CHECK(wave_bytes == 15260);
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 1, 15259, &wave_bytes));
    CHECK(wave_bytes == 15260);
    CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 2, 28784, &wave_bytes));
    /* Double density changes downstream grid/height-map allocations, not
     * any of waves_alloc's six base allocations. All nonzero bytes activate. */
    for (unsigned density = 0; density <= 255; ++density) {
        wave[0x71] = density;
        CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 1, 15260, &wave_bytes));
        CHECK(wave_bytes == 15260);
        CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 2, 28784, &wave_bytes));
        CHECK(wave_bytes == 28784);
    }
    wave[0x71] = 0;
    CHECK(wave_bytes == 28784);
    wave[0x56] = 0;
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 1, SIZE_MAX, &wave_bytes));
    CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 2, SIZE_MAX, &wave_bytes));
    wave[0x61] = 121; /* Native seed rounding clears the low bit. */
    CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 2, 28784, &wave_bytes));
    wave[0x61] = 1;
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 2, SIZE_MAX, &wave_bytes));
    wave[0x60] = 0x80;
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 2, SIZE_MAX, &wave_bytes));
    wave[0x60] = 0; wave[0x61] = 120; wave[0x57] = 0;
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 2, SIZE_MAX, &wave_bytes));
    wave[0x57] = 8;
    wave[0x56] = 127;
    CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 1, SIZE_MAX, &wave_bytes));
    size_t largest_wave_bytes = wave_bytes;
    for (unsigned subdivisions = 128; subdivisions <= 255; ++subdivisions) {
        wave[0x56] = (unsigned char)subdivisions;
        wave_bytes = largest_wave_bytes;
        CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 1, SIZE_MAX, &wave_bytes));
        CHECK(wave_bytes == largest_wave_bytes);
        CHECK(mdkr_track_header_wave_storage(wave, sizeof wave, 2, 28784, &wave_bytes));
        CHECK(wave_bytes == 28784);
    }
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 3, SIZE_MAX, &wave_bytes));
    CHECK(!mdkr_track_header_wave_storage(NULL, sizeof wave, 2, SIZE_MAX, &wave_bytes));
    CHECK(!mdkr_track_header_wave_storage(wave, sizeof wave, 2, SIZE_MAX, NULL));
    for (size_t n = 0; n < sizeof wave; ++n)
        CHECK(!mdkr_track_header_wave_storage(wave, n, 2, SIZE_MAX, &wave_bytes));
    unsigned char sky[MDKR_TRACK_HEADER_BYTES] = {0};
    CHECK(mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 0));
    sky[0x49] = 0xFF;
    CHECK(!mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 1));
    CHECK(mdkr_track_header_sky_texture_valid(sky, sizeof sky, 1, 0));
    sky[0xA6] = 0x80;
    CHECK(mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 1));
    CHECK(!mdkr_track_header_sky_texture_valid(sky, sizeof sky, 1, 0));
    sky[0xA4] = sky[0xA5] = 0xFF; /* Loader ignores high 16 bits. */
    CHECK(mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 1));
    sky[0xA6] = sky[0xA7] = 0xFF;
    CHECK(mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 32768));
    CHECK(!mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 32767));
    CHECK(!mdkr_track_header_sky_texture_valid(sky, sizeof sky, 0, 32769));
    CHECK(!mdkr_track_header_sky_texture_valid(NULL, sizeof sky, 1, 1));
    for (size_t n = 0; n < sizeof sky; ++n)
        CHECK(!mdkr_track_header_sky_texture_valid(sky, n, 32768, 32768));
    unsigned char colour[40] = {0}, pulse[20] = {0};
    CHECK(mdkr_track_colour_cycle_valid(colour, 24));
    colour[3] = 2; colour[27] = 30; colour[35] = 60;
    CHECK(mdkr_track_colour_cycle_valid(colour, 36));
    for (size_t n = 0; n < 36; ++n)
        CHECK(!mdkr_track_colour_cycle_valid(colour, n));
    colour[35] = 0;
    CHECK(!mdkr_track_colour_cycle_valid(colour, sizeof colour));
    colour[32] = 0x7F; colour[33] = colour[34] = colour[35] = 0xFF;
    CHECK(!mdkr_track_colour_cycle_valid(colour, sizeof colour));
    colour[27] = 0; colour[3] = 1; colour[24] = 0x80;
    CHECK(!mdkr_track_colour_cycle_valid(colour, sizeof colour));
    pulse[1] = 2; pulse[15] = 30; pulse[19] = 60;
    CHECK(mdkr_track_pulse_cycle_valid(pulse, sizeof pulse));
    for (size_t n = 0; n < sizeof pulse; ++n)
        CHECK(!mdkr_track_pulse_cycle_valid(pulse, n));
    pulse[19] = 0;
    CHECK(!mdkr_track_pulse_cycle_valid(pulse, sizeof pulse));
    pulse[18] = pulse[19] = 0xFF;
    CHECK(!mdkr_track_pulse_cycle_valid(pulse, sizeof pulse));
    pulse[15] = 1; pulse[19] = 0xFE;
    CHECK(mdkr_track_pulse_cycle_valid(pulse, sizeof pulse)); /* exact u16 total */
    pulse[1] = 0;
    CHECK(!mdkr_track_pulse_cycle_valid(pulse, sizeof pulse));
    CHECK(!mdkr_track_colour_cycle_valid(NULL, sizeof colour));
    CHECK(!mdkr_track_pulse_cycle_valid(NULL, sizeof pulse));
    memset(colour, 0, sizeof colour); colour[3] = 1;
    memset(pulse, 0, sizeof pulse); pulse[1] = 1;
    CHECK(mdkr_track_colour_cycle_valid(colour, 32));
    CHECK(mdkr_track_pulse_cycle_valid(pulse, 16));
    unsigned char misc_header[MDKR_TRACK_HEADER_BYTES] = {0};
    CHECK(mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 1));
    CHECK(!mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 0));
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned offset = i < 7 ? 0x74 + i * 4 : 0xAC;
        misc_header[offset + 3] = 1;
        CHECK(!mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 1));
        CHECK(mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 2));
        memset(misc_header + offset, 0xFF, 4);
        CHECK(mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 1));
        misc_header[offset + 3] = 0xFE;
        CHECK(!mdkr_track_header_misc_valid(misc_header, sizeof misc_header, INT32_MAX));
        misc_header[offset + 3] = 0xFF;
    }
    CHECK(mdkr_track_header_misc_valid(misc_header, sizeof misc_header, 0));
    for (size_t n = 0; n < sizeof misc_header; ++n)
        CHECK(!mdkr_track_header_misc_valid(misc_header, n, 0));
    CHECK(!mdkr_track_header_misc_valid(NULL, sizeof misc_header, 1));
    CHECK(!mdkr_track_header_misc_valid(misc_header, sizeof misc_header, UINT32_MAX));
    unsigned char original[MDKR_TRACK_HEADER_BYTES + 4], bytes[sizeof original];
    memset(original, 0xA5, sizeof original);
    original[0] = 6;
    original[0x34] = 0; original[0x35] = 3;
    original[0x36] = 0; original[0x37] = 7;
    original[0xBA] = 0; original[0xBB] = 8;
    MdkrTrackHeaderRelocation plan = {-1, -1, -1, 4, 9, 4, 9};
    for (size_t n = 0; n < MDKR_TRACK_HEADER_BYTES; ++n) {
        memcpy(bytes, original, sizeof bytes);
        CHECK(!mdkr_track_header_relocate(bytes, n, &plan));
        CHECK(!memcmp(bytes, original, sizeof bytes));
    }
    CHECK(!mdkr_track_header_relocate(NULL, sizeof bytes, &plan));
    CHECK(!mdkr_track_header_relocate(bytes, sizeof bytes, NULL));
    CHECK(mdkr_track_header_relocate(bytes, sizeof bytes, &plan));
    CHECK(bytes[0] == 1 && !memcmp(bytes + 1, original + 1, sizeof bytes - 1));
    plan = (MdkrTrackHeaderRelocation){256, 257, 32767, 257, 32768, 4, 9};
    CHECK(mdkr_track_header_relocate(bytes, sizeof bytes, &plan));
    CHECK(bytes[0x34] == 1 && bytes[0x35] == 0);
    CHECK(bytes[0x36] == 1 && bytes[0x37] == 1);
    CHECK(bytes[0xBA] == 127 && bytes[0xBB] == 255);
    for (size_t n = 1; n < sizeof bytes; ++n) {
        if (n == 0x34 || n == 0x35 || n == 0x36 || n == 0x37 || n == 0xBA || n == 0xBB) continue;
        CHECK(bytes[n] == original[n]);
    }
    const MdkrTrackHeaderRelocation invalid[] = {
        {-2, -1, -1, 4, 9, 4, 9}, {INT32_MIN, -1, -1, 4, 9, 4, 9},
        {32768, -1, -1, UINT32_MAX, 9, 4, 9}, {INT32_MAX, -1, -1, UINT32_MAX, 9, 4, 9},
        {4, -1, -1, 4, 9, 4, 9}, {-1, 9, -1, 4, 9, 4, 9}, {-1, -1, 9, 4, 9, 4, 9},
        {-1, -1, -1, 0, 9, 0, 9}, {-1, -1, -1, 4, 0, 4, 0},
        {-1, -1, -1, 40, 90, 3, 9}, /* Authored model may not escape retail. */
        {-1, -1, -1, 40, 90, 4, 8}, /* Neither may the retained map. */
        {2, 3, 4, 4, 9, 5, 9}, /* Inconsistent inventory, even with replacements. */
        {2, 3, -2, 4, 9, 4, 9} /* Earlier valid replacements must not leak. */
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        memcpy(bytes, original, sizeof bytes);
        CHECK(!mdkr_track_header_relocate(bytes, sizeof bytes, &invalid[i]));
        CHECK(!memcmp(bytes, original, sizeof bytes));
    }
    plan = (MdkrTrackHeaderRelocation){-1, -1, -1, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    memcpy(bytes, original, sizeof bytes);
    bytes[0xBA] = 0xFF; bytes[0xBB] = 0xFF;
    CHECK(!mdkr_track_header_relocate(bytes, sizeof bytes, &plan));
    CHECK(bytes[0] == 6 && bytes[0xBA] == 0xFF && bytes[0xBB] == 0xFF);
    plan.structure = 0; /* A supplied map replaces an authored placeholder. */
    CHECK(mdkr_track_header_relocate(bytes, sizeof bytes, &plan));
    CHECK(bytes[0xBA] == 0 && bytes[0xBB] == 0);
    for (unsigned world = 0; world <= 255; ++world) {
        memcpy(bytes, original, sizeof bytes);
        bytes[0] = (unsigned char)world;
        CHECK(mdkr_track_header_relocate(bytes, MDKR_TRACK_HEADER_BYTES, &plan));
        CHECK(bytes[0] == (world == 6 ? 1 : world));
    }
    puts("custom track header relocation: PASS");
    return 0;
}
