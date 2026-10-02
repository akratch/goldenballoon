/**
 * mdkr_set_aside_utf8 (fs_utf8.h): how a damaged controller-pak image or a
 * retired ghost record is moved out of service -- renamed to the first free
 * "<name>.bad.N", never deleted, never written over an earlier one.
 *
 * The browser build needs this as much as the desktop does, and its file
 * system has no hard links, so the no-replace move it rests on has a browser
 * arm of its own. Build this test with emcc and run it under node to cover
 * that arm on Emscripten's in-memory file system (the one the browser's
 * IndexedDB save mount sits on); ctest covers the native arms.
 */
#include "fs_utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <wchar.h>
#include <windows.h>
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

static void join(char *output, size_t size, const char *base,
                 const char *name) {
    const size_t length = strlen(base);
    const int trailing =
        length > 0 && (base[length - 1] == '/' || base[length - 1] == '\\');
    if (snprintf(output, size, trailing ? "%s%s" : "%s/%s", base, name) >=
        (int)size) {
        printf("FAIL: fixture path fits its buffer\n");
        exit(1);
    }
}

static void slot_name(char *output, size_t size, const char *path,
                      int slot) {
    if (snprintf(output, size, "%s.bad.%d", path, slot) >= (int)size) {
        printf("FAIL: slot path fits its buffer\n");
        exit(1);
    }
}

static int write_bytes(const char *path, const char *bytes) {
    FILE *file = mdkr_fopen_utf8(path, "wb");
    int okay = file != NULL &&
               fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes);
    if (file != NULL && fclose(file) != 0) okay = 0;
    return okay;
}

/* 1 when `path` holds exactly `bytes`. */
static int holds(const char *path, const char *bytes) {
    char buffer[64];
    size_t got;
    FILE *file = mdkr_fopen_utf8(path, "rb");
    if (file == NULL) return 0;
    got = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    return got == strlen(bytes) && memcmp(buffer, bytes, got) == 0;
}

static int missing(const char *path) {
    return mdkr_path_kind_utf8(path) == MDKR_PATH_MISSING;
}

