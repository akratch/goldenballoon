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
 * The issue #66 cases are about WHERE the work happens. A lookup must answer
 * from the registry's name index without touching a pack, a texture a pack
 * does supply must come back "pending" and be read and decoded off the frame,
 * and the hand-off between the owner thread and the worker must survive every
 * way the store can be torn down or retargeted while a request is in flight.
 * Those cases park the real worker thread at a test seam rather than racing
 * it, and count pack I/O through mod_source's test counters.
 *
 * Every byte written lands beneath the scratch root given by argv[1], which is
 * removed before the first case and after the last.
 */
#include "mod_texture_store.h"

#include "mod_registry.h"
#include "mod_source.h"
#include "mod_texture_store_probe.h"
#include "miniz.h"

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
#define DIGEST_ZIPPED  "a3333333333333333333333333333333"
#define DIGEST_IO      "a5555555555555555555555555555555"
#define DIGEST_BUDGET1 "b1111111111111111111111111111111"
#define DIGEST_BUDGET2 "b3333333333333333333333333333333"
#define DIGEST_FALLBACK "b4444444444444444444444444444444"
#define DIGEST_STALE1  "c1111111111111111111111111111111"
#define DIGEST_STALE2  "c2222222222222222222222222222222"
#define DIGEST_FLIGHT0 "c4444444444444444444444444444444"
#define DIGEST_FLIGHT1 "c5555555555555555555555555555555"
#define DIGEST_FLIGHT2 "c6666666666666666666666666666666"
#define DIGEST_FLIGHT3 "c7777777777777777777777777777777"
#define DIGEST_WTIMED  "f9999999999999999999999999999999"
#define DIGEST_BOMB    "d1111111111111111111111111111111"
/* Asked for in lowercase, as the renderer always does; stored by their packs
 * as Textures/<UPPER>.PNG (folder) and TEXTURES/<UPPER>.Png (zip). */
#define DIGEST_CASE_FOLDER "ab00000000000000000000000000cdef"
#define DIGEST_CASE_ZIP    "cd00000000000000000000000000abef"
/* Eviction fixtures are generated: "9" + 31 digits of the index. */
#define EVICT_FIXTURES 12

/* Rice identities, written as a manifest-less pack the scan recognises by its
 * file names. MERGED has an _rgb and an _a half of the same size; MISMATCHED
 * has an _a half of a different size, which the format reads as opaque. */
#define RICE_MERGED_CRC     0x1234abcdu
#define RICE_MISMATCHED_CRC 0x5678ef01u
#define RICE_UNKNOWN_CRC    0x0badf00du

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

/* An 8x8 RGBA header whose IDAT is a real zlib stream inflating to 64 MiB of
 * zeros: the header admits it, so only a bound on the decoder's own
 * allocations keeps the reject from costing that memory first. Deflated by
 * miniz in pieces, so the fixture itself never holds the expanded bytes. */
static void write_inflation_bomb_png(const char *root, const char *pack,
                                     const char *digest) {
    static unsigned char zeros[1u << 20];
    unsigned char *covered, *file;
    const size_t capacity = 1u << 20;
    size_t idat_length, cursor;
    uint32_t crc;
    mz_stream stream;
    PngBuffer header;
    char relative[128];
    int piece;

    /* "IDAT" followed by the stream, so the CRC covers both in place. */
    covered = (unsigned char *)malloc(4u + capacity);
    if (covered == NULL) fatal("bomb fixture allocation", digest);
    memcpy(covered, "IDAT", 4);
    memset(&stream, 0, sizeof stream);
    if (mz_deflateInit(&stream, MZ_BEST_COMPRESSION) != MZ_OK) fatal("deflate init", digest);
    stream.next_out = covered + 4;
    stream.avail_out = (unsigned int)capacity;
    for (piece = 0; piece < 64; ++piece) {
        stream.next_in = zeros;
        stream.avail_in = sizeof zeros;
        while (stream.avail_in != 0) {
            if (mz_deflate(&stream, MZ_NO_FLUSH) != MZ_OK || stream.avail_out == 0) {
                fatal("deflate bomb fixture", digest);
            }
        }
    }
    if (mz_deflate(&stream, MZ_FINISH) != MZ_STREAM_END) fatal("deflate finish", digest);
    idat_length = (size_t)stream.total_out;
    mz_deflateEnd(&stream);

    /* Signature and IHDR through the ordinary helpers, then the large IDAT
     * and IEND appended by hand: the helpers' buffers are sized for the
     * few-hundred-byte fixtures. */
    png_begin(&header);
    png_put_ihdr(&header, 8u, 8u, 6);
    file = (unsigned char *)malloc(header.length + 12u + idat_length + 12u);
    if (file == NULL) fatal("bomb fixture allocation", digest);
    memcpy(file, header.bytes, header.length);
    cursor = header.length;
    file[cursor++] = (unsigned char)(idat_length >> 24);
    file[cursor++] = (unsigned char)(idat_length >> 16);
    file[cursor++] = (unsigned char)(idat_length >> 8);
    file[cursor++] = (unsigned char)idat_length;
    memcpy(file + cursor, covered, 4u + idat_length);
    cursor += 4u + idat_length;
    crc = crc32_of(covered, 4u + idat_length);
    file[cursor++] = (unsigned char)(crc >> 24);
    file[cursor++] = (unsigned char)(crc >> 16);
    file[cursor++] = (unsigned char)(crc >> 8);
    file[cursor++] = (unsigned char)crc;
    memcpy(file + cursor, "\0\0\0\0IEND", 8u);
    cursor += 8u;
    crc = crc32_of((const unsigned char *)"IEND", 4u);
    file[cursor++] = (unsigned char)(crc >> 24);
    file[cursor++] = (unsigned char)(crc >> 16);
    file[cursor++] = (unsigned char)(crc >> 8);
    file[cursor++] = (unsigned char)crc;
    snprintf(relative, sizeof relative, "textures/%s.png", digest);
    write_pack_bytes(root, pack, relative, file, cursor);
    free(file);
    free(covered);
}

/* Pixel generators for the fixtures below. */
typedef void (*PixelFn)(int x, int y, unsigned char out[4], unsigned char param);

/* (x * 16, y * 16, param, 0xFF): every pixel distinct, so a lookup that
 * returned some other picture fails on the pixels, not just the size. */
static void pixel_gradient(int x, int y, unsigned char out[4],
                           unsigned char param) {
    out[0] = (unsigned char)(x * 16);
    out[1] = (unsigned char)(y * 16);
    out[2] = param;
    out[3] = 0xFFu;
}

/* A Rice `_a` half: the alpha lives in RED. */
static unsigned char alpha_key(int x, int y) {
    return (unsigned char)(x * 32 + y * 4 + 1);
}

static void pixel_alpha_half(int x, int y, unsigned char out[4],
                             unsigned char param) {
    out[0] = alpha_key(x, y);
    out[1] = param;
    out[2] = param;
    out[3] = 0xFFu;
}

/* A real, decodable `side` x `side` RGBA PNG (side <= 8). Its image data is a
 * single stored (BTYPE=0) deflate block, which is a legal zlib stream and
 * keeps the whole fixture inspectable: no compressor is involved in a test
 * about what a decoder is allowed to be asked for. */
