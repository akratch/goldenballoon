#include "gfx_font_sdf.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FONT_SDF_RADIUS 8

static float clamp_float(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

size_t gfx_font_sdf_output_bytes(uint32_t width, uint32_t height,
                                 uint32_t upscale) {
    size_t out_width;
    size_t out_height;
    size_t pixels;

    if (width == 0 || height == 0 || upscale < 2 || upscale > 8 ||
        width > (uint32_t)INT_MAX / upscale ||
        height > (uint32_t)INT_MAX / upscale ||
        width > UINT32_MAX / upscale ||
        height > UINT32_MAX / upscale ||
        (size_t)width > SIZE_MAX / upscale ||
        (size_t)height > SIZE_MAX / upscale) {
        return 0;
    }
    out_width = (size_t)width * upscale;
    out_height = (size_t)height * upscale;
    if (out_width > SIZE_MAX / out_height) {
        return 0;
    }
    pixels = out_width * out_height;
    return pixels <= SIZE_MAX / 4 ? pixels * 4 : 0;
}

static bool clip_region(const GfxFontAtlasRegion *source,
                        uint32_t atlas_width, uint32_t atlas_height,
                        GfxFontAtlasRegion *clipped) {
    uint32_t right;
    uint32_t bottom;

    if (source == NULL || clipped == NULL || source->width == 0 ||
        source->height == 0 || source->x >= atlas_width ||
        source->y >= atlas_height) {
        return false;
    }
    right = (uint32_t)source->x + source->width;
    bottom = (uint32_t)source->y + source->height;
    if (right > atlas_width) {
        right = atlas_width;
    }
    if (bottom > atlas_height) {
        bottom = atlas_height;
    }
    *clipped = (GfxFontAtlasRegion){
        source->x,
        source->y,
        (uint16_t)(right - source->x),
        (uint16_t)(bottom - source->y),
    };
    return clipped->width != 0 && clipped->height != 0;
}

static float sample_distance(const float *distance, uint32_t width,
                             uint32_t height, float x, float y) {
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    float top;
    float bottom;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x0 >= (int)width) x0 = (int)width - 1;
    if (x1 >= (int)width) x1 = (int)width - 1;
    if (y0 >= (int)height) y0 = (int)height - 1;
    if (y1 >= (int)height) y1 = (int)height - 1;

    top = distance[(size_t)y0 * width + (size_t)x0] * (1.0f - fx) +
          distance[(size_t)y0 * width + (size_t)x1] * fx;
    bottom = distance[(size_t)y1 * width + (size_t)x0] * (1.0f - fx) +
             distance[(size_t)y1 * width + (size_t)x1] * fx;
    return top * (1.0f - fy) + bottom * fy;
}

static void sample_colour_in_region(
    const uint8_t *source, uint32_t atlas_width,
    const GfxFontAtlasRegion *region, float local_x, float local_y,
    uint8_t output[3]) {
    int x0 = (int)floorf(local_x);
    int y0 = (int)floorf(local_y);
    float fx = local_x - (float)x0;
    float fy = local_y - (float)y0;
    float weight_sum = 0.0f;
    float sum[3] = { 0.0f, 0.0f, 0.0f };

    for (int oy = 0; oy < 2; oy++) {
        int local_sy = y0 + oy;
        float wy = oy ? fy : 1.0f - fy;
        if (local_sy < 0) local_sy = 0;
        if (local_sy >= (int)region->height) {
            local_sy = (int)region->height - 1;
        }
        for (int ox = 0; ox < 2; ox++) {
            int local_sx = x0 + ox;
            float wx = ox ? fx : 1.0f - fx;
            const uint8_t *pixel;
            float weight;

            if (local_sx < 0) local_sx = 0;
            if (local_sx >= (int)region->width) {
                local_sx = (int)region->width - 1;
            }
            pixel = source +
                ((size_t)(region->y + local_sy) * atlas_width +
                 (size_t)(region->x + local_sx)) * 4;
            weight = wx * wy * ((float)pixel[3] / 255.0f);
            weight_sum += weight;
            for (int channel = 0; channel < 3; channel++) {
                sum[channel] += (float)pixel[channel] * weight;
            }
        }
    }

    if (weight_sum <= 0.00001f) {
        int local_sx = (int)floorf(local_x + 0.5f);
        int local_sy = (int)floorf(local_y + 0.5f);
        const uint8_t *pixel;

        if (local_sx < 0) local_sx = 0;
        if (local_sy < 0) local_sy = 0;
        if (local_sx >= (int)region->width) {
            local_sx = (int)region->width - 1;
        }
        if (local_sy >= (int)region->height) {
            local_sy = (int)region->height - 1;
        }
        pixel = source +
            ((size_t)(region->y + local_sy) * atlas_width +
             (size_t)(region->x + local_sx)) * 4;
        output[0] = pixel[0];
        output[1] = pixel[1];
        output[2] = pixel[2];
        return;
    }
    for (int channel = 0; channel < 3; channel++) {
        output[channel] = (uint8_t)clamp_float(
            sum[channel] / weight_sum + 0.5f, 0.0f, 255.0f);
    }
}

