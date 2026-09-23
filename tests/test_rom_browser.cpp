#include "rom_browser.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("mdkr-rom-browser-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    if (!fs::create_directory(root, ec)) return 1;
    const fs::path folder = root / "a-folder";
    fs::create_directory(folder, ec);
    if (ec) return 2;
    std::ofstream(root / "game.Z64").put('x');
    std::ofstream(root / "other.v64").put('x');
    std::ofstream(root / "Zoo.z64").put('x');
    std::ofstream(root / "apple.z64").put('x');
    std::ofstream(root / "notes.txt").put('x');

    RomBrowser browser;
    browser.open((root / "game.Z64").string());
    if (browser.directory() != root || browser.entries().size() != 5 ||
        !browser.entries()[0].directory ||
        browser.entries()[0].path != folder ||
        browser.entries()[1].path.filename() != "apple.z64" ||
        browser.entries()[2].path.filename() != "game.Z64" ||
        browser.entries()[4].path.filename() != "Zoo.z64" ||
        !RomBrowser::isRomFile("anything.N64") ||
        RomBrowser::isRomFile("notes.txt")) return 3;

    std::ofstream(root / "Textures.ZIP").put('x');
    browser.open((root / "Textures.ZIP").string(), BrowserFileKind::ZipPack);
    if (browser.directory() != root || browser.entries().size() != 2 ||
        browser.entries()[0].path != folder ||
        browser.entries()[1].path.filename() != "Textures.ZIP") return 19;
    browser.open((root / "game.Z64").string());

    browser.goTo(folder);
    if (browser.directory() != folder || !browser.entries().empty()) return 4;
    if (browser.goTo(folder / "missing")) return 5;
    if (browser.directory() != folder || browser.error().empty()) return 5;
    browser.up();
    if (browser.directory() != root || !browser.error().empty()) return 6;

    const fs::path unicodeFolder = root / fs::u8path(u8"Forêt-日本語");
    if (!fs::create_directory(unicodeFolder, ec) || ec) return 20;
    const fs::path unicodeRom = unicodeFolder / fs::u8path(u8"Course-é-日本語.Z64");
    const fs::path unicodeZip = unicodeFolder / fs::u8path(u8"Textures-é-日本語.ZIP");
    std::ofstream(unicodeRom).put('x');
    std::ofstream(unicodeZip).put('x');
    browser.open(unicodeRom.u8string());
    if (browser.directory() != unicodeFolder || browser.entries().size() != 1 ||
        browser.entries()[0].path != unicodeRom) return 21;
    browser.open(unicodeZip.u8string(), BrowserFileKind::ZipPack);
    if (browser.directory() != unicodeFolder || browser.entries().size() != 1 ||
        browser.entries()[0].path != unicodeZip) return 22;
    browser.up();
    if (browser.directory() != root) return 23;

#ifndef _WIN32
    if (setenv("HOME", (root / "missing-home").c_str(), 1) != 0) return 17;
    browser.open((root / "missing" / "game.z64").string());
    if (browser.directory() != "/" || !browser.error().empty()) return 18;

    const fs::path downloads = root / "Downloads";
    fs::create_directory(downloads, ec);
    if (ec || setenv("HOME", root.c_str(), 1) != 0) return 13;
    browser.open((root / "missing" / "game.z64").string());
    if (browser.directory() != downloads || !browser.error().empty()) return 14;
    browser.open("");
    if (browser.directory() != downloads || !browser.error().empty()) return 15;
    browser.goTo(root);

    // A denied removable-media folder must not look like an empty folder.
    // Privileged runners may bypass mode bits, so assert only when the host
    // itself observes EACCES. Always restore permissions before cleanup.
    const fs::path restricted = root / "restricted";
    fs::create_directory(restricted, ec);
    if (ec) return 8;
    fs::permissions(restricted, fs::perms::none, ec);
    if (ec) return 9;
    std::error_code denied;
    fs::directory_iterator probe(restricted, denied);
    (void)probe;
    if (denied) {
        if (browser.goTo(restricted)) return 11;
        if (browser.directory() != root || browser.error().empty()) return 11;
        browser.open((restricted / "game.z64").string());
        if (browser.directory() != downloads || !browser.error().empty()) return 16;
    }
    fs::permissions(restricted, fs::perms::owner_all, ec);
    if (ec) return 10;
    if (denied) {
        browser.goTo(root);
        browser.goTo(folder);
        if (browser.directory() != folder || !browser.error().empty()) return 12;
    }
#endif

    fs::remove_all(root, ec);
    if (ec) return 7;
    std::puts("ROM browser navigation and filtering: ok");
    return 0;
}
