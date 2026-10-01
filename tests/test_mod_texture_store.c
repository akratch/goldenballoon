/*
 * test_mod_texture_store.c — what the pack-texture store does with a PNG
 * BEFORE it decodes it.
 *
 * A content pack is a file a player downloaded from a stranger, and a PNG
 * header is thirteen bytes that can claim any size the format allows. The
 * store's cache cap is what stands between that claim and the machine's
 * memory, and it is only worth anything if it is consulted before the decoder
 * is handed the file: a cap applied afterwards bounds what is RETAINED, never
 * what is ALLOCATED, and the allocation is the whole of the damage.
 *
 * "Rejected" therefore is not the property under test -- an oversized picture
 * came back rejected before this was fixed too, having first been decoded in
 * full. The property is WHEN. Nothing the store returns distinguishes the two,
 * so this test links tests/mod_texture_store_probe.c in place of
 * lib/stb/stb_image_impl.c: same stb_image, with the decode entry point and
 * the decoder's allocator each behind a counter. A store that reads the
 * declared size first enters neither.
 *
 * Every fixture is built byte by byte here rather than by an encoder, because
 * the exact bytes are the test: the giant one is a real, well-formed PNG
 * header (correct CRCs, dimensions stb itself is willing to decode) attached
 * to a few bytes of image data, which is precisely the file an attacker sends
 * and precisely the file an encoder cannot be asked to produce.
 *
 * The last case is about a different file the same module writes: the author
 * dump's `<digest>.txt` record and its `<digest>.texels` companion. Those are
 * parsed by tools/ricepack/, so a field that is absent, misnamed or truncated
 * turns into a confident wrong CRC offline rather than a visible failure here
 * -- which is the only reason a text file is worth asserting field by field.
 *
 * Every byte written lands beneath the scratch root given by argv[1], which is
 * removed before the first case and after the last.
 */
#include "mod_texture_store.h"

#include "mod_registry.h"
#include "mod_texture_store_probe.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_SETENV(name, value) _putenv_s((name), (value))
#else
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0777)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_SETENV(name, value) setenv((name), (value), 1)
#endif

/* The digests the cases address. Only the length is load-bearing (the store
 * refuses anything that is not exactly its digest length), and main() checks
 * it rather than trusting a hand count. */
#define DIGEST_GIANT "a1111111111111111111111111111111"
#define DIGEST_WIDE  "a2222222222222222222222222222222"
#define DIGEST_SOUND "b2222222222222222222222222222222"
#define DIGEST_NOTPNG "c3333333333333333333333333333333"
#define DIGEST_UNTIMED "f6666666666666666666666666666666"
#define DIGEST_TIMED   "f7777777777777777777777777777777"
#define DIGEST_ABSENT  "f8888888888888888888888888888888"

/* Handed to the probe as its refusal ceiling. Far above anything these
 * fixtures legitimately need and far below the gigabyte the giant fixture
 * would cost, so the deliberately-broken build can be run on this machine: the
 * request is measured, never served. */
#define PROBE_REFUSE_ABOVE ((size_t)16u * 1024u * 1024u)

/* The bound the "nothing was allocated" assertion uses. The store's cache cap
 * is 512 MiB and is private to mod_texture_store.c; this is three orders of
 * magnitude under it and three orders over anything a thirteen-byte header
 * parse could want, so it separates "the header was read" from "the picture
 * was allocated" without pinning either implementation's exact appetite. */
#define HEADER_PARSE_CEILING ((size_t)1u * 1024u * 1024u)

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

static void fatal(const char *what, const char *detail) {
    printf("FATAL %s: %s\n", what, detail);
    exit(2);
}

/* ---------------------------------------------------------------- scratch */

static void path2(char *out, size_t size, const char *a, const char *b) {
    int written = snprintf(out, size, "%s/%s", a, b);
    if (written < 0 || (size_t)written >= size) fatal("scratch path too long", a);
}

static void path3(char *out, size_t size, const char *a, const char *b,
                  const char *c) {
    int written = snprintf(out, size, "%s/%s/%s", a, b, c);
    if (written < 0 || (size_t)written >= size) fatal("scratch path too long", a);
}

