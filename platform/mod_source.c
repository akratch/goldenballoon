/**
 * mod_source.c — see mod_source.h.
 */
#include "mod_source.h"
#include "mod_source_stream.h"

#include "fs_utf8.h"
/* Only for MDKR_MOD_PATH_MAX. The registry owns that bound and a source is
 * built from a registry entry's `root`, so borrowing the constant is how the
 * two stay the same number rather than two numbers that agree today. */
#include "mod_registry.h"
#include "miniz.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct MdkrModSource {
    int   is_zip;
    /* NULL for a directory source. Owned here; miniz is told not to close it,
     * so there is exactly one fclose and it is in mdkr_mod_source_close(). */
    FILE *zip_file;
    mz_zip_archive zip;
    char  root[MDKR_MOD_PATH_MAX];
#ifndef _WIN32
    int root_fd; /* Pins directory identity; every payload is opened beneath it. */
#else
    intptr_t root_handle;
#endif
};

/* ---------------------------------------------------------------- paths */

int mdkr_mod_source_path_is_safe(const char *rel) {
    size_t index = 0;
    size_t component_start = 0;

    if (rel == NULL || rel[0] == '\0') return 0;
    /* Absolute on every platform this port builds for. */
    if (rel[0] == '/' || rel[0] == '\\') return 0;

    for (;;) {
        char c = rel[index];

        /* A backslash is a separator on Windows and an ordinary character
         * elsewhere, which is exactly the kind of difference that lets one
         * platform resolve what another refuses. It is never legal here.
         *
         * A colon is refused wherever it appears, not just as a drive prefix:
         * "C:\\x" is absolute, "C:x" is drive-relative, and "sprite.png:hidden"
         * opens an NTFS alternate data stream. None of the three is a filename
         * a cross-platform pack can legitimately want. */
        if (c == '\\' || c == ':') return 0;

        if (c == '/' || c == '\0') {
            size_t length = index - component_start;

            /* Empty covers "a//b" and a trailing separator. A trailing
             * separator names a directory, and a directory has no bytes to
             * hand back. */
            if (length == 0) return 0;
            if (length == 1 && rel[component_start] == '.') return 0;
            if (length == 2 && rel[component_start] == '.' &&
                rel[component_start + 1] == '.') {
                return 0;
            }
            /* "..config" and "...", by contrast, are ordinary filenames. */
            if (c == '\0') break;
            component_start = index + 1;
        }
        index++;
        /* Bail before walking an unbounded string; the join below could not
         * hold it anyway. */
        if (index >= MDKR_MOD_PATH_MAX) return 0;
    }
    return 1;
}

/* ----------------------------------------------------------- directory arm */

#ifndef _WIN32
/* rel has passed the common lexical validator. Walk each component relative
 * to an owned directory descriptor: a check-then-fopen path walk would still
 * follow a symlink swapped in after inspection. NONBLOCK prevents a replaced
 * final FIFO from blocking before fstat rejects it. */
