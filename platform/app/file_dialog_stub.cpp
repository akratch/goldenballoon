// file_dialog_stub.cpp — platforms with no permission-free native panel.
//
// Compiled on Linux and anything else. There IS no honest zero-dependency
// native open-panel here: GTK/Qt/portal each mean a new hard build dependency
// (the xdg-desktop-portal route needs libdbus at configure time), and this
// launcher otherwise needs nothing but SDL2. Rather than add that weight or
// ship a button that fails at runtime, isAvailable() returns false and the ROM
// panel does not draw a Browse button at all.
//
// Acquisition on these platforms is drag-and-drop onto the window plus the
// manual path field, both of which are permission-free and always present.
// Documented in docs/APP_SHELL.md.
#include "file_dialog.h"

namespace filedialog {

bool isAvailable() { return false; }

bool openRom(std::string &out) {
    (void)out;   // never written: callers must gate on isAvailable()
    return false;
}

}  // namespace filedialog
