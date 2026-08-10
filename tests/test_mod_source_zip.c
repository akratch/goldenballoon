/*
 * test_mod_source_zip.c — the two shapes a content pack can take, proved to
 * behave as one.
 *
 * A pack is either a directory a player unzipped or a .zip they did not, and
 * `mod_source` exists so that every consumer sees exactly one of those. Two
 * things have to be true for that to be worth anything, and both are asserted
 * here: the same file read through either kind comes back byte-identical, and
 * a hostile entry name is refused by both kinds for the same reason.
 *
 * The traversal cases are written against bait files that genuinely exist.
 * Every hostile name is a real entry in the archive (proved by asking miniz
 * itself to locate it) and, where the host filesystem can hold the name, a
 * real file that the naive join actually opens (proved by opening it). An
 * implementation that forgot to validate would therefore hand back the bait,
 * so these assertions fail against a broken implementation rather than
 * passing for the wrong reason.
 *
 * Every byte this test writes lands beneath the scratch root given by argv[1],
 * which is removed before the first case and after the last.
 */
#include "mod_source.h"

#include "miniz.h"

#include <dirent.h>
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

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

static void note(const char *what) { printf("note %s\n", what); }

static void fatal(const char *what, const char *detail) {
    printf("FATAL %s: %s\n", what, detail);
    exit(2);
}

/* ------------------------------------------------------------- scratch */

