// rom_validate.h — portable ROM validation for the app shell's ROM picker.
//
// DKR adaptation of mgb64's app-shell validator. mgb64 inspected only the N64
// header's title/country because GoldenEye ships one supported cart; DKR ships
// FIVE released revisions and this port accepts exactly two of them, so a
// title-substring check would cheerfully hand the loader a ROM whose asset LUT
// lives at a different offset (see platform/rom_id.c for the four measured
// SIGSEGVs that motivated the real gate).
//
// So this module does NOT re-implement validation. It reads the 64-byte header,
// normalizes byte order, and delegates to the SAME dkr_rom_identify() the CLI
// (platform/rom_io.c) and the browser shell mirror. One revision table, one
// verdict, three front-ends. Reading 64 bytes keeps the picker responsive: no
// 12 MB read to tell the user their cart is the Japanese one.
#ifndef MDKR64_ROM_VALIDATE_H
#define MDKR64_ROM_VALIDATE_H

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
    char message[320];      // human-readable status (dkr_rom_describe)
} RomInfo;

// Validate the file at `path`. Never modifies global state; safe on any thread.
RomInfo mdkr_validate_rom(const char *path);

// "US 1.1 (NTSC-U, Rev 1) (us.v80) and European 1.1 (PAL, Rev 1) (pal.v80)" —
// the supported set, for the picker's empty/failed state.
const char *mdkr_supported_rom_list(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ROM_VALIDATE_H
