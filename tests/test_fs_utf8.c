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

#ifdef _WIN32
static int visit_unicode_file(const char *name, int directory, void *user) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    expect(!directory && !strcmp(name, (const char *)user),
           "handle enumeration preserves the Unicode filename");
    return 1;
}

static void test_directory_capability(const char *root, const char *filename) {
    const char *bad[] = {NULL, "", "..", "../payload", "a/b", "a\\b", "C:payload"};
    intptr_t handle = mdkr_directory_handle_open_utf8(root);
    intptr_t child;
    FILE *file;
    char bytes[4];
    expect(handle != -1, "retain a Unicode extended-length directory handle");
    if (handle == -1) return;
    child = mdkr_directory_handle_child_utf8(handle, filename, 0);
    expect(child != -1, "open a Unicode child relative to the retained handle");
    file = mdkr_read_handle_stream(child);
    expect(file && fread(bytes, 1, sizeof bytes, file) == sizeof bytes &&
               !memcmp(bytes, "mdkr", sizeof bytes),
           "read bytes through the handle-backed CRT stream");
    if (file) fclose(file);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        child = mdkr_directory_handle_child_utf8(handle, bad[i], 0);
        expect(child == -1, "child capability rejects path syntax");
        mdkr_directory_handle_close(child);
        child = mdkr_directory_handle_create_utf8(handle, bad[i], 0);
        expect(child == -1, "exclusive creation rejects path syntax");
        mdkr_directory_handle_close(child);
    }
    child = mdkr_directory_handle_create_utf8(handle, filename, 0);
    expect(child == -1 && errno == EEXIST,
           "exclusive handle creation refuses an existing payload");
    mdkr_directory_handle_close(child);
    for (int pass = 0; pass < 2; ++pass) {
        child = mdkr_directory_handle_child_utf8(handle, ".", 1);
        expect(child != -1 &&
                   mdkr_directory_handle_visit(child, visit_unicode_file, (void *)filename) == 2,
               "independent Unicode enumeration propagates visitor stop");
        mdkr_directory_handle_close(child);
    }
    {
        const char *created = "created-\xe6\x96\xb0.bin";
        char created_path[2048];
        snprintf(created_path, sizeof created_path, "%s/%s", root, created);
        child = mdkr_directory_handle_create_utf8(handle, created, 0);
        expect(child != -1, "exclusive relative creation accepts a Unicode filename");
        file = mdkr_write_handle_stream(child);
        expect(file != NULL, "created handle transfers into a write stream");
        if (file) {
            expect(fwrite("data", 1, 4, file) == 4, "owned stream writes payload");
            expect(fclose(file) == 0, "owned stream flushes and closes successfully");
        }
        child = mdkr_directory_handle_child_utf8(handle, created, 0);
        file = mdkr_read_handle_stream(child);
        expect(file && fread(bytes, 1, sizeof bytes, file) == sizeof bytes &&
                   !memcmp(bytes, "data", sizeof bytes),
               "created payload reads back through parent capability");
        if (file) fclose(file);
        expect(mdkr_remove_utf8(created_path) == 0, "remove created fixture payload");
        child = mdkr_directory_handle_child_utf8(handle, ".", 1);
        expect(mdkr_write_handle_stream(child) == NULL,
               "write-stream conversion consumes and rejects a directory handle");
        expect(mdkr_write_handle_stream(-1) == NULL, "invalid write handle is refused");
    }
    {
        intptr_t renamed;
        child = mdkr_directory_handle_create_utf8(handle, "rename-source.bin", 0);
        expect(child != -1, "create handle-owned rename fixture");
        if (child != -1) {
            expect(mdkr_handle_rename_child_utf8(child, handle, filename) == -1 && errno == EEXIST,
                   "handle rename refuses to replace an existing payload");
            expect(mdkr_handle_rename_child_utf8(child, handle, "../escape.bin") == -1,
                   "handle rename rejects traversal");
            expect(mdkr_handle_rename_child_utf8(child, handle, "renamed.bin") == 0,
                   "handle rename publishes within pinned parent");
            renamed = mdkr_directory_handle_child_utf8(handle, "renamed.bin", 0);
            expect(renamed != -1, "renamed fixture is visible through parent handle");
            mdkr_directory_handle_close(renamed);
            expect(mdkr_handle_mark_delete(child) == 0,
                   "delete disposition targets the owned renamed file");
            mdkr_directory_handle_close(child);
            renamed = mdkr_directory_handle_child_utf8(handle, "renamed.bin", 0);
            expect(renamed == -1 && errno == ENOENT,
                   "closing deleted fixture removes its directory entry");
            mdkr_directory_handle_close(renamed);
        }
        expect(mdkr_handle_mark_delete(-1) == -1,
               "invalid deletion handle is refused");
    }
    mdkr_directory_handle_close(handle);
}
#endif

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
    int child_exit = -1;
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
#if defined(_WIN32)
    {
        const char *const arguments[] = {"/D", "/C", "exit 7", NULL};
        expect(mdkr_spawn_wait_utf8("cmd.exe", arguments, &child_exit) == 0 &&
                   child_exit == 7,
               "spawn and reap a child with an exact nonzero exit status");
    }
#elif !defined(__EMSCRIPTEN__)
    expect(mdkr_spawn_wait_utf8("false", NULL, &child_exit) == 0 &&
               child_exit != 0,
           "spawn and reap a child with an exact nonzero exit status");
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
        char suffix[64];
        const size_t prefix = strlen(directories[depth - 1]);
        const int count = snprintf(suffix, sizeof suffix,
            "/segment_0123456789_0123456789_0123456789_%d", depth);
        if (count < 0 || (size_t)count >= sizeof suffix ||
            prefix + (size_t)count + 1 > sizeof directories[depth]) {
            expect(0, "extended-length fixture path fits its buffer");
            return 1;
        }
        memcpy(directories[depth], directories[depth - 1], prefix);
        memcpy(directories[depth] + prefix, suffix, (size_t)count + 1);
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
#ifdef _WIN32
    test_directory_capability(directories[depth - 1], rom_suffix + 1);
#endif
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
     * video_config_runtime.c, user_paths.c, text_state_file.c). On Windows
     * the C11 'x' flag is a UCRT feature: the legacy msvcrt CRT rejects the
     * whole mode string with EINVAL, which silently turns EVERY launcher
     * save into a failure — the withdrawn v1.2.1 Windows zip shipped exactly
     * that (issue #32). This block must pass on the exact CRT the shipped
     * executable links. */
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