static int is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static void make_parent_directories(const char *path) {
    char work[1024];
    size_t index;

    if (strlen(path) >= sizeof work) fatal("scratch path too long", path);
    memcpy(work, path, strlen(path) + 1);
    for (index = 1; work[index] != '\0'; index++) {
        if (work[index] != '/') continue;
        work[index] = '\0';
        if (TEST_MKDIR(work) != 0 && !is_directory(work)) {
            fatal("cannot create scratch directory", work);
        }
        work[index] = '/';
    }
}

static void write_bytes(const char *path, const unsigned char *data,
                        size_t length) {
    FILE *file;

    make_parent_directories(path);
    file = fopen(path, "wb");
    if (file == NULL) fatal("cannot create scratch file", path);
    if (length != 0 && fwrite(data, 1, length, file) != length) {
        fclose(file);
        fatal("cannot write scratch file", path);
    }
    fclose(file);
}

/* Writes `data` to `root/pack/rel`, creating every directory on the way. */
static void write_pack_bytes(const char *root, const char *pack,
                             const char *rel, const unsigned char *data,
                             size_t length) {
    char full[1024];
    path3(full, sizeof full, root, pack, rel);
    write_bytes(full, data, length);
}

static void write_pack_text(const char *root, const char *pack,
                            const char *rel, const char *text) {
    write_pack_bytes(root, pack, rel, (const unsigned char *)text,
                     strlen(text));
}

/* Only ever pointed at the scratch root. */
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    struct dirent *entry;

    if (dir == NULL) {
        remove(path);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[1024];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        path2(child, sizeof child, path, entry->d_name);
        if (is_directory(child)) remove_tree(child);
        else                     remove(child);
    }
    closedir(dir);
    TEST_RMDIR(path);
}

/* ------------------------------------------------------------ PNG fixtures */

typedef struct PngBuffer {
    unsigned char bytes[1024];
    size_t        length;
} PngBuffer;

static void png_put(PngBuffer *png, const unsigned char *data, size_t length) {
    if (png->length + length > sizeof png->bytes) {
        fatal("fixture PNG too large for its buffer", "png_put");
    }
    memcpy(png->bytes + png->length, data, length);
    png->length += length;
}

static void png_put_be32(PngBuffer *png, uint32_t value) {
    unsigned char raw[4];
    raw[0] = (unsigned char)(value >> 24);
    raw[1] = (unsigned char)(value >> 16);
    raw[2] = (unsigned char)(value >> 8);
    raw[3] = (unsigned char)value;
    png_put(png, raw, sizeof raw);
}

/* PNG's CRC-32 (ISO-HDLC), computed without a table: the fixtures are a few
 * hundred bytes each and a table would be more of this file to be wrong in. */
