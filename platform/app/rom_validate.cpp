// rom_validate.cpp — see rom_validate.h.
#include "rom_validate.h"

#include "rom_id.h"   // dkr_rom_identify / _describe / _normalize_byte_order / _supported_list

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

// Copy a NUL-terminated C string into a fixed buffer, always terminating.
void put(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == nullptr) { dst[0] = '\0'; return; }
    std::snprintf(dst, cap, "%s", src);
}

}  // namespace

RomInfo mdkr_validate_rom(const char *path) {
    RomInfo info;
    std::memset(&info, 0, sizeof(info));
    put(info.byte_order, sizeof(info.byte_order), "???");
    put(info.region, sizeof(info.region), "??");

    if (path == nullptr || path[0] == '\0') {
        put(info.message, sizeof(info.message), "No ROM selected.");
        return info;
    }

    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::snprintf(info.message, sizeof(info.message),
                      "Could not open '%s'. Check the path and permissions.", path);
        return info;
    }

    // Size first (for display + the "this is far too small to be a cart" case).
    if (std::fseek(f, 0, SEEK_END) == 0) {
        long end = std::ftell(f);
        if (end > 0) info.size_bytes = (unsigned)end;
    }
    std::rewind(f);

    uint8_t hdr[0x40];
    size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (got != sizeof(hdr)) {
        std::snprintf(info.message, sizeof(info.message),
                      "'%s' is only %u bytes — too small to be an N64 ROM.",
                      path, info.size_bytes);
        return info;
    }

    // Normalizing a 64-byte buffer is enough: dkr_rom_normalize_byte_order only
    // needs >= 0x40 bytes and swaps in place, and every field dkr_rom_identify
    // reads (CRC pair, title, game code, version) lives inside the header.
    const char *order = dkr_rom_normalize_byte_order(hdr, (uint32_t)sizeof(hdr));
    if (order == nullptr) {
        std::snprintf(info.message, sizeof(info.message),
                      "'%s' is not an N64 ROM (no 80 37 12 40 magic in any byte order).",
                      path);
        return info;
    }
    put(info.byte_order, sizeof(info.byte_order), order);

    DkrRomId id;
    dkr_rom_identify(hdr, &id);

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

    // The verdict is the engine's, not ours: only DKR_ROM_SUPPORTED may boot.
    info.valid = (id.verdict == DKR_ROM_SUPPORTED) ? 1 : 0;
    dkr_rom_describe(&id, path, info.message, sizeof(info.message));
    return info;
}

const char *mdkr_supported_rom_list(void) {
    static char s_list[256];
    if (s_list[0] == '\0') {
        dkr_rom_supported_list(s_list, sizeof(s_list));
    }
    return s_list;
}
