#include "fs_utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

static int failures;

static void expect(int condition, const char *message) {
    if (condition) {
        printf("ok: %s\n", message);
    } else {
        printf("FAIL: %s (errno=%d)\n", message, errno);
        failures++;
    }
}

int main(void) {
    char root[128];
    char directories[8][1024];
    char path[1024];
    char moved[1024];
    int depth = 0;
    int exists = 0;
    int regular = 0;
    int directory = 0;
    FILE *file;
    char bytes[8] = {0};
    char *running_executable = (char *)"sentinel";
    static const char rom_suffix[] =
        "/ROM_\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88.z64";
    static const char moved_suffix[] = "/moved_\xC3\xA9.bin";

#if defined(_WIN32)
    const long process_id = (long)_getpid();
#else
    const long process_id = (long)getpid();
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    /* Every platform the app ships on can name its own image, and relaunch
     * depends on that answer rather than on argv[0]. */
    expect(mdkr_running_executable_path_utf8(&running_executable) == 0 &&
               running_executable != NULL && running_executable[0] != '\0',
           "resolve the running executable into dynamically sized UTF-8 storage");
#if !defined(_WIN32)
    expect(running_executable != NULL && running_executable[0] == '/',
           "the resolved executable path is absolute, not a PATH lookup");
#endif
    free(running_executable);
    running_executable = NULL;
#else
    expect(mdkr_running_executable_path_utf8(&running_executable) != 0 &&
               running_executable == NULL,
           "unsupported executable-path query clears its output safely");
#endif
    snprintf(root, sizeof(root),
             "fs_utf8_Gr\xC3\xBC\xC3\x9F" "e_\xE8\xB7\xAF\xE5\xBE\x84_%ld",
             process_id);

    (void)mdkr_rmdir_utf8(root);
    expect(mdkr_mkdir_utf8(root) == 0, "create a Unicode directory");
    snprintf(directories[depth++], sizeof(directories[0]), "%s", root);
    while (strlen(directories[depth - 1]) < 320u && depth < 8) {
        snprintf(directories[depth], sizeof(directories[depth]),
                 "%s/segment_0123456789_0123456789_0123456789_%d",
                 directories[depth - 1], depth);
        expect(mdkr_mkdir_utf8(directories[depth]) == 0,
               "create an extended-length path segment");
        depth++;
    }
    {
        const size_t directory_length = strlen(directories[depth - 1]);
        expect(directory_length + sizeof(rom_suffix) <= sizeof(path),
               "Unicode test path fits its buffer");
        expect(directory_length + sizeof(moved_suffix) <= sizeof(moved),
               "moved test path fits its buffer");
        if (directory_length + sizeof(rom_suffix) > sizeof(path) ||
            directory_length + sizeof(moved_suffix) > sizeof(moved)) {
            return 1;
        }
        memcpy(path, directories[depth - 1], directory_length);
        memcpy(path + directory_length, rom_suffix, sizeof(rom_suffix));
        memcpy(moved, directories[depth - 1], directory_length);
        memcpy(moved + directory_length, moved_suffix, sizeof(moved_suffix));
    }

    file = mdkr_fopen_utf8(path, "wb");
    expect(file != NULL, "open a Unicode extended-length file for writing");
    if (file) {
        expect(fwrite("mdkr", 1, 4, file) == 4 && fclose(file) == 0,
               "write and close the file");
    }
    expect(mdkr_path_query_utf8(path, &exists, &regular, &directory) == 0 &&
               exists && regular && !directory,
           "query the Unicode file");
    expect(mdkr_move_utf8(path, moved, 0, 1) == 0,
           "move the Unicode file without replacement");
    file = mdkr_fopen_utf8(moved, "rb");
    expect(file != NULL && fread(bytes, 1, 4, file) == 4 &&
               memcmp(bytes, "mdkr", 4) == 0,
           "read the moved file exactly");
    if (file) fclose(file);

    file = mdkr_fopen_utf8(path, "wb");
    if (file) {
        (void)fwrite("new", 1, 3, file);
        (void)fclose(file);
    }
    errno = 0;
    expect(mdkr_move_utf8(path, moved, 0, 1) != 0,
           "no-replace move refuses an existing destination");
    file = mdkr_fopen_utf8(moved, "rb");
    memset(bytes, 0, sizeof(bytes));
    expect(file != NULL && fread(bytes, 1, 4, file) == 4 &&
               memcmp(bytes, "mdkr", 4) == 0,
           "failed no-replace move preserves the destination");
    if (file) fclose(file);
    expect(mdkr_move_utf8(path, moved, 1, 1) == 0,
           "replace move atomically installs the source");
    file = mdkr_fopen_utf8(moved, "rb");
    memset(bytes, 0, sizeof(bytes));
    expect(file != NULL && fread(bytes, 1, 3, file) == 3 &&
               memcmp(bytes, "new", 3) == 0,
           "replace move exposes the new contents");
    if (file) fclose(file);
    expect(mdkr_remove_utf8(moved) == 0, "remove the Unicode file");

    /* Exclusive create ("wbx") is the foundation of every atomic settings,
     * preferences, and state write in the tree (app_config.cpp,
     * video_config_runtime.c, user_paths.c, taj_mod_state_file.c). The C11
     * 'x' flag is a UCRT feature on Windows: a toolchain that links the
     * legacy msvcrt CRT rejects the whole mode string, which silently turns
     * EVERY launcher save into a failure. This block must pass on the exact
     * CRT the shipped executable links. */
    errno = 0;
    file = mdkr_fopen_utf8(path, "wbx");
    expect(file != NULL, "exclusive create succeeds on a fresh path");
    if (file) {
        expect(fwrite("excl", 1, 4, file) == 4 && fclose(file) == 0,
               "write and close the exclusively created file");
    }
    errno = 0;
    expect(mdkr_fopen_utf8(path, "wbx") == NULL,
           "exclusive create refuses an existing file");
    file = mdkr_fopen_utf8(path, "rb");
    memset(bytes, 0, sizeof(bytes));
    expect(file != NULL && fread(bytes, 1, 4, file) == 4 &&
               memcmp(bytes, "excl", 4) == 0,
           "refused exclusive create leaves the original file intact");
    if (file) fclose(file);
    expect(mdkr_remove_utf8(path) == 0, "remove the exclusive-create file");

    while (depth > 0) {
        depth--;
        expect(mdkr_rmdir_utf8(directories[depth]) == 0,
               "remove an extended-length directory");
    }
    if (failures) {
        printf("%d UTF-8 filesystem failure(s)\n", failures);
        return 1;
    }
    printf("UTF-8 filesystem boundary passed\n");
    return 0;
}
