#include "character_preview_cache.h"

#include "fs_utf8.h"
#include "sha256.h"

#include <cerrno>
#include <filesystem>

namespace CharacterPreviewCache {
namespace {

constexpr const char *kCacheDirectory = ".workshop-preview-cache";
constexpr size_t kMaximumRendererPathBytes = 1023u;

bool contextValid(uint32_t context) {
    return context >= kFirstContext && context <= kLastContext;
}

bool cacheDirectoryPath(const std::string &charactersDirectory,
                        std::string &path) {
    if (charactersDirectory.empty() ||
        charactersDirectory.size() > kMaximumRendererPathBytes) {
        return false;
    }
    path = charactersDirectory;
    if (path.back() != '/' && path.back() != '\\') path.push_back('/');
    path += kCacheDirectory;
    return path.size() <= kMaximumRendererPathBytes;
}

bool capturePathFor(const std::string &charactersDirectory,
                    const std::string &packageId,
                    uint32_t context,
                    Product product,
                    bool alternate,
                    std::string &path) {
    if (packageId.empty() || packageId.size() > 64u ||
        !contextValid(context) ||
        (product != Product::CustomScene &&
         product != Product::RetailDonorScene &&
         product != Product::CustomModelAlpha)) {
        return false;
    }
    std::string directory;
    if (!cacheDirectoryPath(charactersDirectory, directory)) return false;
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(packageId.data(), packageId.size(), digest);
    path = directory + "/" + std::string(digest, 64u) + "-" +
        std::to_string(context) +
        (product == Product::RetailDonorScene
             ? (alternate ? "-donor-next.png" : "-donor.png")
         : product == Product::CustomModelAlpha
             ? (alternate ? "-model-next.png" : "-model.png")
             : (alternate ? "-next.png" : ".png"));
    return path.size() <= kMaximumRendererPathBytes;
}

bool ordinaryDirectory(const std::string &path) {
    int exists = 0;
    int directory = 0;
    return mdkr_path_query_utf8(
               path.c_str(), &exists, nullptr, &directory) == 0 &&
           exists && directory &&
           mdkr_path_is_link_or_reparse_utf8(path.c_str()) == 0;
}

bool ensureCacheDirectory(const std::string &charactersDirectory,
                          std::string &path,
                          std::string &error) {
    if (!cacheDirectoryPath(charactersDirectory, path)) {
        error = "The local character storage path is too long for an exact preview capture.";
        return false;
    }
    int exists = 0;
    if (mdkr_path_query_utf8(path.c_str(), &exists, nullptr, nullptr) == 0 &&
        exists) {
        if (ordinaryDirectory(path)) return true;
        error = "The inline preview cache location is not a safe local directory.";
        return false;
    }
    errno = 0;
    if (mdkr_mkdir_utf8(path.c_str()) != 0 && errno != EEXIST) {
        error = "The launcher could not create its local inline preview cache.";
        return false;
    }
    if (!ordinaryDirectory(path)) {
        error = "The inline preview cache could not be verified after creation.";
        return false;
    }
    return true;
}

bool removeExactPath(const std::string &path) {
    const int link = mdkr_path_is_link_or_reparse_utf8(path.c_str());
    if (link == 1) return false;
    int exists = 0;
    int regular = 0;
    int directory = 0;
    const int query = mdkr_path_query_utf8(
        path.c_str(), &exists, &regular, &directory);
    if (!exists) {
        /* Missing is already removed; a failed query proves nothing. */
        return query == 0;
    }
    if (!regular || directory || link != 0) {
        return false;
    }
    return mdkr_remove_utf8(path.c_str()) == 0;
}

bool managedFilename(const std::string &name) {
    if (name.size() < 70u || name[64] != '-' ||
        name[65] < '1' || name[65] > '4') {
        return false;
    }
    const std::string suffix = name.substr(66u);
    if (suffix != ".png" && suffix != "-next.png" &&
        suffix != "-donor.png" && suffix != "-donor-next.png") {
        if (suffix != "-model.png" && suffix != "-model-next.png") {
            return false;
        }
    }
    for (size_t index = 0u; index < 64u; ++index) {
        const char byte = name[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool prepare(const std::string &charactersDirectory,
             const std::string &packageId,
             uint32_t context,
             Product product,
             std::string &capturePath,
             std::string &error) {
    capturePath.clear();
    error.clear();
    std::string directory;
    if (!ensureCacheDirectory(charactersDirectory, directory, error) ||
        !capturePathFor(
            charactersDirectory, packageId, context, product, false,
            capturePath)) {
        if (error.empty()) {
            error = "The package or preview context cannot own an inline capture path.";
        }
        capturePath.clear();
        return false;
    }
    if (!removeExactPath(capturePath)) {
        error = "The previous inline preview is not a regular launcher-owned file and was preserved.";
        capturePath.clear();
        return false;
    }
    return true;
}

bool preparePreserving(const std::string &charactersDirectory,
                       const std::string &packageId,
                       uint32_t context,
                       Product product,
                       const std::string &retainedPath,
                       std::string &capturePath,
                       std::string &error) {
    capturePath.clear();
    error.clear();
    std::string directory;
    std::string primary;
    std::string alternate;
    if (!ensureCacheDirectory(charactersDirectory, directory, error) ||
        !capturePathFor(charactersDirectory, packageId, context, product,
                        false, primary) ||
        !capturePathFor(charactersDirectory, packageId, context, product,
                        true, alternate)) {
        if (error.empty()) {
            error = "The package or preview context cannot own a preserving inline capture path.";
        }
        return false;
    }
    if (!retainedPath.empty() && retainedPath != primary &&
        retainedPath != alternate) {
        error = "The retained inline preview does not belong to this exact package, context, and product.";
        return false;
    }
    capturePath = retainedPath == primary ? alternate : primary;
    if (!removeExactPath(capturePath)) {
        error = "The inactive inline preview slot is not a regular launcher-owned file and was preserved.";
        capturePath.clear();
        return false;
    }
    return true;
}

bool prepare(const std::string &charactersDirectory,
             const std::string &packageId,
             uint32_t context,
             std::string &capturePath,
             std::string &error) {
    return prepare(charactersDirectory, packageId, context,
                   Product::CustomScene, capturePath, error);
}

bool owns(const std::string &charactersDirectory,
          const std::string &packageId,
          const std::string &capturePath) {
    for (uint32_t context = kFirstContext;
         context <= kLastContext; ++context) {
        for (Product product :
             {Product::CustomScene, Product::RetailDonorScene,
              Product::CustomModelAlpha}) {
            std::string expected;
            for (bool alternate : {false, true}) {
                if (capturePathFor(
                        charactersDirectory, packageId, context, product,
                        alternate, expected) && capturePath == expected) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool remove(const std::string &charactersDirectory,
            const std::string &packageId,
            uint32_t context) {
    bool removed = true;
    for (Product product :
         {Product::CustomScene, Product::CustomModelAlpha}) {
        for (bool alternate : {false, true}) {
            std::string path;
            removed = capturePathFor(
                          charactersDirectory, packageId, context,
                          product, alternate, path) &&
                    removeExactPath(path) && removed;
        }
    }
    return removed;
}

bool removeOwnedPath(const std::string &charactersDirectory,
                     const std::string &packageId,
                     const std::string &capturePath) {
    return owns(charactersDirectory, packageId, capturePath) &&
        removeExactPath(capturePath);
}

void removePackage(const std::string &charactersDirectory,
                   const std::string &packageId) {
    for (uint32_t context = kFirstContext;
         context <= kLastContext; ++context) {
        (void)remove(charactersDirectory, packageId, context);
        std::string donorPath;
        for (bool alternate : {false, true}) {
            if (capturePathFor(
                    charactersDirectory, packageId, context,
                    Product::RetailDonorScene, alternate, donorPath)) {
                (void)removeExactPath(donorPath);
            }
        }
    }
}

bool removeAll(const std::string &charactersDirectory) {
    std::string directory;
    if (!cacheDirectoryPath(charactersDirectory, directory)) return false;
    int exists = 0;
    const int query = mdkr_path_query_utf8(
        directory.c_str(), &exists, nullptr, nullptr);
    if (!exists) {
        /* Missing is already removed; a failed query proves nothing. */
        return query == 0;
    }
    if (!ordinaryDirectory(directory)) return false;
    std::error_code error;
    std::filesystem::directory_iterator current(
        std::filesystem::u8path(directory), error);
    const std::filesystem::directory_iterator end;
    bool removed = true;
    while (!error && current != end) {
        const std::filesystem::path path = current->path();
        std::error_code statusError;
        const std::filesystem::file_status status =
            current->symlink_status(statusError);
        if (!statusError && std::filesystem::is_regular_file(status) &&
            managedFilename(path.filename().u8string())) {
            removed = mdkr_remove_utf8(path.u8string().c_str()) == 0 &&
                removed;
        }
        current.increment(error);
    }
    return !error && removed;
}

}  // namespace CharacterPreviewCache
