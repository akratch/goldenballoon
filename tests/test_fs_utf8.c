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
#include <sys/stat.h>
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

/* Every scratch path lives under the temp directory, so the test runs from any
 * working directory. Windows reads %TEMP% as UTF-16 because the narrow
 * environment is in the active code page, not UTF-8. */
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

static void join(char *output, size_t size, const char *base, const char *name) {
    const size_t length = strlen(base);
    const int trailing = length > 0 && (base[length - 1] == '/' || base[length - 1] == '\\');
    if (snprintf(output, size, trailing ? "%s%s" : "%s/%s", base, name) >= (int)size) {
        printf("FAIL: fixture path fits its buffer\n");
        exit(1);
    }
}

static int write_bytes(const char *path, const char *bytes) {
    FILE *file = mdkr_fopen_utf8(path, "wb");
    int okay = file != NULL && fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes);
    if (file != NULL && fclose(file) != 0) okay = 0;
    return okay;
}

/* 1 when a directory link was created, 0 when this host cannot make one.
 * Windows needs Developer Mode (or the privilege) for a symbolic link; a
 * junction needs neither, so it is the fallback. */
static int make_directory_link(const char *target, const char *link) {
#ifdef _WIN32
    wchar_t wide_target[1024], wide_link[1024], command[2200];
    if (!MultiByteToWideChar(CP_UTF8, 0, target, -1, wide_target, 1024) ||
        !MultiByteToWideChar(CP_UTF8, 0, link, -1, wide_link, 1024)) return 0;
    for (wchar_t *c = wide_target; *c; ++c) if (*c == L'/') *c = L'\\';
    for (wchar_t *c = wide_link; *c; ++c) if (*c == L'/') *c = L'\\';
    if (CreateSymbolicLinkW(wide_link, wide_target, SYMBOLIC_LINK_FLAG_DIRECTORY |
                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) return 1;
    swprintf(command, sizeof command / sizeof command[0],
             L"mklink /J \"%ls\" \"%ls\" >NUL", wide_link, wide_target);
    return _wsystem(command) == 0 &&
           GetFileAttributesW(wide_link) != INVALID_FILE_ATTRIBUTES;
#else
    return symlink(target, link) == 0;
#endif
}

#if defined(_WIN32) && defined(MDKR_FS_UTF8_TESTING)
/* fs_utf8.c's test seam: 1 refuses the listing's attribute query on its
 * handle, 2 its directory query, as a file system that does not implement
 * them would (FAT/exFAT and some network redirectors and filter drivers). */
extern int mdkr_fs_utf8_test_refused_query;
#endif
#ifdef MDKR_FS_UTF8_TESTING
/* fs_utf8.c's test seam: at 0 or more a collected listing stops with EIO once
 * it holds that many names. */
extern int mdkr_fs_utf8_test_names_fail_after;
#endif

typedef struct Listing {
    char names[8][64];
    int kinds[8];
    int count;
    int stop_after;
} Listing;

static int record_entry(const char *name, int kind, void *user) {
    Listing *listing = (Listing *)user;
    if (listing->count < 8) {
        snprintf(listing->names[listing->count], sizeof listing->names[0], "%s", name);
        listing->kinds[listing->count] = kind;
    }
    listing->count++;
    return listing->stop_after > 0 && listing->count >= listing->stop_after;
}

static int listed_kind(const Listing *listing, const char *name) {
    for (int index = 0; index < listing->count && index < 8; ++index) {
        if (!strcmp(listing->names[index], name)) return listing->kinds[index];
    }
    return -2;
}

/* The pathname primitives Windows reads and installs content packs through.
 * They are portable so these assertions are proved here before they run on
 * Windows. */
static void test_path_primitives(const char *root) {
    char folder[1024], file[1024], unicode[1024], link[1024], moved[1024];
    char renamed_folder[1024], nested[1024], empty[1024], missing[1024];
    char inside[1100], through[1100];
    const char *unicode_name = "pack-\xc3\xa9\xe5\x90\x8d.bin";
    Listing listing;
    MdkrFileLock first = {-1}, second = {-1};
    int have_link;

    join(folder, sizeof folder, root, "folder");
    join(file, sizeof file, root, "file.bin");
    join(unicode, sizeof unicode, root, unicode_name);
    join(link, sizeof link, root, "link");
    join(moved, sizeof moved, root, "moved.bin");
    join(renamed_folder, sizeof renamed_folder, root, "renamed-folder");
    join(empty, sizeof empty, root, "empty");
    join(missing, sizeof missing, root, "missing");
    join(nested, sizeof nested, file, "below-a-file");

    expect(mdkr_mkdir_utf8(folder) == 0 && mdkr_mkdir_utf8(empty) == 0,
           "create primitive fixture folders");
    expect(write_bytes(file, "file") && write_bytes(unicode, "unicode"),
           "create primitive fixture files");
    expect(mdkr_path_kind_utf8(file) == MDKR_PATH_FILE, "kind reports a file");
    expect(mdkr_path_kind_utf8(folder) == MDKR_PATH_DIRECTORY, "kind reports a directory");
    expect(mdkr_path_kind_utf8(missing) == MDKR_PATH_MISSING, "kind reports a missing name");
    expect(mdkr_path_kind_utf8(nested) == MDKR_PATH_MISSING,
           "kind reports a name below a file as missing");
    have_link = make_directory_link(folder, link);
    if (have_link) {
        expect(mdkr_path_kind_utf8(link) == MDKR_PATH_LINK,
               "kind reports a directory link as a link, not its target");
    } else {
        printf("SKIP: this host cannot create a directory link\n");
    }

    memset(&listing, 0, sizeof listing);
    expect(mdkr_directory_list_utf8(root, record_entry, &listing) == 0,
           "list a directory to completion");
    expect(listing.count == 4 + have_link, "listing reports every entry once, never . or ..");
    expect(listed_kind(&listing, "folder") == MDKR_PATH_DIRECTORY &&
               listed_kind(&listing, "empty") == MDKR_PATH_DIRECTORY &&
               listed_kind(&listing, "file.bin") == MDKR_PATH_FILE,
           "listing reports each entry's kind");
    expect(listed_kind(&listing, unicode_name) == MDKR_PATH_FILE,
           "listing preserves a Unicode name exactly");
    if (have_link) {
        expect(listed_kind(&listing, "link") == MDKR_PATH_LINK,
               "listing reports a directory link as a link");
        /* Only the entries are never followed. The directory being listed
         * may itself be reached through a link, as opendir() allows: a
         * caller chooses a pack root that way (see mod_source.c). */
        join(through, sizeof through, folder, "through-link.bin");
        expect(write_bytes(through, "through"), "create a file behind the link");
        memset(&listing, 0, sizeof listing);
        expect(mdkr_directory_list_utf8(link, record_entry, &listing) == 0 &&
                   listing.count == 1 &&
                   listed_kind(&listing, "through-link.bin") == MDKR_PATH_FILE,
               "listing a directory link lists the directory it leads to");
        expect(mdkr_remove_utf8(through) == 0, "remove the file behind the link");
    }
#if defined(_WIN32) && defined(MDKR_FS_UTF8_TESTING)
    /* Such a file system is listed through FindFirstFileW, with the same
     * entries and kinds; a refused handle query must not fail the listing. */
    for (int which = 1; which <= 2; ++which) {
        mdkr_fs_utf8_test_refused_query = which;
        memset(&listing, 0, sizeof listing);
        expect(mdkr_directory_list_utf8(root, record_entry, &listing) == 0 &&
                   listing.count == 4 + have_link &&
                   listed_kind(&listing, "folder") == MDKR_PATH_DIRECTORY &&
                   listed_kind(&listing, "file.bin") == MDKR_PATH_FILE &&
                   listed_kind(&listing, unicode_name) == MDKR_PATH_FILE &&
                   (!have_link || listed_kind(&listing, "link") == MDKR_PATH_LINK),
               which == 1
                   ? "a directory whose attributes the file system will not report on a handle is still listed"
                   : "a directory the file system will not list on a handle is still listed");
    }
    mdkr_fs_utf8_test_refused_query = 0;
#endif
    memset(&listing, 0, sizeof listing);
    listing.stop_after = 1;
    expect(mdkr_directory_list_utf8(root, record_entry, &listing) == 2 &&
               listing.count == 1,
           "a visitor can stop the listing");
    memset(&listing, 0, sizeof listing);
    expect(mdkr_directory_list_utf8(empty, record_entry, &listing) == 0 &&
               listing.count == 0,
           "an empty directory lists nothing");
    expect(mdkr_directory_list_utf8(missing, record_entry, &listing) == 1,
           "a missing directory cannot be listed");

    errno = 0;
    expect(mdkr_rename_exclusive_utf8(file, folder) == -1 && errno == EEXIST,
           "exclusive rename refuses an existing directory name");
    if (have_link) {
        errno = 0;
        expect(mdkr_rename_exclusive_utf8(file, link) == -1 && errno == EEXIST &&
                   mdkr_path_kind_utf8(link) == MDKR_PATH_LINK,
               "exclusive rename refuses a name held by a link");
    }
#ifndef _WIN32
    {
        char dangling[1024], absent[1024];
        join(dangling, sizeof dangling, root, "dangling");
        join(absent, sizeof absent, root, "absent-target");
        expect(symlink(absent, dangling) == 0, "create a dangling link fixture");
        errno = 0;
        expect(mdkr_rename_exclusive_utf8(file, dangling) == -1 && errno == EEXIST &&
                   mdkr_path_kind_utf8(absent) == MDKR_PATH_MISSING,
               "exclusive rename refuses a dangling link and creates nothing through it");
        expect(mdkr_unlink_utf8(dangling) == 0, "remove the dangling link fixture");
    }
#endif
    expect(mdkr_rename_exclusive_utf8(file, moved) == 0 &&
               mdkr_path_kind_utf8(file) == MDKR_PATH_MISSING &&
               mdkr_path_kind_utf8(moved) == MDKR_PATH_FILE,
           "exclusive rename moves a file to a free name");
    expect(mdkr_rename_exclusive_utf8(folder, renamed_folder) == 0 &&
               mdkr_path_kind_utf8(renamed_folder) == MDKR_PATH_DIRECTORY,
           "exclusive rename moves a directory to a free name");

    expect(mdkr_directory_hold_utf8(renamed_folder, &first) == 0,
           "hold a directory");
    errno = 0;
    expect(mdkr_directory_hold_utf8(renamed_folder, &second) == -1 && errno == EBUSY &&
               second.handle == -1,
           "a second hold on a held directory reports it busy");
    join(inside, sizeof inside, renamed_folder, "inside.bin");
    expect(write_bytes(inside, "still writable"),
           "a held directory's contents stay writable");
    /* The installer empties the stage it holds, which takes a listing of it
     * while the hold is in place. */
    memset(&listing, 0, sizeof listing);
    expect(mdkr_directory_list_utf8(renamed_folder, record_entry, &listing) == 0 &&
               listing.count == 1 &&
               listed_kind(&listing, "inside.bin") == MDKR_PATH_FILE,
           "a held directory can be listed");
    expect(mdkr_remove_utf8(inside) == 0,
           "a held directory's contents stay removable");
#ifdef _WIN32
    expect(mdkr_rename_exclusive_utf8(renamed_folder, folder) == -1 &&
               mdkr_path_kind_utf8(renamed_folder) == MDKR_PATH_DIRECTORY,
           "Windows refuses to rename a held directory");
#endif
    mdkr_file_lock_release(&first);
    expect(first.handle == -1, "release clears the hold");
    expect(mdkr_directory_hold_utf8(renamed_folder, &second) == 0,
           "a released directory can be held again");
    mdkr_file_lock_release(&second);
    errno = 0;
    expect(mdkr_directory_hold_utf8(moved, &first) == -1 && first.handle == -1,
           "a file cannot be held as a directory");
    if (have_link) {
        expect(mdkr_directory_hold_utf8(link, &first) == -1 && first.handle == -1,
               "a directory link cannot be held");
        expect(mdkr_unlink_utf8(link) == 0 || mdkr_rmdir_utf8(link) == 0,
               "remove the directory link itself");
        expect(mdkr_path_kind_utf8(renamed_folder) == MDKR_PATH_DIRECTORY,
               "removing the link leaves its target");
    }

    expect(mdkr_remove_utf8(moved) == 0 && mdkr_remove_utf8(unicode) == 0 &&
               mdkr_rmdir_utf8(renamed_folder) == 0 && mdkr_rmdir_utf8(empty) == 0,
           "remove primitive fixtures");
}

/* mdkr_path_query_utf8 has one contract on every platform (fs_utf8.h): a
 * missing path answers 0 with every output cleared, and -1 is a real error.
 * Callers create a missing save or character folder on the first answer and
 * report the second, so a platform that answered "missing" with -1 (POSIX did)
 * or a real error with 0 (Windows did) broke whichever callers were written
 * against the other one. */
static void test_query_contract(const char *root) {
    char folder[1024], file[1024], missing[1024];
    char missing_parent[1100], below_file[1100];
    int exists, regular, directory;

    join(folder, sizeof folder, root, "query-folder");
    join(file, sizeof file, root, "query-file.bin");
    join(missing, sizeof missing, root, "query-missing");
    join(missing_parent, sizeof missing_parent, missing, "child.bin");
    join(below_file, sizeof below_file, file, "child.bin");
    expect(mdkr_mkdir_utf8(folder) == 0 && write_bytes(file, "file"),
           "create the query fixtures");

    exists = regular = directory = 7;
    expect(mdkr_path_query_utf8(file, &exists, &regular, &directory) == 0 &&
               exists == 1 && regular == 1 && directory == 0,
           "query reports an existing file");
    exists = regular = directory = 7;
    expect(mdkr_path_query_utf8(folder, &exists, &regular, &directory) == 0 &&
               exists == 1 && regular == 0 && directory == 1,
           "query reports an existing folder");
    exists = regular = directory = 7;
    expect(mdkr_path_query_utf8(missing, &exists, &regular, &directory) == 0 &&
               exists == 0 && regular == 0 && directory == 0,
           "a missing name answers 0 with exists cleared, not an error");
    exists = regular = directory = 7;
    expect(mdkr_path_query_utf8(missing_parent, &exists, &regular,
                                &directory) == 0 &&
               exists == 0 && regular == 0 && directory == 0,
           "a name whose folder is missing is missing, not an error");
    exists = regular = directory = 7;
    expect(mdkr_path_query_utf8(below_file, &exists, &regular, &directory) == 0 &&
               exists == 0 && regular == 0 && directory == 0,
           "a name below a file is missing, not an error");
    expect(mdkr_path_query_utf8(missing, NULL, NULL, NULL) == 0,
           "a missing name answers 0 with every output NULL");

    exists = regular = directory = 7;
    errno = 0;
    expect(mdkr_path_query_utf8(NULL, &exists, &regular, &directory) == -1 &&
               errno == EINVAL && exists == 0 && regular == 0 && directory == 0,
           "no path at all is an error, with every output cleared");
    exists = 7;
    errno = 0;
    expect(mdkr_path_query_utf8("", &exists, NULL, NULL) == -1 &&
               errno == EINVAL && exists == 0,
           "an empty path is an error, not a missing one");
#ifdef _WIN32
    {
        /* '?' is not a legal character in a Windows file name, so the file
         * system refuses the name itself (ERROR_INVALID_NAME). */
        char invalid[1100];
        join(invalid, sizeof invalid, root, "bad?name.bin");
        exists = regular = directory = 7;
        errno = 0;
        expect(mdkr_path_query_utf8(invalid, &exists, &regular, &directory) == -1 &&
                   errno == EINVAL && exists == 0 && regular == 0 && directory == 0,
               "a name Windows refuses is an error, not a missing path");
    }
#else
    {
        char locked[1024], inside[1100];
        join(locked, sizeof locked, root, "query-locked");
        join(inside, sizeof inside, locked, "inside.bin");
        expect(mdkr_mkdir_utf8(locked) == 0 && write_bytes(inside, "inside"),
               "create the permission fixture");
        expect(chmod(locked, 0) == 0, "deny search permission on the fixture folder");
        if (geteuid() == 0) {
            printf("SKIP: root searches a folder whatever its permissions\n");
        } else {
            exists = regular = directory = 7;
            errno = 0;
            expect(mdkr_path_query_utf8(inside, &exists, &regular, &directory) == -1 &&
                       errno == EACCES && exists == 0 && regular == 0 &&
                       directory == 0,
                   "a name in a folder the caller may not search is an error, "
                   "not a missing path");
        }
        expect(chmod(locked, 0700) == 0 && mdkr_remove_utf8(inside) == 0 &&
                   mdkr_rmdir_utf8(locked) == 0,
               "remove the permission fixture");
    }
#endif
    expect(mdkr_remove_utf8(file) == 0 && mdkr_rmdir_utf8(folder) == 0,
           "remove the query fixtures");
}

static int names_hold(const MdkrDirectoryNames *names, const char *name) {
    for (size_t index = 0; index < names->count; ++index) {
        if (!strcmp(names->names[index], name)) return 1;
    }
    return 0;
}

/* The collected listing product code uses in place of narrow
 * opendir()/readdir(): every entry once, Unicode names exact, and nothing
 * held when the folder cannot be listed. */
static void test_directory_names(const char *root) {
    char folder[1024], file[1100], unicode[1100], inner[1100], missing[1024];
    const char *unicode_name = "char-\xc3\xa9\xe5\x90\x8d.mdkc";
    MdkrDirectoryNames names;

    join(folder, sizeof folder, root, "names-folder");
    join(file, sizeof file, folder, "plain.bin");
    join(unicode, sizeof unicode, folder, unicode_name);
    join(inner, sizeof inner, folder, "inner");
    join(missing, sizeof missing, root, "names-missing");
    expect(mdkr_mkdir_utf8(folder) == 0, "create the names fixture folder");

    memset(&names, 0x5a, sizeof names);
    expect(mdkr_directory_names_utf8(folder, &names) == 0 && names.count == 0 &&
               names.unnamed_count == 0,
           "an empty folder has no names");
    mdkr_directory_names_free(&names);

    expect(write_bytes(file, "plain") && write_bytes(unicode, "unicode") &&
               mdkr_mkdir_utf8(inner) == 0,
           "fill the names fixture folder");
    expect(mdkr_directory_names_utf8(folder, &names) == 0 && names.count == 3 &&
               names.unnamed_count == 0 && names_hold(&names, "plain.bin") &&
               names_hold(&names, unicode_name) && names_hold(&names, "inner") &&
               !names_hold(&names, ".") && !names_hold(&names, ".."),
           "every name is collected once, a Unicode name exactly, never . or ..");
    mdkr_directory_names_free(&names);
    expect(names.names == NULL && names.count == 0 && names.unnamed == NULL &&
               names.unnamed_count == 0,
           "releasing the names clears them");
#ifdef MDKR_FS_UTF8_TESTING
    /* A listing that stops part-way keeps what it read (fs_utf8.h), so a
     * caller that can use part of it is not left with nothing. */
    mdkr_fs_utf8_test_names_fail_after = 2;
    errno = 0;
    expect(mdkr_directory_names_utf8(folder, &names) == -1 && errno == EIO &&
               names.count == 2 && names.names != NULL,
           "a listing that stops part-way fails and holds the names read before");
    mdkr_directory_names_free(&names);
    mdkr_fs_utf8_test_names_fail_after = 0;
    expect(mdkr_directory_names_utf8(folder, &names) == -1 && names.count == 0,
           "a listing that stops at once holds nothing");
    mdkr_directory_names_free(&names);
    mdkr_fs_utf8_test_names_fail_after = -1;
#endif

    memset(&names, 0x5a, sizeof names);
    errno = 0;
    expect(mdkr_directory_names_utf8(missing, &names) == -1 && errno != 0 &&
               names.names == NULL && names.count == 0,
           "a missing folder cannot be listed, and nothing is held");
    memset(&names, 0x5a, sizeof names);
    expect(mdkr_directory_names_utf8(file, &names) == -1 &&
               names.names == NULL && names.count == 0,
           "a file cannot be listed as a folder, and nothing is held");
    mdkr_directory_names_free(NULL);

    expect(mdkr_remove_utf8(file) == 0 && mdkr_remove_utf8(unicode) == 0 &&
               mdkr_rmdir_utf8(inner) == 0 && mdkr_rmdir_utf8(folder) == 0,
           "remove the names fixture");
}

/* A name with no UTF-8 spelling -- on Windows a name that is not valid UTF-16
 * (an unpaired surrogate), elsewhere one that is not valid UTF-8 -- is listed
 * as MDKR_PATH_BAD_NAME, never skipped, so a caller copying a folder can
 * refuse instead of silently leaving the entry out. macOS file systems refuse
 * such names outright, so there the case prints a SKIP line. */
static void test_names_without_utf8(const char *root) {
    char folder[1024];
    Listing listing;
    int created = 0;
    join(folder, sizeof folder, root, "names");
    expect(mdkr_mkdir_utf8(folder) == 0, "create the name fixture folder");
#ifdef _WIN32
    wchar_t wide[1100];
    const int length = MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1000);
    if (length > 0) {
        HANDLE file;
        wcscat(wide, L"\\bad\xD800name.bin");
        file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        created = file != INVALID_HANDLE_VALUE;
        if (created) CloseHandle(file);
    }
#else
    char bad[1100];
    FILE *file;
    join(bad, sizeof bad, folder, "bad\xff" "name.bin");
    file = fopen(bad, "wb");
    created = file != NULL;
    if (file != NULL) fclose(file);
#endif
    if (!created) {
        printf("SKIP: this file system cannot hold a name with no UTF-8 spelling (errno=%d)\n",
               errno);
    } else {
        memset(&listing, 0, sizeof listing);
        expect(mdkr_directory_list_utf8(folder, record_entry, &listing) == 0 &&
                   listing.count == 1 && listing.kinds[0] == MDKR_PATH_BAD_NAME,
               "a name with no UTF-8 spelling is listed as such, not skipped");
        {
            MdkrDirectoryNames names;
            expect(mdkr_directory_names_utf8(folder, &names) == 0 &&
                       names.count == 0 && names.unnamed_count == 1 &&
                       strstr(names.unnamed[0], "\xEF\xBF\xBD") != NULL &&
                       strncmp(names.unnamed[0], "bad", 3) == 0 &&
                       strstr(names.unnamed[0], "name.bin") != NULL,
                   "a collected listing leaves out a name with no UTF-8 "
                   "spelling and holds a readable stand-in for it");
#ifndef _WIN32
            expect(names.unnamed_count == 1 &&
                       strcmp(names.unnamed[0], "bad\xEF\xBF\xBD" "name.bin") == 0,
                   "the stand-in replaces exactly the byte with no spelling");
#endif
            mdkr_directory_names_free(&names);
        }
#ifdef _WIN32
        expect(strstr(listing.names[0], "\xEF\xBF\xBD") != NULL,
               "its stand-in name marks what could not convert");
        expect(DeleteFileW(wide) != 0, "remove the unnamed fixture");
#else
        expect(unlink(bad) == 0, "remove the unnamed fixture");
#endif
    }
    expect(mdkr_rmdir_utf8(folder) == 0, "remove the name fixture folder");
}

