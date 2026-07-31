#include "virtual_pak.h"

#include "sha256.h"

#include <limits.h>
#include <string.h>

#define VPAK_HEADER_SIZE 64u
#define VPAK_ENTRY_SIZE 40u
#define VPAK_DIRECTORY_SIZE (MDKR_VPAK_MAX_FILES * VPAK_ENTRY_SIZE)
#define VPAK_PAYLOAD_OFFSET (VPAK_HEADER_SIZE + VPAK_DIRECTORY_SIZE)
#define VPAK_VERSION 1u
#define VPAK_BLOCK_SIZE 256u

static const uint8_t s_magic[8] = {'M', 'D', 'K', 'R', 'P', 'F', 'S', '1'};

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void write_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void write_be64(uint8_t *p, uint64_t value) {
    write_be32(p, (uint32_t)(value >> 32));
    write_be32(p + 4, (uint32_t)value);
}

static void image_digest(
    const uint8_t image[MDKR_VPAK_IMAGE_SIZE],
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE]) {
    static const uint8_t zero_digest[MDKR_SHA256_DIGEST_SIZE] = {0};
    MdkrSha256 context;
    mdkr_sha256_init(&context);
    mdkr_sha256_update(&context, image, 32);
    mdkr_sha256_update(&context, zero_digest, sizeof(zero_digest));
    mdkr_sha256_update(
        &context, image + 64, MDKR_VPAK_IMAGE_SIZE - 64);
    mdkr_sha256_final(&context, digest);
}

static int identity_equal(
    const MdkrVirtualPakFile *file, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE]) {
    return file->used && file->company_code == company_code &&
           file->game_code == game_code &&
           memcmp(file->game_name, game_name, MDKR_VPAK_NAME_SIZE) == 0 &&
           memcmp(file->ext_name, ext_name, MDKR_VPAK_EXT_SIZE) == 0;
}

static MdkrVirtualPakResult validate_model(const MdkrVirtualPak *pak) {
    uint32_t size_sum = 0;
    uint32_t max_end = 0;
    int used = 0;
    int i;
    if (pak == NULL || pak->payload_used > MDKR_VPAK_CAPACITY) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        const MdkrVirtualPakFile *file = &pak->files[i];
        int j;
        if (!file->used) {
            continue;
        }
        if (file->size == 0 || (file->size & (VPAK_BLOCK_SIZE - 1u)) != 0 ||
            file->offset > MDKR_VPAK_CAPACITY ||
            file->size > MDKR_VPAK_CAPACITY - file->offset) {
            return MDKR_VPAK_ERR_FORMAT;
        }
        for (j = 0; j < i; j++) {
            const MdkrVirtualPakFile *prior = &pak->files[j];
            if (identity_equal(
                    prior, file->company_code, file->game_code,
                    file->game_name, file->ext_name)) {
                return MDKR_VPAK_ERR_FORMAT;
            }
            if (prior->used &&
                file->offset < prior->offset + prior->size &&
                prior->offset < file->offset + file->size) {
                return MDKR_VPAK_ERR_FORMAT;
            }
        }
        size_sum += file->size;
        if (file->offset + file->size > max_end) {
            max_end = file->offset + file->size;
        }
        used++;
    }
    /* The live allocator always compacts deletes. Requiring both total extent
     * and size sum to match rejects authenticated-but-malformed images with
     * gaps, hidden tails, or overlapping directory extents. */
    if (size_sum != pak->payload_used || max_end != pak->payload_used ||
        used > MDKR_VPAK_MAX_FILES) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    return MDKR_VPAK_OK;
}

void mdkr_virtual_pak_init(MdkrVirtualPak *pak) {
    if (pak != NULL) {
        memset(pak, 0, sizeof(*pak));
    }
}

