/**
 * rom_id.c — WHICH Diddy Kong Racing ROM is this, and can this build run it?
 *
 * ===========================================================================
 *  Why this file exists
 * ===========================================================================
 *
 * The gate used to be "12 MB, N64 magic, and the internal title contains
 * 'diddy'". ALL FIVE released DKR revisions pass all three. So handing this build
 * the European or Japanese cart did not produce an error — it read the master
 * asset lookup table from the wrong ROM offset and carried on. Measured (with the
 * gate forced off, MDKR_ROM_ANY_REVISION=1, and the us.v80 offsets still in use):
 *
 *     us.v77   LUT entry count reads 3,886,268,156 -> derived ASSET_AUDIO_TABLE
 *              size -453,982,075 -> SIGSEGV in sw32_arr (asset_swap_lut)
 *     pal.v77  entry count 830,361,298 -> size -1,327,275,831 -> same SIGSEGV
 *     pal.v80  entry count 0, so asset_table_load()'s `LUT[0] < assetIndex` guard
 *              returns NULL -> SIGSEGV in audio_init on addrPtr[ASSET_AUDIO_2]
 *     jpn.v79  entry count 0 -> the same NULL path
 *
 * Four SIGSEGVs before the first rendered frame, from a gate that reported the ROM
 * as fine. That is HANDOFF.md §3's dominant shape. Full taxonomy, with the
 * evidence for every number here: docs/ROM_REVISIONS.md.
 *
 * The offsets are per-revision because on the N64 they are LINKER-generated, and
 * there is no linker here. From the decompilation's own per-version splat configs
 * (ver/splat/dkr.*.yaml):
 *
 *     build     assets.lut   assets base   ROM end
 *     us.v77     0x0ECB60     0x0ECC30     0xAC9630
 *     pal.v77    0x0ECBF0     0x0ECCC0     0xAC96C0
 *     jpn.v79    0x0EE5D0     0x0EE6A0     0xB8E4E0
 *     us.v80     0x0ED0E0     0x0ED1B0     0xB8CFD0
 *     pal.v80    0x0ED170     0x0ED240     0xB8D060
 *
 * ===========================================================================
 *  What is actually supported, and why it is two revisions and not five
 * ===========================================================================
 *
 * An earlier draft of this file asserted that correcting the offsets "would NOT
 * make another revision work". That was a prediction, and measurement contradicts
 * it — it is retracted. With each revision pointed at its own LUT offsets, ALL
 * FOUR non-target revisions boot, navigate the menus, and finish a 3-lap Time
 * Trial with the same course time (4713) and a correct EEPROM round-trip.
 *
 * The reason is that this port does not execute ROM code at all. It runs its own
 * compiled C (VERSION_us_v80 / REGION_NA) and uses the ROM purely as a data
 * source, reached through the LUT. The asset payload is STRUCTURALLY identical
 * across revisions — same 50 sections, same asset types, same record layouts —
 * so the loader does not care that the content differs.
 *
 *   pal.v80 -> SUPPORTED. Its asset payload is BYTE-IDENTICAL to us.v80's: all
 *              50 LUT-delimited sections hash the same, and the LUT itself is
 *              identical entry for entry. The cart is us.v80's data relocated by
 *              0x90. Running it is therefore indistinguishable from running
 *              us.v80, which is why it is accepted rather than merely tolerated.
 *
 *   us.v77, pal.v77, jpn.v79 -> STILL REFUSED, despite passing every check run
 *              against them. Their payloads genuinely differ (us.v77 and pal.v77
 *              share a payload that matches us.v80 in only 26 of 50 sections;
 *              jpn.v79 in 46 of 50), and this build is compiled for v80 DATA:
 *
 *                * SUB-ENTRY INDICES. us.v80's GAME_TEXT has 343 entries, v77's
 *                  259. asset_table_load()'s bounds check covers SECTIONS, not
 *                  entries within one, so a v80-only string index on a v77 ROM is
 *                  an out-of-bounds read with no guard — this codebase's silent
 *                  failure class exactly. Auditing that is the bounded piece of
 *                  work that stands between "passes our fixtures" and "supported".
 *                * jpn.v79 needs REGION_JP: font.c, game_text.c and save_data.c
 *                  all change behaviour for it (glyph handling, EEPROM layout),
 *                  and we compile REGION_NA.
 *                * No oracle pixel-parity run exists for any of them, and audio
 *                  is unverified (us.v77's AUDIO section differs and every check
 *                  runs muted by CONTRIBUTING.md's hard rule).
 *
 * So: "cannot run" is wrong and "supported" is unearned. They are refused because
 * they are unvalidated, and the refusal message says so.
 *
 * ===========================================================================
 *  How a revision is identified
 * ===========================================================================
 *
 * The expected values for all five revisions come from the decompilation's own
 * `src/hasm/header.s`, which emits them per `VERSION_*`. Verified against all five
 * real ROMs; each one's sha1 also matches the decomp's
 * ver/verification/dkr.<build>.sha1, so these are the reference dumps.
 *
 *     0x10  CRC1                }  the strongest signal: covers the ROM body,
 *     0x14  CRC2                }  so it survives a renamed or re-headered file
 *     0x20  internal title (20 bytes, "Diddy Kong Racing   " on every revision)
 *     0x3B  media format ('N' = cartridge)
 *     0x3C  cartridge id ("DY" for DKR)
 *     0x3E  country code ('E' = NTSC-U, 'P' = PAL, 'J' = NTSC-J)
 *     0x3F  version = revision | (savetype << 4)
 *
 * Order of preference: CRC pair first, header fields second. A matching CRC pair
 * is decisive — the ROM BODY is that revision whatever the header claims. Only
 * when no CRC pair matches do the header fields decide, and then the caller is
 * told the CRC did not match (a modified dump or a ROM hack).
 *
 * One deliberate identification leniency:
 *
 *   - Only the LOW NIBBLE of 0x3F is compared. header.s writes
 *     `revision | (savetype << 4)` and its NON_MATCHING build sets savetype 1, so
 *     a ROM built from the decomp reads 0x11 rather than 0x01. Retail carts have
 *     savetype 0 (confirmed on all five dumps).
 * A supported-looking header with a CRC mismatch is still IDENTIFIED so the
 * error can name the likely revision. Acceptance is a separate decision in
 * rom_validation.c: its complete-image SHA-256 must match unless the explicit
 * MDKR_ROM_ALLOW_MODIFIED=1 developer override is present.
 *
 * ===========================================================================
 *  Note for anyone editing the constants
 * ===========================================================================
 * Do not write the N64 header magic as a 32-bit constant. tools/check_no_rom.sh
 * greps shipped artifacts for the byte sequence 80 37 12 40 anywhere in the file;
 * a `u32` holding 0x40123780 lands in the data section as exactly those four bytes
 * on a little-endian host and would trip the release gate. Compare the magic byte
 * by byte, as below.
 */
