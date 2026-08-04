/**
 * asset_swap.h  —  Asset endianness-normalization layer for mdkr64.
 *
 * Diddy Kong Racing ships its assets big-endian (N64/MIPS). mdkr64 runs on a
 * little-endian host. Per PLAN.md decision #7 ("normalize at the asset-load
 * boundary"), every multi-byte field of every asset is byte-swapped to native
 * order *once*, right after the asset lands in RAM. Everything downstream — the
 * game logic and the F3DDKR HLE — then sees native-endian data and needs no
 * further swapping.
 *
 * ============================================================================
 *  WHAT GETS SWAPPED
 * ============================================================================
 *  - Struct/header fields that the game reads as multi-byte scalars
 *    (offsets-to-be-patched, counts, s16/s32/u16/u32/f32 values).
 *  - Vertex xyz (s16), Triangle UVs (s16), TriangleBatchInfo offsets+flags,
 *    TextureInfo ids, model/level-model/object-header internal offset tables.
 *
 *  WHAT STAYS UNSWAPPED (deliberately — verified against mgb64 gfx_pc):
 *  - Raw N64 texel payloads (RGBA16, RGBA32, IA and I and CI image bytes). These
 *    formats are byte-order-defined; the HLE texture uploader consumes them
 *    as-is. Only the TextureHeader in front of them is swapped.
 *  - CI4/CI8 TLUT palette data (RGBA16 texels, loaded from ASSET_EMPTY_14).
 *  - The 4 index/flag bytes at the head of each Triangle (flags,vi0,vi1,vi2)
 *    — the game reads them as individual bytes (Triangle.verticesArray / .flags),
 *    never as the disk-loaded u32 union, so they must NOT be swapped.
 *  - char[] names, ASCII/game-text/menu-text/level-name string blobs.
 *  - NOTE: DKR assets embed NO raw Gfx display lists. All F3DDKR command lists
 *    are built at runtime from the Vertex/Triangle/Batch arrays (rcp_dkr.c) and
 *    from runtime-built TextureHeader.cmd lists — so there are no on-disk
 *    two-word Gfx commands to u32-swap in any asset. (Contrast PLAN's generic
 *    note about DL words; it does not apply to DKR's asset payloads.)
 *
 * ============================================================================
 *  GZIP ORDERING (IMPORTANT)
 * ============================================================================
 *  Several sections are stored gzip-compressed in ROM. The swap operates on the
 *  DECOMPRESSED, typed bytes — it must run *after* gzip_inflate(), never on the
 *  compressed stream. The gzip stream's own little-endian length header is
 *  handled by game code (gzip.c byteswap32 / asset_table_load_zipped) and is
 *  NOT this layer's concern. Compressed section types: ASSET_OBJECT_MODELS,
 *  ASSET_OBJECT_ANIMATIONS, ASSET_LEVEL_MODELS, ASSET_LEVEL_OBJECT_MAPS, and
 *  compressed entries of ASSET_TEXTURES_2D/3D.
 *
 * ============================================================================
 *  INTEGRATION CONTRACT  (owner of asset_loading.c wires these in; do not edit
 *  asset_loading.c from this module)
 * ============================================================================
 *  The buffer handed to asset_swap_normalize() must be the RAW N64-layout image
 *  as just materialized in RAM (4-byte pointer/offset fields still present),
 *  BEFORE the game patches any internal offset into a host pointer. Call sites:
 *
 *   1. pi_init() (asset_loading.c): after the dmacopy that fills
 *      gAssetsLookupTable, call
 *          asset_swap_lut(gAssetsLookupTable, assetTableSize);
 *      This is the master asset LUT (array of u32 rom offsets).
 *
 *   2. asset_table_load(assetIndex) (asset_loading.c): this is the single
 *      primary hook. Just before `return out;`, call
 *          asset_swap_normalize(assetIndex, out, size);
 *      It normalizes every section that is loaded wholesale (all *_TABLE
 *      offset tables, FONTS, PARTICLES, PARTICLE_BEHAVIORS, the *_IDS lists,
 *      LEVEL_OBJECT_TRANSLATION_TABLE, TTGHOSTS_TABLE, WEATHER_PARTICLES, MISC,
 *      etc.). Dispatch keys off the AssetSectionsEnum value in `assetIndex`.
 *
 *   3. Gzip-compressed / whole-record loads that go through asset_load() in
 *      callers (asset_load only ever sees the still-compressed or partial
 *      bytes, so the swap CANNOT live inside asset_load). Add one call right
 *      after the data is fully materialized, passing the section enum + the
 *      decompressed byte size:
 *        - object_models.c  ~after gzip_inflate(...objMdl):        ASSET_OBJECT_MODELS
 *        - object_models.c  ~after gzip_inflate(...anim): call
 *              asset_swap_object_animation(anim, size,
 *                                          model->numberOfAnimatedVertices)
 *              (NOT the generic normalize — the blob lacks the vertex count)
 *        - tracks.c         ~after gzip_inflate(...gCurrentLevelModel): ASSET_LEVEL_MODELS
 *        - objects.c        ~after gzip_inflate(...mem) [obj map]: ASSET_LEVEL_OBJECT_MAPS
 *        - textures_sprites.c ~after load/inflate of TextureHeader: ASSET_TEXTURES_2D / _3D
 *        - game.c           ~after asset_load(ASSET_LEVEL_HEADERS): ASSET_LEVEL_HEADERS
 *        - objects.c        ~after asset_load(ASSET_OBJECTS) hdr:   ASSET_OBJECTS
 *        - textures_sprites.c ~after asset_load(ASSET_SPRITES):     ASSET_SPRITES
 *        - game_text.c      ~after asset_load(ASSET_GAME_TEXT_TABLE): ASSET_GAME_TEXT_TABLE
 *
 *  Idempotency: asset_swap_normalize() is NOT idempotent (swapping twice
 *  un-swaps). Call it exactly once per materialized asset. Sub-range partial
 *  loads (e.g. the throwaway TempTexHeader probe, CI-palette loads, game-text
 *  string bodies) must NOT be routed through it — see notes doc.
 *
 * ============================================================================
 *  PUNTED / PARTIAL (see docs/asset_swap_notes.md for the full table)
 * ============================================================================
 *  - ASSET_AUDIO: heterogeneous and owned by typed consumers. Bank/sequence
 *    images use explicit BE parsers, SoundData and MIDI headers convert at
 *    their call sites, raw samples/event streams remain byte-oriented, and
 *    VehicleSoundAsset records use asset_swap_vehicle_sound().
 *  - ASSET_LEVEL_OBJECT_MAPS: partial (file header + per-entry x/y/z; per-
 *    behavior body params punted).
 *  - ASSET_MISC, ASSET_JAPANESE_FONTS*: punted (heterogeneous / not used by
 *    us_v80). ASSET_SCREENS / ASSET_EMPTY_14: no-op (texel/palette data).
 *    ASSET_MISC sub-assets are normalized individually where their type becomes
 *    knowable: asset_swap_misc_lightdata() (LevelHeader_70),
 *    asset_swap_misc_boost() (sub-asset 20, Object_Boost[]) and
 *    objects.c dkr_misc_swap_words() (plain 32-bit-word arrays).
 */
