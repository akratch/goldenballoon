/* mod_texture_key.h — the stable name a pack author addresses a texture by.
 *
 * THIS IS A PUBLISHED CONTRACT. Every pack in existence names its files by the
 * output of this function. Changing which fields participate, their order, or
 * their encoding renames every texture and silently breaks every pack. If it
 * ever must change, bump MDKR_MOD_TEXTURE_DIGEST_VERSION, keep the old path
 * readable, and say so in docs/MODDING.md.
 */
#ifndef MDKR64_MOD_TEXTURE_KEY_H
#define MDKR64_MOD_TEXTURE_KEY_H

#include "gfx_texture_cache_key.h"
#include <stddef.h>
#include <stdint.h>

#define MDKR_MOD_TEXTURE_DIGEST_VERSION 1u

/* Writes 32 lowercase hex characters plus NUL into `out_hex`.
 *
 * Participating inputs, in this exact order:
 *   1. MDKR_MOD_TEXTURE_DIGEST_VERSION  (u32 little-endian)
 *   2. key->width, key->height          (u16 little-endian each)
 *   3. key->fmt, key->siz, key->palette (u8 each)
 *   4. key->palette_hash, key->palette_fmt (u32 little-endian each)
 *   5. the decoded texel payload, `texel_bytes` of it
 *
 * Deliberately EXCLUDED, with reasons:
 *   key->addr              — an allocation address; different every launch
 *   key->source_line_bytes — an addressing detail of the same picture
 *   key->source_size_bytes — implied by width/height/siz
 *   key->line_swapped      — a decode fix, not a difference in the picture
 *   key->font_remastered   — a renderer choice
 *   key->mipmaps           — a renderer choice
 *   key->cutout            — a renderer choice
 *   key->override_generation — the toggle, added in Task 4
 */
void mdkr_mod_texture_digest(const struct DkrTexCacheKey *key,
                             const uint8_t *texels, size_t texel_bytes,
                             char out_hex[33]);

#endif /* MDKR64_MOD_TEXTURE_KEY_H */