#include <stdio.h>
#include <string.h>

#include "rom_id.h"

/* Header field offsets (N64 ROM header, big-endian). */
#define HDR_CRC1        0x10
#define HDR_CRC2        0x14
#define HDR_TITLE       0x20
#define HDR_TITLE_LEN   20
#define HDR_MEDIA       0x3B
#define HDR_CART_ID     0x3C
#define HDR_COUNTRY     0x3E
#define HDR_VERSION     0x3F

typedef struct {
    const char *name;    /* human-facing revision name                    */
    const char *build;   /* decomp build id, matches ver/splat/dkr.*.yaml */
    char country;        /* header 0x3E                                   */
    uint8_t revision;    /* header 0x3F low nibble                        */
    uint32_t crc1;       /* header 0x10                                   */
    uint32_t crc2;       /* header 0x14                                   */
    uint32_t lutStart;   /* ver/splat: `assets.lut` segment start         */
    uint32_t lutEnd;     /* ver/splat: `assets` segment start             */
    uint32_t romEnd;     /* ver/splat: linker-generated assets_ROM_END    */
    int supported;       /* 1 = validated and accepted by this build      */
} DkrRevision;

/*
 * The five released revisions.
 *
 *   name/country/revision/crc  <- the decomp's own src/hasm/header.s.
 *   lutStart/lutEnd            <- the decomp's ver/splat/dkr.<build>.yaml
 *                                 `assets.lut` and `assets` segment starts.
 *
 * Decompilation METADATA (published offsets and header constants), not ROM
 * content. Nothing here is extracted from a ROM body.
 *
 * MIRRORED IN dist/web/rom-id.js — tests/check_rom_revision.py parses both tables
 * and fails if they differ, so edit both or neither.
 */
