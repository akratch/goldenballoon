/* The registry's per-asset scan, driven over a real installed-cache directory.
 *
 * This exists for one defect. registry_init() declared `uint8_t
 * animation_moves[64]` and indexed it with entry.stats.animations, which
 * validate_references() admits up to 256, so an asset with 65 or more
 * animations wrote past it -- 192 bytes of the surrounding stack frame at the
 * ceiling. It needs no malice: the asset compiler emits one record per glTF
 * animation with no cap, so an ordinary 65-clip character does it, and
 * registry_init runs at every boot and every roster refresh without the
 * character ever being selected.
 *
 * Nothing could see it. fuzz_modern_character_asset never called registry_init,
 * and check_array_bounds_sweep drives retail routes whose fixtures never exceed
 * 64 animations. This test is the missing instrument: it walks the same public
 * entry point the launcher does, over a fixture carrying exactly one more
 * animation than the old array held. Built with ASan in the sanitizer arm, the
 * pre-fix code reports a stack-buffer-overflow WRITE at the
 * `animation_moves[index] = 1u` line; the fixed code is clean.
 *
 * It also pins what the scan does with the folder itself, on every platform
 * (the Windows test bundle runs it from a folder with non-ASCII letters):
 *   - a characters folder that does not exist yet is the documented empty
 *     state, not "unavailable" (POSIX used to answer -1, because the path
 *     query answered a missing path differently there);
 *   - a path that exists but cannot be listed is unavailable, never empty;
 *   - a folder and a cache whose names are not ASCII are listed exactly (the
 *     narrow opendir()/readdir() it used to use read Windows names in the
 *     ANSI code page, so such a folder could not be listed at all);
 *   - a cache whose file name has no UTF-8 spelling is not loaded but is
 *     listed as skipped, under a readable stand-in, with what to do -- never
 *     dropped without a word (it needs a file system that can hold such a
 *     name: Linux, or NTFS on Windows; macOS and Wine print a SKIP line).
 *
 * Every scratch path is built under the temporary folder and reached through
 * the UTF-8 helpers; Windows reads %TEMP% as UTF-16.
 */
#include "modern_character_registry.h"
#include "fs_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <wchar.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static int failures;

static void expect(int condition, const char *what) {
    printf("%s   %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) failures++;
}

static int temp_directory(char *output, size_t size) {
#ifdef _WIN32
    const wchar_t *wide = _wgetenv(L"TEMP");
    if (wide == NULL || wide[0] == L'\0') wide = _wgetenv(L"TMP");
    if (wide == NULL || wide[0] == L'\0') return 0;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, output, (int)size,
                               NULL, NULL) > 0;
#else
    const char *value = getenv("TMPDIR");
    if (value == NULL || value[0] == '\0') value = "/tmp";
    return snprintf(output, size, "%s", value) < (int)size;
#endif
}

static int join(char *output, size_t size, const char *base, const char *name) {
    const size_t length = strlen(base);
    const int trailing = length > 0 &&
        (base[length - 1] == '/' || base[length - 1] == '\\');
    const int written = snprintf(output, size, trailing ? "%s%s" : "%s/%s",
                                 base, name);
    return written > 0 && (size_t)written < size;
}

static int copy_file(const char *from, const char *to) {
    FILE *in = mdkr_fopen_utf8(from, "rb");
    FILE *out;
    unsigned char buffer[4096];
    size_t got;
    int okay = 1;
    if (in == NULL) return 0;
    out = mdkr_fopen_utf8(to, "wb");
    if (out == NULL) { fclose(in); return 0; }
    while ((got = fread(buffer, 1, sizeof buffer, in)) != 0u) {
        if (fwrite(buffer, 1, got, out) != got) { okay = 0; break; }
    }
    if (ferror(in)) okay = 0;
    fclose(in);
    return fclose(out) == 0 && okay;
}

/* A copy of `fixture` in `folder` under a name with no UTF-8 spelling:
 * `prefix`, then one byte (POSIX) or one unpaired UTF-16 surrogate (Windows)
 * that has none, then `suffix`. Returns 1 when it was made, 0 when this file
 * system cannot hold such a name. */
typedef struct UnnamedFile {
#ifdef _WIN32
    wchar_t path[1400];
#else
    char path[1400];
#endif
    int made;
} UnnamedFile;

static int make_unnamed_copy(const char *fixture, const char *folder,
                             const char *prefix, const char *suffix,
                             UnnamedFile *file) {
    FILE *in = mdkr_fopen_utf8(fixture, "rb");
    unsigned char buffer[4096];
    size_t got;
    int okay = 1;
    file->made = 0;
    if (in == NULL) return 0;
#ifdef _WIN32
    {
        wchar_t wide_prefix[128], wide_suffix[128];
        HANDLE out;
        int length = MultiByteToWideChar(CP_UTF8, 0, folder, -1, file->path, 1200);
        if (length <= 0 ||
            !MultiByteToWideChar(CP_UTF8, 0, prefix, -1, wide_prefix, 128) ||
            !MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 128)) {
            fclose(in);
            return 0;
        }
        for (wchar_t *c = file->path; *c; ++c) if (*c == L'/') *c = L'\\';
        swprintf(file->path + length - 1, 1400 - (size_t)length,
                 L"\\%ls\xD800%ls", wide_prefix, wide_suffix);
        out = CreateFileW(file->path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            fclose(in);
            return 0;
        }
        file->made = 1;
        while ((got = fread(buffer, 1, sizeof buffer, in)) != 0u) {
            DWORD wrote = 0;
            if (!WriteFile(out, buffer, (DWORD)got, &wrote, NULL) || wrote != got) {
                okay = 0;
                break;
            }
        }
        if (!CloseHandle(out)) okay = 0;
    }