static void path2(char *out, size_t size, const char *a, const char *b) {
    int written = snprintf(out, size, "%s/%s", a, b);
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

/* Returns 0 when the host filesystem refused the name, which is the expected
 * answer on Windows for the two bait names that embed ':' and '\\'. */
static int try_write_file(const char *path, const void *data, size_t size) {
    FILE *file;

    make_parent_directories(path);
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static void write_file(const char *path, const void *data, size_t size) {
    if (!try_write_file(path, data, size)) fatal("cannot write scratch file", path);
}

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

/* ------------------------------------------------------------- fixtures */

/* Deliberately not text: NULs and high bytes prove a byte-for-byte round trip
 * rather than a string comparison that stops at the first zero. */
static const unsigned char kStored[] = {
    0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0xff, 0xfe, 0x80, 0x7f,
    0x00, 0x01, 0x02, 0x03, 0xf0, 0x0f, 0xa5, 0x5a,
    0x00, 0xff, 0x00, 0xff, 0x10, 0x20, 0x30, 0x40
};

/* Big and repetitive so the writer actually deflates it; the test asserts the
 * stored method separately, so "deflated" is a fact and not a hope. */
#define DEFLATED_SIZE 4096u
static unsigned char g_deflated[DEFLATED_SIZE];

/* The file behind behaviour 9: one byte string, written into the directory
 * pack and into the archive, read back through the same call. */
static const unsigned char kParity[] = {
    'p', 'a', 'r', 'i', 't', 'y', 0x00, 0x7f, 0x80, 0xff, '\n'
};

#define ENTRY_STORED   "textures/stored.png"
#define ENTRY_DEFLATED "textures/deflated.png"
#define ENTRY_PARITY   "shared/parity.bin"
#define ENTRY_ABSENT   "textures/never_written.png"
#define ENTRY_BOMB     "textures/bomb.png"

/* Every hostile name, the bait content an unguarded implementation would hand
 * back, and the name the archive is built with. `/etc/passwd` cannot be added
 * through miniz's writer, which refuses a leading slash, so it goes in under a
 * placeholder first byte that is patched back to '/' in the finished bytes. */
typedef struct Hostile {
    const char *rel;        /* what the caller asks for */
    const char *zip_added;  /* what the writer was given */
    const char *bait;       /* distinctive contents */
    const char *dir_bait;   /* where the directory bait is written, or NULL */
} Hostile;

#define HOSTILE_ESCAPE       0
#define HOSTILE_ABSOLUTE     1
#define HOSTILE_BACKSLASH    2
#define HOSTILE_DRIVE        3
#define HOSTILE_NESTED       4
#define HOSTILE_COUNT        5

static Hostile g_hostile[HOSTILE_COUNT];
static int g_dir_bait_present[HOSTILE_COUNT];

static char g_scratch[512];
static char g_dir_root[512];
static char g_zip_path[512];
static char g_bomb_path[512];
static char g_trunc_cdir_path[512];
static char g_trunc_tail_path[512];

static unsigned char *g_zip_bytes;
static size_t         g_zip_size;
static unsigned char *g_bomb_bytes;
static size_t         g_bomb_size;

/* ------------------------------------------------------- zip construction */

static void add_entry(mz_zip_archive *zip, const char *name, const void *data,
                      size_t size, mz_uint level) {
    if (!mz_zip_writer_add_mem(zip, name, data, size, level)) {
        fatal("cannot add zip entry", name);
    }
}

static unsigned read_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static void write_le32(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

/* Scans backwards for the end-of-central-directory record. Returns its offset
 * or (size_t)-1. */
static size_t find_eocd(const unsigned char *data, size_t size) {
    size_t offset;

    if (size < 22u) return (size_t)-1;
    for (offset = size - 22u;; offset--) {
        if (read_le32(data + offset) == 0x06054b50u) return offset;
        if (offset == 0u) break;
    }
    return (size_t)-1;
}

/* Replaces every occurrence of `needle` with `replacement`, which must be the
 * same length. Returns the number of replacements so the caller can assert it
 * patched exactly what it meant to. */
static int patch_all(unsigned char *data, size_t size, const char *needle,
                     const char *replacement) {
    size_t length = strlen(needle);
    size_t offset;
    int count = 0;

    if (strlen(replacement) != length || length == 0u || size < length) return 0;
    for (offset = 0u; offset + length <= size; offset++) {
        if (memcmp(data + offset, needle, length) != 0) continue;
        memcpy(data + offset, replacement, length);
        count++;
    }
    return count;
}

static void build_content_zip(void) {
    mz_zip_archive zip;
    void *buffer = NULL;
    size_t size = 0;
    int index;
    int patched;

    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 64u * 1024u)) {
        fatal("cannot start the in-memory zip writer", "init_heap");
    }
    add_entry(&zip, ENTRY_STORED, kStored, sizeof kStored, MZ_NO_COMPRESSION);
    add_entry(&zip, ENTRY_DEFLATED, g_deflated, sizeof g_deflated,
              MZ_BEST_COMPRESSION);
    add_entry(&zip, ENTRY_PARITY, kParity, sizeof kParity, MZ_BEST_COMPRESSION);
    for (index = 0; index < HOSTILE_COUNT; index++) {
        add_entry(&zip, g_hostile[index].zip_added, g_hostile[index].bait,
                  strlen(g_hostile[index].bait), MZ_NO_COMPRESSION);
    }
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) {
        fatal("cannot finalize the in-memory zip", "finalize_heap_archive");
    }
    mz_zip_writer_end(&zip);

    g_zip_bytes = (unsigned char *)buffer;
    g_zip_size = size;

    /* The writer will not emit a leading-slash name, so the absolute-path bait
     * is smuggled in under a placeholder and restored here. Same length, so no
     * offset in the archive moves; it appears once in the local header and
     * once in the central directory. */
    patched = patch_all(g_zip_bytes, g_zip_size,
                        g_hostile[HOSTILE_ABSOLUTE].zip_added,
                        g_hostile[HOSTILE_ABSOLUTE].rel);
    if (patched != 2) fatal("absolute-path bait was not patched into both headers",
                            g_hostile[HOSTILE_ABSOLUTE].zip_added);
    write_file(g_zip_path, g_zip_bytes, g_zip_size);
}

