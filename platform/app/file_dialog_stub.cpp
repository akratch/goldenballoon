// file_dialog_stub.cpp — platforms with no permission-free native panel.
//
// Compiled on Linux and anything else. There IS no honest zero-dependency
// native open-panel here: GTK/Qt/portal each mean a new hard build dependency
// (the xdg-desktop-portal route needs libdbus at configure time), and this
// launcher otherwise needs nothing but SDL2. Rather than add that weight or
// ship a native-panel button that fails at runtime, isAvailable() returns false.
// Linux ROM and ZIP-pack selection use a separate in-window browser.
//
// Acquisition on these platforms is drag-and-drop onto the window plus the
// manual path field, both of which are permission-free and always present.
// Documented in docs/APP_SHELL.md.
#include "file_dialog.h"

namespace filedialog {

bool isAvailable() { return false; }

bool saveGameProgress(std::string &out, const std::string &suggestedName) {
    (void)out; (void)suggestedName; return false;
}

bool openRom(std::string &out) {
    (void)out;   // never written: callers must gate on isAvailable()
    return false;
}

bool openContentPack(std::string &out) {
    (void)out;   // never written: callers must gate on isAvailable()
    return false;
}

bool openCharacterSource(std::string &out) {
    (void)out;
    return false;
}

bool openCharacterLicense(std::string &out) {
    (void)out;
    return false;
}

bool openPortraitImage(std::string &out) {
    (void)out;
    return false;
}

bool openCharacterDraftBundle(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterConvertedGlb(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterCapture(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterReport(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterDeviceProfile(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterPackage(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterDraftBundle(std::string &out) {
    (void)out;
    return false;
}

bool saveCharacterDiagnostic(std::string &out) {
    (void)out;
    return false;
}

bool revealInFileManager(const std::string &path) {
    (void)path;
    return false;
}

}  // namespace filedialog