MdkrVirtualPakResult mdkr_virtual_pak_decode(
    const uint8_t *image, size_t image_size, MdkrVirtualPak *out) {
    MdkrVirtualPak candidate;
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    uint32_t file_count;
    int i;
    if (image == NULL || out == NULL) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    if (image_size != MDKR_VPAK_IMAGE_SIZE ||
        memcmp(image, s_magic, sizeof(s_magic)) != 0 ||
        read_be32(image + 8) != VPAK_VERSION ||
        read_be32(image + 12) != MDKR_VPAK_IMAGE_SIZE) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    image_digest(image, digest);
    if (memcmp(digest, image + 32, sizeof(digest)) != 0) {
        return MDKR_VPAK_ERR_DIGEST;
    }
    mdkr_virtual_pak_init(&candidate);
    candidate.generation = read_be64(image + 16);
    candidate.payload_used = read_be32(image + 24);
    file_count = read_be32(image + 28);
    if (file_count > MDKR_VPAK_MAX_FILES ||
        candidate.payload_used > MDKR_VPAK_CAPACITY) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        const uint8_t *entry =
            image + VPAK_HEADER_SIZE + (size_t)i * VPAK_ENTRY_SIZE;
        MdkrVirtualPakFile *file = &candidate.files[i];
        if (entry[0] > 1) {
            return MDKR_VPAK_ERR_FORMAT;
        }
        if (!entry[0]) {
            size_t j;
            for (j = 0; j < VPAK_ENTRY_SIZE; j++) {
                if (entry[j] != 0) return MDKR_VPAK_ERR_FORMAT;
            }
            continue;
        }
        file->used = 1;
        file->company_code = read_be16(entry + 2);
        file->game_code = read_be32(entry + 4);
        file->size = read_be32(entry + 8);
        file->offset = read_be32(entry + 12);
        memcpy(file->game_name, entry + 16, MDKR_VPAK_NAME_SIZE);
        memcpy(file->ext_name, entry + 32, MDKR_VPAK_EXT_SIZE);
        if (entry[1] != 0 || read_be32(entry + 36) != 0) {
            return MDKR_VPAK_ERR_FORMAT;
        }
    }
    if (mdkr_virtual_pak_used_files(&candidate) != (int)file_count ||
        validate_model(&candidate) != MDKR_VPAK_OK) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    memcpy(
        candidate.payload, image + VPAK_PAYLOAD_OFFSET,
        candidate.payload_used);
    for (i = (int)candidate.payload_used; i < MDKR_VPAK_CAPACITY; i++) {
        if (image[VPAK_PAYLOAD_OFFSET + (size_t)i] != 0) {
            return MDKR_VPAK_ERR_FORMAT;
        }
    }
    *out = candidate;
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_encode(
    const MdkrVirtualPak *pak, uint8_t *image, size_t image_capacity) {
    uint8_t digest[MDKR_SHA256_DIGEST_SIZE];
    int i;
    if (pak == NULL || image == NULL) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    if (image_capacity < MDKR_VPAK_IMAGE_SIZE) {
        return MDKR_VPAK_ERR_RANGE;
    }
    if (validate_model(pak) != MDKR_VPAK_OK) {
        return MDKR_VPAK_ERR_FORMAT;
    }
    memset(image, 0, MDKR_VPAK_IMAGE_SIZE);
    memcpy(image, s_magic, sizeof(s_magic));
    write_be32(image + 8, VPAK_VERSION);
    write_be32(image + 12, MDKR_VPAK_IMAGE_SIZE);
    write_be64(image + 16, pak->generation);
    write_be32(image + 24, pak->payload_used);
    write_be32(image + 28, (uint32_t)mdkr_virtual_pak_used_files(pak));
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        const MdkrVirtualPakFile *file = &pak->files[i];
        uint8_t *entry =
            image + VPAK_HEADER_SIZE + (size_t)i * VPAK_ENTRY_SIZE;
        if (!file->used) continue;
        entry[0] = 1;
        write_be16(entry + 2, file->company_code);
        write_be32(entry + 4, file->game_code);
        write_be32(entry + 8, file->size);
        write_be32(entry + 12, file->offset);
        memcpy(entry + 16, file->game_name, MDKR_VPAK_NAME_SIZE);
        memcpy(entry + 32, file->ext_name, MDKR_VPAK_EXT_SIZE);
    }
    memcpy(image + VPAK_PAYLOAD_OFFSET, pak->payload, pak->payload_used);
    image_digest(image, digest);
    memcpy(image + 32, digest, sizeof(digest));
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_find(
    const MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE], int *file_number) {
    int i;
    if (pak == NULL || game_name == NULL || ext_name == NULL) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        if (identity_equal(
                &pak->files[i], company_code, game_code, game_name, ext_name)) {
            if (file_number != NULL) *file_number = i;
            return MDKR_VPAK_OK;
        }
    }
    if (file_number != NULL) *file_number = -1;
    return MDKR_VPAK_ERR_NOT_FOUND;
}

