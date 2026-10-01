#include "character_portrait_import.h"

#include "character_png_validation.h"
#include "fs_utf8.h"
#include "sha256.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "stb_image_alloc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr size_t kMaximumPngBytes = 32u * 1024u * 1024u;

static_assert(static_cast<uint64_t>(CharacterPortraitImport::kMaximumDimension) *
                  CharacterPortraitImport::kMaximumDimension <=
                  static_cast<uint64_t>(std::numeric_limits<int>::max()) / 8u,
              "PNG decode intermediates must fit signed integer sizes");

using CharacterPortraitImport::Background;
using CharacterPortraitImport::Image;
using CharacterPortraitImport::Recipe;
using CharacterPortraitImport::Sampling;

size_t pixelOffset(uint32_t width, uint32_t x, uint32_t y) {
    return (static_cast<size_t>(y) * width + x) * 4u;
}

bool imageByteSize(uint32_t width, uint32_t height, size_t &bytes) {
    if (width < CharacterPortraitImport::kMinimumDimension ||
        height < CharacterPortraitImport::kMinimumDimension ||
        width > CharacterPortraitImport::kMaximumDimension ||
        height > CharacterPortraitImport::kMaximumDimension ||
        static_cast<size_t>(width) >
            std::numeric_limits<size_t>::max() / height / 4u) {
        return false;
    }
    bytes = static_cast<size_t>(width) * height * 4u;
    return true;
}

uint8_t roundedByte(double value) {
    return static_cast<uint8_t>(std::lround(
        std::clamp(value, 0.0, 255.0)));
}

std::array<uint8_t, 4> sourcePixel(
    const Image &image, const std::vector<uint8_t> &matte,
    uint32_t x, uint32_t y) {
    std::array<uint8_t, 4> result{};
    const size_t offset = pixelOffset(image.width, x, y);
    std::memcpy(result.data(), image.rgba.data() + offset, result.size());
    if (!matte.empty() && matte[static_cast<size_t>(y) * image.width + x]) {
        result[3] = 0u;
    }
    return result;
}

std::vector<uint8_t> edgeMatte(const Image &image, uint32_t tolerance) {
    if (tolerance == 0u) return {};
    const size_t pixels = static_cast<size_t>(image.width) * image.height;
    std::vector<uint8_t> removed(pixels, 0u);
    std::vector<uint32_t> queue;
    queue.reserve(std::min<size_t>(pixels, 65536u));
    const uint32_t corners[4][2] = {
        {0u, 0u}, {image.width - 1u, 0u},
        {0u, image.height - 1u},
        {image.width - 1u, image.height - 1u},
    };
    for (const auto &corner : corners) {
        const size_t seedOffset = pixelOffset(
            image.width, corner[0], corner[1]);
        const uint8_t seed[3] = {
            image.rgba[seedOffset], image.rgba[seedOffset + 1u],
            image.rgba[seedOffset + 2u],
        };
        queue.clear();
        const uint32_t start = corner[1] * image.width + corner[0];
        if (removed[start] == 0u) {
            removed[start] = 1u;
            queue.push_back(start);
        }
        for (size_t read = 0u; read < queue.size(); ++read) {
            const uint32_t index = queue[read];
            const uint32_t x = index % image.width;
            const uint32_t y = index / image.width;
            const uint32_t neighbours[4] = {
                x != 0u ? index - 1u : UINT32_MAX,
                x + 1u < image.width ? index + 1u : UINT32_MAX,
                y != 0u ? index - image.width : UINT32_MAX,
                y + 1u < image.height ? index + image.width : UINT32_MAX,
            };
            for (uint32_t neighbour : neighbours) {
                if (neighbour == UINT32_MAX || removed[neighbour] != 0u) {
                    continue;
                }
                const size_t offset = static_cast<size_t>(neighbour) * 4u;
                bool similar = true;
                for (unsigned component = 0u; component < 3u; ++component) {
                    const unsigned value = image.rgba[offset + component];
                    const unsigned reference = seed[component];
                    const unsigned difference = value > reference
                        ? value - reference : reference - value;
                    if (difference > tolerance) similar = false;
                }
                if (similar) {
                    removed[neighbour] = 1u;
                    queue.push_back(neighbour);
                }
            }
        }
    }
    return removed;
}

std::array<uint8_t, 4> sampleNearest(
    const Image &image, const std::vector<uint8_t> &matte,
    double x, double y) {
    const uint32_t sourceX = static_cast<uint32_t>(std::clamp(
        std::floor(x), 0.0, static_cast<double>(image.width - 1u)));
    const uint32_t sourceY = static_cast<uint32_t>(std::clamp(
        std::floor(y), 0.0, static_cast<double>(image.height - 1u)));
    return sourcePixel(image, matte, sourceX, sourceY);
}

