#ifndef MDKR_VIRTUAL_PAK_H
#define MDKR_VIRTUAL_PAK_H

#include <stddef.h>
#include <stdint.h>

#define MDKR_VPAK_MAX_FILES 16
#define MDKR_VPAK_CAPACITY 31488
#define MDKR_VPAK_IMAGE_SIZE 32192
#define MDKR_VPAK_NAME_SIZE 16
#define MDKR_VPAK_EXT_SIZE 4

typedef enum {
    MDKR_VPAK_OK = 0,
    MDKR_VPAK_ERR_ARGUMENT,
    MDKR_VPAK_ERR_FORMAT,
    MDKR_VPAK_ERR_DIGEST,
    MDKR_VPAK_ERR_NOT_FOUND,
    MDKR_VPAK_ERR_EXISTS,
    MDKR_VPAK_ERR_DATA_FULL,
    MDKR_VPAK_ERR_DIR_FULL,
    MDKR_VPAK_ERR_RANGE
} MdkrVirtualPakResult;

typedef struct {
    int used;
    uint16_t company_code;
    uint32_t game_code;
    uint8_t game_name[MDKR_VPAK_NAME_SIZE];
    uint8_t ext_name[MDKR_VPAK_EXT_SIZE];
    uint32_t offset;
    uint32_t size;
} MdkrVirtualPakFile;

typedef struct {
    uint64_t generation;
    uint32_t payload_used;
    MdkrVirtualPakFile files[MDKR_VPAK_MAX_FILES];
    uint8_t payload[MDKR_VPAK_CAPACITY];
} MdkrVirtualPak;

void mdkr_virtual_pak_init(MdkrVirtualPak *pak);
MdkrVirtualPakResult mdkr_virtual_pak_decode(
    const uint8_t *image, size_t image_size, MdkrVirtualPak *out);
MdkrVirtualPakResult mdkr_virtual_pak_encode(
    const MdkrVirtualPak *pak, uint8_t *image, size_t image_capacity);

MdkrVirtualPakResult mdkr_virtual_pak_allocate(
    MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE], uint32_t size,
    int *file_number);
MdkrVirtualPakResult mdkr_virtual_pak_find(
    const MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE], int *file_number);
MdkrVirtualPakResult mdkr_virtual_pak_delete(
    MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE]);
MdkrVirtualPakResult mdkr_virtual_pak_read(
    const MdkrVirtualPak *pak, int file_number, uint32_t offset,
    uint8_t *data, uint32_t size);
MdkrVirtualPakResult mdkr_virtual_pak_write(
    MdkrVirtualPak *pak, int file_number, uint32_t offset,
    const uint8_t *data, uint32_t size);
MdkrVirtualPakResult mdkr_virtual_pak_file_state(
    const MdkrVirtualPak *pak, int file_number, MdkrVirtualPakFile *state);
uint32_t mdkr_virtual_pak_free_bytes(const MdkrVirtualPak *pak);
int mdkr_virtual_pak_used_files(const MdkrVirtualPak *pak);

#endif
