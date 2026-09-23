#include "rom_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

static std::string lowerExtension(const fs::path &path) {
    std::string ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool RomBrowser::isRomFile(const fs::path &path) {
    const std::string ext = lowerExtension(path);
    return ext == ".z64" || ext == ".v64" || ext == ".n64";
}

void RomBrowser::open(const std::string &rememberedPath, BrowserFileKind kind) {
    kind_ = kind;
    directory_.clear();
    entries_.clear();
    error_.clear();
    if (!rememberedPath.empty()) {
        const fs::path parent = fs::u8path(rememberedPath).parent_path();
        if (!parent.empty() && goTo(parent)) return;
    }
    const char *home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        const fs::path homePath(home);
        const fs::path downloads = homePath / "Downloads";
        if (goTo(downloads) || goTo(homePath)) return;
    }
    goTo(fs::path("/"));
}

bool RomBrowser::goTo(const fs::path &directory) {
    // Preserve the old location if a removable drive vanished or permissions
    // changed. The error remains visible and the user can still go up/root.
    std::error_code ec;
    fs::path absolute = fs::absolute(directory, ec);
    if (ec || !fs::is_directory(absolute, ec)) {
        error_ = "This folder is unavailable or cannot be opened.";
        return false;
    }
    std::vector<RomBrowserEntry> found;
    // Do not skip permission errors on the requested directory itself: doing
    // so yields an empty listing with no diagnostic, indistinguishable from a
    // genuinely empty folder. Individual inaccessible entries are skipped
    // below so one bad child does not hide its siblings.
    fs::directory_iterator it(absolute, fs::directory_options::none, ec);
    if (ec) {
        error_ = "This folder cannot be listed: " + ec.message();
        return false;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code typeError;
        const bool isDirectory = it->is_directory(typeError);
        if (typeError) continue;
        if (isDirectory) {
            found.push_back({it->path(), true});
        } else if ((kind_ == BrowserFileKind::Rom
                        ? isRomFile(it->path())
                        : lowerExtension(it->path()) == ".zip") &&
                   it->is_regular_file(typeError) && !typeError) {
            found.push_back({it->path(), false});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        if (a.directory != b.directory) return a.directory;
        std::string left = a.path.filename().u8string();
        std::string right = b.path.filename().u8string();
        const std::string leftOriginal = left;
        const std::string rightOriginal = right;
        std::transform(left.begin(), left.end(), left.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(right.begin(), right.end(), right.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return left != right ? left < right : leftOriginal < rightOriginal;
    });
    directory_ = std::move(absolute);
    entries_ = std::move(found);
    error_ = ec ? "Some entries could not be listed: " + ec.message() : "";
    return true;
}

void RomBrowser::up() {
    if (directory_.empty()) return;
    const fs::path parent = directory_.parent_path();
    if (!parent.empty() && parent != directory_) goTo(parent);
}
