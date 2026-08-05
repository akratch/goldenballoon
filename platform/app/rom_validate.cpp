// rom_validate.cpp — see rom_validate.h.
#include "rom_validate.h"
#include "fs_utf8.h"

#include "rom_id.h"
#include "rom_validation.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

namespace {

// Copy a NUL-terminated C string into a fixed buffer, always terminating.
void put(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == nullptr) { dst[0] = '\0'; return; }
    std::snprintf(dst, cap, "%s", src);
}

}  // namespace

RomInfo mdkr_validate_rom_progress(const char *path,
                                   MdkrRomValidationProgress progress,
                                   void *context) {
    RomInfo info;
    std::memset(&info, 0, sizeof(info));
    put(info.byte_order, sizeof(info.byte_order), "???");
    put(info.region, sizeof(info.region), "??");
    info.validation_code = DKR_ROM_VALIDATION_WRONG_SIZE;

    if (path == nullptr || path[0] == '\0') {
        put(info.message, sizeof(info.message), "No ROM selected.");
        return info;
    }

    std::FILE *f = mdkr_fopen_utf8(path, "rb");
    if (f == nullptr) {
        std::snprintf(info.message, sizeof(info.message),
                      "Could not open '%s'. Check the path and permissions.", path);
        return info;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        std::snprintf(info.message, sizeof(info.message),
                      "Could not determine the size of '%s'. Check the file and permissions.",
                      path);
        return info;
    }
    const long end = std::ftell(f);
    if (end > 0 && static_cast<unsigned long>(end) <= UINT32_MAX) {
        info.size_bytes = static_cast<unsigned>(end);
    }
    std::rewind(f);

    if (end <= 0 || static_cast<unsigned long>(end) != DKR_ROM_SIZE_BYTES) {
        DkrRomValidation validation;
        dkr_rom_validate_image(nullptr, info.size_bytes, path, nullptr, &validation);
        info.validation_code = validation.code;
        put(info.message, sizeof(info.message), validation.message);
        std::fclose(f);
        return info;
    }

    auto *bytes = static_cast<uint8_t *>(std::malloc(DKR_ROM_SIZE_BYTES));
    if (bytes == nullptr) {
        std::fclose(f);
        std::snprintf(info.message, sizeof(info.message),
                      "Could not allocate memory to verify '%s'. Close other applications and try again.",
                      path);
        return info;
    }
    size_t got = 0u;
    constexpr size_t kReadChunk = 64u * 1024u;
    if (progress != nullptr && !progress(0u, DKR_ROM_SIZE_BYTES, context)) {
        info.cancelled = 1;
    }
    while (!info.cancelled && got < DKR_ROM_SIZE_BYTES) {
        const size_t remaining = DKR_ROM_SIZE_BYTES - got;
        const size_t requested = remaining < kReadChunk ? remaining : kReadChunk;
        const size_t count = std::fread(bytes + got, 1u, requested, f);
        got += count;
        if (count != requested) {
            break;
        }
        if (progress != nullptr &&
            !progress(static_cast<unsigned>(got), DKR_ROM_SIZE_BYTES, context)) {
            info.cancelled = 1;
        }
    }
    std::fclose(f);
    if (info.cancelled) {
        std::free(bytes);
        put(info.message, sizeof(info.message), "ROM validation cancelled.");
        return info;
    }
    if (got != DKR_ROM_SIZE_BYTES) {
        std::free(bytes);
        std::snprintf(info.message, sizeof(info.message),
                      "Could not read all of '%s' (%zu of %u bytes). Check the drive and try again.",
                      path, got, DKR_ROM_SIZE_BYTES);
        return info;
    }

    const DkrRomValidationOptions options = dkr_rom_validation_options_from_env();
    DkrRomValidation validation;
    info.valid = dkr_rom_validate_image(bytes, DKR_ROM_SIZE_BYTES, path,
                                        &options, &validation);
    std::free(bytes);

    const DkrRomId &id = validation.id;
    info.validation_code = validation.code;
    put(info.byte_order, sizeof(info.byte_order), validation.byteOrder);
    put(info.sha256, sizeof(info.sha256), validation.sha256);
    const char *expected = dkr_rom_reference_sha256(id.decompBuild);
    info.integrity_verified =
        expected != nullptr && std::strcmp(validation.sha256, expected) == 0;

    put(info.title, sizeof(info.title), id.title);
    put(info.revision, sizeof(info.revision), id.revisionName ? id.revisionName : "");
    put(info.build, sizeof(info.build), id.decompBuild ? id.decompBuild : "");
    info.crc1 = id.crc1;
    info.crc2 = id.crc2;
    info.matched_by_crc = id.matchedByCrc;

    // Region from the header country code (0x3E, the 4th game-code byte), which
    // is what the player recognizes on the cart label.
    switch (id.gameCode[3]) {
        case 'E': put(info.region, sizeof(info.region), "US"); break;
        case 'P': put(info.region, sizeof(info.region), "EU"); break;
        case 'J': put(info.region, sizeof(info.region), "JP"); break;
        default:  put(info.region, sizeof(info.region), "??"); break;
    }

    put(info.message, sizeof(info.message), validation.message);
    return info;
}

RomInfo mdkr_validate_rom(const char *path) {
    return mdkr_validate_rom_progress(path, nullptr, nullptr);
}

const char *mdkr_supported_rom_list(void) {
    /* Function-local static initialization runs exactly once and is safe when
     * two threads reach it together, which the launcher's validation worker and
     * its UI thread can. A "recompute while empty" guard would instead race on
     * the buffer and repeat the work for a legitimately empty list. */
    static const std::string list = [] {
        char buffer[256];
        dkr_rom_supported_list(buffer, sizeof(buffer));
        return std::string(buffer);
    }();
    return list.c_str();
}
