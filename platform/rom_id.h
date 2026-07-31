/**
 * rom_id.h — identify WHICH Diddy Kong Racing ROM the user handed us.
 *
 * See rom_id.c for the revision table, how a revision is identified, and why two
 * of the five released revisions are accepted and three are refused. The measured
 * per-revision behaviour is in docs/ROM_REVISIONS.md.
 *
 * The browser shell mirrors this in dist/web/rom-id.js. The two are held in
 * agreement mechanically by tests/check_rom_revision.py, which parses both tables
 * and runs the same ROMs through both implementations.
 */
#ifndef MDKR_ROM_ID_H
#define MDKR_ROM_ID_H

#include <stddef.h>
#include <stdint.h>

/* The native app shell's ROM picker (platform/app/rom_validate.cpp) is C++ and
 * links against this table, so the declarations need C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DKR_ROM_SUPPORTED = 0,     /* a revision this build is validated for       */
    DKR_ROM_OTHER_REVISION,    /* a real DKR ROM, but not one we support       */
    DKR_ROM_UNKNOWN_REVISION,  /* DKR cart id, but a revision not in the table */
    DKR_ROM_NOT_DKR            /* an N64 ROM, but not Diddy Kong Racing        */
} DkrRomVerdict;

typedef struct {
    DkrRomVerdict verdict;
    const char *revisionName;  /* human name; NULL unless a table row matched  */
    const char *decompBuild;   /* "us.v80" etc.; NULL unless a row matched     */
    uint32_t assetLutStart;    /* ROM offset of the master asset LUT, 0 if n/a */
    uint32_t assetLutEnd;      /* == the asset-data base; 0 if n/a             */
    uint32_t romEnd;           /* linker-generated used-content end for build  */
    uint32_t refCrc1;          /* the matched revision's reference CRC1        */
    uint32_t refCrc2;          /* the matched revision's reference CRC2        */
    char gameCode[5];          /* header 0x3B..0x3E, e.g. "NDYE"               */
    char title[21];            /* header 0x20..0x33, NUL-terminated            */
    uint8_t version;           /* header 0x3F (raw, savetype nibble included)  */
    uint32_t crc1;             /* header 0x10                                  */
    uint32_t crc2;             /* header 0x14                                  */
    int matchedByCrc;          /* 1 = the CRC pair picked the row (strongest)  */
} DkrRomId;

/**
 * Classify a ROM from its header. `hdr` must point at >= 0x40 bytes already in
 * BIG-ENDIAN (.z64) order — call dkr_rom_normalize_byte_order() first.
 */
void dkr_rom_identify(const uint8_t *hdr, DkrRomId *out);

/**
 * Write a one-line human explanation of `id` (the message a user sees) into
 * `buf`. Always NUL-terminates. `path` may be NULL.
 */
void dkr_rom_describe(const DkrRomId *id, const char *path, char *buf, size_t bufLen);

/** "US 1.1 (NTSC-U, Rev 1) (us.v80) and European 1.1 (PAL, Rev 1) (pal.v80)". */
void dkr_rom_supported_list(char *buf, size_t bufLen);

/**
 * Convert a .v64 (16-bit byteswapped) or .n64 (32-bit little-endian) image in
 * place to .z64 (big-endian) order. Returns "z64" / "v64" / "n64" for a
 * recognised order (the image is in .z64 order on return), or NULL when the
 * header magic is not an N64 ROM in any of the three orders.
 */
const char *dkr_rom_normalize_byte_order(uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ROM_ID_H */