MdkrVirtualPakResult mdkr_virtual_pak_allocate(
    MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE], uint32_t size,
    int *file_number) {
    uint32_t rounded;
    int slot = -1;
    int i;
    if (pak == NULL || game_name == NULL || ext_name == NULL || size == 0) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    if (mdkr_virtual_pak_find(
            pak, company_code, game_code, game_name, ext_name, NULL) ==
        MDKR_VPAK_OK) {
        return MDKR_VPAK_ERR_EXISTS;
    }
    if (size > UINT32_MAX - (VPAK_BLOCK_SIZE - 1u)) {
        return MDKR_VPAK_ERR_DATA_FULL;
    }
    rounded = (size + VPAK_BLOCK_SIZE - 1u) & ~(VPAK_BLOCK_SIZE - 1u);
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        if (!pak->files[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return MDKR_VPAK_ERR_DIR_FULL;
    if (rounded > MDKR_VPAK_CAPACITY - pak->payload_used) {
        return MDKR_VPAK_ERR_DATA_FULL;
    }
    pak->files[slot].used = 1;
    pak->files[slot].company_code = company_code;
    pak->files[slot].game_code = game_code;
    pak->files[slot].offset = pak->payload_used;
    pak->files[slot].size = rounded;
    memcpy(pak->files[slot].game_name, game_name, MDKR_VPAK_NAME_SIZE);
    memcpy(pak->files[slot].ext_name, ext_name, MDKR_VPAK_EXT_SIZE);
    memset(pak->payload + pak->payload_used, 0, rounded);
    pak->payload_used += rounded;
    pak->generation++;
    if (file_number != NULL) *file_number = slot;
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_delete(
    MdkrVirtualPak *pak, uint16_t company_code, uint32_t game_code,
    const uint8_t game_name[MDKR_VPAK_NAME_SIZE],
    const uint8_t ext_name[MDKR_VPAK_EXT_SIZE]) {
    int file_number;
    uint32_t offset;
    uint32_t size;
    int i;
    MdkrVirtualPakResult result = mdkr_virtual_pak_find(
        pak, company_code, game_code, game_name, ext_name, &file_number);
    if (result != MDKR_VPAK_OK) return result;
    offset = pak->files[file_number].offset;
    size = pak->files[file_number].size;
    memmove(
        pak->payload + offset, pak->payload + offset + size,
        pak->payload_used - offset - size);
    pak->payload_used -= size;
    memset(pak->payload + pak->payload_used, 0, size);
    memset(&pak->files[file_number], 0, sizeof(pak->files[file_number]));
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        if (pak->files[i].used && pak->files[i].offset > offset) {
            pak->files[i].offset -= size;
        }
    }
    pak->generation++;
    return MDKR_VPAK_OK;
}

static MdkrVirtualPakResult check_range(
    const MdkrVirtualPak *pak, int file_number, uint32_t offset,
    uint32_t size) {
    const MdkrVirtualPakFile *file;
    if (pak == NULL || file_number < 0 ||
        file_number >= MDKR_VPAK_MAX_FILES) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    file = &pak->files[file_number];
    if (!file->used) return MDKR_VPAK_ERR_NOT_FOUND;
    if (offset > file->size || size > file->size - offset) {
        return MDKR_VPAK_ERR_RANGE;
    }
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_read(
    const MdkrVirtualPak *pak, int file_number, uint32_t offset,
    uint8_t *data, uint32_t size) {
    MdkrVirtualPakResult result;
    if (data == NULL && size != 0) return MDKR_VPAK_ERR_ARGUMENT;
    result = check_range(pak, file_number, offset, size);
    if (result != MDKR_VPAK_OK) return result;
    if (size != 0) {
        memcpy(
            data, pak->payload + pak->files[file_number].offset + offset,
            size);
    }
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_write(
    MdkrVirtualPak *pak, int file_number, uint32_t offset,
    const uint8_t *data, uint32_t size) {
    MdkrVirtualPakResult result;
    if (data == NULL && size != 0) return MDKR_VPAK_ERR_ARGUMENT;
    result = check_range(pak, file_number, offset, size);
    if (result != MDKR_VPAK_OK) return result;
    if (size != 0) {
        memcpy(
            pak->payload + pak->files[file_number].offset + offset, data,
            size);
    }
    pak->generation++;
    return MDKR_VPAK_OK;
}

MdkrVirtualPakResult mdkr_virtual_pak_file_state(
    const MdkrVirtualPak *pak, int file_number, MdkrVirtualPakFile *state) {
    if (pak == NULL || state == NULL || file_number < 0 ||
        file_number >= MDKR_VPAK_MAX_FILES) {
        return MDKR_VPAK_ERR_ARGUMENT;
    }
    if (!pak->files[file_number].used) return MDKR_VPAK_ERR_NOT_FOUND;
    *state = pak->files[file_number];
    return MDKR_VPAK_OK;
}

uint32_t mdkr_virtual_pak_free_bytes(const MdkrVirtualPak *pak) {
    if (pak == NULL || pak->payload_used > MDKR_VPAK_CAPACITY) return 0;
    return MDKR_VPAK_CAPACITY - pak->payload_used;
}

int mdkr_virtual_pak_used_files(const MdkrVirtualPak *pak) {
    int count = 0;
    int i;
    if (pak == NULL) return 0;
    for (i = 0; i < MDKR_VPAK_MAX_FILES; i++) {
        if (pak->files[i].used) count++;
    }
    return count;
}
