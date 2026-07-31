#ifndef MDKR_SAVE_CONTAINER_H
#define MDKR_SAVE_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#include "save_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_SAVE_CONTAINER_MAX_INPUT (64u * 1024u)
#define MDKR_SAVE_CONTAINER_FORMAT "mdkr64-save"
#define MDKR_SAVE_CONTAINER_PAYLOAD_FORMAT "dkr-eeprom-4k-be-v1"

typedef enum MdkrSaveContainerResult {
    MDKR_SAVE_CONTAINER_OK = 0,
    MDKR_SAVE_CONTAINER_ERR_ARGUMENT = -1,
    MDKR_SAVE_CONTAINER_ERR_SIZE = -2,
    MDKR_SAVE_CONTAINER_ERR_FORMAT = -3,
    MDKR_SAVE_CONTAINER_ERR_VERSION = -4,
    MDKR_SAVE_CONTAINER_ERR_PAYLOAD = -5,
    MDKR_SAVE_CONTAINER_ERR_DIGEST = -6,
    MDKR_SAVE_CONTAINER_ERR_CAPACITY = -7
} MdkrSaveContainerResult;

typedef enum MdkrSaveInputFormat {
    MDKR_SAVE_INPUT_RAW = 0,
    MDKR_SAVE_INPUT_CONTAINER = 1
} MdkrSaveInputFormat;

typedef struct MdkrSaveContainerMetadata {
    char created_at[64];
    char app_version[64];
    char source[32];
} MdkrSaveContainerMetadata;

/*
 * Accept either an exact 512-byte raw EEPROM or the version-1 JSON container.
 * `payload_out`, `format_out`, and `metadata_out` are committed only on success.
 */
MdkrSaveContainerResult mdkr_save_container_decode(
    const uint8_t *input, size_t input_size,
    uint8_t payload_out[MDKR_SAVE_IMAGE_SIZE], MdkrSaveInputFormat *format_out,
    MdkrSaveContainerMetadata *metadata_out);

/*
 * Encode a deterministic version-1 JSON container. `output_size` excludes the
 * trailing NUL, while `required_capacity` includes it. Supplying output == NULL
 * is a supported size query.
 */
MdkrSaveContainerResult mdkr_save_container_encode(
    const uint8_t payload[MDKR_SAVE_IMAGE_SIZE],
    const MdkrSaveContainerMetadata *metadata, char *output,
    size_t output_capacity, size_t *output_size, size_t *required_capacity);

const char *mdkr_save_container_result_string(MdkrSaveContainerResult result);

#ifdef __cplusplus
}
#endif

#endif