static uint32_t crc32_of(const unsigned char *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t   index;
    int      bit;

    for (index = 0; index < length; index++) {
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t adler32_of(const unsigned char *data, size_t length) {
    uint32_t low = 1;
    uint32_t high = 0;
    size_t   index;

    for (index = 0; index < length; index++) {
        low = (low + data[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

/* One chunk: length, type, payload, and the CRC over type+payload. The CRCs
 * are correct on purpose. stb does not check them, but a fixture that is only
 * a PNG to the one decoder under test would be arguing with itself. */
static void png_put_chunk(PngBuffer *png, const char *type,
                          const unsigned char *data, size_t length) {
    unsigned char covered[1024];

    if (length + 4u > sizeof covered) {
        fatal("fixture chunk too large for its buffer", type);
    }
    memcpy(covered, type, 4);
    if (length != 0) memcpy(covered + 4, data, length);

    png_put_be32(png, (uint32_t)length);
    png_put(png, covered, length + 4u);
    png_put_be32(png, crc32_of(covered, length + 4u));
}

static void png_begin(PngBuffer *png) {
    static const unsigned char signature[8] = {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
    };
    png->length = 0;
    png_put(png, signature, sizeof signature);
}

static void png_put_ihdr(PngBuffer *png, uint32_t width, uint32_t height,
                         unsigned char colour_type) {
    unsigned char ihdr[13];

    ihdr[0] = (unsigned char)(width >> 24);
    ihdr[1] = (unsigned char)(width >> 16);
    ihdr[2] = (unsigned char)(width >> 8);
    ihdr[3] = (unsigned char)width;
    ihdr[4] = (unsigned char)(height >> 24);
    ihdr[5] = (unsigned char)(height >> 16);
    ihdr[6] = (unsigned char)(height >> 8);
    ihdr[7] = (unsigned char)height;
    ihdr[8] = 8;            /* bit depth */
    ihdr[9] = colour_type;
    ihdr[10] = 0;           /* deflate */
    ihdr[11] = 0;           /* adaptive filtering */
    ihdr[12] = 0;           /* no interlace */
    png_put_chunk(png, "IHDR", ihdr, sizeof ihdr);
}

/* A well-formed PNG header declaring `width` x `height` greyscale, followed by
 * a token IDAT and IEND.
 *
 * Greyscale, not RGBA, and that is the point of the case. stb_image carries a
 * ceiling of its own -- it refuses a picture where (1<<30) / width / channels
 * < height -- but it counts the channels the FILE stores, and this store always
 * asks the decoder for RGBA. One byte a pixel on the way in, four on the way
 * out: 32768x32768 greyscale is a size stb is perfectly willing to decode and
 * four gigabytes by the time the store would hold it. That gap is exactly what
 * the cache cap is for, and exactly why the cap has to be applied to the size
 * the header DECLARES rather than to the picture once it exists.
 *
 * The IDAT is eight bytes of nonsense on purpose. It never has to decompress:
 * a decoder reaching IEND sizes its output buffer from the header first, which
 * is the allocation this test exists to catch. */
static void write_giant_declared_png(const char *root, const char *pack,
                                     const char *digest, uint32_t width,
                                     uint32_t height) {
    static const unsigned char nonsense[8] = {
        0x78u, 0x01u, 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x00u, 0x00u
    };
    PngBuffer png;
    char      relative[128];

    png_begin(&png);
    png_put_ihdr(&png, width, height, 0);
    png_put_chunk(&png, "IDAT", nonsense, sizeof nonsense);
    png_put_chunk(&png, "IEND", NULL, 0);

    snprintf(relative, sizeof relative, "textures/%s.png", digest);
    write_pack_bytes(root, pack, relative, png.bytes, png.length);
}

/* A real, decodable 8x8 RGBA PNG. Its image data is a single stored (BTYPE=0)
 * deflate block, which is a legal zlib stream and keeps the whole fixture
 * inspectable: no compressor is involved in a test about what a decoder is
 * allowed to be asked for. Pixel (x, y) is (x * 16, y * 16, 0x40, 0xFF), so a
 * lookup that returned some other picture would not match. */
static void write_small_rgba_png(const char *root, const char *pack,
                                 const char *digest, unsigned char blue) {
    unsigned char raw[8 * (1 + 8 * 4)];
    unsigned char idat[5 + sizeof raw + 6];
    PngBuffer     png;
    char          relative[128];
    size_t        cursor = 0;
    uint32_t      adler;
    int           x;
    int           y;

    for (y = 0; y < 8; y++) {
        raw[cursor++] = 0; /* filter: none */
        for (x = 0; x < 8; x++) {
            raw[cursor++] = (unsigned char)(x * 16);
            raw[cursor++] = (unsigned char)(y * 16);
            raw[cursor++] = blue;
            raw[cursor++] = 0xFFu;
        }
    }

    cursor = 0;
    idat[cursor++] = 0x78u;  /* zlib: deflate, 32 KiB window */
    idat[cursor++] = 0x01u;  /* zlib: no preset dictionary, check bits */
    idat[cursor++] = 0x01u;  /* final block, stored */
    idat[cursor++] = (unsigned char)(sizeof raw & 0xFFu);
    idat[cursor++] = (unsigned char)((sizeof raw >> 8) & 0xFFu);
    idat[cursor++] = (unsigned char)(~(sizeof raw) & 0xFFu);
    idat[cursor++] = (unsigned char)((~(sizeof raw) >> 8) & 0xFFu);
    memcpy(idat + cursor, raw, sizeof raw);
    cursor += sizeof raw;
    adler = adler32_of(raw, sizeof raw);
    idat[cursor++] = (unsigned char)(adler >> 24);
    idat[cursor++] = (unsigned char)(adler >> 16);
    idat[cursor++] = (unsigned char)(adler >> 8);
    idat[cursor++] = (unsigned char)adler;

    png_begin(&png);
    png_put_ihdr(&png, 8, 8, 6);
    png_put_chunk(&png, "IDAT", idat, cursor);
    png_put_chunk(&png, "IEND", NULL, 0);

    snprintf(relative, sizeof relative, "textures/%s.png", digest);
    write_pack_bytes(root, pack, relative, png.bytes, png.length);
}

/* ------------------------------------------------------------------ cases */

/* 1. The defect this file exists for. A header declaring more pixels than the
 * cache can ever hold must be refused with the file still on the heap as a few
 * hundred bytes -- not decoded and then regretted. */
static void test_declared_size_is_refused_before_the_decode(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_GIANT, &texture);

    expect(found == 0, "a pack texture whose header declares more than the cache "
                       "holds is refused");
    expect(mdkr_texture_probe_decode_calls() == 0,
           "and the decoder is never entered for it");
    expect(mdkr_texture_probe_largest_request() < HEADER_PARSE_CEILING,
           "and nothing the size of the picture is ever allocated");
    expect(texture.rgba == NULL && texture.width == 0 && texture.height == 0,
           "and the caller is handed nothing to read");
}

/* 1b. Under the byte ceiling, over the per-side one.
 *
 * The byte cap admits ~11585 per side, and no shipping backend uploads a side
 * over 4096 -- gfx_opengl_upload_texture refuses outright. A failed bind SKIPS
 * the draw rather than falling back to the ROM texture, so an admitted
 * oversized replacement made its object invisible for the whole session while
 * its pixels held cache the store believed were in use, and the only clue was a
 * generic bind-failed line naming neither the pack nor the digest. The importer
 * capped here already; a pack built by any other tool did not. */
static void test_a_side_no_backend_can_upload_is_refused(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_WIDE, &texture);

    expect(found == 0, "a texture wider than any backend uploads is refused "
                       "even though it fits the cache");
    expect(mdkr_texture_probe_decode_calls() == 0,
           "and it is refused from the header, before the decode");
    expect(texture.rgba == NULL && texture.width == 0 && texture.height == 0,
           "and the caller is handed nothing to read");
}

/* 2. The gate must not cost a legitimate pack its textures. */
static void test_a_texture_that_fits_still_loads(void) {
    MdkrModTexture texture;
    MdkrModTextureMemoryStats before, loaded, cached;
    int            found;

    mdkr_mod_texture_memory_stats(&before);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_SOUND, &texture);

    expect(found == 1, "a pack texture inside the cap still loads");
    expect(texture.width == 8 && texture.height == 8,
           "at the size its header declares");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "having actually been decoded, once");
    mdkr_mod_texture_memory_stats(&loaded);
    expect(loaded.pixel_bytes == before.pixel_bytes + 8u * 8u * 4u,
           "resident census counts the admitted RGBA bytes exactly once");
    expect(loaded.table_bytes > 0 && loaded.entries > 0,
           "resident census includes its lookup table and entries");
    expect(mdkr_mod_texture_lookup(DIGEST_SOUND, &texture) == 1,
           "a repeated lookup remains a cache hit");
    mdkr_mod_texture_memory_stats(&cached);
    expect(cached.pixel_bytes == loaded.pixel_bytes &&
           cached.table_bytes == loaded.table_bytes && cached.entries == loaded.entries &&
           cached.pixel_limit == loaded.pixel_limit,
           "a cache hit does not inflate retained ownership counters");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "a cache hit does not decode again");
    if (found && texture.rgba != NULL) {
        /* Pixel (1, 0) of the fixture, so a lookup that found some other
         * picture fails here rather than passing on the dimensions alone. */
        expect(texture.rgba[4] == 16 && texture.rgba[5] == 0 &&
               texture.rgba[6] == 0x40u && texture.rgba[7] == 0xFFu,
               "and the pixels handed back are the pack's own");
    } else {
        expect(0, "and the pixels handed back are the pack's own");
    }
}

/* 3. No bounded header means no pixel-decode admission. The header inspector
 * already supplies the decoder library's own error message. */
static void test_an_unreadable_header_is_refused_before_decode(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_NOTPNG, &texture);

    expect(found == 0, "a pack texture that is not a PNG at all is refused");
    expect(mdkr_texture_probe_decode_calls() == 0,
           "and unreadable dimensions never reach the pixel decoder");
    expect(texture.rgba == NULL && texture.width == 0 && texture.height == 0,
           "and rejected header admission leaves no readable pixels");
    found = mdkr_mod_texture_lookup(DIGEST_NOTPNG, &texture);
    expect(found == 0 && mdkr_texture_probe_decode_calls() == 0,
           "and a repeated lookup keeps the rejection without decoding");
}

