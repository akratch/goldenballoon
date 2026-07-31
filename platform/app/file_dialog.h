// file_dialog.h — a user-driven native "open file" dialog, and nothing else.
//
// WHY THIS EXISTS
//
// The launcher used to find ROMs by scanning $HOME/Downloads, Documents and
// Desktop. On macOS, opendir() on any of those trips TCC and raises a system
// permission prompt ("mdkr64 would like to access files in your Documents
// folder") before the user has asked for anything. For an unsigned fan-made
// emulator that is exactly the wrong first impression, and the scan was also
// picking the wrong ROM when several were present. Automatic discovery is gone
// (see docs/APP_SHELL.md); acquisition is manual only.
//
// A file the user explicitly chooses in a native panel is TCC-EXEMPT: the
// panel runs (on macOS) outside the app's sandbox and hands back a path the
// user personally selected, so no prompt appears no matter where the file
// lives. That is the whole point of routing through the OS panel instead of
// enumerating directories ourselves.
//
// PLATFORM SUPPORT
//   macOS    NSOpenPanel                (file_dialog_mac.mm)
//   Windows  GetOpenFileNameW/comdlg32  (file_dialog_win.cpp)
//   other    unavailable                (file_dialog_stub.cpp)
//
// On a platform without an implementation, isAvailable() returns false and the
// UI simply does not draw the button — drag-and-drop and the manual path field
// are the documented paths there. Nothing is silently non-functional.
#ifndef MDKR64_FILE_DIALOG_H
#define MDKR64_FILE_DIALOG_H

#include <string>

namespace filedialog {

// True when this build has a native panel to show. When false, callers must not
// offer a "Browse" affordance at all.
bool isAvailable();

// Show a modal native open-panel filtered to N64 ROM images. Returns true and
// writes the chosen absolute path to `out` when the user picks a file; false
// when they cancel (or no implementation exists). `out` is untouched on false.
//
// Blocks until dismissed. The caller must not be mid-ImGui-frame in a way that
// depends on wall-clock continuity — the launcher calls this between widgets,
// which is fine because ImGui state is retained across the modal.
bool openRom(std::string &out);

}  // namespace filedialog

#endif  // MDKR64_FILE_DIALOG_H
