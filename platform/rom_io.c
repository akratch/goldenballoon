/**
 * rom_io.c — load baserom.us.v80.z64 into memory and serve DMA from it.
 *
 * DKR accesses cart data via osPiStartDma; on native every DMA becomes a memcpy
 * out of this in-memory image (see stubs_dkr.c osPiStartDma). The ROM bytes are
 * kept as raw big-endian .z64 bytes — per-asset byteswap is a separate later
 * workstream and is NOT done here.
 *
 * The load gate runs in this order, and the order matters:
 *
 *   1. SIZE      — 12 MB exactly (order-independent, so it can come first and
 *                  gives the clearest message for "wrong game / truncated").
 *   2. BYTE ORDER — a .v64/.n64 image is converted IN PLACE to .z64 order here,
 *                  before anything reads a single field. Accepting a byteswapped
 *                  image without converting it would be worse than rejecting it:
 *                  the engine would read every asset through a transposition and
 *                  produce silent garbage. Byte-order parity is locked by
 *                  tests/check_rom_revision.py.
 *   3. REVISION  — which DKR is this? See rom_id.c. Size + magic + "the title
 *                  contains 'diddy'" is true of ALL FIVE DKR revisions, so the
 *                  old gate let a European or Japanese cart straight through into
 *                  a boot that read its asset table from the wrong ROM offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_os.h"
#include "rom_id.h"
#include "address_domains.h"
#include "sha256.h"

uint8_t *g_romData = NULL;
uint32_t g_romSize = 0;
static int s_sourceTvType = 1;

/* DKR (US v1.1 / v80) is a 12 MB cartridge, like every DKR region. */
#define DKR_ROM_SIZE_BYTES 0x00C00000

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

int platform_source_tv_type(void) {
    return s_sourceTvType;
}

int platform_source_field_hz(void) {
    return s_sourceTvType == 0 ? 50 : 60;
}

static const char *reference_sha256(const char *build) {
    if (build != NULL && strcmp(build, "us.v80") == 0) {
        return "7de1a8fb2a9558cfc3d9ad4497df698c1e89cf7095ac1531557df2af40ba8bcf";
    }
    if (build != NULL && strcmp(build, "pal.v80") == 0) {
        return "584d59412b3a8c675f5569516a0406128028929e31544490a4dbc3ab16a038b9";
    }
    return NULL;
}

static int validate_asset_lut(const DkrRomId *id) {
    uint32_t count;
    uint32_t previous;
    uint32_t relativeLimit;
    uint32_t i;
    uint64_t tableBytes;

    if (id->assetLutStart >= id->assetLutEnd || id->assetLutEnd > g_romSize) {
        return -1;
    }
    count = read_be32(g_romData + id->assetLutStart);
    tableBytes = ((uint64_t)count + 2u) * sizeof(uint32_t);
    if (count == 0 || tableBytes > (uint64_t)id->assetLutEnd - id->assetLutStart) {
        return -1;
    }
    relativeLimit = g_romSize - id->assetLutEnd;
    previous = 0;
    for (i = 0; i <= count; i++) {
        uint32_t offset = read_be32(g_romData + id->assetLutStart + (i + 1u) * 4u);
        if (offset < previous || offset > relativeLimit) {
            return -1;
        }
        previous = offset;
    }
    return 0;
}