static void build_rgba_png(PngBuffer *png, int side, PixelFn fn,
                           unsigned char param) {
    unsigned char raw[8 * (1 + 8 * 4)];
    unsigned char idat[5 + sizeof raw + 6];
    size_t        raw_length = (size_t)side * (1u + (size_t)side * 4u);
    size_t        cursor = 0;
    uint32_t      adler;
    int           x;
    int           y;

    if (side < 1 || side > 8) fatal("fixture PNG side out of range", "build");
    for (y = 0; y < side; y++) {
        raw[cursor++] = 0; /* filter: none */
        for (x = 0; x < side; x++) {
            fn(x, y, raw + cursor, param);
            cursor += 4;
        }
    }

    cursor = 0;
    idat[cursor++] = 0x78u;  /* zlib: deflate, 32 KiB window */
    idat[cursor++] = 0x01u;  /* zlib: no preset dictionary, check bits */
    idat[cursor++] = 0x01u;  /* final block, stored */
    idat[cursor++] = (unsigned char)(raw_length & 0xFFu);
    idat[cursor++] = (unsigned char)((raw_length >> 8) & 0xFFu);
    idat[cursor++] = (unsigned char)(~raw_length & 0xFFu);
    idat[cursor++] = (unsigned char)((~raw_length >> 8) & 0xFFu);
    memcpy(idat + cursor, raw, raw_length);
    cursor += raw_length;
    adler = adler32_of(raw, raw_length);
    idat[cursor++] = (unsigned char)(adler >> 24);
    idat[cursor++] = (unsigned char)(adler >> 16);
    idat[cursor++] = (unsigned char)(adler >> 8);
    idat[cursor++] = (unsigned char)adler;

    png_begin(png);
    png_put_ihdr(png, (uint32_t)side, (uint32_t)side, 6);
    png_put_chunk(png, "IDAT", idat, cursor);
    png_put_chunk(png, "IEND", NULL, 0);
}

static void write_png_file(const char *root, const char *pack,
                           const char *relative, int side, PixelFn fn,
                           unsigned char param) {
    PngBuffer png;
    build_rgba_png(&png, side, fn, param);
    write_pack_bytes(root, pack, relative, png.bytes, png.length);
}

/* The 8x8 gradient, at textures/<digest>.png: what a digest-keyed pack
 * supplies. */
static void write_small_rgba_png(const char *root, const char *pack,
                                 const char *digest, unsigned char blue) {
    char relative[128];
    snprintf(relative, sizeof relative, "textures/%s.png", digest);
    write_png_file(root, pack, relative, 8, pixel_gradient, blue);
}

/* A zipped pack: pack.ini plus one 8x8 gradient at `relative`, deflated by
 * miniz so the worker's own archive handle has something real to inflate. */
static void write_zip_png_pack(const char *root, const char *name,
                               const char *manifest, const char *relative,
                               unsigned char blue) {
    char           full[1024];
    PngBuffer      png;
    mz_zip_archive zip;

    path2(full, sizeof full, root, name);
    make_parent_directories(full);
    build_rgba_png(&png, 8, pixel_gradient, blue);
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_file(&zip, full, 0) ||
        !mz_zip_writer_add_mem(&zip, "pack.ini", manifest, strlen(manifest),
                               MZ_DEFAULT_COMPRESSION) ||
        !mz_zip_writer_add_mem(&zip, relative, png.bytes, png.length,
                               MZ_DEFAULT_COMPRESSION) ||
        !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        fatal("cannot write the scratch archive", full);
    }
    mz_zip_writer_end(&zip);
}

static void write_zip_digest_pack(const char *root, const char *name,
                                  const char *digest, unsigned char blue) {
    char relative[128];
    snprintf(relative, sizeof relative, "textures/%s.png", digest);
    write_zip_png_pack(root, name, "[pack]\nname=Zipped\npriority=200\n",
                       relative, blue);
}

/* `textures/<digest>.png` as a pack author might spell it: the directory,
 * every hex digit and the extension in other cases. */
static void spell_upper(char *out, size_t size, const char *directory,
                        const char *digest, const char *extension) {
    char   upper[33];
    size_t index;
    for (index = 0; index < 32 && digest[index] != '\0'; index++) {
        char c = digest[index];
        upper[index] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
    upper[index] = '\0';
    snprintf(out, size, "%s/%s%s", directory, upper, extension);
}

static void rice_name(char *out, size_t size, uint32_t crc,
                      const char *variant) {
    snprintf(out, size, "DIDDY KONG RACING/DIDDY KONG RACING#%08x#0#2_%s.png",
             (unsigned)crc, variant);
}

static void eviction_digest(char out[33], int index) {
    snprintf(out, 33, "9%031d", index);
}

/* ------------------------------------------------------------- lookups */

/* A texture a pack supplies now answers "pending" first and is read and
 * decoded off the frame. The admission cases below are about what happens to
 * the FILE, wherever that runs, so they ask, let the store settle, and ask
 * again -- exactly what the renderer does across frames. */
static int lookup_settled(const char *digest, MdkrModTexture *texture) {
    int found = mdkr_mod_texture_lookup(digest, texture);
    if (!found && texture->pending) {
        mdkr_mod_texture_store_settle();
        found = mdkr_mod_texture_lookup(digest, texture);
    }
    return found;
}

static int lookup_rice_settled(uint32_t crc, MdkrModTexture *texture) {
    int found = mdkr_mod_texture_lookup_rice(crc, 0, 2, texture);
    if (!found && texture->pending) {
        mdkr_mod_texture_store_settle();
        found = mdkr_mod_texture_lookup_rice(crc, 0, 2, texture);
    }
    return found;
}

static void store_reinit(MdkrModRegistry *registry, MdkrModTextureLoader loader) {
    mdkr_mod_texture_store_set_loader(loader);
    mdkr_mod_texture_store_init(registry);
}

static int pixels_are_gradient(const MdkrModTexture *texture,
                               unsigned char blue) {
    return texture->rgba != NULL && texture->width == 8 &&
           texture->height == 8 && texture->rgba[4] == 16 &&
           texture->rgba[5] == 0 && texture->rgba[6] == blue &&
           texture->rgba[7] == 0xFFu;
}

/* ------------------------------------------------------------------ cases */

/* 1. The defect this file exists for. A header declaring more pixels than the
 * cache can ever hold must be refused with the file still on the heap as a few
 * hundred bytes -- not decoded and then regretted. */
static void test_declared_size_is_refused_before_the_decode(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = lookup_settled(DIGEST_GIANT, &texture);

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
    found = lookup_settled(DIGEST_WIDE, &texture);

    expect(found == 0, "a texture wider than any backend uploads is refused "
                       "even though it fits the cache");
    expect(mdkr_texture_probe_decode_calls() == 0,
           "and it is refused from the header, before the decode");
    expect(texture.rgba == NULL && texture.width == 0 && texture.height == 0,
           "and the caller is handed nothing to read");
}

/* 1c. Admitted by its header, refused by its contents, and bounded on the way.
 * stb's zlib stage grows its buffer by doubling until the stream ends, so
 * before the per-decode bound this 8x8 picture asked for 64 MiB and more. */
static void test_an_inflation_bomb_is_bounded_by_its_header(void) {
    MdkrModTexture texture;
    int            found;

    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = lookup_settled(DIGEST_BOMB, &texture);

    expect(found == 0, "a texture whose IDAT inflates past its header is refused");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "its header admits it, so the decoder is entered");
    expect(mdkr_texture_probe_largest_request() < HEADER_PARSE_CEILING,
           "and no decoder allocation exceeds what an 8x8 header can need");
    expect(texture.rgba == NULL, "and the caller is handed nothing to read");
}