/* 4. The decoder's successful result must agree with the admitted dimensions.
 * Reuse the ordinary 8x8 fixture; only the wrapper's reported metadata changes,
 * never the PNG bytes or the decoder implementation. */
static void test_decode_matches_admitted_dimensions(MdkrModRegistry *registry) {
    const int dimensions[][2] = {{7, 8}, {8, 7}};
    size_t index;
    for (index = 0; index < sizeof dimensions / sizeof dimensions[0]; ++index) {
        MdkrModTexture texture;
        int found;
        mdkr_mod_texture_store_init(registry);
        mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
        mdkr_texture_probe_override_decoded_dimensions(
            dimensions[index][0], dimensions[index][1]);
        found = mdkr_mod_texture_lookup(DIGEST_SOUND, &texture);
        expect(mdkr_texture_probe_decode_calls() == 1,
               "dimension agreement is checked after a real ordinary decode");
        expect(found == 0 && texture.rgba == NULL &&
               texture.width == 0 && texture.height == 0,
               "a decoded dimension mismatch cannot publish a texture");
    }
    mdkr_mod_texture_store_init(registry);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    test_a_texture_that_fits_still_loads();
}

static void test_generated_portrait_alias(const char *mods,
                                          MdkrModRegistry *registry) {
    MdkrModTexture texture;
    int found = mdkr_mod_texture_lookup(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(found && texture.width == 8 && texture.height == 8 &&
           texture.rgba[2] == 0x40u,
           "a pre-1.7 Taj pack still replaces the recoloured card");

    mdkr_mod_texture_set_enabled(false);
    found = mdkr_mod_texture_lookup(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(!found && texture.rgba == NULL,
           "turning packs off also disables generated-portrait aliases");
    mdkr_mod_texture_set_enabled(true);

    found = mdkr_mod_texture_lookup("dcd45f4f32c9e1da4abeb3c1c1f8011a", &texture);
    expect(!found, "a different digest cannot inherit Taj's legacy override");

    write_small_rgba_png(mods, "probe", MDKR_MOD_TAJ_PORTRAIT_DIGEST, 0xE0u);
    mdkr_mod_texture_store_init(registry);
    found = mdkr_mod_texture_lookup(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(found && texture.rgba[2] == 0xE0u,
           "a current-name replacement takes precedence over the legacy name");
    found = mdkr_mod_texture_lookup(MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST, &texture);
    expect(found && texture.rgba[2] == 0x40u,
           "direct legacy lookup still returns the legacy file");
}

/* ------------------------------------------------ resolve timing (#66) */

/* A clock that advances a fixed step per call, so every timed phase comes out
 * as a whole number of steps and a phase that was never timed comes out zero
 * rather than "small". */
#define FAKE_CLOCK_STEP 1000u
static uint64_t fake_clock_ns;
static unsigned fake_clock_calls;

static uint64_t fake_clock(void) {
    fake_clock_calls++;
    fake_clock_ns += FAKE_CLOCK_STEP;
    return fake_clock_ns;
}

static int timing_is_zero(const MdkrModTextureResolveTiming *t) {
    return t->resolves == 0 && t->loaded == 0 && t->probes == 0 &&
           t->absent == 0 && t->files == 0 && t->file_bytes == 0 &&
           t->decoded_bytes == 0 && t->open_ns == 0 && t->absent_ns == 0 &&
           t->read_ns == 0 && t->decode_ns == 0 && t->resolve_ns == 0;
}

static int timing_equal(const MdkrModTextureResolveTiming *a,
                        const MdkrModTextureResolveTiming *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

/* The issue #66 instrument: what a resolution cost, split into opening the
 * pack to find the file, reading the file, and decoding it. Off must mean
 * off -- every earlier case in this file resolved textures with no clock
 * installed, and none of that may have been counted -- and on must attribute
 * each phase, count the bytes it actually read and decoded, and leave a cache
 * hit uncounted. */
static void test_resolve_timing(const char *mods) {
    MdkrModTexture              texture;
    MdkrModTextureResolveTiming before, after;
    char                        path[1024];
    struct stat                 info;
    long                        png_size = -1;

    mdkr_mod_texture_timing_totals(NULL);
    mdkr_mod_texture_timing_totals(&before);
    expect(timing_is_zero(&before),
           "resolutions made with no clock installed are not counted");

    /* Installed, then removed again: the store must stop asking at once. */
    mdkr_mod_texture_timing_set_clock(fake_clock);
    mdkr_mod_texture_timing_set_clock(NULL);
    expect(mdkr_mod_texture_lookup(DIGEST_UNTIMED, &texture) == 1,
           "an untimed fresh texture still resolves");
    mdkr_mod_texture_timing_totals(&after);
    expect(fake_clock_calls == 0 && timing_is_zero(&after),
           "and with the clock removed it reads no clock and counts nothing");

    path3(path, sizeof path, mods, "probe", "textures/" DIGEST_TIMED ".png");
    if (stat(path, &info) == 0) png_size = (long)info.st_size;

    mdkr_mod_texture_timing_set_clock(fake_clock);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    mdkr_mod_texture_timing_totals(&before);
    expect(mdkr_mod_texture_lookup(DIGEST_TIMED, &texture) == 1,
           "a timed fresh texture resolves");
    mdkr_mod_texture_timing_totals(&after);
    expect(after.resolves == before.resolves + 1 &&
           after.loaded == before.loaded + 1 &&
           after.probes == before.probes + 1 &&
           after.absent == before.absent &&
           after.files == before.files + 1,
           "one resolution, one pack probe, one resident result, one file");
    expect(after.absent_ns == before.absent_ns,
           "and a probe that found its file is not charged as absent");
    expect(png_size > 0 &&
           after.file_bytes == before.file_bytes + (uint64_t)png_size,
           "the bytes read are the PNG file's own size");
    expect(after.decoded_bytes == before.decoded_bytes + 8u * 8u * 4u,
           "the bytes decoded are the RGBA8 picture's size");
    expect(after.open_ns - before.open_ns >= FAKE_CLOCK_STEP &&
           after.read_ns - before.read_ns >= FAKE_CLOCK_STEP &&
           after.decode_ns - before.decode_ns >= FAKE_CLOCK_STEP,
           "opening, reading and decoding are each timed");
    expect(after.resolve_ns - before.resolve_ns >=
               (after.open_ns - before.open_ns) +
               (after.read_ns - before.read_ns) +
               (after.decode_ns - before.decode_ns),
           "the whole resolution contains all three of its phases");

    before = after;
    expect(mdkr_mod_texture_lookup(DIGEST_TIMED, &texture) == 1,
           "the timed texture is now a cache hit");
    mdkr_mod_texture_timing_totals(&after);
    expect(timing_equal(&before, &after), "and a cache hit costs nothing");

    expect(mdkr_mod_texture_lookup(DIGEST_ABSENT, &texture) == 0,
           "a digest no pack supplies is absent");
    mdkr_mod_texture_timing_totals(&after);
    expect(after.resolves == before.resolves + 1 &&
           after.probes == before.probes + 1 &&
           after.absent == before.absent + 1 &&
           after.absent_ns - before.absent_ns ==
               after.open_ns - before.open_ns &&
           after.loaded == before.loaded && after.files == before.files &&
           after.decoded_bytes == before.decoded_bytes &&
           after.read_ns == before.read_ns &&
           after.decode_ns == before.decode_ns &&
           after.open_ns - before.open_ns >= FAKE_CLOCK_STEP,
           "an absent answer is a timed probe that reads and decodes nothing");

    mdkr_mod_texture_timing_set_clock(NULL);
}

/* ------------------------------------------------------- the author dump */

/* Reads a whole scratch file into `out`, returning its length, or -1 when the
 * file does not exist. "Does not exist" is a result the cases below assert,
 * not a fatal condition. */
static long read_scratch_file(const char *path, char *out, size_t size) {
    FILE  *file = fopen(path, "rb");
    size_t length;

    if (file == NULL) return -1;
    length = fread(out, 1, size - 1, file);
    fclose(file);
    out[length] = '\0';
    return (long)length;
}

static int record_has(const char *record, const char *line) {
    return strstr(record, line) != NULL;
}

/* 5. The dump record carries the raw source span and its addressing.
 *
 * The reason this is worth a case of its own: the sidecar is no longer read
 * only by a human looking for a filename. An offline importer parses it and
 * hashes the `.texels` bytes with somebody else's algorithm, so a field that
 * is absent, misnamed or -- worst -- silently truncated produces a confident
 * wrong answer rather than a visible failure. Nothing about the decoded PNG
 * would show any of that.
 *
 * The span here is deliberately PADDED: line_bytes (12) is larger than a
 * four-texel 16-bit row (8), so the four bytes between rows are addressing and
 * not picture. A dump that wrote a tightly packed span, or that reported the
 * packed pitch, would pass a test built on an unpadded fixture and mis-hash
 * every real padded tile. */
static void test_dump_record_carries_the_source_span(const char *scratch) {
    /* Six texels of RGBA8 for the picture, and a 24-byte source span holding
     * two 8-byte rows twelve bytes apart. Neither is derived from the other:
     * the point of the record is that they are different pictures of the same
     * texture. */
    static const unsigned char rgba[4 * 2 * 4] = {
        0x10, 0x11, 0x12, 0xff, 0x20, 0x21, 0x22, 0xff,
        0x30, 0x31, 0x32, 0xff, 0x40, 0x41, 0x42, 0xff,
        0x50, 0x51, 0x52, 0xff, 0x60, 0x61, 0x62, 0xff,
        0x70, 0x71, 0x72, 0xff, 0x80, 0x81, 0x82, 0xff,
    };
    static const unsigned char span[24] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, /* row 0 picture */
        0xde, 0xad, 0xbe, 0xef,                         /* row 0 padding  */
        0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, /* row 1 picture */
        0xde, 0xad, 0xbe, 0xef,                         /* row 1 padding  */
    };
    const char *with_span = "d4444444444444444444444444444444";
    const char *no_span   = "e5555555555555555555555555555555";
    MdkrModTextureSource source;
    char dump_dir[1024];
    char path[1024];
    char record[1024];
    char bytes[128];
    long length;

    path2(dump_dir, sizeof dump_dir, scratch, "dump");
    if (TEST_MKDIR(dump_dir) != 0 && !is_directory(dump_dir)) {
        fatal("cannot create the dump scratch directory", dump_dir);
    }
    /* The store resolves MDKR_MOD_TEXTURE_DUMP once and caches it, so this
     * must land before the first call into the dump path. It does: nothing
     * above this line touches it, and main() runs this case last. */
    TEST_SETENV("MDKR_MOD_TEXTURE_DUMP", dump_dir);
    expect(mdkr_mod_texture_dump_active(),
           "a dump directory in the environment arms the author dump");

    source.texels = span;
    source.texel_bytes = sizeof span;
    source.line_bytes = 12u;
    source.size_bytes = 24u;
    source.width = 4;
    source.height = 2;
    mdkr_mod_texture_dump_observe(with_span, rgba, 4, 2, 0u, 2u, &source,
                                  "frame 7, texture unit 0");

    path3(path, sizeof path, scratch, "dump", "d4444444444444444444444444444444.txt");
    length = read_scratch_file(path, record, sizeof record);
    expect(length > 0, "an observed texture writes its record");
    expect(record_has(record, "dump_format=2\n"),
           "the record declares the widened dump format");
    expect(record_has(record, "source_line_bytes=12\n") &&
           record_has(record, "source_size_bytes=24\n"),
           "the record carries the source row pitch and declared span length");
    expect(record_has(record, "source_width=4\n") &&
           record_has(record, "source_height=2\n"),
           "the record carries the SOURCE tile's geometry, not the buffer's");
    expect(record_has(record, "source_texel_bytes=24\n"),
           "the record says how many bytes of span actually reached disk");
    expect(record_has(record, "width=4\n") && record_has(record, "height=2\n") &&
           record_has(record, "fmt=0\n") && record_has(record, "siz=2\n") &&
           record_has(record, "first_seen=frame 7, texture unit 0\n"),
           "and every field the pre-widening record carried is still there");

    path3(path, sizeof path, scratch, "dump",
          "d4444444444444444444444444444444.texels");
    length = read_scratch_file(path, bytes, sizeof bytes);
    expect(length == (long)sizeof span &&
           memcmp(bytes, span, sizeof span) == 0,
           "the raw source span is written beside it, byte for byte, padding "
           "included");

    /* Once per process, for the span as much as for the PNG: the second
     * observation carries different bytes, so a re-write would be visible. */
    source.texels = rgba;
    source.texel_bytes = 16u;
    mdkr_mod_texture_dump_observe(with_span, rgba, 4, 2, 0u, 2u, &source,
                                  "frame 900, texture unit 1");
    length = read_scratch_file(path, bytes, sizeof bytes);
    expect(length == (long)sizeof span &&
           memcmp(bytes, span, sizeof span) == 0,
           "a repeat observation of the same digest rewrites neither file");

    /* A caller with no span writes a record and no companion, rather than an
     * empty companion a reader would try to hash. */
    mdkr_mod_texture_dump_observe(no_span, rgba, 4, 2, 0u, 2u, NULL,
                                  "frame 12, texture unit 0");
    path3(path, sizeof path, scratch, "dump",
          "e5555555555555555555555555555555.texels");
    expect(read_scratch_file(path, bytes, sizeof bytes) == -1,
           "a texture observed without a source span writes no .texels file");
    path3(path, sizeof path, scratch, "dump",
          "e5555555555555555555555555555555.txt");
    length = read_scratch_file(path, record, sizeof record);
    expect(length > 0 && record_has(record, "width=4\n") &&
           !record_has(record, "source_line_bytes="),
           "and its record omits the source fields instead of inventing them");
}

int main(int argc, char **argv) {
    const char     *scratch = argc > 1 ? argv[1] : "mod_texture_store_scratch";
    const char     *notpng = "this is not a PNG, it is a sentence";
    char            mods[1024];
    MdkrModRegistry registry;

    if (strlen(DIGEST_GIANT) != 32 || strlen(DIGEST_SOUND) != 32 ||
        strlen(DIGEST_NOTPNG) != 32 || strlen(DIGEST_UNTIMED) != 32 ||
        strlen(DIGEST_TIMED) != 32 || strlen(DIGEST_ABSENT) != 32) {
        fatal("a fixture digest is not 32 characters", "the store would refuse "
              "it before reaching a pack");
    }

    remove_tree(scratch);
    path2(mods, sizeof mods, scratch, "mods");

    write_pack_text(mods, "probe", "pack.ini", "[pack]\nname=Probe\n");
    write_giant_declared_png(mods, "probe", DIGEST_GIANT, 32768u, 32768u);
    /* Under the cache-bytes ceiling (8192*2048*4 = 64 MiB of 512) and over
     * the per-side one. This is the shape the byte cap alone admitted. */
    write_giant_declared_png(mods, "probe", DIGEST_WIDE, 8192u, 2048u);
    write_small_rgba_png(mods, "probe", DIGEST_SOUND, 0x40u);
    write_small_rgba_png(mods, "probe", MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST, 0x40u);
    write_small_rgba_png(mods, "probe", DIGEST_UNTIMED, 0x50u);
    write_small_rgba_png(mods, "probe", DIGEST_TIMED, 0x60u);
    write_pack_bytes(mods, "probe", "textures/" DIGEST_NOTPNG ".png",
                     (const unsigned char *)notpng, strlen(notpng));

    if (mdkr_mod_registry_init(&registry, mods) != 0) {
        fatal("the scratch pack tree did not initialise", mods);
    }
    mdkr_mod_texture_store_init(&registry);
    if (!mdkr_mod_texture_store_active()) {
        fatal("the store is inactive over the scratch pack", mods);
    }
    {
        MdkrModTextureMemoryStats stats;
        mdkr_mod_texture_memory_stats(NULL);
        mdkr_mod_texture_memory_stats(&stats);
        expect(stats.pixel_bytes == 0 && stats.table_bytes == 0 && stats.entries == 0,
               "new store census has no retained allocations");
        expect(stats.pixel_limit > 0, "store census reports a nonzero pixel budget");
    }

    test_declared_size_is_refused_before_the_decode();
    test_a_side_no_backend_can_upload_is_refused();
    test_a_texture_that_fits_still_loads();
    test_an_unreadable_header_is_refused_before_decode();
    test_decode_matches_admitted_dimensions(&registry);
    test_generated_portrait_alias(mods, &registry);
    test_resolve_timing(mods);
    test_dump_record_carries_the_source_span(scratch);

    mdkr_mod_texture_store_shutdown();
    {
        MdkrModTextureMemoryStats stats;
        mdkr_mod_texture_memory_stats(&stats);
        expect(stats.pixel_bytes == 0 && stats.table_bytes == 0 && stats.entries == 0,
               "shutdown clears retained memory census");
    }
    mdkr_mod_registry_shutdown(&registry);
    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n"
                    : "all pack-texture store assertions passed\n", failures);
    return failures ? 1 : 0;
}
