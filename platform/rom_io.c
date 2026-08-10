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
#include "fs_utf8.h"
#include "rom_id.h"
#include "rom_validation.h"
#include "address_domains.h"

uint8_t *g_romData = NULL;
uint32_t g_romSize = 0;
static int s_sourceTvType = 1;
static int s_sourceIsEuropean = 0;

int platform_source_tv_type(void) {
    return s_sourceTvType;
}

int platform_source_field_hz(void) {
    return s_sourceTvType == 0 ? 50 : 60;
}

int platform_source_is_european(void) {
    return s_sourceIsEuropean;
}

int platformInitRom(const char *path) {
    s_sourceTvType = 1;
    s_sourceIsEuropean = 0;
    FILE *f = mdkr_fopen_utf8(path, "rb");
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
                path, size, (int)DKR_ROM_SIZE_BYTES);
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

    /* --- 2-5. Authoritative image validation. -------------------------------
     * This same pure validator runs in the launcher before a candidate is
     * persisted. It runs again here because the selected file can be replaced
     * on disk between the user's validation and Play. */
    {
        DkrRomValidation validation;
        DkrRomValidationOptions options = dkr_rom_validation_options_from_env();
        DkrRomId *id;
        char msg[1024]; /* matches DkrRomValidation.message; see rom_validation.h */
        /* MDKR_ROM_ANY_REVISION=1 downgrades the refusal to a warning. This is an
         * INVESTIGATION hook, not a compatibility switch: it is how the
         * per-revision failure taxonomy in docs/ROM_REVISIONS.md was measured and
         * how it can be re-measured. What follows past this point on a non-us.v80
         * ROM is undefined — the asset lookup table is read from the wrong offset
         * (see segment_consts.c). Do not suggest it to a user as a way to play
         * another region. */
        if (!dkr_rom_validate_image(g_romData, g_romSize, path, &options,
                                    &validation)) {
            fprintf(stderr, "[ROM] %s\n", validation.message);
            free(g_romData); g_romData = NULL; g_romSize = 0;
            return -1;
        }
        id = &validation.id;
        dkr_rom_describe(id, path, msg, sizeof(msg));
        if (strcmp(validation.byteOrder, "z64") != 0) {
            printf("[ROM] %s is a .%s image - converted to big-endian .z64 order on load.\n",
                   path, validation.byteOrder);
        }
        if (id->verdict != DKR_ROM_SUPPORTED) {
            fprintf(stderr, "[ROM] %s\n", msg);
            fprintf(stderr, "[ROM] MDKR_ROM_ANY_REVISION=1 - loading it anyway. This revision is "
                            "UNVALIDATED; see docs/ROM_REVISIONS.md.\n");
        } else if (!id->matchedByCrc) {
            fprintf(stderr, "[ROM] WARNING: %s\n", msg);
        } else {
            /* Print the SAME sentence dkr_rom_describe() produces on every path,
             * accept included: tests/check_rom_revision.py compares it against the
             * browser mirror's character for character, and an accept path with its
             * own bespoke wording would be the one case that drifts unnoticed. */
            printf("[ROM] %s\n", msg);
        }
        if (options.allowModified &&
            dkr_rom_reference_sha256(id->decompBuild) != NULL &&
            strcmp(validation.sha256,
                   dkr_rom_reference_sha256(id->decompBuild)) != 0) {
            fprintf(stderr,
                    "[ROM] WARNING: modified %s image accepted because "
                    "MDKR_ROM_ALLOW_MODIFIED=1 (SHA-256 %s).\n",
                    id->decompBuild, validation.sha256);
        } else if (dkr_rom_reference_sha256(id->decompBuild) != NULL) {
            printf("[ROM] SHA-256 %s (verified)\n", validation.sha256);
        }

        /* Point the asset loader at THIS revision's lookup table. These are
         * linker-generated on the N64, so every revision keeps them somewhere
         * else, and reading them from the wrong offset is what used to SIGSEGV
         * four of the five carts before the first frame. Defaults in
         * asset_loading.c are us.v80's; this is the only writer. */
        if (id->assetLutStart != 0u) {
            extern MdkrRomOffset gDkrAssetsLutStart, gDkrAssetsLutEnd;
            extern MdkrRomOffset gDkrRomEnd;
            gDkrAssetsLutStart = id->assetLutStart;
            gDkrAssetsLutEnd = id->assetLutEnd;
            gDkrRomEnd = id->romEnd;
            printf("[ROM] asset LUT 0x%06X..0x%06X, ROM end 0x%06X (%s)\n",
                   id->assetLutStart, id->assetLutEnd, id->romEnd, id->decompBuild);
        } else {
            fprintf(stderr, "[ROM] Invalid revision bounds for %s.\n",
                    id->decompBuild != NULL ? id->decompBuild : "unknown revision");
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
            (id->decompBuild != NULL &&
             strncmp(id->decompBuild, "pal.", 4) == 0) ||
                    id->gameCode[3] == 'P'
                ? 0
                : 1;
        /* The launcher/engine validation above has accepted this exact known
         * revision (including its full-image validation policy).  Language
         * choice must follow that identity, rather than generic PAL timing or
         * a country byte which a modified image can carry independently. */
        s_sourceIsEuropean = id->verdict == DKR_ROM_SUPPORTED &&
                             id->decompBuild != NULL &&
                             strcmp(id->decompBuild, "pal.v80") == 0;
        printf("[ROM] source video: %s (%d Hz fields)\n",
               s_sourceTvType == 0 ? "PAL" : "NTSC",
               platform_source_field_hz());
        printf("[ROM] CRC1 %08X CRC2 %08X\n", id->crc1, id->crc2);
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
    s_sourceIsEuropean = 0;
}