std::array<uint8_t, 4> sampleArea(
    const Image &image, const std::vector<uint8_t> &matte,
    double left, double top, double right, double bottom) {
    double sum[4] = {};
    double totalWeight = 0.0;
    const uint32_t x0 = static_cast<uint32_t>(std::floor(left));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(top));
    const uint32_t x1 = static_cast<uint32_t>(std::ceil(right));
    const uint32_t y1 = static_cast<uint32_t>(std::ceil(bottom));
    for (uint32_t y = y0; y < y1 && y < image.height; ++y) {
        const double vertical = std::max(
            0.0, std::min(bottom, static_cast<double>(y + 1u)) -
                     std::max(top, static_cast<double>(y)));
        for (uint32_t x = x0; x < x1 && x < image.width; ++x) {
            const double horizontal = std::max(
                0.0, std::min(right, static_cast<double>(x + 1u)) -
                         std::max(left, static_cast<double>(x)));
            const double weight = horizontal * vertical;
            if (weight == 0.0) continue;
            const auto pixel = sourcePixel(image, matte, x, y);
            const double alpha = pixel[3] / 255.0;
            for (unsigned component = 0u; component < 3u; ++component) {
                sum[component] += pixel[component] * alpha * weight;
            }
            sum[3] += pixel[3] * weight;
            totalWeight += weight;
        }
    }
    if (totalWeight <= 0.0) return {};
    const double alpha = sum[3] / totalWeight;
    std::array<uint8_t, 4> result{};
    result[3] = roundedByte(alpha);
    if (alpha > 0.0) {
        const double premultipliedScale = 255.0 / alpha / totalWeight;
        for (unsigned component = 0u; component < 3u; ++component) {
            result[component] = roundedByte(
                sum[component] * premultipliedScale);
        }
    }
    return result;
}

std::array<uint8_t, 3> backgroundColour(
    Background background, uint32_t x, uint32_t y, uint32_t size) {
    const double horizontal = size > 1u
        ? static_cast<double>(x) / (size - 1u) : 0.0;
    const double vertical = size > 1u
        ? static_cast<double>(y) / (size - 1u) : 0.0;
    std::array<uint8_t, 3> top{};
    std::array<uint8_t, 3> bottom{};
    switch (background) {
        case Background::Sunset:
            top = {92u, 55u, 112u};
            bottom = {226u, 128u, 63u};
            break;
        case Background::Sky:
            top = {47u, 104u, 168u};
            bottom = {136u, 201u, 210u};
            break;
        case Background::Charcoal:
            top = {22u, 28u, 38u};
            bottom = {67u, 74u, 82u};
            break;
        case Background::Transparent:
        default:
            return {};
    }
    const double vignette = std::abs(horizontal - 0.5) * 0.20;
    std::array<uint8_t, 3> result{};
    for (unsigned component = 0u; component < 3u; ++component) {
        result[component] = roundedByte(
            top[component] * (1.0 - vertical) +
            bottom[component] * vertical - vignette * 255.0);
    }
    return result;
}

void compositeBackground(std::array<uint8_t, 4> &pixel,
                         Background background,
                         uint32_t x, uint32_t y, uint32_t size) {
    if (background == Background::Transparent) return;
    const auto colour = backgroundColour(background, x, y, size);
    const unsigned alpha = pixel[3];
    for (unsigned component = 0u; component < 3u; ++component) {
        pixel[component] = static_cast<uint8_t>(
            (static_cast<unsigned>(pixel[component]) * alpha +
             static_cast<unsigned>(colour[component]) * (255u - alpha) +
             127u) / 255u);
    }
    pixel[3] = 255u;
}

}  // namespace

namespace CharacterPortraitImport {

bool validImage(const Image &image) {
    size_t bytes = 0u;
    return imageByteSize(image.width, image.height, bytes) &&
        image.rgba.size() == bytes && image.sha256.size() == 64u &&
        std::all_of(image.sha256.begin(), image.sha256.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        });
}