static float boundary_distance_squared(uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height) {
    /*
     * Treat the first virtual texel outside the glyph cell as transparent.
     * Texel centres are one unit apart, so a boundary texel is distance 1
     * (not 0.5) from that virtual sample.
     */
    float left = (float)x + 1.0f;
    float right = (float)(width - x);
    float top = (float)y + 1.0f;
    float bottom = (float)(height - y);
    float best = left;

    if (right < best) best = right;
    if (top < best) best = top;
    if (bottom < best) best = bottom;
    return best * best;
}

static bool derive_region(
    const uint8_t *source, uint32_t atlas_width,
    const GfxFontAtlasRegion *region, uint32_t upscale,
    uint8_t *output, uint32_t out_width, float *distance) {
    const uint32_t width = region->width;
    const uint32_t height = region->height;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *pixel =
                source + ((size_t)(region->y + y) * atlas_width +
                          (size_t)(region->x + x)) * 4;
            bool inside = pixel[3] >= 128;
            float best2 = inside
                ? boundary_distance_squared(x, y, width, height)
                : (float)(FONT_SDF_RADIUS * FONT_SDF_RADIUS);
            int min_y = (int)y - FONT_SDF_RADIUS;
            int max_y = (int)y + FONT_SDF_RADIUS;
            int min_x = (int)x - FONT_SDF_RADIUS;
            int max_x = (int)x + FONT_SDF_RADIUS;

            if (min_y < 0) min_y = 0;
            if (min_x < 0) min_x = 0;
            if (max_y >= (int)height) max_y = (int)height - 1;
            if (max_x >= (int)width) max_x = (int)width - 1;
            for (int sy = min_y; sy <= max_y; sy++) {
                for (int sx = min_x; sx <= max_x; sx++) {
                    const uint8_t *sample =
                        source +
                        ((size_t)(region->y + sy) * atlas_width +
                         (size_t)(region->x + sx)) * 4;
                    bool sample_inside = sample[3] >= 128;
                    if (sample_inside != inside) {
                        float dx = (float)sx - (float)x;
                        float dy = (float)sy - (float)y;
                        float candidate = dx * dx + dy * dy;
                        if (candidate < best2) {
                            best2 = candidate;
                        }
                    }
                }
            }
            distance[(size_t)y * width + x] =
                inside ? sqrtf(best2) - 0.5f : -(sqrtf(best2) - 0.5f);
        }
    }

    for (uint32_t out_y = 0; out_y < height * upscale; out_y++) {
        float local_y = ((float)out_y + 0.5f) / (float)upscale - 0.5f;
        for (uint32_t out_x = 0; out_x < width * upscale; out_x++) {
            float local_x =
                ((float)out_x + 0.5f) / (float)upscale - 0.5f;
            float signed_distance =
                sample_distance(distance, width, height, local_x, local_y);
            float coverage = clamp_float(
                0.5f + signed_distance * (float)upscale, 0.0f, 1.0f);
            uint32_t atlas_x = (uint32_t)region->x * upscale + out_x;
            uint32_t atlas_y = (uint32_t)region->y * upscale + out_y;
            uint8_t *out_pixel =
                output + ((size_t)atlas_y * out_width + atlas_x) * 4;

            sample_colour_in_region(
                source, atlas_width, region, local_x, local_y, out_pixel);
            out_pixel[3] = (uint8_t)(coverage * 255.0f + 0.5f);
        }
    }
    return true;
}

bool gfx_font_sdf_upscale_rgba(
    const uint8_t *source, uint32_t width, uint32_t height, uint32_t upscale,
    const GfxFontAtlasRegion *regions, size_t region_count,
    uint8_t *output, size_t output_size) {
    size_t required = gfx_font_sdf_output_bytes(width, height, upscale);
    size_t largest_region_pixels = 0;
    float *distance;

    if (source == NULL || regions == NULL || region_count == 0 ||
        output == NULL || required == 0 || output_size < required) {
        return false;
    }
    for (size_t index = 0; index < region_count; index++) {
        GfxFontAtlasRegion clipped;
        size_t pixels;
        if (!clip_region(&regions[index], width, height, &clipped)) {
            continue;
        }
        pixels = (size_t)clipped.width * clipped.height;
        if (pixels > largest_region_pixels) {
            largest_region_pixels = pixels;
        }
    }
    if (largest_region_pixels == 0 ||
        largest_region_pixels > SIZE_MAX / sizeof(float)) {
        return false;
    }
    distance = (float *)malloc(largest_region_pixels * sizeof(float));
    if (distance == NULL) {
        return false;
    }

    memset(output, 0, required);
    for (size_t index = 0; index < region_count; index++) {
        GfxFontAtlasRegion clipped;
        if (clip_region(&regions[index], width, height, &clipped)) {
            derive_region(source, width, &clipped, upscale, output,
                          width * upscale, distance);
        }
    }
    free(distance);
    return true;
}
