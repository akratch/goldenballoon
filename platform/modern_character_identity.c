#include "modern_character_identity.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "stb_image_alloc.h"

#define MODERN_PORTRAIT_SOURCE_MIN 16
#define MODERN_PORTRAIT_SOURCE_MAX 1024
#define MODERN_PORTRAIT_ENCODED_MAX (8u * 1024u * 1024u)

_Static_assert((uint64_t)MODERN_PORTRAIT_SOURCE_MAX *
                   MODERN_PORTRAIT_SOURCE_MAX <= (uint64_t)INT_MAX / 8u,
               "PNG decode intermediates must fit signed integer sizes");

_Static_assert(MODERN_PORTRAIT_ENCODED_MAX <= (unsigned)INT_MAX,
               "the bounded portrait must fit stb_image's int length");

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0u) {
        (void)snprintf(error, size, "%s",
                       message != NULL ? message : "unknown portrait error");
    }
}

static uint32_t sample_coordinate(unsigned target, unsigned source) {
    uint64_t center = ((uint64_t)target * 2u + 1u) * source * 65536u;
    uint64_t scaled = center / (MDKR_MODERN_PORTRAIT_SIZE * 2u);
    return scaled <= 32768u ? 0u :
        (uint32_t)(scaled - 32768u);
}

/* Integer, premultiplied-alpha bilinear sampling is deterministic and avoids
 * dark fringes around transparent hair/fur. */
static void resize_portrait(const uint8_t *source, unsigned source_size,
                            uint8_t *target) {
    unsigned y;
    for (y = 0u; y < MDKR_MODERN_PORTRAIT_SIZE; y++) {
        uint32_t sy = sample_coordinate(y, source_size);
        unsigned y0 = sy >> 16u;
        unsigned y1;
        uint32_t fy = sy & 0xFFFFu;
        unsigned x;
        if (y0 >= source_size) y0 = source_size - 1u;
        y1 = y0 + 1u < source_size ? y0 + 1u : y0;
        for (x = 0u; x < MDKR_MODERN_PORTRAIT_SIZE; x++) {
            uint32_t sx = sample_coordinate(x, source_size);
            unsigned x0 = sx >> 16u;
            unsigned x1;
            uint32_t fx = sx & 0xFFFFu;
            const uint8_t *samples[4];
            uint64_t weights[4];
            uint64_t alpha_sum = 0u;
            unsigned component;
            unsigned sample;
            uint8_t *pixel = target +
                (y * MDKR_MODERN_PORTRAIT_SIZE + x) * 4u;
            if (x0 >= source_size) x0 = source_size - 1u;
            x1 = x0 + 1u < source_size ? x0 + 1u : x0;
            samples[0] = source + (y0 * source_size + x0) * 4u;
            samples[1] = source + (y0 * source_size + x1) * 4u;
            samples[2] = source + (y1 * source_size + x0) * 4u;
            samples[3] = source + (y1 * source_size + x1) * 4u;
            weights[0] = (uint64_t)(65536u - fx) * (65536u - fy);
            weights[1] = (uint64_t)fx * (65536u - fy);
            weights[2] = (uint64_t)(65536u - fx) * fy;
            weights[3] = (uint64_t)fx * fy;
            for (sample = 0u; sample < 4u; sample++) {
                alpha_sum += weights[sample] * samples[sample][3];
            }
            pixel[3] = (uint8_t)((alpha_sum + (UINT64_C(1) << 31u)) >> 32u);
            for (component = 0u; component < 3u; component++) {
                uint64_t premultiplied = 0u;
                for (sample = 0u; sample < 4u; sample++) {
                    premultiplied += weights[sample] * samples[sample][3] *
                                     samples[sample][component];
                }
                pixel[component] = alpha_sum != 0u
                    ? (uint8_t)((premultiplied + alpha_sum / 2u) / alpha_sum)
                    : 0u;
            }
        }
    }
}

int mdkr_modern_identity_init(const MdkrModernCharacterAsset *asset,
                              MdkrModernDecodedIdentity *out,
                              char *error, size_t error_size) {
    MdkrModernIdentity identity;
    const uint8_t *encoded = NULL;
    unsigned char *decoded;
    int width = 0;
    int height = 0;
    int components = 0;
    int inspected_width;
    int inspected_height;
    if (out == NULL || asset == NULL) {
        set_error(error, error_size, "portrait arguments are invalid");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!mdkr_modern_character_asset_identity(asset, &identity, &encoded)) {
        set_error(error, error_size, "");
        return 1;
    }
    if (identity.portrait_size > MODERN_PORTRAIT_ENCODED_MAX ||
        identity.portrait_size > (uint32_t)INT_MAX ||
        !stbi_info_from_memory(encoded, (int)identity.portrait_size,
                               &width, &height, &components) ||
        width < MODERN_PORTRAIT_SOURCE_MIN ||
        width > MODERN_PORTRAIT_SOURCE_MAX || height != width ||
        (components != 3 && components != 4)) {
        set_error(error, error_size,
                  "compiled portrait is not a bounded square RGB/RGBA PNG");
        return 0;
    }
    inspected_width = width;
    inspected_height = height;
    {
        /* Bound every decoder allocation by what this header can need. */
        const size_t bound = mdkr_stbi_png_allocation_bound(
            width, height, components,
            stbi_is_16_bit_from_memory(encoded, (int)identity.portrait_size), 4,
            identity.portrait_size);
        const size_t previous = mdkr_stbi_set_allocation_limit(bound);
        decoded = bound != 0u
                      ? stbi_load_from_memory(encoded, (int)identity.portrait_size,
                                              &width, &height, &components, 4)
                      : NULL;
        (void)mdkr_stbi_set_allocation_limit(previous);
    }
    if (decoded == NULL) {
        set_error(error, error_size,
                  "compiled portrait PNG could not be decoded");
        return 0;
    }
    if (width != inspected_width || height != inspected_height ||
        width < MODERN_PORTRAIT_SOURCE_MIN ||
        width > MODERN_PORTRAIT_SOURCE_MAX || height != width ||
        (components != 3 && components != 4)) {
        stbi_image_free(decoded);
        set_error(error, error_size,
                  "compiled portrait PNG changed dimensions during bounded decode");
        return 0;
    }
    resize_portrait(decoded, (unsigned)width, out->portrait_rgba);
    stbi_image_free(decoded);
    out->minimap_rgba[0] = (uint8_t)identity.minimap_rgba;
    out->minimap_rgba[1] = (uint8_t)(identity.minimap_rgba >> 8u);
    out->minimap_rgba[2] = (uint8_t)(identity.minimap_rgba >> 16u);
    out->minimap_rgba[3] = (uint8_t)(identity.minimap_rgba >> 24u);
    out->has_portrait = 1;
    set_error(error, error_size, "");
    return 1;
}