#ifndef _PLATFORM_ASSET_SWAP_H_
#define _PLATFORM_ASSET_SWAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Normalize one materialized asset in place, BE -> host-native.
 *
 * @param assetType  an AssetSectionsEnum value (game/include/asset_enums.h)
 *                   identifying the section the buffer belongs to.
 * @param data       pointer to the raw (post-inflate, pre-offset-patch) bytes.
 * @param size       size of the materialized buffer in bytes.
 *
 * No-op on a big-endian host, and for data==NULL / size==0.
 */
void asset_swap_normalize(int assetType, void *data, uint32_t size);

/**
 * Normalize an asset lookup table (the master gAssetsLookupTable or any
 * ASSET_*_TABLE section): a flat array of big-endian u32 words. Swaps
 * size/4 words in place.
 */
void asset_swap_lut(void *data, uint32_t size);

/**
 * Normalize one 0x4C-byte vehicle-engine sound record from ASSET_AUDIO.
 *
 * ASSET_AUDIO is heterogeneous and cannot be passed to
 * asset_swap_normalize() as a whole. Vehicle records contain two u16 sound
 * IDs, ten u16 pitch control points, and seven s16 pitch-scale fields mixed
 * with byte-sized control data. This helper swaps only those serialized
 * multi-byte fields and leaves the u8 tables untouched.
 *
 * @return 1 when a complete record was normalized, otherwise 0. A short or
 *         NULL buffer is rejected without modification.
 */
int asset_swap_vehicle_sound(void *data, uint32_t size);

/**
 * Full normalization of one ASSET_OBJECT_ANIMATIONS blob (post-inflate).
 *
 * The animation blob does NOT store its own animated-vertex count, so the body
 * stride (keyframeSize = numAnimatedVertices*3 + 12) must be supplied by the
 * caller from the owning model's ObjectModel.numberOfAnimatedVertices. Call this
 * instead of asset_swap_normalize(ASSET_OBJECT_ANIMATIONS, ...) — the generic
 * dispatcher can only swap the leading keyframeCount. Layout is authoritative
 * from game/src/hasm_native/obj_animate.c. Do not double-call.
 *
 * @param data                  decompressed animation blob (leading s32 count).
 * @param size                  blob size in bytes.
 * @param numAnimatedVertices   owning model's numberOfAnimatedVertices.
 */
void asset_swap_object_animation(void *data, uint32_t size, uint32_t numAnimatedVertices);