#else
    {
        FILE *out;
        if (snprintf(file->path, sizeof file->path, "%s/%s\xff%s", folder,
                     prefix, suffix) >= (int)sizeof file->path) {
            fclose(in);
            return 0;
        }
        out = fopen(file->path, "wb");
        if (out == NULL) {
            fclose(in);
            return 0;
        }
        file->made = 1;
        while ((got = fread(buffer, 1, sizeof buffer, in)) != 0u) {
            if (fwrite(buffer, 1, got, out) != got) { okay = 0; break; }
        }
        if (fclose(out) != 0) okay = 0;
    }
#endif
    fclose(in);
    return okay;
}

static void remove_unnamed(const UnnamedFile *file) {
    if (!file->made) return;
#ifdef _WIN32
    (void)DeleteFileW(file->path);
#else
    (void)remove(file->path);
#endif
}

static int write_text(const char *path, const char *text) {
    FILE *file = mdkr_fopen_utf8(path, "wb");
    int okay = file != NULL && fwrite(text, 1, strlen(text), file) == strlen(text);
    if (file != NULL && fclose(file) != 0) okay = 0;
    return okay;
}

int main(int argc, char **argv) {
    static const char unicode_folder[] =
        "characters-Gr\xC3\xBC\xC3\x9F" "e-\xE8\xB7\xAF\xE5\xBE\x84";
    static const char unicode_cache[] = "pr\xC3\xB6" "be-\xE5\xAD\x97.mdkc";
    MdkrModernCharacterRegistry registry;
    char temp[1024];
    char name[128];
    char root[1200];
    char directory[1300];
    char installed[1400];
    char missing[1300];
    char not_a_folder[1300];
    char unicode_directory[1300];
    char unicode_installed[1400];
    char revision[1600];
    char unnamed_directory[1300];
    char unnamed_neighbour[1400];
    UnnamedFile unnamed_cache;
    UnnamedFile unnamed_note;
    const char *fixture;
    int revision_written = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <sixtyfive-animations.mdkc>\n", argv[0]);
        return 2;
    }
    fixture = argv[1];
#ifdef _WIN32
    snprintf(name, sizeof name, "mdkr_registry_test_%ld", (long)_getpid());
#else
    snprintf(name, sizeof name, "mdkr_registry_test_%ld", (long)getpid());