/* One deflated entry whose central-directory and local headers are rewritten
 * to declare very nearly 4 GiB uncompressed. Exactly 0xFFFFFFFF is the zip64
 * sentinel and would send the reader down a different path, so the declared
 * size stops just short of it. The compressed size is left alone: miniz
 * validates that one against the real archive length at open time. */
#define BOMB_DECLARED_SIZE 0xFFFFFF00u

static void build_bomb_zip(void) {
    mz_zip_archive zip;
    void *buffer = NULL;
    size_t size = 0;
    unsigned char *payload;
    size_t eocd;
    size_t cdir_offset;
    size_t local_offset;

    payload = (unsigned char *)calloc(1u, 64u * 1024u);
    if (payload == NULL) fatal("out of memory", "bomb payload");

    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 64u * 1024u)) {
        fatal("cannot start the in-memory zip writer", "bomb init_heap");
    }
    add_entry(&zip, ENTRY_BOMB, payload, 64u * 1024u, MZ_BEST_COMPRESSION);
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) {
        fatal("cannot finalize the bomb zip", "finalize_heap_archive");
    }
    mz_zip_writer_end(&zip);
    free(payload);

    g_bomb_bytes = (unsigned char *)buffer;
    g_bomb_size = size;

    eocd = find_eocd(g_bomb_bytes, g_bomb_size);
    if (eocd == (size_t)-1) fatal("bomb zip has no end-of-central-directory", "");
    cdir_offset = read_le32(g_bomb_bytes + eocd + 16);
    if (cdir_offset + 46u > g_bomb_size ||
        read_le32(g_bomb_bytes + cdir_offset) != 0x02014b50u) {
        fatal("bomb zip central directory is not where it says", "");
    }
    local_offset = read_le32(g_bomb_bytes + cdir_offset + 42);
    if (local_offset + 30u > g_bomb_size ||
        read_le32(g_bomb_bytes + local_offset) != 0x04034b50u) {
        fatal("bomb zip local header is not where it says", "");
    }
    /* MZ_ZIP_CDH_DECOMPRESSED_SIZE_OFS / MZ_ZIP_LDH_DECOMPRESSED_SIZE_OFS. */
    write_le32(g_bomb_bytes + cdir_offset + 24, BOMB_DECLARED_SIZE);
    write_le32(g_bomb_bytes + local_offset + 22, BOMB_DECLARED_SIZE);

    write_file(g_bomb_path, g_bomb_bytes, g_bomb_size);
}

/* Two flavours of damage. The first keeps a perfectly good
 * end-of-central-directory record that still points at a central directory
 * half of which is gone, which is what a copy interrupted mid-write looks
 * like. The second removes the record entirely. */
static void build_truncated_zips(void) {
    size_t eocd = find_eocd(g_zip_bytes, g_zip_size);
    size_t cdir_offset;
    size_t cdir_size;
    size_t keep;
    unsigned char *cut;

    if (eocd == (size_t)-1) fatal("content zip has no end-of-central-directory", "");
    cdir_size = read_le32(g_zip_bytes + eocd + 12);
    cdir_offset = read_le32(g_zip_bytes + eocd + 16);
    if (cdir_offset + cdir_size > g_zip_size) {
        fatal("content zip central directory runs past the end", "");
    }

    keep = cdir_offset + cdir_size / 2u;
    cut = (unsigned char *)malloc(keep + 22u);
    if (cut == NULL) fatal("out of memory", "truncated zip");
    memcpy(cut, g_zip_bytes, keep);
    memcpy(cut + keep, g_zip_bytes + eocd, 22u);
    write_file(g_trunc_cdir_path, cut, keep + 22u);
    free(cut);

    write_file(g_trunc_tail_path, g_zip_bytes, eocd + 10u);
}

/* --------------------------------------------------------- bait evidence */

/* Asks miniz directly whether the archive really contains this name, so the
 * traversal assertions cannot pass because the bait was never written. */