/* 2. The gate must not cost a legitimate pack its textures. */
static void test_a_texture_that_fits_still_loads(void) {
    MdkrModTexture texture;
    MdkrModTextureMemoryStats before, loaded, cached;
    int            found;

    mdkr_mod_texture_memory_stats(&before);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    found = lookup_settled(DIGEST_SOUND, &texture);

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
    found = lookup_settled(DIGEST_NOTPNG, &texture);

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
        found = lookup_settled(DIGEST_SOUND, &texture);
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
    int found = lookup_settled(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(found && texture.width == 8 && texture.height == 8 &&
           texture.rgba[2] == 0x40u,
           "a pre-1.7 Taj pack still replaces the recoloured card");

    mdkr_mod_texture_set_enabled(false);
    found = mdkr_mod_texture_lookup(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(!found && texture.rgba == NULL,
           "turning packs off also disables generated-portrait aliases");
    mdkr_mod_texture_set_enabled(true);

    found = lookup_settled("dcd45f4f32c9e1da4abeb3c1c1f8011a", &texture);
    expect(!found, "a different digest cannot inherit Taj's legacy override");

    /* A file added to a pack is seen at the next scan: the name index is the
     * scan's (issue #66), and a rescan is store shutdown, registry rescan,
     * store init -- the order platform_content_packs_init() uses. */
    write_small_rgba_png(mods, "probe", MDKR_MOD_TAJ_PORTRAIT_DIGEST, 0xE0u);
    mdkr_mod_texture_store_shutdown();
    mdkr_mod_registry_shutdown(registry);
    if (mdkr_mod_registry_init(registry, mods) != 0) {
        fatal("the scratch pack tree did not rescan", mods);
    }
    mdkr_mod_texture_store_init(registry);
    found = lookup_settled(MDKR_MOD_TAJ_PORTRAIT_DIGEST, &texture);
    expect(found && texture.rgba[2] == 0xE0u,
           "a current-name replacement takes precedence over the legacy name");
    found = lookup_settled(MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST, &texture);
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

/* Atomic: once a request carries the clock, the worker reads it too. */
static uint64_t fake_clock(void) {
    __atomic_fetch_add(&fake_clock_calls, 1u, __ATOMIC_RELAXED);
    return __atomic_add_fetch(&fake_clock_ns, FAKE_CLOCK_STEP,
                              __ATOMIC_RELAXED);
}

static int timing_is_zero(const MdkrModTextureResolveTiming *t) {
    return t->resolves == 0 && t->loaded == 0 && t->probes == 0 &&
           t->absent == 0 && t->files == 0 && t->file_bytes == 0 &&
           t->decoded_bytes == 0 && t->open_ns == 0 && t->absent_ns == 0 &&
           t->read_ns == 0 && t->decode_ns == 0 && t->resolve_ns == 0 &&
           t->pump_ns == 0 && t->worker_ns == 0 && t->queued == 0 &&
           t->installed == 0 && t->discarded == 0;
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
static void test_resolve_timing(const char *mods, MdkrModRegistry *registry) {
    MdkrModTexture              texture;
    MdkrModTextureResolveTiming before, after;
    char                        path[1024];
    struct stat                 info;
    long                        png_size = -1;

    /* The INLINE loader, where a resolution still nests all three phases
     * inside the lookup; the off-frame accounting has its own case below. */
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_INLINE);
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
           "locating, reading and decoding are each timed");
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

/* ------------------------------------------------ off the frame (#66) */

/* Every number this file compares is one of these, so the cases below can
 * read "what did that step cost" as a difference. */
static MdkrModTextureMemoryStats memory_now(void) {
    MdkrModTextureMemoryStats stats;
    mdkr_mod_texture_memory_stats(&stats);
    return stats;
}

static MdkrModTextureResolveTiming timing_now_totals(void) {
    MdkrModTextureResolveTiming timing;
    mdkr_mod_texture_timing_totals(&timing);
    return timing;
}

/* 6. The Rice convention's split halves, merged off the frame.
 *
 * `_rgb` carries colour, `_a` carries alpha in its RED channel, and the store
 * merges them -- now on the worker, through its own pack handle. An `_a` of a
 * different size is not alpha for this picture: the result is opaque and the
 * mismatch is reported. A CRC the pack does not index is answered from the
 * index alone. */
static void test_rice_split_alpha_merge(void) {
    MdkrModTexture texture;
    const int      used_before = mdkr_mod_texture_rice_resident();
    int            found;

    found = mdkr_mod_texture_lookup_rice(RICE_MERGED_CRC, 0, 2, &texture);
    expect(found == 0 && texture.pending == 1,
           "a Rice texture the pack supplies is pending at first ask");
    found = lookup_rice_settled(RICE_MERGED_CRC, &texture);
    expect(found == 1 && texture.width == 8 && texture.height == 8,
           "and resolves to the pack's picture once settled");
    if (found && texture.rgba != NULL) {
        const unsigned char *p10 = texture.rgba + (0 * 8 + 1) * 4;
        const unsigned char *p23 = texture.rgba + (3 * 8 + 2) * 4;
        expect(p10[0] == 16 && p10[1] == 0 && p10[2] == 0x33u &&
               p23[0] == 32 && p23[1] == 48 && p23[2] == 0x33u,
               "its colour comes from the _rgb half");
        expect(p10[3] == alpha_key(1, 0) && p23[3] == alpha_key(2, 3),
               "and its alpha from the RED channel of the _a half");
    }
    found = lookup_rice_settled(RICE_MISMATCHED_CRC, &texture);
    expect(found == 1 && texture.rgba != NULL &&
           texture.rgba[3] == 0xFFu && texture.rgba[8 * 8 * 4 - 1] == 0xFFu,
           "an _a half of another size leaves the texture opaque");
    expect(mdkr_mod_texture_rice_resident() == used_before + 2,
           "and both identities count as used, once each");
    found = mdkr_mod_texture_lookup_rice(RICE_UNKNOWN_CRC, 0, 2, &texture);
    expect(found == 0 && texture.pending == 0,
           "a CRC the pack does not index is a final no, not pending");
}

/* 7. A zipped pack read through the worker's own archive handle. */
static void test_zip_pack_through_the_worker(void) {
    MdkrModTexture texture;
    expect(mdkr_mod_texture_store_loader() == MDKR_MOD_TEXTURE_LOADER_WORKER,
           "the native store runs the worker loader");
    expect(mdkr_mod_texture_lookup(DIGEST_ZIPPED, &texture) == 0 &&
           texture.pending == 1,
           "a digest a zipped pack supplies is pending at first ask");
    expect(lookup_settled(DIGEST_ZIPPED, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x70u),
           "and the worker inflates and decodes it from the zip");
}

/* 8. A lookup does no pack I/O, and a pack is opened once per generation.
 *
 * T12a measured the pack being opened -- its 2,120-entry central directory
 * re-parsed -- for every texture lookup, half of them for a digest-named file
 * a Rice pack can never hold. The worker is parked first, so any I/O counted
 * between "before" and "after" can only have been the lookup's own. */
static void test_lookups_do_no_pack_io(MdkrModRegistry *registry) {
    MdkrModTexture texture;
    unsigned long  opens_at_init;
    unsigned long  opens;
    unsigned long  queries;
    int            absent_ok;
    int            settled;

    opens_at_init = mdkr_mod_source_test_opens();
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    expect(mdkr_mod_source_test_opens() - opens_at_init == 5,
           "init opens each texture pack once (probe, zipped, Rice, and the "
           "two case packs)");

    mdkr_mod_texture_store_test_hold_worker(1);
    opens = mdkr_mod_source_test_opens();
    queries = mdkr_mod_source_test_queries();
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    absent_ok =
        mdkr_mod_texture_lookup("0123456789abcdef0123456789abcdef", &texture) == 0 &&
        texture.pending == 0 &&
        mdkr_mod_texture_lookup("1123456789abcdef0123456789abcdef", &texture) == 0 &&
        texture.pending == 0 &&
        mdkr_mod_texture_lookup_rice(RICE_UNKNOWN_CRC, 0, 2, &texture) == 0 &&
        texture.pending == 0;
    expect(absent_ok, "absent digests and unknown Rice keys are a final no");
    expect(mdkr_mod_texture_lookup(DIGEST_IO, &texture) == 0 &&
           texture.pending == 1,
           "a present digest is pending");
    expect(mdkr_mod_texture_store_test_wait_parked() == 1,
           "and its request reached the worker, which is parked on it");
    expect(mdkr_mod_source_test_opens() == opens &&
           mdkr_mod_source_test_queries() == queries,
           "none of those lookups opened, probed or read a pack");
    expect(mdkr_texture_probe_decode_calls() == 0,
           "and nothing was decoded on the asking thread");
    mdkr_mod_texture_store_test_hold_worker(0);

    settled = lookup_settled(DIGEST_IO, &texture);
    expect(settled == 1 && pixels_are_gradient(&texture, 0x55u),
           "the worker then reads and decodes it");
    expect(mdkr_mod_source_test_opens() == opens,
           "through the archive the store already had open");
}

static unsigned s_observed;
static char     s_observed_key[64];
static int      s_observed_resident;
static uint32_t s_observed_pumps;

static void observe_install(const char *key, int resident, uint32_t pumps,
                            uint64_t ns, size_t bytes) {
    (void)ns;
    (void)bytes;
    s_observed++;
    snprintf(s_observed_key, sizeof s_observed_key, "%s", key);
    s_observed_resident = resident;
    s_observed_pumps = pumps;
}

/* 9. PENDING -> RESIDENT on the owner thread, inside the pump's budget.
 *
 * The browser build has no worker; it serves the same queue from the pump,
 * bounded per frame. Nothing is read at the lookup, one install per pump when
 * the budget says one, and the serial a renderer polls moves exactly when a
 * texture settles. */
static void test_budget_loader_installs_within_budget(MdkrModRegistry *registry) {
    const MdkrModTexturePumpBudget one = { 1u, MDKR_MOD_TEXTURE_INSTALL_BYTES,
                                           MDKR_MOD_TEXTURE_WORK_NS };
    MdkrModTexture texture;
    uint32_t       serial;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_BUDGET);
    expect(mdkr_mod_texture_store_loader() == MDKR_MOD_TEXTURE_LOADER_BUDGET,
           "the budget loader can be chosen");
    mdkr_mod_texture_set_install_observer(observe_install);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET1, &texture) == 0 &&
           texture.pending == 1 &&
           mdkr_mod_texture_lookup(DIGEST_BUDGET2, &texture) == 0 &&
           texture.pending == 1,
           "two present digests are pending");
    expect(mdkr_texture_probe_decode_calls() == 0 &&
           memory_now().pending == 2,
           "and neither was decoded by the lookup");
    serial = mdkr_mod_texture_settled_serial();
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET1, &texture) == 0 &&
           texture.pending == 1 && mdkr_texture_probe_decode_calls() == 0,
           "asking again while pending queues nothing new");

    mdkr_mod_texture_store_pump(&one, NULL);
    expect(mdkr_texture_probe_decode_calls() == 1 && memory_now().pending == 1,
           "a pump with an install budget of one decodes and installs one");
    expect(mdkr_mod_texture_settled_serial() != serial,
           "and the settled serial moves");
    expect(s_observed == 1 && strcmp(s_observed_key, DIGEST_BUDGET1) == 0 &&
           s_observed_resident == 1 && s_observed_pumps == 1,
           "the observer sees the first request, resident, one pump after it "
           "was queued");
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET1, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x11u),
           "the first is resident with its own pixels");
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET2, &texture) == 0 &&
           texture.pending == 1,
           "the second is still pending");
    serial = mdkr_mod_texture_settled_serial();
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET1, &texture) == 1 &&
           mdkr_mod_texture_settled_serial() == serial,
           "a resident hit does not move the serial");
    mdkr_mod_texture_store_pump(NULL, NULL);
    expect(mdkr_mod_texture_lookup(DIGEST_BUDGET2, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x33u) && s_observed == 2,
           "the next pump installs the second");
    mdkr_mod_texture_set_install_observer(NULL);
}