bool loadPng(const std::string &path, Image &output, std::string &error) {
    FILE *file = path.empty() ? nullptr : mdkr_fopen_utf8(path.c_str(), "rb");
    if (file == nullptr) {
        error = "The portrait source PNG could not be opened.";
        return false;
    }
    std::vector<unsigned char> encoded;
    std::array<unsigned char, 8192u> block{};
    bool bounded = true;
    for (;;) {
        const size_t count = std::fread(block.data(), 1u, block.size(), file);
        if (count != 0u) {
            if (encoded.size() > kMaximumPngBytes - count) {
                bounded = false;
                break;
            }
            encoded.insert(encoded.end(), block.begin(), block.begin() + count);
        }
        if (count != block.size()) break;
    }
    const bool readOk = std::ferror(file) == 0;
    const bool closeOk = std::fclose(file) == 0;
    CharacterPngValidation::Info info;
    if (!bounded || !readOk || !closeOk ||
        !CharacterPngValidation::validate(
            encoded.data(), encoded.size(), 0u, 0u, info, error)) {
        if (error.empty()) {
            error = !bounded
                ? "The portrait source exceeds the 32 MiB encoded limit."
                : "The portrait source PNG could not be read completely.";
        }
        return false;
    }
    size_t decodedBytes = 0u;
    if (info.bitDepth != 8u ||
        (info.colourType != 2u && info.colourType != 6u) ||
        !imageByteSize(info.width, info.height, decodedBytes)) {
        error = "Portrait sources must be 16-4096 px, non-interlaced, 8-bit RGB or RGBA PNGs.";
        return false;
    }
    int width = 0;
    int height = 0;
    int components = 0;
    /* Bound every decoder allocation by what the validated header needs. */
    const size_t decodeBound = mdkr_stbi_png_allocation_bound(
        static_cast<int>(info.width), static_cast<int>(info.height),
        info.colourType == 6u ? 4 : 3, 0, 4, encoded.size());
    const size_t previousBound = mdkr_stbi_set_allocation_limit(decodeBound);
    unsigned char *decoded = decodeBound != 0u
        ? stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                &width, &height, &components, 4)
        : nullptr;
    (void)mdkr_stbi_set_allocation_limit(previousBound);
    if (decoded == nullptr || width != static_cast<int>(info.width) ||
        height != static_cast<int>(info.height)) {
        if (decoded != nullptr) stbi_image_free(decoded);
        const char *reason = stbi_failure_reason();
        error = "The portrait PNG pixel stream could not be decoded";
        if (reason != nullptr && reason[0] != '\0') {
            error += ": ";
            error += reason;
        } else {
            error += ".";
        }
        return false;
    }
    Image loaded;
    loaded.width = info.width;
    loaded.height = info.height;
    loaded.rgba.assign(decoded, decoded + decodedBytes);
    stbi_image_free(decoded);
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(encoded.data(), encoded.size(), digest);
    loaded.sha256 = digest;
    output = std::move(loaded);
    error.clear();
    return true;
}

Recipe centredRecipe(const Image &image) {
    Recipe result;
    if (!validImage(image)) return result;
    result.cropSize = std::min(image.width, image.height);
    result.cropX = (image.width - result.cropSize) / 2u;
    result.cropY = (image.height - result.cropSize) / 2u;
    return result;
}

bool validRecipe(const Image &image, const Recipe &recipe) {
    return validImage(image) && recipe.cropSize != 0u &&
        recipe.cropSize <= image.width && recipe.cropSize <= image.height &&
        recipe.cropX <= image.width - recipe.cropSize &&
        recipe.cropY <= image.height - recipe.cropSize &&
        recipe.edgeMatteTolerance <= 96u &&
        recipe.sampling >= Sampling::Crisp &&
        recipe.sampling <= Sampling::Area &&
        recipe.background >= Background::Transparent &&
        recipe.background <= Background::Charcoal;
}

bool validSubjectMask(const SubjectMask &mask) {
    (void)mask;
    return true;
}

bool validSourceRecord(const SourceRecord &record) {
    if (record.kind == SourceKind::Canvas) {
        const Recipe defaults;
        return record.sha256.empty() && record.width == 0u &&
            record.height == 0u && record.recipe.cropX == 0u &&
            record.recipe.cropY == 0u && record.recipe.cropSize == 0u &&
            record.recipe.edgeMatteTolerance == 0u &&
            record.recipe.sampling == defaults.sampling &&
            record.recipe.background == defaults.background &&
            validSubjectMask(record.subjectMask) &&
            !record.subjectMask.enabled &&
            std::all_of(record.subjectMask.alpha.begin(),
                        record.subjectMask.alpha.end(),
                        [](uint8_t alpha) { return alpha == 255u; });
    }
    if (record.kind != SourceKind::LocalPng &&
        record.kind != SourceKind::ExactRenderer) return false;
    size_t bytes = 0u;
    return imageByteSize(record.width, record.height, bytes) &&
        record.sha256.size() == 64u &&
        std::all_of(record.sha256.begin(), record.sha256.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        }) &&
        record.recipe.cropSize != 0u &&
        record.recipe.cropSize <= record.width &&
        record.recipe.cropSize <= record.height &&
        record.recipe.cropX <= record.width - record.recipe.cropSize &&
        record.recipe.cropY <= record.height - record.recipe.cropSize &&
        record.recipe.edgeMatteTolerance <= 96u &&
        record.recipe.sampling >= Sampling::Crisp &&
        record.recipe.sampling <= Sampling::Area &&
        record.recipe.background >= Background::Transparent &&
        record.recipe.background <= Background::Charcoal &&
        validSubjectMask(record.subjectMask);
}

