#ifndef MDKR_FILE_BROWSER_UI_H
#define MDKR_FILE_BROWSER_UI_H

#include "rom_browser.h"

// Only the versioned SDL gamepad smoke may select the in-window picker on
// platforms whose ordinary file selection uses a native dialog.
const char *FileBrowserUi_smokePath(bool pack);

// Explicitly opened only; construction does not enumerate the filesystem.
class FileBrowserUi {
public:
    void open(const std::string &remembered, BrowserFileKind kind);
    // Call in a stable ImGui ID scope. Returns a selection, never installs it.
    bool draw(const char *title, std::string &selection);
    // Read-only observations of rendered controls for SDL input journeys.
    bool visible() const { return open_ && visible_; }
    bool focusedPath(const char *path, bool directory) const {
        return visible() && path && focusedDirectory_ == directory && focusedPath_ == path;
    }
    bool atDirectory(const char *path) const {
        return visible() && path && browser_.directory() == std::filesystem::u8path(path);
    }
    bool rootFocused() const { return visible() && rootFocused_; }
    bool upFocused() const { return visible() && upFocused_; }
    bool scrolled() const { return visible() && scrolled_; }
private:
    RomBrowser browser_;
    bool open_ = false;
    bool focusFirst_ = false;
    bool visible_ = false;
    bool focusedDirectory_ = false;
    bool rootFocused_ = false;
    bool upFocused_ = false;
    bool scrolled_ = false;
    std::string focusedPath_;
};
#endif
