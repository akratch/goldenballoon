#ifndef GFX_PALETTE_H
#define GFX_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

/* N64 TLUT entry -> RGBA32 texel decode, shared by the CI4/CI8 run-dl import
 * path (gfx_pc.c) and unit-testable in isolation (tests/test_palette_decode.c).
 *
 * IA16 TLUT layout on N64 hardware is intensity in the HIGH byte, alpha in the
 * LOW byte (I8A8, MSB first) -- the same convention the settex-path decoders in
 * gfx_pc.c use ((c >> 8) for intensity). The run-dl path shipped byte-swapped
 * for months, which made every opaque-dark IA16 palette entry decode as
 * white-with-alpha-0: under G_CC_MODULATEIA the texel RGB collapsed to pure
 * white and the whole texture rendered as flat vertex shade (the Dam monitor
 * flat-green screens, DAM_PARITY_DEEP_DIVE 2026-07-17 §4.7). */
static inline void gfx_palette_entry_to_rgba32(uint16_t palentry, bool is_ia16,
                                               uint8_t *rgba32_out)
{
    if (is_ia16) {
        uint8_t intensity = (uint8_t)(palentry >> 8);
        uint8_t alpha = (uint8_t)(palentry & 0xFF);
        rgba32_out[0] = intensity;
        rgba32_out[1] = intensity;
        rgba32_out[2] = intensity;
        rgba32_out[3] = alpha;
    } else {
        uint8_t a = palentry & 1;
        uint8_t r = (uint8_t)(palentry >> 11);
        uint8_t g = (palentry >> 6) & 0x1F;
        uint8_t b = (palentry >> 1) & 0x1F;
        rgba32_out[0] = (uint8_t)((r << 3) | (r >> 2));
        rgba32_out[1] = (uint8_t)((g << 3) | (g >> 2));
        rgba32_out[2] = (uint8_t)((b << 3) | (b >> 2));
        rgba32_out[3] = a ? 255 : 0;
    }
}

#endif /* GFX_PALETTE_H */
