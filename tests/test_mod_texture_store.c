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
#else
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0777)
#define TEST_RMDIR(path) rmdir(path)
#endif

/* The digests the cases address. Only the length is load-bearing (the store
 * refuses anything that is not exactly its digest length), and main() checks
 * it rather than trusting a hand count. */
#define DIGEST_GIANT "a1111111111111111111111111111111"
#define DIGEST_SOUND "b2222222222222222222222222222222"
#define DIGEST_NOTPNG "c3333333333333333333333333333333"

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
                                 const char *digest) {
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
            raw[cursor++] = 0x40u;
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

/* 2. The gate must not cost a legitimate pack its textures. */
static void test_a_texture_that_fits_still_loads(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_SOUND, &texture);

    expect(found == 1, "a pack texture inside the cap still loads");
    expect(texture.width == 8 && texture.height == 8,
           "at the size its header declares");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "having actually been decoded, once");
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

/* 3. A header the size check cannot read is not a header the size check gets
 * to reject. Reading the declared size first must not become a second, blinder
 * rejection path: a file stbi_info() cannot parse still goes to the decoder,
 * which is what puts the decoder's own account of the defect in the log. */
static void test_an_unreadable_header_still_reaches_the_decoder(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = mdkr_mod_texture_lookup(DIGEST_NOTPNG, &texture);

    expect(found == 0, "a pack texture that is not a PNG at all is refused");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "by the decoder, which was still given the chance to name the defect");
}

int main(int argc, char **argv) {
    const char     *scratch = argc > 1 ? argv[1] : "mod_texture_store_scratch";
    const char     *notpng = "this is not a PNG, it is a sentence";
    char            mods[1024];
    MdkrModRegistry registry;

    if (strlen(DIGEST_GIANT) != 32 || strlen(DIGEST_SOUND) != 32 ||
        strlen(DIGEST_NOTPNG) != 32) {
        fatal("a fixture digest is not 32 characters", "the store would refuse "
              "it before reaching a pack");
    }

    remove_tree(scratch);
    path2(mods, sizeof mods, scratch, "mods");

    write_pack_text(mods, "probe", "pack.ini", "[pack]\nname=Probe\n");
    write_giant_declared_png(mods, "probe", DIGEST_GIANT, 32768u, 32768u);
    write_small_rgba_png(mods, "probe", DIGEST_SOUND);
    write_pack_bytes(mods, "probe", "textures/" DIGEST_NOTPNG ".png",
                     (const unsigned char *)notpng, strlen(notpng));

    if (mdkr_mod_registry_init(&registry, mods) != 0) {
        fatal("the scratch pack tree did not initialise", mods);
    }
    mdkr_mod_texture_store_init(&registry);
    if (!mdkr_mod_texture_store_active()) {
        fatal("the store is inactive over the scratch pack", mods);
    }

    test_declared_size_is_refused_before_the_decode();
    test_a_texture_that_fits_still_loads();
    test_an_unreadable_header_still_reaches_the_decoder();

    mdkr_mod_texture_store_shutdown();
    mdkr_mod_registry_shutdown(&registry);
    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n"
                    : "all pack-texture store assertions passed\n", failures);
    return failures ? 1 : 0;
}
