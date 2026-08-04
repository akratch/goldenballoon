/**
 * rom_validation.h — authoritative, side-effect-free validation of a loaded
 * Diddy Kong Racing cartridge image.
 *
 * Every user-facing entry point must use this contract before calling the
 * engine.  The engine uses it again at its trust boundary.  That deliberate
 * second call is cheap compared with boot and prevents a file replaced between
 * selection and Play from bypassing validation.
 */
#ifndef MDKR_ROM_VALIDATION_H
#define MDKR_ROM_VALIDATION_H

#include <stdint.h>

#include "rom_id.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DKR_ROM_SIZE_BYTES 0x00C00000u

typedef enum {
    DKR_ROM_VALIDATION_OK = 0,
    DKR_ROM_VALIDATION_WRONG_SIZE,
    DKR_ROM_VALIDATION_WRONG_BYTE_ORDER,
    DKR_ROM_VALIDATION_UNSUPPORTED_REVISION,
    DKR_ROM_VALIDATION_HASH_MISMATCH,
    DKR_ROM_VALIDATION_INVALID_LAYOUT
} DkrRomValidationCode;

typedef struct {
    int allowAnyRevision;
    int allowModified;
} DkrRomValidationOptions;

typedef struct {
    DkrRomValidationCode code;
    DkrRomId id;
    char byteOrder[4];
    char sha256[MDKR_SHA256_HEX_SIZE];
    char message[512];
} DkrRomValidation;

/** Return the explicit developer overrides currently set in the environment. */
DkrRomValidationOptions dkr_rom_validation_options_from_env(void);

/** Reference SHA-256 for a supported decomp build, or NULL if none is known. */
const char *dkr_rom_reference_sha256(const char *build);

/**
 * Validate `data` in the same order as the engine loads it: exact size, byte
 * order, revision, complete-image SHA-256, revision bounds, and asset LUT.
 *
 * .v64/.n64 input is normalized in place to canonical .z64 order.  Returns 1
 * only when the image is safe to boot under `options`.  `displayName` is used
 * only in the actionable result message and may be NULL.
 */
int dkr_rom_validate_image(uint8_t *data, uint32_t size,
                           const char *displayName,
                           const DkrRomValidationOptions *options,
                           DkrRomValidation *out);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ROM_VALIDATION_H */
