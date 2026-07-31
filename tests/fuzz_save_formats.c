#include "save_codec.h"
#include "save_container.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    MdkrSaveDocument document;
    MdkrSaveContainerResult container_result;
    uint8_t sentinel[MDKR_SAVE_IMAGE_SIZE];

    memset(payload, 0xA5, sizeof(payload));
    memcpy(sentinel, payload, sizeof(sentinel));
    container_result =
        mdkr_save_container_decode(data, size, payload, NULL, NULL);
    if (container_result != MDKR_SAVE_CONTAINER_OK) {
        if (memcmp(payload, sentinel, sizeof(payload)) != 0) {
            __builtin_trap();
        }
        return 0;
    }
    if (mdkr_save_decode(payload, sizeof(payload), &document) != MDKR_SAVE_OK) {
        __builtin_trap();
    }
    {
        uint8_t encoded[MDKR_SAVE_IMAGE_SIZE];
        if (mdkr_save_encode(&document, encoded, sizeof(encoded)) !=
                MDKR_SAVE_OK ||
            memcmp(encoded, payload, sizeof(encoded)) != 0) {
            __builtin_trap();
        }
    }
    return 0;
}
