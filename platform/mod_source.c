/**
 * mod_source.c — see mod_source.h.
 */
#include "mod_source.h"

#include "fs_utf8.h"
/* Only for MDKR_MOD_PATH_MAX. The registry owns that bound and a source is
 * built from a registry entry's `root`, so borrowing the constant is how the
 * two stay the same number rather than two numbers that agree today. */
#include "mod_registry.h"
#include "miniz.h"

#include <stdlib.h>
#include <string.h>

struct MdkrModSource {
    int   is_zip;
    /* NULL for a directory source. Owned here; miniz is told not to close it,
     * so there is exactly one fclose and it is in mdkr_mod_source_close(). */
    FILE *zip_file;
    mz_zip_archive zip;
    char  root[MDKR_MOD_PATH_MAX];
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

/* Joins with '/', which every platform here accepts. Returns 0 rather than
 * truncating: a truncated path names a different file, and probing a different
 * file is worse than not finding one. */
static int path_join(char *out, size_t out_size, const char *base,
                     const char *leaf) {
    size_t base_length;
    size_t leaf_length;

    if (out == NULL || out_size == 0) return 0;
    out[0] = '\0';
    if (base == NULL || leaf == NULL) return 0;

    base_length = strlen(base);
    while (base_length > 0 &&
           (base[base_length - 1] == '/' || base[base_length - 1] == '\\')) {
        base_length--;
    }
    leaf_length = strlen(leaf);
    if (base_length + 1 + leaf_length + 1 > out_size) return 0;

    memcpy(out, base, base_length);
    out[base_length] = '/';
    memcpy(out + base_length + 1, leaf, leaf_length);
    out[base_length + 1 + leaf_length] = '\0';
    return 1;
}

/* ----------------------------------------------------------- directory arm */

/* Reached only from mod_source_query(), below the one call to the validator. */
static int directory_query(MdkrModSource *src, const char *rel, int want_data,
                           void *buf, size_t cap, size_t *out_len) {
    char full[MDKR_MOD_PATH_MAX];
    FILE *file;
    long end;
    size_t size;
    int is_regular = 0;

    if (!path_join(full, sizeof full, src->root, rel)) {
        /* The name passed the validator; it is this particular pack root that
         * makes the joined path too long to open. */
        return MDKR_MOD_SOURCE_IO_ERROR;
    }
    if (mdkr_path_query_utf8(full, NULL, &is_regular, NULL) != 0 || !is_regular) {
        return MDKR_MOD_SOURCE_ABSENT;
    }

    file = mdkr_fopen_utf8(full, "rb");
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

/* The single place a relative path is checked, and the single place the two
 * arms are chosen between. Both public lookups come through here, so neither
 * arm can be reached with a name the validator has not seen. Keep it that way:
 * a second entry point into either arm is how one of them keeps a hole a fix
 * closed in the other. */
static int mod_source_query(MdkrModSource *src, const char *rel, int want_data,
                            void *buf, size_t cap, size_t *out_len) {
    if (out_len != NULL) *out_len = 0;
    if (src == NULL) return MDKR_MOD_SOURCE_IO_ERROR;
    if (!mdkr_mod_source_path_is_safe(rel)) return MDKR_MOD_SOURCE_REJECTED;

    return src->is_zip ? zip_query(src, rel, want_data, buf, cap, out_len)
                       : directory_query(src, rel, want_data, buf, cap, out_len);
}

/* ------------------------------------------------------------------- API */

MdkrModSource *mdkr_mod_source_open(const char *path, int is_zip) {
    MdkrModSource *src;
    size_t length;
    long end;

    if (path == NULL || path[0] == '\0') return NULL;
    length = strlen(path);
    if (length + 1 > MDKR_MOD_PATH_MAX) return NULL;

    src = (MdkrModSource *)calloc(1, sizeof *src);
    if (src == NULL) return NULL;
    memcpy(src->root, path, length + 1);
    src->is_zip = is_zip ? 1 : 0;

    if (!src->is_zip) {
        int is_directory = 0;
        if (mdkr_path_query_utf8(path, NULL, NULL, &is_directory) != 0 ||
            !is_directory) {
            free(src);
            return NULL;
        }
        return src;
    }

    src->zip_file = mdkr_fopen_utf8(path, "rb");
    if (src->zip_file == NULL) {
        free(src);
        return NULL;
    }
    if (fseek(src->zip_file, 0, SEEK_END) != 0 ||
        (end = ftell(src->zip_file)) < 0) {
        fclose(src->zip_file);
        free(src);
        return NULL;
    }
    rewind(src->zip_file);
    /* A malformed or truncated central directory fails here, with a reason
     * miniz records and a pack that simply does not load. */
    if (!mz_zip_reader_init_cfile(&src->zip, src->zip_file, (mz_uint64)end, 0)) {
        fclose(src->zip_file);
        free(src);
        return NULL;
    }
    return src;
}

void mdkr_mod_source_close(MdkrModSource *src) {
    if (src == NULL) return;
    if (src->is_zip) {
        mz_zip_reader_end(&src->zip);
        if (src->zip_file != NULL) fclose(src->zip_file);
    }
    free(src);
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
