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
 * It also pins what the scan does with the folder itself, on every platform:
 *   - a characters folder that does not exist yet is the documented empty
 *     state, not "unavailable" (POSIX used to answer -1, because the path
 *     query answered a missing path differently there);
 *   - a path that exists but cannot be listed is unavailable, never empty.
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

static int write_text(const char *path, const char *text) {
    FILE *file = mdkr_fopen_utf8(path, "wb");
    int okay = file != NULL && fwrite(text, 1, strlen(text), file) == strlen(text);
    if (file != NULL && fclose(file) != 0) okay = 0;
    return okay;
}

int main(int argc, char **argv) {
    MdkrModernCharacterRegistry registry;
    char temp[1024];
    char name[128];
    char root[1200];
    char directory[1300];
    char installed[1400];
    char missing[1300];
    char not_a_folder[1300];
    const char *fixture;

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
        !join(not_a_folder, sizeof not_a_folder, root, "a-file")) {
        fprintf(stderr, "could not name the scratch folders\n");
        return 2;
    }
    if (mdkr_mkdir_utf8(root) != 0 || mdkr_mkdir_utf8(directory) != 0) {
        fprintf(stderr, "could not create the scratch folders under %s\n", temp);
        return 2;
    }
    if (!copy_file(fixture, installed) ||
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