static const DkrRevision kDkrRevisions[] = {
    { "US 1.0 (NTSC-U, Rev 0)",     "us.v77",  'E', 0x0, 0x53D440E7, 0x7519B011, 0x0ECB60, 0x0ECC30, 0xAC9630, 0 },
    { "European (PAL, Rev 0)",      "pal.v77", 'P', 0x0, 0xFD73F775, 0x9724755A, 0x0ECBF0, 0x0ECCC0, 0xAC96C0, 0 },
    { "Japanese (NTSC-J)",          "jpn.v79", 'J', 0x0, 0x7435C9BB, 0x39763CF4, 0x0EE5D0, 0x0EE6A0, 0xB8E4E0, 0 },
    { "US 1.1 (NTSC-U, Rev 1)",     "us.v80",  'E', 0x1, 0xE402430D, 0xD2FCFC9D, 0x0ED0E0, 0x0ED1B0, 0xB8CFD0, 1 },
    { "European 1.1 (PAL, Rev 1)",  "pal.v80", 'P', 0x1, 0x596E145B, 0xF7D9879F, 0x0ED170, 0x0ED240, 0xB8D060, 1 },
};
#define DKR_REVISION_COUNT ((int) (sizeof(kDkrRevisions) / sizeof(kDkrRevisions[0])))

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

void dkr_rom_supported_list(char *buf, size_t bufLen) {
    int i, first = 1;
    size_t used = 0;

    if (bufLen == 0) {
        return;
    }
    buf[0] = '\0';
    for (i = 0; i < DKR_REVISION_COUNT; i++) {
        int n;
        if (!kDkrRevisions[i].supported) {
            continue;
        }
        n = snprintf(buf + used, bufLen - used, "%s%s (%s)", first ? "" : " and ",
                     kDkrRevisions[i].name, kDkrRevisions[i].build);
        if (n < 0 || (size_t) n >= bufLen - used) {
            break;
        }
        used += (size_t) n;
        first = 0;
    }
    buf[bufLen - 1] = '\0';
}

const char *dkr_rom_normalize_byte_order(uint8_t *data, uint32_t size) {
    const char *order;
    uint32_t i;

    if (data == NULL || size < 0x40u) {
        return NULL;
    }
    /* .v64 — every 16-bit halfword byteswapped: 37 80 40 12 */
    if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
        for (i = 0; i + 1 < size; i += 2) {
            uint8_t t = data[i];
            data[i] = data[i + 1];
            data[i + 1] = t;
        }
        order = "v64";
    /* .n64 — every 32-bit word reversed: 40 12 37 80 */
    } else if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
        for (i = 0; i + 3 < size; i += 4) {
            uint8_t a = data[i], b = data[i + 1], c = data[i + 2], d = data[i + 3];
            data[i] = d;
            data[i + 1] = c;
            data[i + 2] = b;
            data[i + 3] = a;
        }
        order = "n64";
    } else {
        order = "z64";
    }
    /* Whatever happened above, the result must now be big-endian. */
    if (data[0] != 0x80 || data[1] != 0x37 || data[2] != 0x12 || data[3] != 0x40) {
        return NULL;
    }
    return order;
}