int platformInitRom(const char *path) {
    s_sourceTvType = 1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ROM] Failed to open: %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0) {
        fprintf(stderr, "[ROM] Could not size: %s\n", path);
        fclose(f);
        return -1;
    }
    /* --- 1. Size (byte-order independent, so it can be checked first). ------ */
    if (size != DKR_ROM_SIZE_BYTES) {
        fprintf(stderr,
                "[ROM] %s is %ld bytes; a Diddy Kong Racing ROM must be exactly "
                "%d bytes (12 MB). Wrong game, headered dump, or truncated file.\n",
                path, size, DKR_ROM_SIZE_BYTES);
        fclose(f);
        return -1;
    }
    g_romData = (uint8_t *)malloc((size_t)size);
    if (!g_romData) {
        fprintf(stderr, "[ROM] malloc(%ld) failed\n", size);
        fclose(f);
        return -1;
    }
    size_t rd = fread(g_romData, 1, (size_t)size, f);
    fclose(f);
    if ((long)rd != size) {
        fprintf(stderr, "[ROM] short read: %zu of %ld\n", rd, size);
        free(g_romData);
        g_romData = NULL;
        return -1;
    }
    g_romSize = (uint32_t)size;

    /* --- 2. Byte order: convert .v64/.n64 in place to big-endian .z64. ------ */
    {
        const char *order = dkr_rom_normalize_byte_order(g_romData, g_romSize);
        if (order == NULL) {
            fprintf(stderr,
                    "[ROM] %s is not an N64 ROM - its first four bytes are %02x %02x %02x %02x, "
                    "which is none of .z64 (80 37 12 40), .v64 (37 80 40 12) or .n64 "
                    "(40 12 37 80).\n",
                    path, g_romData[0], g_romData[1], g_romData[2], g_romData[3]);
            free(g_romData); g_romData = NULL; g_romSize = 0;
            return -1;
        }
        if (strcmp(order, "z64") != 0) {
            printf("[ROM] %s is a .%s image - converted to big-endian .z64 order on load.\n",
                   path, order);
        }
    }

    /* --- 3. Revision: WHICH Diddy Kong Racing is this? ---------------------- */
    {
        DkrRomId id;
        char msg[512];
        /* MDKR_ROM_ANY_REVISION=1 downgrades the refusal to a warning. This is an
         * INVESTIGATION hook, not a compatibility switch: it is how the
         * per-revision failure taxonomy in docs/ROM_REVISIONS.md was measured and
         * how it can be re-measured. What follows past this point on a non-us.v80
         * ROM is undefined — the asset lookup table is read from the wrong offset
         * (see segment_consts.c). Do not suggest it to a user as a way to play
         * another region. */
        const char *anyRev = getenv("MDKR_ROM_ANY_REVISION");
        int forceAny = (anyRev != NULL && anyRev[0] == '1');
        const char *allowModifiedValue = getenv("MDKR_ROM_ALLOW_MODIFIED");
        int allowModified = allowModifiedValue != NULL && allowModifiedValue[0] == '1';
        char actualSha256[MDKR_SHA256_HEX_SIZE];
        const char *expectedSha256;

        dkr_rom_identify(g_romData, &id);
        dkr_rom_describe(&id, path, msg, sizeof(msg));
        if (id.verdict != DKR_ROM_SUPPORTED && !forceAny) {
            fprintf(stderr, "[ROM] %s\n", msg);
            free(g_romData); g_romData = NULL; g_romSize = 0;
            return -1;
        }
        if (id.verdict != DKR_ROM_SUPPORTED) {
            fprintf(stderr, "[ROM] %s\n", msg);
            fprintf(stderr, "[ROM] MDKR_ROM_ANY_REVISION=1 - loading it anyway. This revision is "
                            "UNVALIDATED; see docs/ROM_REVISIONS.md.\n");
        } else if (!id.matchedByCrc) {
            fprintf(stderr, "[ROM] WARNING: %s\n", msg);
        } else {
            /* Print the SAME sentence dkr_rom_describe() produces on every path,
             * accept included: tests/check_rom_revision.py compares it against the
             * browser mirror's character for character, and an accept path with its
             * own bespoke wording would be the one case that drifts unnoticed. */
            printf("[ROM] %s\n", msg);
        }

        expectedSha256 = reference_sha256(id.decompBuild);
        mdkr_sha256_hex(g_romData, g_romSize, actualSha256);
        if (id.verdict == DKR_ROM_SUPPORTED && expectedSha256 != NULL &&
            strcmp(actualSha256, expectedSha256) != 0) {
            if (!allowModified) {
                fprintf(stderr,
                        "[ROM] SHA-256 mismatch for %s: got %s, expected %s. "
                        "The image is modified or damaged. Set "
                        "MDKR_ROM_ALLOW_MODIFIED=1 only for explicit mod development.\n",
                        id.decompBuild, actualSha256, expectedSha256);
                free(g_romData); g_romData = NULL; g_romSize = 0;
                return -1;
            }
            fprintf(stderr,
                    "[ROM] WARNING: modified %s image accepted because "
                    "MDKR_ROM_ALLOW_MODIFIED=1 (SHA-256 %s).\n",
                    id.decompBuild, actualSha256);
        } else if (expectedSha256 != NULL) {
            printf("[ROM] SHA-256 %s (verified)\n", actualSha256);
        }

        /* Point the asset loader at THIS revision's lookup table. These are
         * linker-generated on the N64, so every revision keeps them somewhere
         * else, and reading them from the wrong offset is what used to SIGSEGV
         * four of the five carts before the first frame. Defaults in
         * asset_loading.c are us.v80's; this is the only writer. */
        if (id.assetLutStart != 0u && id.romEnd > id.assetLutEnd && id.romEnd <= g_romSize) {
            extern MdkrRomOffset gDkrAssetsLutStart, gDkrAssetsLutEnd;
            extern MdkrRomOffset gDkrRomEnd;
            gDkrAssetsLutStart = id.assetLutStart;
            gDkrAssetsLutEnd = id.assetLutEnd;
            gDkrRomEnd = id.romEnd;
            printf("[ROM] asset LUT 0x%06X..0x%06X, ROM end 0x%06X (%s)\n",
                   id.assetLutStart, id.assetLutEnd, id.romEnd, id.decompBuild);
        } else {
            fprintf(stderr, "[ROM] Invalid revision bounds for %s.\n",
                    id.decompBuild != NULL ? id.decompBuild : "unknown revision");
            free(g_romData); g_romData = NULL; g_romSize = 0;
            return -1;
        }
        if (validate_asset_lut(&id) != 0) {
            fprintf(stderr, "[ROM] Invalid asset lookup table for %s.\n",
                    id.decompBuild != NULL ? id.decompBuild : "unknown revision");
            free(g_romData); g_romData = NULL; g_romSize = 0;
            return -1;
        }
        /*
         * The compiled game code supports the byte-identical PAL v80 payload,
         * but its authored timing remains PAL. CRC-selected decompBuild is
         * stronger evidence than a possibly modified header; the country byte
         * remains the fallback for a supported modified build.
         */
        s_sourceTvType =
            (id.decompBuild != NULL &&
             strncmp(id.decompBuild, "pal.", 4) == 0) ||
                    id.gameCode[3] == 'P'
                ? 0
                : 1;
        printf("[ROM] source video: %s (%d Hz fields)\n",
               s_sourceTvType == 0 ? "PAL" : "NTSC",
               platform_source_field_hz());
        printf("[ROM] CRC1 %08X CRC2 %08X\n", id.crc1, id.crc2);
    }

    printf("[ROM] Loaded %u bytes (%.1f MB) from %s\n",
           g_romSize, (float)g_romSize / (1024.0f * 1024.0f), path);
    return 0;
}

int platform_rom_read(uint32_t romOffset, void *dst, int32_t len) {
    uint64_t end;

    if (len == 0) {
        return 0;
    }
    if (!g_romData || dst == NULL || len < 0) {
        fprintf(stderr, "[ROM] Invalid DMA request off=0x%x dst=%p len=%d\n",
                romOffset, dst, len);
        return -1;
    }
    end = (uint64_t)romOffset + (uint32_t)len;
    if (romOffset >= g_romSize || end > g_romSize) {
        fprintf(stderr, "[ROM] DMA range 0x%x..0x%llx exceeds ROM size 0x%x\n",
                romOffset, (unsigned long long)end, g_romSize);
        memset(dst, 0, (size_t)len);
        return -1;
    }
    memcpy(dst, g_romData + romOffset, (size_t)len);
    return 0;
}

void platform_rom_shutdown(void) {
    free(g_romData);
    g_romData = NULL;
    g_romSize = 0;
    s_sourceTvType = 1;
}
