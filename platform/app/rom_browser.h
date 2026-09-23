// User-initiated, controller-friendly file listing for the ROM and ZIP-pack
// pickers. No constructor or query searches disk: open() is the boundary.
#ifndef MDKR64_ROM_BROWSER_H
#define MDKR64_ROM_BROWSER_H

#include <filesystem>
#include <string>
#include <vector>

struct RomBrowserEntry {
    std::filesystem::path path;
    bool directory = false;
};

enum class BrowserFileKind { Rom, ZipPack };

class RomBrowser {
public:
    void open(const std::string &rememberedPath,
              BrowserFileKind kind = BrowserFileKind::Rom);
    // Returns false when the target cannot be opened; the current listing is
    // left intact so a failed removable-media visit never strands navigation.
    bool goTo(const std::filesystem::path &directory);
    void up();
    const std::filesystem::path &directory() const { return directory_; }
    const std::vector<RomBrowserEntry> &entries() const { return entries_; }
    const std::string &error() const { return error_; }
    static bool isRomFile(const std::filesystem::path &path);

private:
    std::filesystem::path directory_;
    std::vector<RomBrowserEntry> entries_;
    std::string error_;
    BrowserFileKind kind_ = BrowserFileKind::Rom;
};

#endif
