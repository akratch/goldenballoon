/*
 * test_mod_texture_key.c — the pack-addressing digest is a published contract.
 *
 * These groups guard the four properties a content pack depends on: the digest
 * is the same every time it is computed, it ignores state that belongs to the
 * running process rather than to the picture, it changes when the picture
 * changes, and it changes when the RDP format changes. The pinned value in the
 * first group is the contract itself.
 */
#include "mod_texture_key.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (condition) {
        printf("ok: %s\n", message);
    } else {
        printf("FAIL: %s\n", message);
        failures++;
    }
}

/* Regenerate ONLY with a deliberate MDKR_MOD_TEXTURE_DIGEST_VERSION bump. */
static const char kPinnedDigest[] = "db74904875bc2d4b6a65618db875b970";

static void test_digest_is_stable_across_runs(void) {
    struct DkrTexCacheKey k = {0};
    k.width = 32;
    k.height = 32;
    k.fmt = 0;
    k.siz = 2;
    static const uint8_t texels[32 * 32 * 2] = {0x12, 0x34};
    char a[33], b[33];
    mdkr_mod_texture_digest(&k, texels, sizeof texels, a);
    mdkr_mod_texture_digest(&k, texels, sizeof texels, b);
    expect(!strcmp(a, b), "digest is deterministic");
    expect(strlen(a) == 32, "digest is 32 hex characters");
    /* Pinned value: changing it invalidates every pack in existence.
     * Regenerate ONLY with a deliberate format version bump. */
    if (strcmp(a, kPinnedDigest) != 0) {
        printf("  pinned %s\n  actual %s\n", kPinnedDigest, a);
    }
    expect(!strcmp(a, kPinnedDigest), "digest matches the pinned contract value");
}

static void test_digest_ignores_transient_fields(void) {
    struct DkrTexCacheKey a = {0}, b = {0};
    a.width = b.width = 8;
    a.height = b.height = 8;
    a.siz = b.siz = 2;
    /* Allocation identity: transient. The uintptr_t hop keeps the two distinct
     * addresses the assertion needs without a narrowing-cast diagnostic. */
    a.addr = (const uint8_t *)(uintptr_t)0x1000;
    b.addr = (const uint8_t *)(uintptr_t)0x2000;
    a.mipmaps = 0;
    b.mipmaps = 1; /* a renderer choice, not content */
    static const uint8_t t[8 * 8 * 2] = {1};
    char da[33], db[33];
    mdkr_mod_texture_digest(&a, t, sizeof t, da);
    mdkr_mod_texture_digest(&b, t, sizeof t, db);
    expect(!strcmp(da, db),
           "allocation address and mip choice do not change the digest");
}

static void test_digest_is_sensitive_to_content(void) {
    struct DkrTexCacheKey k = {0};
    k.width = 8;
    k.height = 8;
    k.siz = 2;
    uint8_t t1[8 * 8 * 2] = {1}, t2[8 * 8 * 2] = {2};
    char d1[33], d2[33];
    mdkr_mod_texture_digest(&k, t1, sizeof t1, d1);
    mdkr_mod_texture_digest(&k, t2, sizeof t2, d2);
    expect(strcmp(d1, d2) != 0, "one changed texel changes the digest");
}

static void test_digest_is_sensitive_to_format(void) {
    /* Two textures with identical bytes but different RDP formats are
     * different pictures and must not share an override. */
    struct DkrTexCacheKey a = {0}, b = {0};
    a.width = b.width = 8;
    a.height = b.height = 8;
    a.siz = 2;
    b.siz = 3;
    static const uint8_t t[8 * 8 * 4] = {7};
    char da[33], db[33];
    mdkr_mod_texture_digest(&a, t, 8 * 8 * 2, da);
    mdkr_mod_texture_digest(&b, t, 8 * 8 * 4, db);
    expect(strcmp(da, db) != 0, "format participates in the digest");
}

/*
 * Padding independence.
 *
 * struct DkrTexCacheKey carries trailing padding, and padding bytes belong to
 * no field, so nothing ever gives them a defined value. An implementation that
 * hashed the struct's raw object representation would fold those bytes into
 * the digest, and the same texture would then be named differently between two
 * call sites, two runs, or two compilers — renaming every file in every pack
 * for a reason no pack author could observe. Neither ASan nor UBSan reports a
 * read of uninitialised memory, so this group is the only thing in the suite
 * that would catch the regression.
 *
 * Three routes build the same participating fields with deliberately different
 * object representations, and all three must agree. The union route is the
 * sharp one: its padding definitely holds 0xaa rather than incidental zeroes.
 */
union KeyStorage {
    struct DkrTexCacheKey key;
    unsigned char         bytes[sizeof(struct DkrTexCacheKey)];
};

static void fill_participating_fields(struct DkrTexCacheKey *key) {
    key->width = 16;
    key->height = 8;
    key->fmt = 1;
    key->siz = 2;
    key->palette = 3;
    key->palette_hash = 0x0badf00du;
    key->palette_fmt = 2u;
}

/* Every excluded field set to the same value on both routes, so padding is the
 * only byte-level difference left between the two objects. */
static void fill_excluded_fields(struct DkrTexCacheKey *key) {
    key->addr = NULL;
    key->source_line_bytes = 0;
    key->source_size_bytes = 0;
    key->line_swapped = false;
    key->font_outline = false;
    key->font_remastered = false;
    key->mipmaps = false;
    key->cutout = false;
    key->override_generation = 0;
}

static void test_digest_ignores_struct_padding(void) {
    union KeyStorage      zeroed, dirty;
    struct DkrTexCacheKey literal = {
        .width = 16,
        .height = 8,
        .fmt = 1,
        .siz = 2,
        .palette = 3,
        .palette_hash = 0x0badf00du,
        .palette_fmt = 2u,
    };
    static const uint8_t t[16 * 8 * 2] = {9, 8, 7};
    char                 dz[33], dl[33], dd[33];

    memset(zeroed.bytes, 0, sizeof zeroed.bytes);
    fill_participating_fields(&zeroed.key);
    fill_excluded_fields(&zeroed.key);

    memset(dirty.bytes, 0xaa, sizeof dirty.bytes);
    fill_participating_fields(&dirty.key);
    fill_excluded_fields(&dirty.key);

    /* Positive control: without this the group could pass vacuously on a
     * layout whose two routes happen to produce identical bytes. */
    expect(memcmp(zeroed.bytes, dirty.bytes, sizeof zeroed.bytes) != 0,
           "the two routes really do differ in their padding bytes");

    mdkr_mod_texture_digest(&zeroed.key, t, sizeof t, dz);
    mdkr_mod_texture_digest(&literal, t, sizeof t, dl);
    mdkr_mod_texture_digest(&dirty.key, t, sizeof t, dd);

    expect(!strcmp(dz, dl),
           "field-by-field and compound-literal keys digest identically");
    expect(!strcmp(dz, dd), "non-zero padding does not change the digest");
}

int main(void) {
    test_digest_is_stable_across_runs();
    test_digest_ignores_transient_fields();
    test_digest_is_sensitive_to_content();
    test_digest_is_sensitive_to_format();
    test_digest_ignores_struct_padding();

    if (failures != 0) {
        fprintf(stderr, "%d mod texture key assertion(s) failed\n", failures);
        return 1;
    }
    puts("all mod texture key tests passed");
    return 0;
}