int main(void) {
    char temp[1024];
    char root[1200];
    char pak[1300];
    char slot[1320];
    char bad[1320];
    char other[1300];
    char folder[1300];
    int result;
    int index;

    if (!temp_directory(temp, sizeof(temp))) {
        printf("FAIL: no temporary directory\n");
        return 1;
    }
    {
        char name[64];
        snprintf(name, sizeof(name), "mdkr64-set-aside-%ld",
                 (long)
#if defined(_WIN32)
                 _getpid()
#else
                 getpid()
#endif
        );
        join(root, sizeof(root), temp, name);
    }
    expect(mdkr_mkdir_utf8(root) == 0, "create the fixture folder");
    /* The controller pak's own spelling (stubs_dkr.c virtual_pak_paths). */
    join(pak, sizeof(pak), root, "controller-pak-1.mdp");

    /* A damaged image moves to the first slot, bytes intact. */
    expect(write_bytes(pak, "damaged-1"), "write a damaged pak image");
    result = mdkr_set_aside_utf8(pak, bad, sizeof(bad));
    slot_name(slot, sizeof(slot), pak, 1);
    expect(result == 1, "set aside a damaged pak image");
    expect(strcmp(bad, slot) == 0, "it is named after the first free slot");
    expect(missing(pak), "the damaged image is out of service");
    expect(holds(slot, "damaged-1"), "the set-aside copy keeps its bytes");

    /* The next one never overwrites the first: it takes slot 2. */
    expect(write_bytes(pak, "damaged-2"), "write a second damaged image");
    result = mdkr_set_aside_utf8(pak, bad, sizeof(bad));
    slot_name(slot, sizeof(slot), pak, 2);
    expect(result == 1 && strcmp(bad, slot) == 0,
           "a second damaged image takes the next slot");
    slot_name(slot, sizeof(slot), pak, 1);
    expect(holds(slot, "damaged-1"),
           "the earlier set-aside copy is not overwritten");
    slot_name(slot, sizeof(slot), pak, 2);
    expect(holds(slot, "damaged-2"), "the second copy keeps its bytes");
    expect(missing(pak), "the second damaged image is out of service");

#if !defined(_WIN32)
    /* A dangling link in a slot is taken, not followed and not replaced. */
    {
        char target[1300];
        join(target, sizeof(target), root, "nothing-here");
        slot_name(slot, sizeof(slot), pak, 3);
        expect(symlink(target, slot) == 0, "plant a dangling link in slot 3");
        expect(write_bytes(pak, "damaged-3"), "write a third damaged image");
        result = mdkr_set_aside_utf8(pak, bad, sizeof(bad));
        slot_name(slot, sizeof(slot), pak, 4);
        expect(result == 1 && strcmp(bad, slot) == 0,
               "a slot holding a dangling link is skipped");
        expect(holds(slot, "damaged-3"), "the third copy keeps its bytes");
        slot_name(slot, sizeof(slot), pak, 3);
        expect(mdkr_path_kind_utf8(slot) == MDKR_PATH_LINK && missing(target),
               "the dangling link is left exactly as it was");
    }
#endif

    /* The no-replace move every set-aside rests on refuses an existing
     * destination of any kind and leaves both sides as they were. */
    join(other, sizeof(other), root, "other.mdp");
    join(folder, sizeof(folder), root, "folder");
    expect(write_bytes(pak, "source") && write_bytes(other, "destination") &&
               mdkr_mkdir_utf8(folder) == 0,
           "create move fixtures");
    errno = 0;
    expect(mdkr_move_utf8(pak, other, 0, 1) == -1 && errno == EEXIST,
           "a no-replace move refuses an existing file");
    expect(holds(pak, "source") && holds(other, "destination"),
           "the refused move changes neither file");
    errno = 0;
    expect(mdkr_move_utf8(pak, folder, 0, 1) == -1 && errno == EEXIST,
           "a no-replace move refuses an existing folder");
    expect(holds(pak, "source") &&
               mdkr_path_kind_utf8(folder) == MDKR_PATH_DIRECTORY,
           "the refused move leaves the folder a folder");
#if !defined(_WIN32)
    slot_name(slot, sizeof(slot), pak, 3);
    errno = 0;
    expect(mdkr_move_utf8(pak, slot, 0, 1) == -1 && errno == EEXIST,
           "a no-replace move refuses a dangling link");
    expect(mdkr_path_kind_utf8(slot) == MDKR_PATH_LINK && holds(pak, "source"),
           "the refused move leaves the link and the source alone");
#endif
    expect(mdkr_remove_utf8(other) == 0 && mdkr_move_utf8(pak, other, 0, 1) == 0,
           "a no-replace move to a free name succeeds");
    expect(missing(pak) && holds(other, "source"),
           "the moved file keeps its bytes under the new name");
    expect(mdkr_remove_utf8(other) == 0 && mdkr_rmdir_utf8(folder) == 0,
           "remove move fixtures");

    /* Failures are reported, and the file stays where it is. */
    errno = 0;
    expect(mdkr_set_aside_utf8(pak, bad, sizeof(bad)) == -1 &&
               errno == ENOENT,
           "setting aside a missing file is an error");
    expect(missing(bad), "and creates nothing in the slot it tried");
    expect(write_bytes(pak, "damaged-5"), "write a fifth damaged image");
    errno = 0;
    expect(mdkr_set_aside_utf8(pak, bad, strlen(pak) + 6u) == -1 &&
               errno == ENAMETOOLONG,
           "a slot name that does not fit is an error");
    expect(holds(pak, "damaged-5"), "and the file stays in service");
    errno = 0;
    expect(mdkr_set_aside_utf8(NULL, bad, sizeof(bad)) == -1 &&
               errno == EINVAL,
           "a missing path is an error");

    /* With every slot taken the file stays in place, and nothing is
     * overwritten. */
    for (index = 1; index <= 99; ++index) {
        slot_name(slot, sizeof(slot), pak, index);
        if (missing(slot) && !write_bytes(slot, "earlier")) {
            expect(0, "fill every slot");
            break;
        }
    }
    result = mdkr_set_aside_utf8(pak, bad, sizeof(bad));
    expect(result == 0, "with every slot taken nothing is set aside");
    expect(holds(pak, "damaged-5"), "the file stays in service");
    slot_name(slot, sizeof(slot), pak, 99);
    expect(holds(slot, "earlier"), "the last slot is not overwritten");
    slot_name(slot, sizeof(slot), pak, 100);
    expect(missing(slot), "no slot past the last is used");

    /* Clean up. */
    (void)mdkr_remove_utf8(pak);
    for (index = 1; index <= 99; ++index) {
        slot_name(slot, sizeof(slot), pak, index);
        (void)mdkr_unlink_utf8(slot);
    }
    expect(mdkr_rmdir_utf8(root) == 0, "remove the fixture folder");

    if (failures != 0) {
        printf("fs_set_aside: %d failure(s)\n", failures);
        return 1;
    }
    puts("fs_set_aside: PASS");
    return 0;
}