static int zip_really_contains(const char *name) {
    mz_zip_archive zip;
    mz_uint32 index = 0;
    int found;

    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, g_zip_bytes, g_zip_size, 0)) return 0;
    found = mz_zip_reader_locate_file_v2(&zip, name, NULL,
                                         MZ_ZIP_FLAG_CASE_SENSITIVE, &index)
                ? 1
                : 0;
    mz_zip_reader_end(&zip);
    return found;
}

/* Performs the exact join an implementation without a validator would perform
 * and opens the result. Returns 1 when that naive join reaches a real file —
 * which is precisely the hole the validator exists to close. */
static int naive_join_opens(const char *rel) {
    char joined[1024];
    FILE *file;
    int written = snprintf(joined, sizeof joined, "%s/%s", g_dir_root, rel);

    if (written < 0 || (size_t)written >= sizeof joined) return 0;
    file = fopen(joined, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

/* ------------------------------------------------------------ fixture set */

static void fixture_build(const char *scratch) {
    static char escape_bait_path[512];
    static char etc_bait_path[512];
    static char backslash_bait_path[512];
    static char drive_bait_path[512];
    char full[1024];
    size_t index;
    int hostile;

    for (index = 0u; index < DEFLATED_SIZE; index++) {
        g_deflated[index] = (unsigned char)('A' + (index % 7u));
    }

    if (strlen(scratch) + 1u > sizeof g_scratch) fatal("scratch path too long", scratch);
    memcpy(g_scratch, scratch, strlen(scratch) + 1u);
    path2(g_dir_root, sizeof g_dir_root, g_scratch, "dirpack");
    path2(g_zip_path, sizeof g_zip_path, g_scratch, "pack.zip");
    path2(g_bomb_path, sizeof g_bomb_path, g_scratch, "bomb.zip");
    path2(g_trunc_cdir_path, sizeof g_trunc_cdir_path, g_scratch,
          "truncated_cdir.zip");
    path2(g_trunc_tail_path, sizeof g_trunc_tail_path, g_scratch,
          "truncated_tail.zip");

    /* `dirpack/../escape.png` and `dirpack/textures/../../escape.png` both
     * land on this one file, so both traversal shapes have somewhere real to
     * arrive at. */
    path2(escape_bait_path, sizeof escape_bait_path, g_scratch, "escape.png");
    /* `dirpack//etc/passwd` collapses to this, so the absolute-path bait is a
     * real file the naive join opens rather than a name that misses. */
    path2(etc_bait_path, sizeof etc_bait_path, g_dir_root, "etc/passwd");
    path2(backslash_bait_path, sizeof backslash_bait_path, g_dir_root,
          "bad\\name.png");
    path2(drive_bait_path, sizeof drive_bait_path, g_dir_root, "C:\\bait.png");

    g_hostile[HOSTILE_ESCAPE].rel = "../escape.png";
    g_hostile[HOSTILE_ESCAPE].zip_added = "../escape.png";
    g_hostile[HOSTILE_ESCAPE].bait = "ESCAPED-PARENT";
    g_hostile[HOSTILE_ESCAPE].dir_bait = escape_bait_path;

    g_hostile[HOSTILE_ABSOLUTE].rel = "/etc/passwd";
    g_hostile[HOSTILE_ABSOLUTE].zip_added = "_etc/passwd";
    g_hostile[HOSTILE_ABSOLUTE].bait = "ESCAPED-ABSOLUTE";
    g_hostile[HOSTILE_ABSOLUTE].dir_bait = etc_bait_path;

    g_hostile[HOSTILE_BACKSLASH].rel = "bad\\name.png";
    g_hostile[HOSTILE_BACKSLASH].zip_added = "bad\\name.png";
    g_hostile[HOSTILE_BACKSLASH].bait = "ESCAPED-BACKSLASH";
    g_hostile[HOSTILE_BACKSLASH].dir_bait = backslash_bait_path;

    g_hostile[HOSTILE_DRIVE].rel = "C:\\bait.png";
    g_hostile[HOSTILE_DRIVE].zip_added = "C:\\bait.png";
    g_hostile[HOSTILE_DRIVE].bait = "ESCAPED-DRIVE";
    g_hostile[HOSTILE_DRIVE].dir_bait = drive_bait_path;

    g_hostile[HOSTILE_NESTED].rel = "textures/../../escape.png";
    g_hostile[HOSTILE_NESTED].zip_added = "textures/../../escape.png";
    g_hostile[HOSTILE_NESTED].bait = "ESCAPED-NESTED";
    g_hostile[HOSTILE_NESTED].dir_bait = escape_bait_path;

    /* The directory pack: the three legitimate files, then the baits. */
    path2(full, sizeof full, g_dir_root, ENTRY_STORED);
    write_file(full, kStored, sizeof kStored);
    path2(full, sizeof full, g_dir_root, ENTRY_DEFLATED);
    write_file(full, g_deflated, sizeof g_deflated);
    path2(full, sizeof full, g_dir_root, ENTRY_PARITY);
    write_file(full, kParity, sizeof kParity);

    /* `textures/` must exist for the nested-traversal join to walk through it
     * the way an unguarded implementation would; the two files above create
     * it, and this asserts as much rather than assuming it. */
    path2(full, sizeof full, g_dir_root, "textures");
    if (!is_directory(full)) fatal("the textures directory was not created", full);

    for (hostile = 0; hostile < HOSTILE_COUNT; hostile++) {
        const char *bait = g_hostile[hostile].bait;
        g_dir_bait_present[hostile] =
            try_write_file(g_hostile[hostile].dir_bait, bait, strlen(bait));
    }

    build_content_zip();
    build_bomb_zip();
    build_truncated_zips();
}

static void fixture_free(void) {
    free(g_zip_bytes);
    free(g_bomb_bytes);
    g_zip_bytes = NULL;
    g_bomb_bytes = NULL;
}

/* ------------------------------------------------------------------ cases */

static MdkrModSource *open_zip(void) {
    MdkrModSource *src = mdkr_mod_source_open(g_zip_path, 1);
    if (src == NULL) fatal("cannot open the fixture archive", g_zip_path);
    return src;
}

static MdkrModSource *open_dir(void) {
    MdkrModSource *src = mdkr_mod_source_open(g_dir_root, 0);
    if (src == NULL) fatal("cannot open the fixture directory", g_dir_root);
    return src;
}

/* B1 */
static void test_stored_and_deflated_round_trip(void) {
    MdkrModSource *src = open_zip();
    unsigned char buffer[DEFLATED_SIZE];
    size_t length = 0;
    mz_zip_archive zip;
    mz_zip_archive_file_stat stat;
    mz_uint32 index = 0;
    int stored_method = -1;
    int deflated_method = -1;

    /* The two entries must genuinely differ in storage method, or "stored and
     * deflated both round trip" is one assertion made twice. */
    memset(&zip, 0, sizeof zip);
    if (mz_zip_reader_init_mem(&zip, g_zip_bytes, g_zip_size, 0)) {
        if (mz_zip_reader_locate_file_v2(&zip, ENTRY_STORED, NULL,
                                         MZ_ZIP_FLAG_CASE_SENSITIVE, &index) &&
            mz_zip_reader_file_stat(&zip, index, &stat)) {
            stored_method = (int)stat.m_method;
        }
        if (mz_zip_reader_locate_file_v2(&zip, ENTRY_DEFLATED, NULL,
                                         MZ_ZIP_FLAG_CASE_SENSITIVE, &index) &&
            mz_zip_reader_file_stat(&zip, index, &stat)) {
            deflated_method = (int)stat.m_method;
        }
        mz_zip_reader_end(&zip);
    }
    expect(stored_method == 0 && deflated_method == MZ_DEFLATED,
           "B1 the fixture really holds one stored and one deflated entry");

    memset(buffer, 0xcd, sizeof buffer);
    expect(mdkr_mod_source_read(src, ENTRY_STORED, buffer, sizeof buffer,
                                &length) == MDKR_MOD_SOURCE_OK &&
           length == sizeof kStored &&
           memcmp(buffer, kStored, sizeof kStored) == 0,
           "B1 a stored entry reads back byte-identically");

    memset(buffer, 0xcd, sizeof buffer);
    expect(mdkr_mod_source_read(src, ENTRY_DEFLATED, buffer, sizeof buffer,
                                &length) == MDKR_MOD_SOURCE_OK &&
           length == sizeof g_deflated &&
           memcmp(buffer, g_deflated, sizeof g_deflated) == 0,
           "B1 a deflated entry reads back byte-identically");

    expect(mdkr_mod_source_has(src, ENTRY_STORED) == 1 &&
           mdkr_mod_source_has(src, ENTRY_DEFLATED) == 1,
           "B1 has() agrees that both entries are servable");

    mdkr_mod_source_close(src);
}

/* B2..B6 all have the same shape: prove the bait is real, then prove both
 * source kinds refuse the name for the same stated reason. */
static void check_hostile(int which, const char *label) {
    MdkrModSource *zip = open_zip();
    MdkrModSource *dir = open_dir();
    const char *rel = g_hostile[which].rel;
    unsigned char buffer[64];
    size_t length = 123u;
    char message[256];

    snprintf(message, sizeof message,
             "%s the archive really contains the bait entry \"%s\"", label, rel);
    expect(zip_really_contains(rel), message);

    snprintf(message, sizeof message,
             "%s the naive directory join really opens a bait file", label);
    if (g_dir_bait_present[which]) {
        expect(naive_join_opens(rel), message);
    } else {
        note("this host filesystem cannot hold that bait name; "
             "the rejection below is still asserted");
    }

    memset(buffer, 0xcd, sizeof buffer);
    snprintf(message, sizeof message, "%s the zip source refuses \"%s\"", label, rel);
    expect(mdkr_mod_source_read(zip, rel, buffer, sizeof buffer, &length) ==
               MDKR_MOD_SOURCE_REJECTED &&
           length == 0u && buffer[0] == 0xcd &&
           mdkr_mod_source_has(zip, rel) == 0,
           message);

    memset(buffer, 0xcd, sizeof buffer);
    snprintf(message, sizeof message,
             "%s the directory source refuses \"%s\"", label, rel);
    expect(mdkr_mod_source_read(dir, rel, buffer, sizeof buffer, &length) ==
               MDKR_MOD_SOURCE_REJECTED &&
           length == 0u && buffer[0] == 0xcd &&
           mdkr_mod_source_has(dir, rel) == 0,
           message);

    mdkr_mod_source_close(zip);
    mdkr_mod_source_close(dir);
}

/* B7 */
static void test_truncated_central_directory(void) {
    expect(mdkr_mod_source_open(g_trunc_cdir_path, 1) == NULL,
           "B7 an archive whose central directory is half gone fails to open");
    expect(mdkr_mod_source_open(g_trunc_tail_path, 1) == NULL,
           "B7 an archive with no end-of-central-directory fails to open");
    expect(mdkr_mod_source_open(g_dir_root, 1) == NULL,
           "B7 a directory opened as an archive fails rather than half-works");
}

/* B8 */
static void test_zip_bomb_is_refused(void) {
    MdkrModSource *src;
    unsigned char buffer[64];
    size_t length = 123u;
    mz_zip_archive zip;
    mz_zip_archive_file_stat stat;
    mz_uint32 index = 0;
    int declared_matches = 0;

    /* The forgery has to be a forgery: a reader with no cap must believe this
     * entry is nearly 4 GiB, otherwise the refusal below proves nothing. */
    memset(&zip, 0, sizeof zip);
    if (mz_zip_reader_init_mem(&zip, g_bomb_bytes, g_bomb_size, 0)) {
        if (mz_zip_reader_locate_file_v2(&zip, ENTRY_BOMB, NULL,
                                         MZ_ZIP_FLAG_CASE_SENSITIVE, &index) &&
            mz_zip_reader_file_stat(&zip, index, &stat)) {
            declared_matches = stat.m_uncomp_size == BOMB_DECLARED_SIZE;
        }
        mz_zip_reader_end(&zip);
    }
    expect(declared_matches,
           "B8 the bomb entry really declares nearly 4 GiB uncompressed");
    expect(BOMB_DECLARED_SIZE > MDKR_MOD_SOURCE_ENTRY_MAX,
           "B8 and that is above the per-entry cap");

    src = mdkr_mod_source_open(g_bomb_path, 1);
    if (src == NULL) fatal("cannot open the bomb archive", g_bomb_path);

    memset(buffer, 0xcd, sizeof buffer);
    expect(mdkr_mod_source_read(src, ENTRY_BOMB, buffer, sizeof buffer,
                                &length) == MDKR_MOD_SOURCE_TOO_LARGE,
           "B8 reading it is refused for being too large");
    /* Not the declared size: reporting it would invite the caller to allocate
     * it, which is the whole point of the attack. */
    expect(length == 0u,
           "B8 and the refusal reports no size for the caller to allocate");
    expect(buffer[0] == 0xcd, "B8 and nothing was written into the buffer");
    expect(mdkr_mod_source_has(src, ENTRY_BOMB) == 0,
           "B8 has() reports it as unservable rather than available");

    mdkr_mod_source_close(src);
}

/* B9 */
static void test_directory_and_zip_agree(void) {
    MdkrModSource *zip = open_zip();
    MdkrModSource *dir = open_dir();
    static const char *const shared[] = { ENTRY_STORED, ENTRY_DEFLATED,
                                          ENTRY_PARITY };
    size_t which;

    for (which = 0u; which < sizeof shared / sizeof shared[0]; which++) {
        unsigned char from_zip[DEFLATED_SIZE];
        unsigned char from_dir[DEFLATED_SIZE];
        size_t zip_length = 0;
        size_t dir_length = 0;
        char message[256];
        int ok;

        memset(from_zip, 0xa1, sizeof from_zip);
        memset(from_dir, 0xb2, sizeof from_dir);
        ok = mdkr_mod_source_read(zip, shared[which], from_zip, sizeof from_zip,
                                  &zip_length) == MDKR_MOD_SOURCE_OK &&
             mdkr_mod_source_read(dir, shared[which], from_dir, sizeof from_dir,
                                  &dir_length) == MDKR_MOD_SOURCE_OK &&
             zip_length == dir_length &&
             memcmp(from_zip, from_dir, zip_length) == 0;
        snprintf(message, sizeof message,
                 "B9 \"%s\" is byte-identical through both source kinds",
                 shared[which]);
        expect(ok, message);
    }

    expect(mdkr_mod_source_has(zip, ENTRY_PARITY) ==
               mdkr_mod_source_has(dir, ENTRY_PARITY),
           "B9 has() answers the same for both source kinds");

    mdkr_mod_source_close(zip);
    mdkr_mod_source_close(dir);
}

/* B10 */
static void test_short_buffer_refuses_and_reports(void) {
    MdkrModSource *zip = open_zip();
    MdkrModSource *dir = open_dir();
    unsigned char small[8];
    size_t length = 0;

    memset(small, 0xcd, sizeof small);
    expect(mdkr_mod_source_read(zip, ENTRY_STORED, small, sizeof small,
                                &length) == MDKR_MOD_SOURCE_BUFFER_TOO_SMALL,
           "B10 a zip read into an undersized buffer is refused");
    expect(length == sizeof kStored,
           "B10 and reports the size the caller actually needs");
    expect(small[0] == 0xcd && small[sizeof small - 1] == 0xcd,
           "B10 and wrote no truncated prefix into the buffer");

    memset(small, 0xcd, sizeof small);
    length = 0;
    expect(mdkr_mod_source_read(dir, ENTRY_STORED, small, sizeof small,
                                &length) == MDKR_MOD_SOURCE_BUFFER_TOO_SMALL &&
           length == sizeof kStored && small[0] == 0xcd,
           "B10 the directory source refuses the same read the same way");

    length = 0;
    expect(mdkr_mod_source_read(zip, ENTRY_STORED, NULL, 0, &length) ==
               MDKR_MOD_SOURCE_BUFFER_TOO_SMALL && length == sizeof kStored,
           "B10 a null buffer is a zero capacity, not a crash");

    mdkr_mod_source_close(zip);
    mdkr_mod_source_close(dir);
}

/* B11 */
static void test_absent_is_not_an_error(void) {
    MdkrModSource *zip = open_zip();
    MdkrModSource *dir = open_dir();
    unsigned char buffer[64];
    size_t length = 123u;

    memset(buffer, 0xcd, sizeof buffer);
    expect(mdkr_mod_source_read(zip, ENTRY_ABSENT, buffer, sizeof buffer,
                                &length) == MDKR_MOD_SOURCE_ABSENT &&
           length == 0u,
           "B11 a name the archive does not hold reads back as absent");
    length = 123u;
    expect(mdkr_mod_source_read(dir, ENTRY_ABSENT, buffer, sizeof buffer,
                                &length) == MDKR_MOD_SOURCE_ABSENT &&
           length == 0u,
           "B11 and so does a name the directory does not hold");
    expect(MDKR_MOD_SOURCE_ABSENT != MDKR_MOD_SOURCE_IO_ERROR &&
           MDKR_MOD_SOURCE_ABSENT != MDKR_MOD_SOURCE_REJECTED &&
           MDKR_MOD_SOURCE_ABSENT != MDKR_MOD_SOURCE_OK,
           "B11 absent is its own answer, distinct from an error");
    expect(mdkr_mod_source_has(zip, ENTRY_ABSENT) == 0 &&
           mdkr_mod_source_has(dir, ENTRY_ABSENT) == 0,
           "B11 has() agrees it is not there");
    expect(mdkr_mod_source_result_text(MDKR_MOD_SOURCE_ABSENT) != NULL &&
           strcmp(mdkr_mod_source_result_text(MDKR_MOD_SOURCE_ABSENT),
                  mdkr_mod_source_result_text(MDKR_MOD_SOURCE_REJECTED)) != 0,
           "B11 and each answer has its own human-readable reason");

    mdkr_mod_source_close(zip);
    mdkr_mod_source_close(dir);
}

static void test_open_and_close_are_total(void) {
    expect(mdkr_mod_source_open(NULL, 0) == NULL &&
           mdkr_mod_source_open("", 1) == NULL,
           "an empty or null path opens nothing");
    mdkr_mod_source_close(NULL);
    expect(mdkr_mod_source_has(NULL, ENTRY_STORED) == 0,
           "a null source holds nothing");
}

int main(int argc, char **argv) {
    const char *scratch = argc > 1 ? argv[1] : "mod_source_scratch";

    remove_tree(scratch);
    fixture_build(scratch);

    test_stored_and_deflated_round_trip();
    check_hostile(HOSTILE_ESCAPE, "B2");
    check_hostile(HOSTILE_ABSOLUTE, "B3");
    check_hostile(HOSTILE_BACKSLASH, "B4");
    check_hostile(HOSTILE_DRIVE, "B5");
    check_hostile(HOSTILE_NESTED, "B6");
    test_truncated_central_directory();
    test_zip_bomb_is_refused();
    test_directory_and_zip_agree();
    test_short_buffer_refuses_and_reports();
    test_absent_is_not_an_error();
    test_open_and_close_are_total();

    fixture_free();
    remove_tree(scratch);

    printf(failures ? "FAILURES: %d\n" : "all mod source assertions passed\n",
           failures);
    return failures ? 1 : 0;
}
