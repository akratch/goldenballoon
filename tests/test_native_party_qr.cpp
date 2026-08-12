/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the registration calls the asserts wrap. */
#undef NDEBUG

#include "qrcodegen.hpp"

#include <cassert>
#include <cstdint>

int main() {
    const auto qr = qrcodegen::QrCode::encodeText(
        "https://party.example/controller/#AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        qrcodegen::QrCode::Ecc::QUARTILE);
    assert(qr.getSize() == 45);
    uint32_t hash = 2166136261u;
    unsigned dark = 0u;
    for (int y = 0; y < qr.getSize(); y++) {
        for (int x = 0; x < qr.getSize(); x++) {
            const uint32_t value = qr.getModule(x, y) ? 1u : 0u;
            dark += value;
            hash = (hash ^ value) * 16777619u;
        }
    }
    /* Cross-language oracle from the pinned browser implementation. */
    assert(dark == 980u);
    assert(hash == 0x9bd8b513u);
    return 0;
}