/* 10. A worker that cannot start leaves the budget loader doing its job. */
static void test_worker_failure_falls_back_to_budget(MdkrModRegistry *registry) {
    MdkrModTexture texture;

    mdkr_mod_texture_store_test_fail_worker_start(1);
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    mdkr_mod_texture_store_test_fail_worker_start(0);
    expect(mdkr_mod_texture_store_loader() == MDKR_MOD_TEXTURE_LOADER_BUDGET,
           "a worker that fails to start falls back to the budget loader");
    expect(mdkr_mod_texture_store_test_wait_parked() == 0,
           "with no worker running");
    expect(mdkr_mod_texture_lookup(DIGEST_FALLBACK, &texture) == 0 &&
           texture.pending == 1,
           "a present digest is pending");
    mdkr_mod_texture_store_pump(NULL, NULL);
    expect(mdkr_mod_texture_lookup(DIGEST_FALLBACK, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x44u),
           "and the next pump installs it on the owner thread");
}

/* 11. A completion from before a toggle is discarded, its buffer freed, and
 * never written into the slot -- whether the toggle caught the request still
 * waiting to be read or already decoded. ASan and the leak check run this
 * file; a buffer this drops on the floor fails them. */
static void test_stale_completions_are_discarded(MdkrModRegistry *registry) {
    MdkrModTexture              texture;
    MdkrModTextureResolveTiming before;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    mdkr_mod_texture_timing_set_clock(fake_clock);

    /* (a) toggled while the worker holds the request, before any read. */
    mdkr_mod_texture_store_test_hold_worker(1);
    mdkr_texture_probe_reset(PROBE_REFUSE_ABOVE);
    expect(mdkr_mod_texture_lookup(DIGEST_STALE1, &texture) == 0 &&
           texture.pending == 1 &&
           mdkr_mod_texture_store_test_wait_parked() == 1,
           "a request is in the worker's hands");
    before = timing_now_totals();
    mdkr_mod_texture_set_enabled(false);
    mdkr_mod_texture_set_enabled(true);
    expect(memory_now().pending == 0,
           "a toggle returns every pending texture to unresolved");
    expect(mdkr_mod_texture_lookup(DIGEST_STALE1, &texture) == 0 &&
           texture.pending == 1,
           "and asking again queues a fresh request");
    mdkr_mod_texture_store_test_hold_worker(0);
    mdkr_mod_texture_store_settle();
    expect(timing_now_totals().discarded == before.discarded + 1 &&
           timing_now_totals().installed == before.installed + 1,
           "the stale request is discarded and only the fresh one installs");
    expect(mdkr_texture_probe_decode_calls() == 1,
           "the stale request was never decoded");
    expect(timing_now_totals().files == before.files + 1,
           "nor even read: the worker drops it before touching the pack");
    expect(mdkr_mod_texture_lookup(DIGEST_STALE1, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x11u),
           "and the slot holds the fresh result");

    /* (b) toggled after the worker decoded, before the owner installed. */
    expect(mdkr_mod_texture_lookup(DIGEST_STALE2, &texture) == 0 &&
           texture.pending == 1,
           "a second request is queued");
    expect(mdkr_mod_texture_store_test_wait_idle() == 1 &&
           memory_now().inflight_bytes == 8u * 8u * 4u,
           "the worker finished it; its pixels are in flight, not installed");
    before = timing_now_totals();
    mdkr_mod_texture_set_enabled(false);
    mdkr_mod_texture_set_enabled(true);
    mdkr_mod_texture_store_pump(NULL, NULL);
    expect(timing_now_totals().discarded == before.discarded + 1 &&
           timing_now_totals().installed == before.installed &&
           memory_now().inflight_bytes == 0,
           "the decoded completion is discarded and its bytes released");
    expect(mdkr_mod_texture_lookup(DIGEST_STALE2, &texture) == 0 &&
           texture.pending == 1,
           "the slot was not written; asking again queues it afresh");
    expect(lookup_settled(DIGEST_STALE2, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x22u),
           "and the fresh request installs");
    mdkr_mod_texture_timing_set_clock(NULL);
}

/* 12. Shutdown and reload with requests at every stage of the hand-off: one
 * finished and waiting, one in the worker's hands, two still queued. The join
 * must not hang, nothing may leak, and the next generation must work. */
