// rom_validate.h — portable ROM validation for the app shell's ROM picker.
//
// DKR adaptation of mgb64's app-shell validator. mgb64 inspected only the N64
// header's title/country because GoldenEye ships one supported cart; DKR ships
// FIVE released revisions and this port accepts exactly two of them, so a
// title-substring check would cheerfully hand the loader a ROM whose asset LUT
// lives at a different offset (see platform/rom_id.c for the four measured
// SIGSEGVs that motivated the real gate).
//
// This module does NOT re-implement validation. It reads the complete image and
// delegates size, byte-order, revision, full-image SHA-256, and asset-layout
// checks to platform/rom_validation.c — the same authoritative contract used by
// the engine at boot. The launcher performs the complete 12 MB read on a
// cancellable worker so a slow removable/network drive cannot stall ImGui, and
// a header-only false positive can never become "Ready".
#ifndef MDKR64_ROM_VALIDATE_H
#define MDKR64_ROM_VALIDATE_H

#include "rom_validation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  valid;             // 1 only when this build is validated for the ROM
    char byte_order[4];     // "z64" | "v64" | "n64" | "???"
    char region[4];         // "US" | "EU" | "JP" | "??" (header country code)
    char title[24];         // internal ROM image name
    char revision[64];      // "US 1.1 (NTSC-U, Rev 1)" — "" when no row matched
    char build[16];         // decomp build tag, e.g. "us.v80"; "" when unmatched
    unsigned crc1, crc2;    // header CRC pair (the strongest revision signal)
    int  matched_by_crc;    // 1 when the CRC pair (not the weaker keys) matched
    unsigned size_bytes;
    DkrRomValidationCode validation_code;
    int  integrity_verified;
    int  cancelled;
    char sha256[MDKR_SHA256_HEX_SIZE];
    char message[512];      // human-readable, actionable validation status
} RomInfo;

// Validate the file at `path`. Never modifies global state; safe on any thread.
RomInfo mdkr_validate_rom(const char *path);

/* Chunked variant for the native launcher worker. `progress` returns nonzero
 * to continue or zero to cancel; it is called at byte boundaries and never
 * from the UI thread unless the caller invokes this function there. */
typedef int (*MdkrRomValidationProgress)(unsigned completed_bytes,
                                         unsigned total_bytes,
                                         void *context);
RomInfo mdkr_validate_rom_progress(const char *path,
                                   MdkrRomValidationProgress progress,
                                   void *context);

// "US 1.1 (NTSC-U, Rev 1) (us.v80) and European 1.1 (PAL, Rev 1) (pal.v80)" —
// the supported set, for the picker's empty/failed state.
const char *mdkr_supported_rom_list(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ROM_VALIDATE_H
