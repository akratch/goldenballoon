#ifndef MDKR_FILE_BROWSER_UI_H
#define MDKR_FILE_BROWSER_UI_H

#include "rom_browser.h"

// Explicitly opened only; construction does not enumerate the filesystem.
class FileBrowserUi {
public:
    void open(const std::string &remembered, BrowserFileKind kind);
    // Call in a stable ImGui ID scope. Returns a selection, never installs it.
    bool draw(const char *title, std::string &selection);
private:
    RomBrowser browser_;
    bool open_ = false;
    bool focusFirst_ = false;
};
#endif