void dkr_rom_identify(const uint8_t *hdr, DkrRomId *out) {
    int i, row = -1;
    char country;
    uint8_t revision;

    memset(out, 0, sizeof(*out));
    /* DKR_ROM_SUPPORTED is the zero value, so a cleared struct claims the
     * strongest verdict. Refusal is the only safe default: every path below
     * either matches a revision row or leaves this standing. */
    out->verdict = DKR_ROM_NOT_DKR;
    if (hdr == NULL) {
        return;
    }

    out->crc1 = rd_be32(hdr + HDR_CRC1);
    out->crc2 = rd_be32(hdr + HDR_CRC2);
    out->version = hdr[HDR_VERSION];
    out->gameCode[0] = (char) hdr[HDR_MEDIA];
    out->gameCode[1] = (char) hdr[HDR_CART_ID];
    out->gameCode[2] = (char) hdr[HDR_CART_ID + 1];
    out->gameCode[3] = (char) hdr[HDR_COUNTRY];
    out->gameCode[4] = '\0';
    for (i = 0; i < HDR_TITLE_LEN; i++) {
        unsigned char c = hdr[HDR_TITLE + i];
        /* Keep it printable: the title is space-padded ASCII on every DKR
         * revision, but a non-DKR ROM's is arbitrary bytes and this string is
         * printed straight back at the user. */
        out->title[i] = (c >= 0x20 && c < 0x7F) ? (char) c : '?';
    }
    out->title[HDR_TITLE_LEN] = '\0';

    /* 1. CRC pair — decisive when it matches, because it covers the ROM body. */
    for (i = 0; i < DKR_REVISION_COUNT; i++) {
        if (kDkrRevisions[i].crc1 == out->crc1 && kDkrRevisions[i].crc2 == out->crc2) {
            row = i;
            out->matchedByCrc = 1;
            break;
        }
    }

    /* 2. Header fields, for a modified or self-built ROM whose CRCs differ. */
    if (row < 0) {
        /* Must be a DKR cartridge at all: media 'N' + cart id "DY". */
        if (hdr[HDR_MEDIA] != 'N' || hdr[HDR_CART_ID] != 'D' || hdr[HDR_CART_ID + 1] != 'Y') {
            out->verdict = DKR_ROM_NOT_DKR;
            return;
        }
        country = (char) hdr[HDR_COUNTRY];
        revision = (uint8_t) (hdr[HDR_VERSION] & 0x0Fu); /* high nibble = savetype */
        for (i = 0; i < DKR_REVISION_COUNT; i++) {
            if (kDkrRevisions[i].country == country && kDkrRevisions[i].revision == revision) {
                row = i;
                break;
            }
        }
        if (row < 0) {
            out->verdict = DKR_ROM_UNKNOWN_REVISION;
            return;
        }
    }

    out->revisionName = kDkrRevisions[row].name;
    out->decompBuild = kDkrRevisions[row].build;
    out->assetLutStart = kDkrRevisions[row].lutStart;
    out->assetLutEnd = kDkrRevisions[row].lutEnd;
    out->romEnd = kDkrRevisions[row].romEnd;
    out->refCrc1 = kDkrRevisions[row].crc1;
    out->refCrc2 = kDkrRevisions[row].crc2;
    out->verdict = kDkrRevisions[row].supported ? DKR_ROM_SUPPORTED : DKR_ROM_OTHER_REVISION;
}

void dkr_rom_describe(const DkrRomId *id, const char *path, char *buf, size_t bufLen) {
    const char *p = (path != NULL) ? path : "that file";
    char supported[192];

    if (bufLen == 0) {
        return;
    }
    dkr_rom_supported_list(supported, sizeof(supported));

    switch (id->verdict) {
    case DKR_ROM_SUPPORTED:
        if (id->matchedByCrc) {
            snprintf(buf, bufLen, "%s is %s (%s) - a revision this build supports.", p,
                     id->revisionName, id->decompBuild);
        } else {
            /* Identification is intentionally richer than acceptance. The full
             * image validator decides whether an explicit developer override is
             * allowed; this sentence must never promise that boot will continue. */
            snprintf(buf, bufLen,
                     "%s has a %s header (%s) but its CRC1/CRC2 (0x%08X / 0x%08X) do not match "
                     "that revision's reference pair (0x%08X / 0x%08X) - a modified or "
                     "imperfect dump. Use a clean reference image, or enable the explicit "
                     "modified-ROM developer override.",
                     p, id->revisionName, id->decompBuild, id->crc1, id->crc2, id->refCrc1,
                     id->refCrc2);
        }
        break;
    case DKR_ROM_OTHER_REVISION:
        /* Deliberately NOT "cannot run it": measurement says it very likely can
         * (docs/ROM_REVISIONS.md). It is refused because it is UNVALIDATED — this
         * build is compiled for v80 data and nothing bounds-checks a v80 string
         * index against a v77 table. */
        snprintf(buf, bufLen,
                 "%s is the %s release of Diddy Kong Racing (decomp build %s), which this build "
                 "does not support: it is compiled for %s. Other revisions are refused rather "
                 "than run because they are unvalidated against this build's asset "
                 "assumptions - see docs/ROM_REVISIONS.md.",
                 p, id->revisionName, id->decompBuild, supported);
        break;
    case DKR_ROM_UNKNOWN_REVISION:
        snprintf(buf, bufLen,
                 "%s is a Diddy Kong Racing cartridge (game code \"%s\", version 0x%02X) but not a "
                 "revision this build recognises; it supports %s.",
                 p, id->gameCode, (unsigned) id->version, supported);
        break;
    case DKR_ROM_NOT_DKR:
    default:
        snprintf(buf, bufLen,
                 "%s is an N64 ROM but not Diddy Kong Racing (game code \"%s\", internal title "
                 "\"%s\"); this build supports %s.",
                 p, id->gameCode, id->title, supported);
        break;
    }
    buf[bufLen - 1] = '\0';
}