int main(void) {
    char temp[1024];
    char root[1200];
    char primitives[1300];
    char directories[8][1600];
    char path[1700];
    char moved[1700];
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
    if (!temp_directory(temp, sizeof temp)) {
        printf("FAIL: locate the temporary directory\n");
        return 1;
    }
    {
        char name[128];
        snprintf(name, sizeof(name),
                 "fs_utf8_Gr\xC3\xBC\xC3\x9F" "e_\xE8\xB7\xAF\xE5\xBE\x84_%ld",
                 process_id);
        join(root, sizeof root, temp, name);
    }

    (void)mdkr_rmdir_utf8(root);
    expect(mdkr_mkdir_utf8(root) == 0, "create a Unicode directory");
    snprintf(directories[depth++], sizeof(directories[0]), "%s", root);
    while (strlen(directories[depth - 1]) < strlen(root) + 320u && depth < 8) {
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

    join(primitives, sizeof primitives, root, "primitives");
    expect(mdkr_mkdir_utf8(primitives) == 0, "create the primitive fixture root");
    test_path_primitives(primitives);
    test_query_contract(primitives);
    test_directory_names(primitives);
    test_names_without_utf8(primitives);
    expect(mdkr_rmdir_utf8(primitives) == 0, "remove the primitive fixture root");

    while (depth > 0) {
        depth--;
        expect(mdkr_rmdir_utf8(directories[depth]) == 0,
               "remove an extended-length directory");
    }
    if (failures) {
        printf("FAIL: %d UTF-8 filesystem failure(s)\n", failures);
        return 1;
    }
    printf("PASS: UTF-8 filesystem boundary\n");
    return 0;
}