/**
 * In-place BE->host swap of ONE LevelHeader_70 "pulsating light" sub-asset
 * fetched from the (punted, heterogeneous) ASSET_MISC section via
 * get_misc_asset(). ASSET_MISC is not normalized at load, so these records are
 * still big-endian; their type is only knowable at the func_8007F1E8 call sites
 * (game.c LevelHeader.unk74[] loop, game_ui.c ASSET_MISC_58).
 *
 * Layout (docs/OPEN_ITEMS.md): 0x00 s32 count, 0x04/0x08/0x0C s32 (swap),
 * 0x10/0x14 ColourRGBA (u8x4, no swap), then count x { s32 (swap); u8 r,g,b,a }
 * starting at 0x18 (8-byte stride).
 *
 * gAssetsMiscSection is loaded once and persists, but level_load() runs per
 * level and re-fetches the same offsets, so this guards against double-swap by
 * remembering every blob pointer it has already normalized (no-op on a repeat).
 * No-op on a big-endian host and for blob==NULL.
 */
void asset_swap_misc_lightdata(void *blob);

/**
 * In-place BE->host swap of ONE PulsatingLightData sub-asset fetched from the
 * (punted, heterogeneous) ASSET_MISC section via get_misc_asset(). Called from
 * game.c level_load() next to the LevelHeader_70 loop, for the same reason:
 * ASSET_MISC is not normalized at load and this record's type is only knowable
 * at the call site. Every field is multi-byte (4 x u16 header + s32 + u16 frame
 * pairs); left big-endian, init_pulsating_light_data() reads a byte-reversed
 * numberFrames (us.v80 sub-asset 64: 4 -> 1024) and walks far past the blob.
 * Guards against per-level re-swap by blob pointer, and clamps the frame walk
 * to `size`. No-op on a big-endian host and for blob==NULL.
 *
 * @param blob  the raw sub-asset bytes inside gAssetsMiscSection.
 * @param size  byte length of the sub-asset.
 */
void asset_swap_misc_pulsating(void *blob, uint32_t size);

/**
 * In-place BE->host swap of the ASSET_MISC magic-code (cheat) table, sub-asset
 * ASSET_MISC_MAGIC_CODES. The blob is MIXED — a u16 index block
 * (numberOfCheats, then 3 byte-offsets per cheat) followed by raw ASCII cheat
 * strings — so only the index block may be swapped; a blanket halfword swap
 * would byte-reverse every string.
 *
 * MUST be called AFTER decrypt_magic_codes(). The cipher transposes bit pairs
 * across each four-byte group and does not commute with a halfword byte swap;
 * moreover, the count that bounds the index block exists only in plaintext.
 *
 * Self-checking: the first string offset must equal the index block's byte
 * length ((1 + numberOfCheats*3) * 2; us.v80: 29 cheats -> 176), and every
 * offset must point to a NUL-terminated string inside the payload. If any
 * invariant does not hold, the function reverts its own writes and leaves the
 * blob untouched rather than exposing a partial table to the menu.
 *
 * @param blob  the raw sub-asset bytes inside gAssetsMiscSection.
 * @param size  byte length of the sub-asset.
 * @return 1 when the complete plaintext table is valid and normalized, else 0.
 */
int asset_swap_misc_magic_codes(void *blob, uint32_t size);

/**
 * Convert the ASSET_MISC sub-asset 20 boost/exhaust graphics table from its
 * big-endian, 0x80-stride ON-DISK form into a native-endian, host-stride
 * Object_Boost[] array owned by the caller.
 *
 * This one cannot be done in place, and cannot use a blanket word swap:
 *  - the record packs `s16 spriteId; s16 textureId` at 0x6C and four u8/s8 at
 *    0x70, so a 32-bit word swap would scramble them (that is why
 *    objects.c dkr_misc_swap_words() is not usable here); and
 *  - the ROM stride is 0x80 while sizeof(Object_Boost) is 0x88 on LP64 (two
 *    4-byte runtime pointer fields become 8-byte host pointers), so the blob
 *    cannot simply be indexed as Object_Boost[] after swapping.
 * See the block comment above the implementation for the confirmed field map.
 *
 * @param dst         caller-owned native array (Object_Boost[dstCapacity]).
 * @param dstStride   sizeof(Object_Boost) on this host.
 * @param dstCapacity number of entries `dst` can hold.
 * @param src         the raw ASSET_MISC_20 bytes inside gAssetsMiscSection.
 * @param srcBytes    byte length of the sub-asset (10 * 0x80 for us.v80).
 * @return number of entries written (min(srcBytes/0x80, dstCapacity)).
 *
 * Not idempotent with respect to `src` only in the trivial sense that it never
 * writes to `src`; it fully rewrites `dst` each call, so calling it again
 * discards any runtime state the game stored in the native array.
 */
uint32_t asset_swap_misc_boost(void *dst, uint32_t dstStride, uint32_t dstCapacity, const void *src,
                               uint32_t srcBytes);

/* Byteswap ONE TextureHeader (0x20 bytes) in place, big-endian -> host. Used by
 * load_texture(), whose asset_load() path does not run asset_swap_normalize, so
 * it normalizes each per-frame header itself while walking numberOfTextures. */
void swap_texture_header(void *texHeader);

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_ASSET_SWAP_H_ */