SourceRecord sourceRecord(const Image &image, const Recipe &recipe,
                          SourceKind kind,
                          const SubjectMask &subjectMask) {
    SourceRecord record;
    record.kind = kind;
    record.sha256 = image.sha256;
    record.width = image.width;
    record.height = image.height;
    record.recipe = recipe;
    record.subjectMask = subjectMask;
    return validImage(image) && validRecipe(image, recipe) &&
            validSourceRecord(record)
        ? record : SourceRecord{};
}

bool render(const Image &image, const Recipe &recipe,
            CharacterPortraitStudio::Canvas &output, std::string &error) {
    return render(image, recipe, SubjectMask{}, output, error);
}

bool render(const Image &image, const Recipe &recipe,
            const SubjectMask &subjectMask,
            CharacterPortraitStudio::Canvas &output, std::string &error) {
    if (!validRecipe(image, recipe) || !validSubjectMask(subjectMask)) {
        error = "The portrait crop or conversion recipe is invalid.";
        return false;
    }
    const auto matte = edgeMatte(image, recipe.edgeMatteTolerance);
    CharacterPortraitStudio::Canvas rendered{};
    constexpr uint32_t size = CharacterPortraitStudio::kSize;
    const double sourcePerPixel =
        static_cast<double>(recipe.cropSize) / size;
    for (uint32_t y = 0u; y < size; ++y) {
        for (uint32_t x = 0u; x < size; ++x) {
            const double left = recipe.cropX + x * sourcePerPixel;
            const double top = recipe.cropY + y * sourcePerPixel;
            std::array<uint8_t, 4> pixel = recipe.sampling == Sampling::Crisp
                ? sampleNearest(image, matte,
                      left + sourcePerPixel * 0.5,
                      top + sourcePerPixel * 0.5)
                : sampleArea(image, matte, left, top,
                      left + sourcePerPixel, top + sourcePerPixel);
            if (subjectMask.enabled) {
                const size_t maskIndex = static_cast<size_t>(y) * size + x;
                pixel[3] = static_cast<uint8_t>(
                    (static_cast<unsigned>(pixel[3]) *
                     subjectMask.alpha[maskIndex] + 127u) / 255u);
                if (pixel[3] == 0u) {
                    pixel[0] = pixel[1] = pixel[2] = 0u;
                }
            }
            compositeBackground(pixel, recipe.background, x, y, size);
            std::memcpy(
                rendered.data() + (static_cast<size_t>(y) * size + x) * 4u,
                pixel.data(), pixel.size());
        }
    }
    output = rendered;
    error.clear();
    return true;
}

bool makeThumbnail(const Image &image, Thumbnail &output) {
    if (!validImage(image)) return false;
    const double scale = std::min(
        1.0, static_cast<double>(kThumbnailMaximumDimension) /
                 std::max(image.width, image.height));
    Thumbnail thumbnail;
    thumbnail.width = std::max<uint32_t>(
        1u, static_cast<uint32_t>(std::lround(image.width * scale)));
    thumbnail.height = std::max<uint32_t>(
        1u, static_cast<uint32_t>(std::lround(image.height * scale)));
    thumbnail.rgba.resize(
        static_cast<size_t>(thumbnail.width) * thumbnail.height * 4u);
    const std::vector<uint8_t> noMatte;
    for (uint32_t y = 0u; y < thumbnail.height; ++y) {
        const double top = static_cast<double>(y) * image.height /
            thumbnail.height;
        const double bottom = static_cast<double>(y + 1u) * image.height /
            thumbnail.height;
        for (uint32_t x = 0u; x < thumbnail.width; ++x) {
            const double left = static_cast<double>(x) * image.width /
                thumbnail.width;
            const double right = static_cast<double>(x + 1u) * image.width /
                thumbnail.width;
            const auto pixel = sampleArea(
                image, noMatte, left, top, right, bottom);
            std::memcpy(
                thumbnail.rgba.data() +
                    (static_cast<size_t>(y) * thumbnail.width + x) * 4u,
                pixel.data(), pixel.size());
        }
    }
    output = std::move(thumbnail);
    return true;
}

}  // namespace CharacterPortraitImport
