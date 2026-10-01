#include "custom_track_texture.h"
#include "runtime_contracts.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "texture admission line %d\n", __LINE__); return 1; } } while (0)
static void header(unsigned char *p, unsigned frames) {
    memset(p, 0, 32); p[0] = 8; p[1] = 2; p[2] = 0x11;
    p[0x12] = (unsigned char)frames; p[0x17] = 64;
}
int main(void) {
    /* Admission shares the renderer's upload plan; the emitted commands and
     * the sampled bytes are decoded from the production material function in
     * test_pulse_material.c. Here: the plan for every admitted shape. */
    for (unsigned width = 4; width <= 64; width *= 2)
        for (unsigned height = 2; height <= 64; height *= 2) {
            if (width * height > 2048) continue;
            MdkrPulseUpload plan;
            CHECK(mdkr_texture_pulse_upload(width, height, 1, &plan));
            const size_t bytes = (size_t)width * height * 2;
            CHECK(plan.textureBytes == bytes && plan.wrapped == (bytes < 4096));
            const MdkrPulseTile *tiles[2] = {&plan.texel1, &plan.texel0};
            for (unsigned i = 0; i < 2; ++i) {
                const MdkrPulseTile *tile = tiles[i];
                const size_t loaded = (size_t)tile->width * tile->height * 2;
                CHECK(tile->offsetBytes + loaded <= bytes);
                CHECK((1u << tile->maskS) == tile->width && (1u << tile->maskT) == tile->height);
                CHECK(tile->clampT == (tile->height == 1));
                CHECK(loaded == (bytes < 2048 ? bytes : 2048));
            }
            if (bytes == 4096) CHECK(plan.texel1.offsetBytes == 0 && plan.texel0.offsetBytes == 2048);
            else CHECK(plan.texel1.offsetBytes == 0 && plan.texel0.offsetBytes == 0);
        }
    unsigned char authored[32 + 129 * 129 * 4] = {0};
    static const unsigned bits[] = {32, 16, 8, 4, 16, 8, 4};
    for (unsigned format = 0; format < 7; ++format)
        for (unsigned width = 1; width <= 129; ++width)
            for (unsigned height = 1; height <= 129; ++height) {
                header(authored, 1);
                authored[0] = width; authored[1] = height; authored[2] = 0x10 | format;
                authored[0x17] = 0; /* Static final-frame extent belongs to payload. */
                const size_t texels = ((width * bits[format] + 7) / 8) * height;
                const size_t size = texels + 32 < 40 ? 40 : texels + 32;
                MdkrTrackTextureInfo result = {99, 99, 99, 99};
                CHECK(mdkr_track_texture_payload(authored, size, NULL, 0, &result) == MDKR_TRACK_TEXTURE_OK);
                result = (MdkrTrackTextureInfo){99, 99, 99, 99};
                const int valid = width <= 64 && height <= 64 &&
                    !(width & (width - 1)) && !(height & (height - 1)) && texels <= 4096;
                CHECK(mdkr_track_texture_authored_payload(authored, size, NULL, 0, &result) ==
                    (valid ? MDKR_TRACK_TEXTURE_OK : MDKR_TRACK_TEXTURE_MATERIAL_LIMIT));
                if (!valid) CHECK(result.frames == 99 && result.translucent_frames == 99 &&
                    result.meaningful_bytes == 99 && result.runtime_allocation_bytes == 99);
                result = (MdkrTrackTextureInfo){99, 99, 99, 99};
                const int pulse_valid = valid && format == 1 && width >= 4 && height >= 2;
                MdkrPulseUpload upload;
                memset(&upload, 0x63, sizeof(upload));
                MdkrPulseUpload untouched = upload;
                CHECK(mdkr_texture_pulse_upload(width, height, format, &upload) == pulse_valid);
                if (pulse_valid) CHECK(upload.textureBytes == texels);
                else CHECK(!memcmp(&upload, &untouched, sizeof(upload)));
                CHECK(mdkr_track_texture_pulse_payload(authored, size, NULL, 0, &result) ==
                    (pulse_valid ? MDKR_TRACK_TEXTURE_OK : MDKR_TRACK_TEXTURE_MATERIAL_LIMIT));
                if (!pulse_valid) CHECK(result.frames == 99 && result.translucent_frames == 99 &&
                    result.meaningful_bytes == 99 && result.runtime_allocation_bytes == 99);
            }
    unsigned char pulse_frames[2 * 4128] = {0};
    for (unsigned frame = 0; frame < 2; ++frame) {
        unsigned char *p = pulse_frames + frame * 4128;
        header(p, 2); p[0] = 64; p[1] = 32;
        p[0x16] = 0x10; p[0x17] = 0x20;
    }
    MdkrTrackTextureInfo pulse_info;
    CHECK(mdkr_track_texture_pulse_payload(pulse_frames, sizeof pulse_frames,
        NULL, 0, &pulse_info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(pulse_info.frames == 2 && pulse_info.meaningful_bytes == sizeof pulse_frames);
    pulse_frames[4128 + 2] = 0x14; /* Same byte count, wrong material format. */
    pulse_info = (MdkrTrackTextureInfo){99, 99, 99, 99};
    CHECK(mdkr_track_texture_pulse_payload(pulse_frames, sizeof pulse_frames,
        NULL, 0, &pulse_info) == MDKR_TRACK_TEXTURE_MATERIAL_LIMIT);
    CHECK(pulse_info.frames == 99 && pulse_info.runtime_allocation_bytes == 99);
    unsigned char data[128] = {0};
    MdkrTrackTextureInfo info = {99, 99, 99, 99};
    header(data, 1);
    CHECK(mdkr_track_texture_frames(data, 64, 1, &info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(info.frames == 1 && !info.translucent_frames && info.meaningful_bytes == 64);
    CHECK(info.runtime_allocation_bytes == 175);
    header(data, 2); header(data + 64, 2);
    CHECK(mdkr_track_texture_frames(data, 128, 2, &info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(info.runtime_allocation_bytes == 335);
    CHECK(mdkr_track_texture_authored_payload(data, 128, NULL, 0, &info) == MDKR_TRACK_TEXTURE_OK);
    data[64] = 3;
    CHECK(mdkr_track_texture_payload(data, 128, NULL, 0, &info) == MDKR_TRACK_TEXTURE_OK);
    info = (MdkrTrackTextureInfo){99, 99, 99, 99};
    CHECK(mdkr_track_texture_authored_payload(data, 128, NULL, 0, &info) == MDKR_TRACK_TEXTURE_MATERIAL_LIMIT);
    CHECK(info.frames == 99 && info.runtime_allocation_bytes == 99);
    data[64] = 8;
    for (size_t n = 0; n < 128; ++n) {
        info = (MdkrTrackTextureInfo){99, 99, 99, 99};
        CHECK(mdkr_track_texture_frames(data, n, 2, &info) != MDKR_TRACK_TEXTURE_OK);
        CHECK(info.frames == 99 && info.translucent_frames == 99 && info.meaningful_bytes == 99);
        CHECK(info.runtime_allocation_bytes == 99);
    }
    data[0x17] = 0;
    CHECK(mdkr_track_texture_frames(data, 128, 2, &info) == MDKR_TRACK_TEXTURE_INVALID);
    header(data, 1); data[0] = 3; data[1] = 3; data[2] = 0x13; data[0x17] = 0;
    CHECK(mdkr_track_texture_frames(data, 38, 1, &info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(mdkr_track_texture_frames(data, 37, 1, &info) == MDKR_TRACK_TEXTURE_INVALID);
    data[2] = 0x17;
    CHECK(mdkr_track_texture_frames(data, 64, 1, &info) == MDKR_TRACK_TEXTURE_PALETTE_REQUIRED);
    data[2] = 0x19;
    CHECK(mdkr_track_texture_frames(data, 64, 1, &info) == MDKR_TRACK_TEXTURE_INVALID);
    data[2] = 0x41;
    CHECK(mdkr_track_texture_frames(data, 64, 1, &info) == MDKR_TRACK_TEXTURE_INVALID);
    CHECK(mdkr_track_texture_frames(NULL, 64, 1, &info) == MDKR_TRACK_TEXTURE_INVALID);
    CHECK(mdkr_track_texture_frames(data, 64, 1, NULL) == MDKR_TRACK_TEXTURE_INVALID);
    CHECK(mdkr_track_texture_frames(data, 64, 0, &info) == MDKR_TRACK_TEXTURE_INVALID);
    unsigned char compressed[32 + 10 + 64] = {0}, scratch[64];
    header(compressed, 1); compressed[0x1D] = 1;
    compressed[32] = 64; compressed[36] = 9;
    compressed[37] = 1; compressed[38] = 64;
    compressed[40] = 191; compressed[41] = 255;
    header(compressed + 42, 1);
    CHECK(mdkr_track_texture_payload(compressed, sizeof compressed, scratch, sizeof scratch, &info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(info.frames == 1 && info.meaningful_bytes == 64);
    CHECK(info.runtime_allocation_bytes == 207);
    CHECK(mdkr_track_texture_authored_payload(compressed, sizeof compressed,
        scratch, sizeof scratch, &info) == MDKR_TRACK_TEXTURE_OK);
    compressed[0] = compressed[42] = 7;
    info = (MdkrTrackTextureInfo){99, 99, 99, 99};
    CHECK(mdkr_track_texture_authored_payload(compressed, sizeof compressed,
        scratch, sizeof scratch, &info) == MDKR_TRACK_TEXTURE_MATERIAL_LIMIT);
    CHECK(info.frames == 99 && info.runtime_allocation_bytes == 99);
    compressed[0] = compressed[42] = 8;
    for (size_t n = 0; n < sizeof compressed; ++n) {
        info = (MdkrTrackTextureInfo){99, 99, 99, 99};
        CHECK(mdkr_track_texture_payload(compressed, n, scratch, sizeof scratch, &info) != MDKR_TRACK_TEXTURE_OK);
        CHECK(info.frames == 99 && info.meaningful_bytes == 99);
        CHECK(info.runtime_allocation_bytes == 99);
    }
    CHECK(mdkr_track_texture_payload(compressed, sizeof compressed, scratch, 63, &info) == MDKR_TRACK_TEXTURE_INVALID);
    compressed[0] = 7;
    CHECK(mdkr_track_texture_payload(compressed, sizeof compressed, scratch, 64, &info) == MDKR_TRACK_TEXTURE_INVALID);
    compressed[0] = 8; compressed[0x12] = 2;
    CHECK(mdkr_track_texture_payload(compressed, sizeof compressed, scratch, 64, &info) == MDKR_TRACK_TEXTURE_INVALID);
    header(data, 1);
    CHECK(mdkr_track_texture_payload(data, 64, NULL, 0, &info) == MDKR_TRACK_TEXTURE_OK);
    CHECK(mdkr_track_texture_payload(data, 39, NULL, 0, &info) == MDKR_TRACK_TEXTURE_INVALID);
    puts("custom track decoded/compressed texture admission: PASS");
    return 0;
}