static FILE *directory_open_file(MdkrModSource *src, const char *rel) {
    char path[MDKR_MOD_PATH_MAX];
    char *component, *slash;
    int parent = openat(src->root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int fd;
    struct stat status;
    FILE *file;
    if (parent < 0) return NULL;
    memcpy(path, rel, strlen(rel) + 1);
    component = path;
    while ((slash = strchr(component, '/')) != NULL) {
        *slash = '\0';
        fd = openat(parent, component,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            close(parent);
            errno = error;
            return NULL;
        }
        close(parent);
        parent = fd;
        component = slash + 1;
    }
    fd = openat(parent, component,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        const int error = errno;
        close(parent);
        errno = error;
        return NULL;
    }
    close(parent);
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    file = fdopen(fd, "rb");
    if (file == NULL) close(fd);
    return file;
}
#else
static FILE *directory_open_file(MdkrModSource *src, const char *rel) {
    char path[MDKR_MOD_PATH_MAX];
    char *component, *slash;
    intptr_t parent = mdkr_directory_handle_child_utf8(src->root_handle, ".", 1);
    intptr_t child;
    if (parent == -1) return NULL;
    memcpy(path, rel, strlen(rel) + 1);
    component = path;
    while ((slash = strchr(component, '/')) != NULL) {
        *slash = '\0';
        child = mdkr_directory_handle_child_utf8(parent, component, 1);
        mdkr_directory_handle_close(parent);
        if (child == -1) return NULL;
        parent = child;
        component = slash + 1;
    }
    child = mdkr_directory_handle_child_utf8(parent, component, 0);
    mdkr_directory_handle_close(parent);
    return mdkr_read_handle_stream(child);
}
#endif

/* Reached only from mod_source_query(), below the one call to the validator. */
static int directory_query(MdkrModSource *src, const char *rel, int want_data,
                           void *buf, size_t cap, size_t *out_len) {
    FILE *file;
    long end;
    size_t size;
    file = directory_open_file(src, rel);
    if (file == NULL && (errno == ENOENT || errno == ENOTDIR)) {
        return MDKR_MOD_SOURCE_ABSENT;
    }
#ifndef _WIN32
    if (file == NULL && errno == ELOOP) return MDKR_MOD_SOURCE_REJECTED;
#endif
    if (file == NULL) return MDKR_MOD_SOURCE_IO_ERROR;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    end = ftell(file);
    if (end < 0) {
        fclose(file);
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    size = (size_t)end;

    /* The same ceiling the archive arm applies, for the same reason. */
    if ((unsigned long long)size > (unsigned long long)MDKR_MOD_SOURCE_ENTRY_MAX) {
        fclose(file);
        return MDKR_MOD_SOURCE_TOO_LARGE;
    }
    if (!want_data) {
        fclose(file);
        if (out_len != NULL) *out_len = size;
        return MDKR_MOD_SOURCE_OK;
    }
    if (cap < size) {
        fclose(file);
        if (out_len != NULL) *out_len = size;
        return MDKR_MOD_SOURCE_BUFFER_TOO_SMALL;
    }

    rewind(file);
    if (size != 0 && fread(buf, 1, size, file) != size) {
        fclose(file);
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    fclose(file);
    if (out_len != NULL) *out_len = size;
    return MDKR_MOD_SOURCE_OK;
}

/* --------------------------------------------------------------- zip arm */

/* Reached only from mod_source_query(), below the one call to the validator. */
static int zip_query(MdkrModSource *src, const char *rel, int want_data,
                     void *buf, size_t cap, size_t *out_len) {
    mz_zip_archive_file_stat stat;
    mz_uint32 index = 0;
    size_t size;

    /* Case-sensitive on purpose. miniz will happily match case-insensitively,
     * but a directory pack on Linux will not, and the two kinds are supposed
     * to answer identically wherever the game runs. */
    if (!mz_zip_reader_locate_file_v2(&src->zip, rel, NULL,
                                      MZ_ZIP_FLAG_CASE_SENSITIVE, &index)) {
        return MDKR_MOD_SOURCE_ABSENT;
    }
    if (!mz_zip_reader_file_stat(&src->zip, index, &stat)) {
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    /* A directory entry has a name but no bytes. The validator already refuses
     * a trailing separator, so this only catches an archive that marks a
     * separator-free name as a directory through its attribute bits. */
    if (stat.m_is_directory) return MDKR_MOD_SOURCE_ABSENT;

    /* m_uncomp_size is a number the pack author wrote. Everything below this
     * line that is sized from it happens after the cap, and nothing above it
     * allocated anything proportional to it — miniz read a fixed-size header,
     * not a payload. */
    if (stat.m_uncomp_size > (mz_uint64)MDKR_MOD_SOURCE_ENTRY_MAX) {
        return MDKR_MOD_SOURCE_TOO_LARGE;
    }
    size = (size_t)stat.m_uncomp_size;

    if (!want_data) {
        if (out_len != NULL) *out_len = size;
        return MDKR_MOD_SOURCE_OK;
    }
    if (cap < size) {
        if (out_len != NULL) *out_len = size;
        return MDKR_MOD_SOURCE_BUFFER_TOO_SMALL;
    }
    /* Extracts straight into the caller's buffer: miniz allocates only its
     * fixed 64 KiB streaming window, never a buffer the entry's header asked
     * for. This also verifies the entry's CRC-32, so a truncated or corrupted
     * payload comes back as an error rather than as plausible garbage. */
    if (size != 0 &&
        !mz_zip_reader_extract_to_mem(&src->zip, index, buf, cap, 0)) {
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    if (out_len != NULL) *out_len = size;
    return MDKR_MOD_SOURCE_OK;
}

/* ------------------------------------------------------------- the funnel */

/* Bounded-buffer funnel. Every public lookup, including the directory stream
 * primitive below, uses the same relative-path validator before opening. */
static int mod_source_query(MdkrModSource *src, const char *rel, int want_data,
                            void *buf, size_t cap, size_t *out_len) {
    if (out_len != NULL) *out_len = 0;
    if (src == NULL) return MDKR_MOD_SOURCE_IO_ERROR;
    if (!mdkr_mod_source_path_is_safe(rel)) return MDKR_MOD_SOURCE_REJECTED;

    return src->is_zip ? zip_query(src, rel, want_data, buf, cap, out_len)
                       : directory_query(src, rel, want_data, buf, cap, out_len);
}

/* ------------------------------------------------------------------- API */

MdkrModSource *mdkr_mod_source_from_zip_stream(FILE *stream) {
    MdkrModSource *src;
    long end;
    if (stream == NULL) return NULL;
    src = (MdkrModSource *)calloc(1, sizeof *src);
    if (src == NULL) { fclose(stream); return NULL; }
    /* calloc leaves the directory capability at 0. Close treats that as a
     * live descriptor, so an unset ZIP source would close stdin. */
#ifndef _WIN32
    src->root_fd = -1;
#else
    src->root_handle = -1;
#endif
    src->is_zip = 1;
    src->zip_file = stream;
    if (fseek(stream, 0, SEEK_END) != 0 || (end = ftell(stream)) < 0 ||
        fseek(stream, 0, SEEK_SET) != 0 ||
        !mz_zip_reader_init_cfile(&src->zip, stream, (mz_uint64)end, 0)) {
        fclose(stream);
        free(src);
        return NULL;
    }
    return src;
}

#ifndef _WIN32
MdkrModSource *mdkr_mod_source_from_directory_fd(int fd) {
    MdkrModSource *src = (MdkrModSource *)calloc(1, sizeof *src);
    if (src == NULL) return NULL;
    src->root_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (src->root_fd < 0) { free(src); return NULL; }
    return src;
}
#else
MdkrModSource *mdkr_mod_source_from_directory_handle(intptr_t handle) {
    MdkrModSource *src = (MdkrModSource *)calloc(1, sizeof *src);
    if (src == NULL) return NULL;
    src->root_handle = mdkr_directory_handle_child_utf8(handle, ".", 1);
    if (src->root_handle == -1) { free(src); return NULL; }
    return src;
}
#endif

MdkrModSource *mdkr_mod_source_open(const char *path, int is_zip) {
    MdkrModSource *src;
    size_t length;

    if (path == NULL || path[0] == '\0') return NULL;
    length = strlen(path);
    if (length + 1 > MDKR_MOD_PATH_MAX) return NULL;
    if (is_zip) {
        src = mdkr_mod_source_from_zip_stream(mdkr_fopen_utf8(path, "rb"));
        if (src != NULL) memcpy(src->root, path, length + 1);
        return src;
    }

    src = (MdkrModSource *)calloc(1, sizeof *src);
    if (src == NULL) return NULL;
#ifndef _WIN32
    src->root_fd = -1;
#else
    src->root_handle = -1;
#endif
    memcpy(src->root, path, length + 1);
    src->is_zip = is_zip ? 1 : 0;

    if (!src->is_zip) {
#ifdef _WIN32
        src->root_handle = mdkr_directory_handle_open_utf8(path);
        if (src->root_handle == -1) {
            free(src);
            return NULL;
        }
#else
        /* The caller chooses the root; payload descendants may not be links.
         * Pin the selected directory even if its pathname later changes. */
        src->root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (src->root_fd < 0) {
            free(src);
            return NULL;
        }
#endif
        return src;
    }

    return src;
}

FILE *mdkr_mod_source_open_directory_file(MdkrModSource *src, const char *rel) {
    if (src == NULL || src->is_zip || !mdkr_mod_source_path_is_safe(rel)) return NULL;
    return directory_open_file(src, rel);
}

void mdkr_mod_source_close(MdkrModSource *src) {
    if (src == NULL) return;
    if (src->is_zip) {
        mz_zip_reader_end(&src->zip);
        if (src->zip_file != NULL) fclose(src->zip_file);
    }
#ifndef _WIN32
    if (src->root_fd >= 0) close(src->root_fd);
#else
    mdkr_directory_handle_close(src->root_handle);
#endif
    free(src);
}


/* ----------------------------------------------------------- enumeration */

/* Depth-first walk of a directory pack. `prefix` is the path relative to the
 * pack root, so the callback sees exactly the string read()/has() would take.
 *
 * Returns 0 for a complete walk, 1 for "could not open", and 2 for "the
 * visitor asked to stop". Those are three different answers and the caller
 * needs them apart: folding "stopped" into "finished" made the early-stop
 * contract silently untrue for nested packs -- which is every Rice pack -- so
 * the cheap "is this a Rice pack?" probe walked and stat()ed every file in it.
 */

#ifdef _WIN32
static int directory_enumerate(intptr_t handle, const char *prefix, int depth,
                               MdkrModSourceVisit visit, void *user);
typedef struct DirectoryVisitContext {
    intptr_t handle;
    const char *prefix;
    int depth;
    MdkrModSourceVisit visit;
    void *user;
} DirectoryVisitContext;

static int directory_visit_child(const char *name, int directory, void *opaque) {
    DirectoryVisitContext *context = (DirectoryVisitContext *)opaque;
    char rel[MDKR_MOD_PATH_MAX];
    int written;
    intptr_t child;
    if (name[0] == '.') return 0;
    written = context->prefix[0] == '\0'
        ? snprintf(rel, sizeof rel, "%s", name)
        : snprintf(rel, sizeof rel, "%s/%s", context->prefix, name);
    if (written < 0 || (size_t)written >= sizeof rel ||
        !mdkr_mod_source_path_is_safe(rel)) return 0;
    /* Revalidate with a no-follow child open, including when a directory
     * entry was swapped after the OS enumerated it. */
    child = mdkr_directory_handle_child_utf8(context->handle, name, directory);
    if (child == -1) return 0;
    if (directory) {
        return directory_enumerate(child, rel, context->depth + 1,
                                    context->visit, context->user) == 2;
    }
    mdkr_directory_handle_close(child);
    return context->visit(rel, context->user) != 0;
}

static int directory_enumerate(intptr_t handle, const char *prefix, int depth,
                               MdkrModSourceVisit visit, void *user) {
    DirectoryVisitContext context = {handle, prefix, depth, visit, user};
    int result;
    if (handle == -1) return 1;
    if (depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX) {
        mdkr_directory_handle_close(handle);
        return 0;
    }
    result = mdkr_directory_handle_visit(handle, directory_visit_child, &context);
    mdkr_directory_handle_close(handle);
    return result;
}
#else
/* Takes ownership of fd on every path. Each recursive directory is opened
 * relative to its parent, never by rebuilding an untrusted pathname. */
static int directory_enumerate(int fd, const char *prefix, int depth,
                               MdkrModSourceVisit visit, void *user) {
    DIR *directory;
    struct dirent *entry;
    int result = 0;
    if (fd < 0) return 1;
    if (depth > MDKR_MOD_SOURCE_WALK_DEPTH_MAX) {
        close(fd);
        return 0;
    }
    directory = fdopendir(fd);
    if (directory == NULL) {
        close(fd);
        return 1;
    }
    while (result == 0 && (entry = readdir(directory)) != NULL) {
        char rel[MDKR_MOD_PATH_MAX];
        struct stat status;
        int written;
        if (entry->d_name[0] == '.') continue;
        written = prefix[0] == '\0'
            ? snprintf(rel, sizeof rel, "%s", entry->d_name)
            : snprintf(rel, sizeof rel, "%s/%s", prefix, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof rel ||
            !mdkr_mod_source_path_is_safe(rel)) continue;
        if (fstatat(fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(status.st_mode)) {
            const int child = openat(fd, entry->d_name,
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int nested = directory_enumerate(child, rel, depth + 1, visit, user);
            if (nested == 2) result = 2;
        } else if (S_ISREG(status.st_mode)) {
            if (visit(rel, user) != 0) result = 2;
        }
    }
    closedir(directory);
    return result;
}
#endif

int mdkr_mod_source_enumerate(MdkrModSource *src, MdkrModSourceVisit visit,
                              void *user) {
    if (src == NULL || visit == NULL) return 1;
    if (!src->is_zip) {
#ifdef _WIN32
        const int walked = directory_enumerate(
            mdkr_directory_handle_child_utf8(src->root_handle, ".", 1),
            "", 0, visit, user);
#else
        /* openat rather than dup: independent directory offsets make repeated
         * enumerations and nested visitor reads deterministic. */
        const int walked = directory_enumerate(
            openat(src->root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC),
            "", 0, visit, user);
#endif
        return walked == 1 ? 1 : 0;   /* a stop is a completed request */
    }

    {
        const mz_uint count = mz_zip_reader_get_num_files(&src->zip);
        mz_uint i;
        for (i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&src->zip, i, &stat)) continue;
            if (stat.m_is_directory) continue;
            if (!mdkr_mod_source_path_is_safe(stat.m_filename)) continue;
            if (visit(stat.m_filename, user) != 0) break;
        }
    }
    return 0;
}

int mdkr_mod_source_has(MdkrModSource *src, const char *rel) {
    return mod_source_query(src, rel, 0, NULL, 0, NULL) == MDKR_MOD_SOURCE_OK;
}

int mdkr_mod_source_read(MdkrModSource *src, const char *rel, void *buf,
                         size_t cap, size_t *out_len) {
    /* A null buffer has no capacity whatever the caller claimed, which makes
     * read(src, rel, NULL, 0, &n) the supported way to ask how big an entry
     * is instead of undefined behaviour. */
    if (buf == NULL) cap = 0;
    return mod_source_query(src, rel, 1, buf, cap, out_len);
}

const char *mdkr_mod_source_result_text(int result) {
    switch (result) {
    case MDKR_MOD_SOURCE_OK:
        return "read";
    case MDKR_MOD_SOURCE_ABSENT:
        return "this pack does not contain that file";
    case MDKR_MOD_SOURCE_REJECTED:
        return "that file name cannot be used inside a pack";
    case MDKR_MOD_SOURCE_TOO_LARGE:
        return "that file is too large for a content pack to replace";
    case MDKR_MOD_SOURCE_BUFFER_TOO_SMALL:
        return "that file did not fit the buffer offered for it";
    case MDKR_MOD_SOURCE_IO_ERROR:
    default:
        return "that file could not be read";
    }
}