static void prime_requests_in_flight(void) {
    MdkrModTexture texture;
    expect(mdkr_mod_texture_lookup(DIGEST_FLIGHT0, &texture) == 0 &&
           texture.pending == 1 &&
           mdkr_mod_texture_store_test_wait_idle() == 1,
           "one completion is waiting to be installed");
    mdkr_mod_texture_store_test_hold_worker(1);
    expect(mdkr_mod_texture_lookup(DIGEST_FLIGHT1, &texture) == 0 &&
           mdkr_mod_texture_lookup(DIGEST_FLIGHT2, &texture) == 0 &&
           mdkr_mod_texture_lookup(DIGEST_FLIGHT3, &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_parked() == 1 &&
           memory_now().pending == 4,
           "one request is in the worker's hands and two are queued");
}

static void test_shutdown_and_reload_in_flight(MdkrModRegistry *registry) {
    MdkrModTexture            texture;
    MdkrModTextureMemoryStats stats;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    prime_requests_in_flight();
    mdkr_mod_texture_store_shutdown();
    mdkr_mod_texture_store_test_hold_worker(0);
    stats = memory_now();
    expect(!mdkr_mod_texture_store_active() &&
           mdkr_mod_texture_store_loader() == MDKR_MOD_TEXTURE_LOADER_AUTO &&
           stats.pending == 0 && stats.inflight_bytes == 0 &&
           stats.pixel_bytes == 0 && stats.entries == 0,
           "shutdown joins the worker and frees every request in flight");

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    prime_requests_in_flight();
    /* Init over a live store is a reload: it shuts the old generation down
     * first, with the worker still parked on a request. */
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    mdkr_mod_texture_store_test_hold_worker(0);
    expect(memory_now().pending == 0 && memory_now().entries == 0,
           "a reload discards the old generation's requests");
    expect(lookup_settled(DIGEST_FLIGHT1, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x51u),
           "and the new generation resolves the same texture");
}

/* 13. Eviction picks only resident textures. A pending one has no pixels, so
 * "least recently used" must never be read as "never used yet": evicting it
 * would orphan its completion, which would then be discarded and the texture
 * would never appear. Budget loader for a deterministic install order. */
static void test_eviction_never_touches_pending(MdkrModRegistry *registry) {
    const MdkrModTexturePumpBudget one = { 1u, MDKR_MOD_TEXTURE_INSTALL_BYTES,
                                           MDKR_MOD_TEXTURE_WORK_NS };
    MdkrModTexture texture;
    char           digest[33];
    int            index;
    int            ok = 1;

    /* Room for exactly eight 8x8 textures. */
    mdkr_mod_texture_store_test_set_cache_limit(8u * 8u * 8u * 4u);
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_BUDGET);
    for (index = 0; index < 8; index++) {
        eviction_digest(digest, index);
        ok &= lookup_settled(digest, &texture);
    }
    expect(ok && memory_now().pixel_bytes == 8u * 8u * 8u * 4u &&
           memory_now().evictions == 0,
           "the cache fills to its limit without evicting");
    eviction_digest(digest, 8);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 0 && texture.pending,
           "texture 8 is queued");
    for (index = 1; index < 8; index++) {        /* 0 becomes the LRU */
        char held[33];
        eviction_digest(held, index);
        ok &= mdkr_mod_texture_lookup(held, &texture);
    }
    eviction_digest(digest, 9);
    expect(ok && mdkr_mod_texture_lookup(digest, &texture) == 0 &&
           texture.pending,
           "texture 9 is queued behind it");
    mdkr_mod_texture_store_pump(&one, NULL);
    expect(memory_now().evictions == 1 && memory_now().pending == 1,
           "installing texture 8 evicts one resident");
    eviction_digest(digest, 0);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 0 && texture.pending,
           "the least recently used resident was the one evicted");
    eviction_digest(digest, 9);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 0 && texture.pending,
           "while texture 9 stayed pending through that eviction");
    mdkr_mod_texture_store_pump(&one, NULL);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 1 &&
           pixels_are_gradient(&texture, 0x99u),
           "and installs normally on the next pump");
    mdkr_mod_texture_store_settle();
    eviction_digest(digest, 0);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 1 &&
           memory_now().reloads == 1 && memory_now().first_resident == 10 &&
           memory_now().first_resident_bytes == 10u * 8u * 8u * 4u,
           "the evicted texture reloads once and counts as a reload, not as "
           "a new identity");
    mdkr_mod_texture_store_test_set_cache_limit(0);
}

/* 14. In-flight pixels are counted against the cache budget. With the worker
 * running, residents are held to the limit minus the in-flight cap (one
 * eighth), so resident plus in-flight never exceeds the limit. */