#endif
    if (!temp_directory(temp, sizeof temp) ||
        !join(root, sizeof root, temp, name) ||
        !join(directory, sizeof directory, root, "installed") ||
        !join(installed, sizeof installed, directory, "probe.mdkc") ||
        !join(missing, sizeof missing, root, "not-created-yet") ||
        !join(not_a_folder, sizeof not_a_folder, root, "a-file") ||
        !join(unicode_directory, sizeof unicode_directory, root,
              unicode_folder) ||
        !join(unicode_installed, sizeof unicode_installed, unicode_directory,
              unicode_cache) ||
        !join(unnamed_directory, sizeof unnamed_directory, root, "unnamed") ||
        !join(unnamed_neighbour, sizeof unnamed_neighbour, unnamed_directory,
              "probe.mdkc")) {
        fprintf(stderr, "could not name the scratch folders\n");
        return 2;
    }
    if (mdkr_mkdir_utf8(root) != 0 || mdkr_mkdir_utf8(directory) != 0 ||
        mdkr_mkdir_utf8(unicode_directory) != 0) {
        fprintf(stderr, "could not create the scratch folders under %s\n", temp);
        return 2;
    }
    if (!copy_file(fixture, installed) ||
        !copy_file(fixture, unicode_installed) ||
        !write_text(not_a_folder, "not a folder")) {
        fprintf(stderr, "could not stage %s\n", fixture);
        return 2;
    }

    memset(&registry, 0, sizeof registry);
    expect(mdkr_modern_character_registry_init(&registry, directory) == 0,
           "an existing characters folder is scanned");

    /* The point is that the scan COMPLETES over an asset with more animations
     * than the old local held. A skip would mean the fixture never reached the
     * loop, and the test would be proving nothing. */
    expect(mdkr_modern_character_registry_count(&registry) == 1,
           "a 65-animation character is admitted by the registry scan");
    expect(mdkr_modern_character_registry_skipped(&registry) == 0,
           "and is not skipped before the animation walk runs");

    memset(&registry, 0x5a, sizeof registry);
    expect(mdkr_modern_character_registry_init(&registry, missing) == 0 &&
               mdkr_modern_character_registry_count(&registry) == 0 &&
               mdkr_modern_character_registry_skipped(&registry) == 0,
           "a characters folder that does not exist yet is empty, not unavailable");
    memset(&registry, 0x5a, sizeof registry);
    expect(mdkr_modern_character_registry_init_inventory(&registry, missing) == 0 &&
               mdkr_modern_character_registry_count(&registry) == 0,
           "the inventory of a missing characters folder is empty, not unavailable");
    expect(mdkr_modern_character_registry_init(&registry, not_a_folder) == -1,
           "a path that exists but cannot be listed is unavailable, not empty");

    memset(&registry, 0, sizeof registry);
    expect(mdkr_modern_character_registry_init(&registry, unicode_directory) == 0 &&
               mdkr_modern_character_registry_count(&registry) == 1 &&
               mdkr_modern_character_registry_skipped(&registry) == 0,
           "a non-ASCII characters folder and cache name are scanned exactly");
    if (mdkr_modern_character_registry_count(&registry) == 1) {
        const MdkrModernCharacterEntry *entry =
            mdkr_modern_character_registry_entry(&registry, 0);
        char leaf[256];
        /* A retained source revision: <id>.<64 hex digits>.mdkrchar. The
         * inventory counts it in a second listing of the same folder. */
        revision_written =
            snprintf(leaf, sizeof leaf, "%s.%064d.mdkrchar", entry->id, 0) > 0 &&
            join(revision, sizeof revision, unicode_directory, leaf) &&
            write_text(revision, "retained source");
        expect(revision_written, "stage a retained source revision");
        memset(&registry, 0, sizeof registry);
        expect(mdkr_modern_character_registry_init_inventory(
                   &registry, unicode_directory) == 0 &&
                   mdkr_modern_character_registry_count(&registry) == 1 &&
                   mdkr_modern_character_registry_entry(&registry, 0)
                           ->source_revisions == 1u,
               "the inventory of a non-ASCII folder counts its retained source");
    }

    memset(&unnamed_cache, 0, sizeof unnamed_cache);
    memset(&unnamed_note, 0, sizeof unnamed_note);
    expect(mdkr_mkdir_utf8(unnamed_directory) == 0 &&
               copy_file(fixture, unnamed_neighbour),
           "stage a folder for a cache whose name has no UTF-8 spelling");
    if (!make_unnamed_copy(fixture, unnamed_directory, "lost-", ".mdkc",
                           &unnamed_cache)) {
        printf("SKIP   this file system cannot hold a name with no UTF-8 "
               "spelling\n");
    } else {
        const char *skip_name;
        const char *skip_reason;
        expect(make_unnamed_copy(fixture, unnamed_directory, "notes-", ".txt",
                                 &unnamed_note),
               "stage a file that is not a cache under such a name");
        memset(&registry, 0, sizeof registry);
        expect(mdkr_modern_character_registry_init(&registry,
                                                   unnamed_directory) == 0 &&
                   mdkr_modern_character_registry_count(&registry) == 1,
               "a cache whose name has no UTF-8 spelling does not stop the "
               "others loading");
        skip_name = mdkr_modern_character_registry_skip_name(&registry, 0);
        skip_reason = mdkr_modern_character_registry_skip_reason(&registry, 0);
        expect(mdkr_modern_character_registry_skipped(&registry) == 1 &&
                   skip_name != NULL && skip_reason != NULL &&
                   strncmp(skip_name, "lost-", 5) == 0 &&
                   strstr(skip_name, "\xEF\xBF\xBD") != NULL &&
                   strstr(skip_name, ".mdkc") != NULL &&
                   strstr(skip_reason, "not valid Unicode") != NULL &&
                   strstr(skip_reason, "rename") != NULL,
               "it is listed as skipped under a readable stand-in, with what "
               "to do, and a non-cache file under such a name is not listed");
        memset(&registry, 0, sizeof registry);
        expect(mdkr_modern_character_registry_init_inventory(
                   &registry, unnamed_directory) == 0 &&
                   mdkr_modern_character_registry_count(&registry) == 1 &&
                   mdkr_modern_character_registry_skipped(&registry) == 1,
               "the inventory lists it as skipped too");
    }
    remove_unnamed(&unnamed_note);
    remove_unnamed(&unnamed_cache);
    (void)mdkr_remove_utf8(unnamed_neighbour);
    (void)mdkr_rmdir_utf8(unnamed_directory);

    if (revision_written) (void)mdkr_remove_utf8(revision);
    (void)mdkr_remove_utf8(unicode_installed);
    (void)mdkr_rmdir_utf8(unicode_directory);
    (void)mdkr_remove_utf8(not_a_folder);
    (void)mdkr_remove_utf8(installed);
    (void)mdkr_rmdir_utf8(directory);
    (void)mdkr_rmdir_utf8(root);
    if (failures != 0) {
        fprintf(stderr, "modern character registry: %d failure(s)\n", failures);
        return 1;
    }
    puts("modern character registry: PASS -- the animation walk survives a "
         "count past the old fixed bound");
    return 0;
}
