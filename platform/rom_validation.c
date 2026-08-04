/** See rom_validation.h. */
#include "rom_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int validate_asset_lut(const uint8_t *data, uint32_t size,
                              const DkrRomId *id) {
    uint32_t count;
    uint32_t previous;
    uint32_t relativeLimit;
    uint32_t i;
    uint64_t tableBytes;

    if (id->assetLutStart >= id->assetLutEnd || id->assetLutEnd > size ||
        id->romEnd <= id->assetLutEnd || id->romEnd > size) {
        return 0;
    }
    count = read_be32(data + id->assetLutStart);
    tableBytes = ((uint64_t)count + 2u) * sizeof(uint32_t);
    if (count == 0 || tableBytes > (uint64_t)id->assetLutEnd - id->assetLutStart) {
        return 0;
    }
    /* Offsets are relative to the revision's linker-authored asset base. Do
     * not let a developer-modified image point into unused cartridge padding
     * merely because the physical dump is larger than the authored ROM end. */
    relativeLimit = id->romEnd - id->assetLutEnd;
    previous = 0;
    for (i = 0; i <= count; i++) {
        uint32_t offset = read_be32(data + id->assetLutStart + (i + 1u) * 4u);
        if (offset < previous || offset > relativeLimit) {
            return 0;
        }
        previous = offset;
    }
    return 1;
}

DkrRomValidationOptions dkr_rom_validation_options_from_env(void) {
    DkrRomValidationOptions options;
    const char *anyRevision = getenv("MDKR_ROM_ANY_REVISION");
    const char *allowModified = getenv("MDKR_ROM_ALLOW_MODIFIED");
    options.allowAnyRevision = anyRevision != NULL && anyRevision[0] == '1';
    options.allowModified = allowModified != NULL && allowModified[0] == '1';
    return options;
}

const char *dkr_rom_reference_sha256(const char *build) {
    if (build != NULL && strcmp(build, "us.v80") == 0) {
        return "7de1a8fb2a9558cfc3d9ad4497df698c1e89cf7095ac1531557df2af40ba8bcf";
    }
    if (build != NULL && strcmp(build, "pal.v80") == 0) {
        return "584d59412b3a8c675f5569516a0406128028929e31544490a4dbc3ab16a038b9";
    }
    return NULL;
}

int dkr_rom_validate_image(uint8_t *data, uint32_t size,
                           const char *displayName,
                           const DkrRomValidationOptions *options,
                           DkrRomValidation *out) {
    const DkrRomValidationOptions strict = {0, 0};
    const DkrRomValidationOptions *policy = options != NULL ? options : &strict;
    const char *name = displayName != NULL && displayName[0] != '\0'
                           ? displayName
                           : "That file";
    const char *order;
    const char *expectedSha256;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->code = DKR_ROM_VALIDATION_WRONG_SIZE;
    snprintf(out->byteOrder, sizeof(out->byteOrder), "%s", "???");

    if (data == NULL || size != DKR_ROM_SIZE_BYTES) {
        if (size < 1024u * 1024u) {
            snprintf(out->message, sizeof(out->message),
                     "%s is only %u bytes; a Diddy Kong Racing ROM must be "
                     "exactly 12 MB (.z64, .v64, or .n64). The file is truncated "
                     "or is not a cartridge image.", name, size);
        } else {
            snprintf(out->message, sizeof(out->message),
                     "%s is %.1f MB; a Diddy Kong Racing ROM must be exactly "
                     "12 MB (.z64, .v64, or .n64). Choose an unheadered US 1.1 "
                     "or European 1.1 dump.",
                     name, (double)size / (1024.0 * 1024.0));
        }
        return 0;
    }

    order = dkr_rom_normalize_byte_order(data, size);
    if (order == NULL) {
        out->code = DKR_ROM_VALIDATION_WRONG_BYTE_ORDER;
        snprintf(out->message, sizeof(out->message),
                 "%s is 12 MB but is not an N64 ROM. Choose an unheadered "
                 ".z64, .v64, or .n64 image.", name);
        return 0;
    }
    snprintf(out->byteOrder, sizeof(out->byteOrder), "%s", order);

    dkr_rom_identify(data, &out->id);
    if (out->id.verdict != DKR_ROM_SUPPORTED && !policy->allowAnyRevision) {
        out->code = DKR_ROM_VALIDATION_UNSUPPORTED_REVISION;
        dkr_rom_describe(&out->id, name, out->message, sizeof(out->message));
        return 0;
    }

    mdkr_sha256_hex(data, size, out->sha256);
    expectedSha256 = dkr_rom_reference_sha256(out->id.decompBuild);
    if (out->id.verdict == DKR_ROM_SUPPORTED && expectedSha256 != NULL &&
        strcmp(out->sha256, expectedSha256) != 0 && !policy->allowModified) {
        out->code = DKR_ROM_VALIDATION_HASH_MISMATCH;
        snprintf(out->message, sizeof(out->message),
                 "%s identifies as %s, but its complete SHA-256 does not match "
                 "the supported reference image. The dump is modified or "
                 "damaged; choose a clean copy of your cartridge.",
                 name, out->id.revisionName != NULL ? out->id.revisionName
                                                    : "a supported revision");
        return 0;
    }

    if (!validate_asset_lut(data, size, &out->id)) {
        out->code = DKR_ROM_VALIDATION_INVALID_LAYOUT;
        snprintf(out->message, sizeof(out->message),
                 "%s has an invalid asset table for %s and cannot be started "
                 "safely. Choose a clean supported ROM.",
                 name, out->id.decompBuild != NULL ? out->id.decompBuild
                                                   : "this revision");
        return 0;
    }

    out->code = DKR_ROM_VALIDATION_OK;
    dkr_rom_describe(&out->id, name, out->message, sizeof(out->message));
    if (expectedSha256 != NULL && strcmp(out->sha256, expectedSha256) == 0) {
        size_t used = strlen(out->message);
        snprintf(out->message + used, sizeof(out->message) - used,
                 " Full-image integrity verified.");
    } else if (policy->allowModified && expectedSha256 != NULL) {
        snprintf(out->message, sizeof(out->message),
                 "%s is a modified %s image accepted only because the "
                 "MDKR_ROM_ALLOW_MODIFIED developer override is enabled.",
                 name, out->id.decompBuild);
    }
    return 1;
}
