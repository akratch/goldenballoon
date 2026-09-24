// file_dialog.h — user-driven native file selection and destination panels.
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
// isAvailable() describes native panels only. Linux ROM/ZIP intake has a
// separate explicit in-window browser; other callers must retain their
// typed-path/drop fallback rather than infer native-panel support from it.
#ifndef MDKR64_FILE_DIALOG_H
#define MDKR64_FILE_DIALOG_H

#include <string>

namespace filedialog {

// True when this build has a native panel to show. False does not forbid a
// caller with its own in-window picker from offering that separate capability.
bool isAvailable();

// Show a modal native open-panel filtered to N64 ROM images. Returns true and
// writes the chosen absolute path to `out` when the user picks a file; false
// when they cancel (or no implementation exists). `out` is untouched on false.
//
// Blocks until dismissed. The caller must not be mid-ImGui-frame in a way that
// depends on wall-clock continuity — the launcher calls this between widgets,
// which is fine because ImGui state is retained across the modal.
bool openRom(std::string &out);

// Choose a content pack to install: a `.zip`, or a folder holding a pack.ini.
// The reader treats both identically -- a zipped pack goes through the same
// path validation as an unzipped one -- so the installer copies whichever the
// player picked and never unpacks anything itself.
bool openContentPack(std::string &out);

// Choose a reviewable package, canonical data-only adapter result,
// runtime/conversion input, or common DCC source.
// The launcher inspects the suffix and never installs bytes merely because the
// user selected them. DAE/ZIP require an explicit converted destination;
// FBX/OBJ/glTF/USD/native projects receive guidance and are never executed.
// A .mdkrsource file is reviewed and revalidated before exclusive extraction;
// it never grants authority to execute the adapter that produced it.
bool openCharacterSource(std::string &out);

// Choose the exact license/notice text embedded beside a raw GLB. Extension is
// intentionally unrestricted: conventional files are often named LICENSE or
// COPYING without a suffix.
bool openCharacterLicense(std::string &out);

// Choose a still PNG for custom-character portrait authoring. Validation and
// resampling happen in the Workshop; the dialog grants no import authority.
bool openPortraitImage(std::string &out);

// Choose a bounded named-draft transfer. This is editor state only; the
// Workshop authenticates it and requires the exact installed source revision
// before offering an additive import.
bool openCharacterDraftBundle(std::string &out);

// Choose a destination for a self-contained GLB derived from a DAE/ZIP source.
// Conversion uses exclusive create, so a returned existing path still cannot
// authorize replacement.
bool saveCharacterConvertedGlb(std::string &out);

// Choose a destination for a new exact-renderer PNG. The capture writer still
// opens it exclusively, so selecting an existing file never grants overwrite
// authority; the Workshop asks for another name instead.
bool saveCharacterCapture(std::string &out);

// Choose a destination for a self-contained visual qualification report.
// Like captures, reports are exclusive-create: choosing an existing file does
// not authorize the Workshop to replace it.
bool saveCharacterReport(std::string &out);

// Choose a destination for a privacy-bounded real-device qualification JSON.
// The report writer still uses exclusive creation, and the Workshop separately
// requires acknowledgement before publishing adapter/driver and device IDs.
bool saveCharacterDeviceProfile(std::string &out);

// Choose a destination for an exact or compiler-enriched portable package.
// Export remains exclusive-create; the dialog does not imply redistribution
// rights and callers must collect that confirmation separately.
bool saveCharacterPackage(std::string &out);

// Choose a destination for a privacy-normalized named-draft bundle. The writer
// remains exclusive-create and never treats the panel as overwrite authority.
bool saveCharacterDraftBundle(std::string &out);

// Choose a destination for bounded failed-import metadata (never model bytes).
bool saveCharacterDiagnostic(std::string &out);

// Ask the operating system's file manager to reveal and select an existing
// file. This is deliberately separate from opening the file: package/source
// bytes are never executed or handed to an associated application.
bool revealInFileManager(const std::string &path);

}  // namespace filedialog

#endif  // MDKR64_FILE_DIALOG_H