static void test_inflight_counts_against_the_budget(MdkrModRegistry *registry) {
    MdkrModTexture texture;
    char           digest[33];
    int            index;
    int            ok = 1;

    mdkr_mod_texture_store_test_set_cache_limit(8u * 8u * 8u * 4u);
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    for (index = 0; index < 8; index++) {
        eviction_digest(digest, index);
        ok &= lookup_settled(digest, &texture);
    }
    expect(ok && memory_now().pixel_limit == 8u * 8u * 8u * 4u &&
           memory_now().pixel_bytes == 7u * 8u * 8u * 4u &&
           memory_now().evictions == 1,
           "with a worker, residents stop one in-flight cap short of the limit");
    eviction_digest(digest, 10);
    expect(mdkr_mod_texture_lookup(digest, &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_idle() == 1 &&
           memory_now().pixel_bytes + memory_now().inflight_bytes <=
               memory_now().pixel_limit &&
           memory_now().inflight_bytes == 8u * 8u * 4u,
           "and a decoded texture waiting to install fits in that gap");
    mdkr_mod_texture_store_settle();
    mdkr_mod_texture_store_test_set_cache_limit(0);
}

/* 15. Off-frame accounting. The lookup on the owner thread is charged to
 * resolve_ns and queues a request; the read and decode are charged when the
 * completion is installed, along with the worker's whole time. */
static void test_worker_timing(MdkrModRegistry *registry) {
    MdkrModTexture              texture;
    MdkrModTextureResolveTiming before, queued, after;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    mdkr_mod_texture_timing_set_clock(fake_clock);
    before = timing_now_totals();
    expect(mdkr_mod_texture_lookup(DIGEST_WTIMED, &texture) == 0 &&
           texture.pending == 1, "a timed fresh texture is pending");
    mdkr_mod_texture_store_test_wait_idle();
    queued = timing_now_totals();
    expect(queued.resolves == before.resolves + 1 &&
           queued.probes == before.probes + 1 &&
           queued.queued == before.queued + 1 &&
           queued.resolve_ns > before.resolve_ns,
           "the lookup is one timed resolution that queued one request");
    expect(queued.files == before.files && queued.read_ns == before.read_ns &&
           queued.decode_ns == before.decode_ns &&
           queued.installed == before.installed,
           "and no read or decode is charged until the result is installed");
    mdkr_mod_texture_store_pump(NULL, fake_clock);
    after = timing_now_totals();
    expect(after.installed == queued.installed + 1 &&
           after.loaded == queued.loaded + 1 &&
           after.files == queued.files + 1 &&
           after.decoded_bytes == queued.decoded_bytes + 8u * 8u * 4u &&
           after.read_ns - queued.read_ns >= FAKE_CLOCK_STEP &&
           after.decode_ns - queued.decode_ns >= FAKE_CLOCK_STEP,
           "installing it charges one file, its bytes, its read and decode");
    expect(after.worker_ns - queued.worker_ns >=
               (after.read_ns - queued.read_ns) +
               (after.decode_ns - queued.decode_ns) &&
           after.pump_ns > queued.pump_ns,
           "the worker's whole time contains both; the pump is timed too");
    expect(after.resolve_ns == queued.resolve_ns,
           "and none of it lands in the owner's resolve time");
    mdkr_mod_texture_timing_set_clock(NULL);
}

/* 16. The owner keeps working while the worker does. Every case above hands
 * the worker one request and waits; this one is what a scene change looks
 * like -- the owner asks for a dozen textures every frame, pumps with a small
 * budget and, halfway, toggles packs off and on, all while the worker is
 * reading and decoding. Run under ThreadSanitizer it is the case that puts
 * owner-side state and worker-side execution in the same instant. */
static void test_owner_and_worker_overlap(MdkrModRegistry *registry) {
    const MdkrModTexturePumpBudget two = { 2u, MDKR_MOD_TEXTURE_INSTALL_BYTES,
                                           MDKR_MOD_TEXTURE_WORK_NS };
    MdkrModTexture texture;
    char           digest[33];
    int            round;
    int            index;
    int            resident = 0;
    int            pixels_ok = 1;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    mdkr_mod_texture_timing_set_clock(fake_clock);
    for (round = 0; round < 20000 && resident < EVICT_FIXTURES; round++) {
        resident = 0;
        for (index = 0; index < EVICT_FIXTURES; index++) {
            eviction_digest(digest, index);
            if (mdkr_mod_texture_lookup(digest, &texture)) resident++;
        }
        (void)memory_now();
        mdkr_mod_texture_store_pump(&two, fake_clock);
        if (round == 3) {
            mdkr_mod_texture_set_enabled(false);
            mdkr_mod_texture_store_pump(&two, fake_clock);
            mdkr_mod_texture_set_enabled(true);
        }
    }
    mdkr_mod_texture_store_settle();
    for (index = 0; index < EVICT_FIXTURES; index++) {
        eviction_digest(digest, index);
        if (!lookup_settled(digest, &texture) ||
            !pixels_are_gradient(&texture, (unsigned char)(0x90 + index))) {
            pixels_ok = 0;
        }
    }
    expect(pixels_ok && memory_now().pending == 0 &&
           memory_now().inflight_bytes == 0,
           "every texture asked for under load ends resident with its own "
           "pixels, nothing left pending or in flight");
    mdkr_mod_texture_timing_set_clock(NULL);
}

/* 17. A digest-named texture is found whatever case its pack spells it in,
 * as it was in 1.7.0 on case-insensitive filesystems -- in a folder pack and
 * in a zip -- and read back through the worker by the pack's own spelling. */
static void test_digest_names_ignore_case(MdkrModRegistry *registry) {
    MdkrModTexture texture;

    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
    expect(lookup_settled(DIGEST_CASE_FOLDER, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xC1u),
           "a folder pack's Textures/<UPPER>.PNG replaces the lowercase digest");
    expect(lookup_settled(DIGEST_CASE_ZIP, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xC2u),
           "and so does a zip's TEXTURES/<UPPER>.Png");
}

/* ------------------------------------------------------------ T12d cases */

/* One file of a zipped fixture: an 8x8 gradient whose blue is `blue`. */
typedef struct ZipPng {
    const char   *name;
    unsigned char blue;
} ZipPng;

static void write_zip_pngs(const char *root, const char *name,
                           const char *manifest, const ZipPng *files,
                           size_t count) {
    char           full[1024];
    mz_zip_archive zip;
    size_t         index;
    int            ok;

    path2(full, sizeof full, root, name);
    make_parent_directories(full);
    memset(&zip, 0, sizeof zip);
    ok = mz_zip_writer_init_file(&zip, full, 0) &&
         mz_zip_writer_add_mem(&zip, "pack.ini", manifest, strlen(manifest),
                               MZ_DEFAULT_COMPRESSION);
    for (index = 0; ok && index < count; index++) {
        PngBuffer png;
        build_rgba_png(&png, 8, pixel_gradient, files[index].blue);
        ok = mz_zip_writer_add_mem(&zip, files[index].name, png.bytes,
                                   png.length, MZ_DEFAULT_COMPRESSION);
    }
    if (!ok || !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        fatal("cannot write the scratch archive", full);
    }
    mz_zip_writer_end(&zip);
}

/* A registry over `mods`, and the store bound to it with the worker. */
static void bind_store(MdkrModRegistry *registry, const char *mods) {
    if (mdkr_mod_registry_init(registry, mods) != 0) {
        fatal("a scratch pack tree did not initialise", mods);
    }
    store_reinit(registry, MDKR_MOD_TEXTURE_LOADER_WORKER);
}

static void unbind_store(MdkrModRegistry *registry) {
    mdkr_mod_texture_store_shutdown();
    mdkr_mod_registry_shutdown(registry);
}

/* 18. One pack holding a digest twice, spelled differently only in case: the
 * picture drawn is the lowercase file's, whichever order the pack lists them
 * in (T12d). It used to be whichever spelling the index probed first. A
 * folder can hold both only on a case-sensitive file system; elsewhere that
 * arm prints a SKIP. */
#define DIGEST_DUP_A "a6000000000000000000000000000001"
#define DIGEST_DUP_B "a6000000000000000000000000000002"
#define DIGEST_DUP_C "a6000000000000000000000000000003"
static void test_case_duplicates_read_the_lowercase_file(const char *scratch) {
    char            mods[1024];
    char            upper_a[128], upper_b[128], upper_c[128];
    char            lower_c[128], probe[1200];
    ZipPng          files[4];
    MdkrModRegistry registry;
    MdkrModTexture  texture;
    struct stat     status;
    int             folder_arm;

    path2(mods, sizeof mods, scratch, "casedup");
    spell_upper(upper_a, sizeof upper_a, "textures", DIGEST_DUP_A, ".PNG");
    spell_upper(upper_b, sizeof upper_b, "textures", DIGEST_DUP_B, ".PNG");
    spell_upper(upper_c, sizeof upper_c, "textures", DIGEST_DUP_C, ".PNG");
    snprintf(lower_c, sizeof lower_c, "textures/%s.png", DIGEST_DUP_C);
    files[0].name = upper_a;                       files[0].blue = 0xA1u;
    files[1].name = "textures/" DIGEST_DUP_A ".png"; files[1].blue = 0xA2u;
    files[2].name = "textures/" DIGEST_DUP_B ".png"; files[2].blue = 0xA3u;
    files[3].name = upper_b;                       files[3].blue = 0xA4u;
    write_zip_pngs(mods, "dup.zip", "[pack]\nname=Dup\n", files, 4);
    write_pack_text(mods, "dupfolder", "pack.ini", "[pack]\nname=DupFolder\n");
    write_png_file(mods, "dupfolder", upper_c, 8, pixel_gradient, 0xA5u);
    path3(probe, sizeof probe, mods, "dupfolder", lower_c);
    folder_arm = stat(probe, &status) != 0;
    if (folder_arm) {
        write_png_file(mods, "dupfolder", lower_c, 8, pixel_gradient, 0xA6u);
    } else {
        printf("SKIP this file system ignores case, so a folder cannot hold "
               "both spellings\n");
    }

    bind_store(&registry, mods);
    expect(lookup_settled(DIGEST_DUP_A, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xA2u),
           "a zip listing UPPER then lower draws the lowercase file");
    expect(lookup_settled(DIGEST_DUP_B, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xA3u),
           "and listing lower then UPPER, the lowercase file too");
    if (folder_arm) {
        expect(lookup_settled(DIGEST_DUP_C, &texture) == 1 &&
               pixels_are_gradient(&texture, 0xA6u),
               "a folder holding both draws the lowercase file");
    }
    unbind_store(&registry);
}

/* 19. Nine packs each spelling one digest their own way, the highest-priority
 * pack last in the index. The store asked the index for eight spellings, so
 * that pack was dropped and the next one down was drawn (T12d). Every holder
 * is a candidate, highest first, down to the last: with the eight above it
 * gone from disk after the scan, the lowest pack's file is still read. */
#define DIGEST_SPELLED  "abcdefabcdefabcdefabcdefabcdefab"
#define DIGEST_SPELLED2 "fedcbafedcbafedcbafedcbafedcbafe"
static void test_every_spelling_pack_is_a_candidate(const char *scratch) {
    char            mods[1024];
    char            gone[9][1200];
    MdkrModRegistry registry;
    MdkrModTexture  texture;
    int             index;
    int             removed = 1;

    path2(mods, sizeof mods, scratch, "spellings");
    for (index = 0; index < 9; index++) {
        char pack[16];
        char manifest[64];
        char relative[64];
        snprintf(pack, sizeof pack, "p%d", index);
        snprintf(manifest, sizeof manifest, "[pack]\nname=P%d\npriority=%d\n",
                 index, 10 * (index + 1));
        write_pack_text(mods, pack, "pack.ini", manifest);
        /* Pack i upper-cases the i-th hex letter: nine distinct spellings. */
        snprintf(relative, sizeof relative, "textures/%s.png", DIGEST_SPELLED);
        relative[9 + index] = (char)(relative[9 + index] - 'a' + 'A');
        write_png_file(mods, pack, relative, 8, pixel_gradient,
                       (unsigned char)(0xB0 + index));
        snprintf(relative, sizeof relative, "textures/%s.png",
                 DIGEST_SPELLED2);
        relative[9 + index] = (char)(relative[9 + index] - 'a' + 'A');
        write_png_file(mods, pack, relative, 8, pixel_gradient,
                       (unsigned char)(0xC0 + index));
        path3(gone[index], sizeof gone[index], mods, pack, relative);
    }
    bind_store(&registry, mods);
    expect(lookup_settled(DIGEST_SPELLED, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xB8u),
           "the highest-priority of nine spellings is the one drawn");
    for (index = 1; index < 9; index++) removed &= remove(gone[index]) == 0;
    expect(removed && lookup_settled(DIGEST_SPELLED2, &texture) == 1 &&
           pixels_are_gradient(&texture, 0xC0u),
           "and with the eight above it gone, the ninth holder is still read");
    unbind_store(&registry);
}

/* 20. A Rice _rgb + _a pair is one request for two images, and it reserves
 * both before decoding either (T12d). Reserving them one at a time let the
 * worker decode the _rgb half, then sit holding those pixels while it waited
 * for room for the _a half -- room only the owner can free. With the 2 KiB
 * cache seam the in-flight share is 256 bytes: three 64-byte completions
 * leave room for one 4x4 half but not for the pair, so the pair must wait
 * holding nothing. A pair larger than the share (two 8x8 halves, 512 bytes)
 * proceeds only once nothing else is in flight, and is then the one request
 * that may exceed the share: the documented bound. */
#define DIGEST_ROOM1 "e1000000000000000000000000000001"
#define DIGEST_ROOM2 "e1000000000000000000000000000002"
#define DIGEST_ROOM3 "e1000000000000000000000000000003"
#define DIGEST_ROOM4 "e1000000000000000000000000000004"
#define RICE_SMALL_PAIR_CRC 0x0000a11cu
#define RICE_LARGE_PAIR_CRC 0x0000b16cu
static void test_rice_pair_reserves_both_halves(const char *scratch) {
    char            mods[1024];
    char            rice[256];
    char            relative[64];
    MdkrModRegistry registry;
    MdkrModTexture  texture;
    const char     *fills[4] = { DIGEST_ROOM1, DIGEST_ROOM2, DIGEST_ROOM3,
                                 DIGEST_ROOM4 };
    int             index;
    int             ok = 1;

    path2(mods, sizeof mods, scratch, "pairroom");
    write_pack_text(mods, "fill", "pack.ini", "[pack]\nname=Fill\n");
    for (index = 0; index < 4; index++) {
        snprintf(relative, sizeof relative, "textures/%s.png", fills[index]);
        write_png_file(mods, "fill", relative, 4, pixel_gradient, 0x10u);
    }
    rice_name(rice, sizeof rice, RICE_SMALL_PAIR_CRC, "rgb");
    write_png_file(mods, "ricepair", rice, 4, pixel_gradient, 0x21u);
    rice_name(rice, sizeof rice, RICE_SMALL_PAIR_CRC, "a");
    write_png_file(mods, "ricepair", rice, 4, pixel_alpha_half, 0x07u);
    rice_name(rice, sizeof rice, RICE_LARGE_PAIR_CRC, "rgb");
    write_png_file(mods, "ricepair", rice, 8, pixel_gradient, 0x22u);
    rice_name(rice, sizeof rice, RICE_LARGE_PAIR_CRC, "a");
    write_png_file(mods, "ricepair", rice, 8, pixel_alpha_half, 0x07u);

    mdkr_mod_texture_store_test_set_cache_limit(2048u);
    bind_store(&registry, mods);
    for (index = 0; index < 3; index++) {
        ok &= mdkr_mod_texture_lookup(fills[index], &texture) == 0 &&
              texture.pending == 1;
    }
    expect(ok && mdkr_mod_texture_store_test_wait_idle() == 1 &&
           memory_now().inflight_bytes == 3u * 4u * 4u * 4u,
           "three decoded 4x4 textures wait to install: 192 of 256 bytes");
    expect(mdkr_mod_texture_lookup_rice(RICE_SMALL_PAIR_CRC, 0, 2,
                                        &texture) == 0 &&
           texture.pending == 1 &&
           mdkr_mod_texture_store_test_wait_room_blocked(5000) == 1,
           "a 4x4 Rice pair (128 bytes) waits for room");
    expect(memory_now().inflight_bytes == 3u * 4u * 4u * 4u,
           "holding none of it: neither half was claimed or decoded alone");
    mdkr_mod_texture_store_pump(NULL, NULL);
    expect(lookup_rice_settled(RICE_SMALL_PAIR_CRC, &texture) == 1 &&
           texture.width == 4 && texture.rgba != NULL &&
           texture.rgba[(0 * 4 + 1) * 4 + 3] == alpha_key(1, 0) &&
           texture.rgba[(0 * 4 + 1) * 4 + 2] == 0x21u,
           "once the owner drains, the pair decodes and merges as before");
    expect(mdkr_mod_texture_store_test_inflight_peak() <= 2048u / 8u,
           "and in flight never passed the share");

    expect(mdkr_mod_texture_lookup(DIGEST_ROOM4, &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_idle() == 1 &&
           memory_now().inflight_bytes == 4u * 4u * 4u,
           "one 4x4 completion is in flight");
    expect(mdkr_mod_texture_lookup_rice(RICE_LARGE_PAIR_CRC, 0, 2,
                                        &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_room_blocked(5000) == 1 &&
           memory_now().inflight_bytes == 4u * 4u * 4u,
           "an 8x8 pair, larger than the share, waits holding nothing");
    mdkr_mod_texture_store_pump(NULL, NULL);
    expect(mdkr_mod_texture_store_test_wait_idle() == 1 &&
           mdkr_mod_texture_store_test_inflight_peak() == 2u * 8u * 8u * 4u,
           "and proceeds only alone: one request, both halves, is the most "
           "ever in flight past the share");
    expect(memory_now().inflight_bytes == 8u * 8u * 4u,
           "the _a half is released once merged; the result holds one image");
    expect(lookup_rice_settled(RICE_LARGE_PAIR_CRC, &texture) == 1 &&
           texture.width == 8 && memory_now().inflight_bytes == 0,
           "then installs, releasing it all");
    unbind_store(&registry);
    mdkr_mod_texture_store_test_set_cache_limit(0);
}

/* 21. The worker blocked on in-flight room -- the share full of completions
 * the owner has not drained -- must come back promptly when the owner toggles
 * packs and when the store stops (T12d: the wait path had no case). Each wait
 * here has a timeout, so a worker that never wakes fails the case; a stop
 * that does not return exits the test rather than joining a thread that
 * never will. */
#define DIGEST_CAP0 "e2000000000000000000000000000000"
#define DIGEST_CAP1 "e2000000000000000000000000000001"
static void test_room_wait_releases_on_toggle_and_stop(const char *scratch) {
    char                      mods[1024];
    MdkrModRegistry           registry;
    MdkrModTexture            texture;
    MdkrModTextureMemoryStats stats;

    path2(mods, sizeof mods, scratch, "capwait");
    write_pack_text(mods, "cap", "pack.ini", "[pack]\nname=Cap\n");
    write_small_rgba_png(mods, "cap", DIGEST_CAP0, 0x60u);
    write_small_rgba_png(mods, "cap", DIGEST_CAP1, 0x61u);

    mdkr_mod_texture_store_test_set_cache_limit(2048u);
    bind_store(&registry, mods);
    expect(mdkr_mod_texture_lookup(DIGEST_CAP0, &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_idle() == 1 &&
           memory_now().inflight_bytes == 8u * 8u * 4u,
           "one undrained 8x8 completion fills the 256-byte share");
    expect(mdkr_mod_texture_lookup(DIGEST_CAP1, &texture) == 0 &&
           mdkr_mod_texture_store_test_wait_room_blocked(5000) == 1,
           "the next request blocks the worker on room");

    mdkr_mod_texture_set_enabled(false);
    expect(mdkr_mod_texture_store_test_wait_idle_for(5000) == 1,
           "a toggle releases the blocked worker promptly");
    mdkr_mod_texture_set_enabled(true);
    expect(mdkr_mod_texture_lookup(DIGEST_CAP1, &texture) == 0 &&
           texture.pending == 1 &&
           mdkr_mod_texture_store_test_wait_room_blocked(5000) == 1,
           "asked again, it blocks again: the stale completion still holds "
           "the share");

    if (mdkr_mod_texture_store_test_stop_worker(5000) != 1) {
        expect(0, "a stop releases the blocked worker promptly");
        fatal("the worker did not stop", "joining it would hang");
    }
    expect(1, "a stop releases the blocked worker promptly");
    unbind_store(&registry);
    mdkr_mod_texture_memory_stats(&stats);
    expect(stats.pending == 0 && stats.inflight_bytes == 0 &&
           stats.entries == 0,
           "and shutdown frees the stale completion and the cancelled request");
    mdkr_mod_texture_store_test_set_cache_limit(0);
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
    char            rice[256];
    MdkrModRegistry registry;
    int             index;

    if (strlen(DIGEST_GIANT) != 32 || strlen(DIGEST_SOUND) != 32 ||
        strlen(DIGEST_NOTPNG) != 32 || strlen(DIGEST_UNTIMED) != 32 ||
        strlen(DIGEST_TIMED) != 32 || strlen(DIGEST_ABSENT) != 32 ||
        strlen(DIGEST_ZIPPED) != 32 || strlen(DIGEST_IO) != 32 ||
        strlen(DIGEST_BUDGET1) != 32 || strlen(DIGEST_BUDGET2) != 32 ||
        strlen(DIGEST_FALLBACK) != 32 || strlen(DIGEST_STALE1) != 32 ||
        strlen(DIGEST_STALE2) != 32 || strlen(DIGEST_FLIGHT0) != 32 ||
        strlen(DIGEST_FLIGHT1) != 32 || strlen(DIGEST_FLIGHT2) != 32 ||
        strlen(DIGEST_FLIGHT3) != 32 || strlen(DIGEST_WTIMED) != 32 ||
        strlen(DIGEST_CASE_FOLDER) != 32 || strlen(DIGEST_CASE_ZIP) != 32 ||
        strlen(DIGEST_BOMB) != 32 || strlen(DIGEST_DUP_A) != 32 ||
        strlen(DIGEST_DUP_B) != 32 || strlen(DIGEST_DUP_C) != 32 ||
        strlen(DIGEST_SPELLED) != 32 || strlen(DIGEST_SPELLED2) != 32 ||
        strlen(DIGEST_ROOM1) != 32 || strlen(DIGEST_ROOM4) != 32 ||
        strlen(DIGEST_CAP0) != 32 || strlen(DIGEST_CAP1) != 32) {
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
    write_inflation_bomb_png(mods, "probe", DIGEST_BOMB);
    write_small_rgba_png(mods, "probe", MDKR_MOD_TAJ_PORTRAIT_LEGACY_DIGEST, 0x40u);
    write_small_rgba_png(mods, "probe", DIGEST_UNTIMED, 0x50u);
    write_small_rgba_png(mods, "probe", DIGEST_TIMED, 0x60u);
    write_small_rgba_png(mods, "probe", DIGEST_IO, 0x55u);
    write_small_rgba_png(mods, "probe", DIGEST_BUDGET1, 0x11u);
    write_small_rgba_png(mods, "probe", DIGEST_BUDGET2, 0x33u);
    write_small_rgba_png(mods, "probe", DIGEST_FALLBACK, 0x44u);
    write_small_rgba_png(mods, "probe", DIGEST_STALE1, 0x11u);
    write_small_rgba_png(mods, "probe", DIGEST_STALE2, 0x22u);
    write_small_rgba_png(mods, "probe", DIGEST_FLIGHT0, 0x50u);
    write_small_rgba_png(mods, "probe", DIGEST_FLIGHT1, 0x51u);
    write_small_rgba_png(mods, "probe", DIGEST_FLIGHT2, 0x52u);
    write_small_rgba_png(mods, "probe", DIGEST_FLIGHT3, 0x53u);
    write_small_rgba_png(mods, "probe", DIGEST_WTIMED, 0x77u);
    for (index = 0; index < EVICT_FIXTURES; index++) {
        char digest[33];
        eviction_digest(digest, index);
        write_small_rgba_png(mods, "probe", digest, (unsigned char)(0x90 + index));
    }
    write_pack_bytes(mods, "probe", "textures/" DIGEST_NOTPNG ".png",
                     (const unsigned char *)notpng, strlen(notpng));
    write_zip_digest_pack(mods, "zipped.zip", DIGEST_ZIPPED, 0x70u);
    /* A Rice pack: no pack.ini, recognised by its file names. */
    write_pack_text(mods, "casepack", "pack.ini", "[pack]\nname=Case\n");
    spell_upper(rice, sizeof rice, "Textures", DIGEST_CASE_FOLDER, ".PNG");
    write_png_file(mods, "casepack", rice, 8, pixel_gradient, 0xC1u);
    spell_upper(rice, sizeof rice, "TEXTURES", DIGEST_CASE_ZIP, ".Png");
    write_zip_png_pack(mods, "casezip.zip", "[pack]\nname=CaseZip\n", rice,
                       0xC2u);
    rice_name(rice, sizeof rice, RICE_MERGED_CRC, "rgb");
    write_png_file(mods, "ricepack", rice, 8, pixel_gradient, 0x33u);
    rice_name(rice, sizeof rice, RICE_MERGED_CRC, "a");
    write_png_file(mods, "ricepack", rice, 8, pixel_alpha_half, 0x07u);
    rice_name(rice, sizeof rice, RICE_MISMATCHED_CRC, "rgb");
    write_png_file(mods, "ricepack", rice, 8, pixel_gradient, 0x34u);
    rice_name(rice, sizeof rice, RICE_MISMATCHED_CRC, "a");
    write_png_file(mods, "ricepack", rice, 4, pixel_alpha_half, 0x07u);

    if (mdkr_mod_registry_init(&registry, mods) != 0) {
        fatal("the scratch pack tree did not initialise", mods);
    }
    mdkr_mod_texture_store_set_loader(MDKR_MOD_TEXTURE_LOADER_WORKER);
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
    test_an_inflation_bomb_is_bounded_by_its_header();
    test_a_side_no_backend_can_upload_is_refused();
    test_a_texture_that_fits_still_loads();
    test_an_unreadable_header_is_refused_before_decode();
    test_decode_matches_admitted_dimensions(&registry);
    test_generated_portrait_alias(mods, &registry);
    test_rice_split_alpha_merge();
    test_zip_pack_through_the_worker();
    test_lookups_do_no_pack_io(&registry);
    test_budget_loader_installs_within_budget(&registry);
    test_worker_failure_falls_back_to_budget(&registry);
    test_shutdown_and_reload_in_flight(&registry);
    test_eviction_never_touches_pending(&registry);
    test_inflight_counts_against_the_budget(&registry);
    /* Every case above ran with no timing clock; this one checks that none of
     * it was counted, then installs the clock itself. */
    test_resolve_timing(mods, &registry);
    test_stale_completions_are_discarded(&registry);
    test_worker_timing(&registry);
    test_owner_and_worker_overlap(&registry);
    test_digest_names_ignore_case(&registry);
    test_case_duplicates_read_the_lowercase_file(scratch);
    test_every_spelling_pack_is_a_candidate(scratch);
    test_rice_pair_reserves_both_halves(scratch);
    test_room_wait_releases_on_toggle_and_stop(scratch);
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
