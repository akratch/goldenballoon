#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>        /* _commit — Windows has no fsync */
#include <windows.h>   /* MoveFileExA — the CRT's rename() cannot replace */
#endif

#include "save_codec.h"
#include "save_container.h"
#include "save_tools_core.h"

/* CMake threads the shared MDKR_VERSION release value in. A build that omits
   it is an unversioned developer build, and says so rather than claiming a
   release version it does not have. */
#ifndef MDKR_APP_VERSION_STR
#define MDKR_APP_VERSION_STR "development"
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct InputFile {
    uint8_t *bytes;
    size_t size;
} InputFile;

static void usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s inspect INPUT\n"
            "  %s export INPUT OUTPUT.mdkr-save\n"
            "  %s export-raw INPUT OUTPUT.eep\n"
            "  %s import INPUT DESTINATION.eep [--recover-corrupt]\n"
            "  %s recover INPUT OUTPUT.eep\n"
            "  %s edit-slot-state INPUT OUTPUT SLOT create|erase\n"
            "  %s edit-slot-name INPUT OUTPUT SLOT NAME\n"
            "  %s edit-slot-field INPUT OUTPUT SLOT FIELD VALUE\n"
            "      FIELD: taj-flags|trophies|bosses|tt-amulet|wizpig-amulet|"
            "keys|cutscenes\n"
            "  %s edit-course INPUT OUTPUT SLOT COURSE STATUS\n"
            "  %s edit-course-all INPUT OUTPUT SLOT STATUS\n"
            "  %s edit-balloon INPUT OUTPUT SLOT WORLD COUNT\n"
            "  %s edit-balloon-all INPUT OUTPUT SLOT COUNT\n"
            "  %s edit-world-flags INPUT OUTPUT SLOT WORLD FLAGS\n"
            "  %s edit-world-flags-all INPUT OUTPUT SLOT FLAGS\n"
            "  %s edit-config INPUT OUTPUT FIELD VALUE\n"
            "      FIELD: adventure-two|drumstick|language|tt-courses|"
            "default|subtitles\n"
            "  %s reset-records INPUT OUTPUT MASK\n"
            "\n"
            "The edit-* commands read and validate INPUT, apply exactly one\n"
            "field write through the engine's own save codec (platform/"
            "save_codec.c),\n"
            "and atomically install the result at OUTPUT (which may equal "
            "INPUT).\n"
            "COURSE/WORLD counts and field widths are the codec's compile-"
            "time\n"
            "constants (MDKR_SAVE_COURSE_COUNT / MDKR_SAVE_WORLD_COUNT), "
            "never a\n"
            "caller-supplied guess -- run `inspect` on a fresh slot to see "
            "them.\n",
            program, program, program, program, program, program, program,
            program, program, program, program, program, program, program,
            program, program);
}

static int read_input_file(const char *path, InputFile *out) {
    FILE *file;
    long length;
    uint8_t *bytes;
    size_t got;
    InputFile result = { NULL, 0 };
    if (path == NULL || out == NULL) return 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "mdkr-save: cannot open %s: %s\n", path,
                strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "mdkr-save: cannot determine the size of %s\n", path);
        fclose(file);
        return 0;
    }
    if ((unsigned long) length > MDKR_SAVE_CONTAINER_MAX_INPUT) {
        fprintf(stderr, "mdkr-save: %s exceeds the 64 KiB input limit\n", path);
        fclose(file);
        return 0;
    }
    bytes = (uint8_t *) malloc(length == 0 ? 1u : (size_t) length);
    if (bytes == NULL) {
        fprintf(stderr, "mdkr-save: out of memory reading %s\n", path);
        fclose(file);
        return 0;
    }
    got = fread(bytes, 1, (size_t) length, file);
    {
        int read_failed = got != (size_t) length || ferror(file);
        int close_failed = fclose(file) != 0;
        if (read_failed || close_failed) {
            fprintf(stderr, "mdkr-save: failed to read %s\n", path);
            free(bytes);
            return 0;
        }
    }
    result.bytes = bytes;
    result.size = got;
    *out = result;
    return 1;
}

static int decode_input(const char *path,
                        uint8_t payload[MDKR_SAVE_IMAGE_SIZE]) {
    InputFile file;
    MdkrSaveContainerResult result;
    memset(&file, 0, sizeof(file));
    if (!read_input_file(path, &file)) return 0;
    result = mdkr_save_container_decode(file.bytes, file.size, payload, NULL,
                                        NULL);
    free(file.bytes);
    if (result != MDKR_SAVE_CONTAINER_OK) {
        fprintf(stderr, "mdkr-save: %s: %s\n", path,
                mdkr_save_container_result_string(result));
        return 0;
    }
    return 1;
}

static int make_sibling_path(const char *path, const char *suffix, char **out) {
    size_t path_length;
    size_t suffix_length;
    char *result;
    if (path == NULL || suffix == NULL || out == NULL) return 0;
    path_length = strlen(path);
    suffix_length = strlen(suffix);
    if (path_length > SIZE_MAX - suffix_length - 1) return 0;
    result = (char *) malloc(path_length + suffix_length + 1);
    if (result == NULL) return 0;
    memcpy(result, path, path_length);
    memcpy(result + path_length, suffix, suffix_length + 1);
    *out = result;
    return 1;
}

/* Windows has no fsync(); _commit(fd) carries the same "flush this descriptor
 * to stable storage" guarantee. Named separately so the call sites read the
 * same on every platform. */
#ifdef _WIN32
#define mdkr_cli_fsync(fd) _commit(fd)
#else
#define mdkr_cli_fsync(fd) fsync(fd)
#endif

/* The Windows CRT's rename() FAILS when the destination exists, which is
 * exactly the case this tool's atomic install hits every time it overwrites an
 * existing save. MoveFileExA with REPLACE_EXISTING is the real atomic replace;
 * WRITE_THROUGH commits it before returning, standing in for the directory
 * flush Win32 cannot do. Same mapping as platform/stubs_dkr.c's dkr_fs_replace. */
static int mdkr_cli_replace(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0
               : -1;
#else
    return rename(from, to);
#endif
}

/* gmtime_r is POSIX; the Windows CRT spells it gmtime_s with the arguments the
 * other way round and an errno_t return. */
static struct tm *mdkr_cli_gmtime(const time_t *when, struct tm *out) {
#ifdef _WIN32
    return gmtime_s(out, when) == 0 ? out : NULL;
#else
    return gmtime_r(when, out);
#endif
}

static int sync_parent_directory(const char *path) {
#ifdef _WIN32
    /* Win32 cannot open a directory as a file descriptor, so there is no
     * directory-entry flush to perform. The rename below is the durability
     * boundary there (see platform/stubs_dkr.c's dkr_fs_replace). */
    (void) path;
    return 1;
#else
    const char *slash = strrchr(path, '/');
    char *directory = NULL;
    const char *open_path = ".";
    int descriptor;
    int ok;
    if (slash != NULL) {
        size_t length = (size_t) (slash - path);
        if (length == 0) {
            open_path = "/";
        } else {
            directory = (char *) malloc(length + 1);
            if (directory == NULL) return 0;
            memcpy(directory, path, length);
            directory[length] = '\0';
            open_path = directory;
        }
    }
    descriptor = open(open_path, O_RDONLY | O_DIRECTORY);
    ok = descriptor >= 0 && fsync(descriptor) == 0;
    if (descriptor >= 0 && close(descriptor) != 0) ok = 0;
    free(directory);
    return ok;
#endif
}

static int write_all(int descriptor, const uint8_t *bytes, size_t size) {
    while (size != 0) {
        ssize_t wrote = write(descriptor, bytes, size);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (wrote == 0) return 0;
        bytes += (size_t) wrote;
        size -= (size_t) wrote;
    }
    return 1;
}

static int verify_file(const char *path, const uint8_t *expected,
                       size_t size) {
    InputFile file;
    int equal;
    memset(&file, 0, sizeof(file));
    if (!read_input_file(path, &file)) return 0;
    equal = file.size == size && memcmp(file.bytes, expected, size) == 0;
    free(file.bytes);
    return equal;
}

static int fault_injected(const char *point) {
    const char *requested = getenv("MDKR_SAVE_FAULT");
    if (requested == NULL || strcmp(requested, point) != 0) {
        return 0;
    }
    fprintf(stderr, "mdkr-save: injected transaction failure at %s\n", point);
    errno = EIO;
    return 1;
}

static int write_atomic(const char *path, const uint8_t *bytes, size_t size,
                        int retain_previous) {
    char *staging = NULL;
    char *previous = NULL;
    int descriptor = -1;
    int destination_exists;
    int moved_previous = 0;
    int installed = 0;
    int ok = 0;

    if (!make_sibling_path(path, ".importing", &staging) ||
        (retain_previous &&
         !make_sibling_path(path, ".previous", &previous))) {
        fprintf(stderr, "mdkr-save: output path is too long\n");
        goto done;
    }
    descriptor = open(staging, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (descriptor < 0) {
        fprintf(stderr, "mdkr-save: failed while staging %s: %s\n", path,
                strerror(errno));
        goto done;
    }
    {
        int write_failed =
            !write_all(descriptor, bytes, size) ||
            mdkr_cli_fsync(descriptor) != 0;
        int close_failed = close(descriptor) != 0;
        descriptor = -1;
        if (write_failed || close_failed) {
            fprintf(stderr, "mdkr-save: failed while staging %s: %s\n", path,
                    strerror(errno));
            goto done;
        }
    }
    if (fault_injected("after-stage-write")) {
        goto done;
    }
    if (!verify_file(staging, bytes, size)) {
        fprintf(stderr, "mdkr-save: staged-file verification failed\n");
        goto done;
    }
    if (fault_injected("after-stage-verify")) {
        goto done;
    }
    destination_exists = access(path, F_OK) == 0;
    if (retain_previous && destination_exists) {
        (void) unlink(previous);
        if (mdkr_cli_replace(path, previous) != 0) {
            fprintf(stderr, "mdkr-save: could not create rollback image: %s\n",
                    strerror(errno));
            goto done;
        }
        moved_previous = 1;
    }
    if (fault_injected("after-backup")) {
        goto rollback;
    }
    if (mdkr_cli_replace(staging, path) != 0) {
        fprintf(stderr, "mdkr-save: could not install %s: %s\n", path,
                strerror(errno));
        goto rollback;
    }
    installed = 1;
    if (fault_injected("after-install") ||
        !sync_parent_directory(path) ||
        fault_injected("after-directory-sync") ||
        !verify_file(path, bytes, size) ||
        fault_injected("after-post-verify")) {
        fprintf(stderr,
                "mdkr-save: post-write synchronization or verification failed\n");
        goto rollback;
    }
    ok = 1;
    goto done;

rollback:
    if (installed) {
        (void) unlink(path);
        installed = 0;
    }
    if (moved_previous) {
        if (mdkr_cli_replace(previous, path) != 0) {
            fprintf(stderr,
                    "mdkr-save: rollback failed; the old image remains at %s\n",
                    previous);
        } else {
            (void) sync_parent_directory(path);
        }
    }

done:
    if (descriptor >= 0) close(descriptor);
    if (staging != NULL) (void) unlink(staging);
    free(staging);
    free(previous);
    return ok;
}

static int report_and_validate(const uint8_t payload[MDKR_SAVE_IMAGE_SIZE],
                               int allow_corrupt, unsigned *corrupt_mask_out) {
    MdkrSaveDocument document;
    MdkrSaveReport report;
    unsigned mask = 0;
    unsigned i;
    if (mdkr_save_decode(payload, MDKR_SAVE_IMAGE_SIZE, &document) !=
            MDKR_SAVE_OK ||
        mdkr_save_validate(&document, &report) != MDKR_SAVE_OK) {
        fprintf(stderr, "mdkr-save: save codec failed\n");
        return 0;
    }
    for (i = 0; i < MDKR_SAVE_BLOCK_COUNT; i++) {
        if (report.block_status[i] == MDKR_SAVE_BLOCK_CORRUPT) {
            mask |= 1u << i;
        }
    }
    if (mask != 0 && !allow_corrupt) {
        fprintf(stderr,
                "mdkr-save: input contains %u corrupt block(s); use recover "
                "explicitly or export the raw bytes for forensics\n",
                report.corrupt_block_count);
        return 0;
    }
    if (report.noncanonical_block_count != 0) {
        fprintf(stderr,
                "mdkr-save: warning: input contains %u safe but "
                "non-canonical block(s)\n",
                report.noncanonical_block_count);
    }
    if (corrupt_mask_out != NULL) *corrupt_mask_out = mask;
    return 1;
}

static int command_inspect(const char *input_path) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    size_t required;
    size_t size;
    char *summary;
    if (!decode_input(input_path, payload)) return 1;
    if (mdkr_save_summary_json(payload, sizeof(payload), NULL, 0, &size,
                               &required) != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: could not summarize input\n");
        return 1;
    }
    summary = (char *) malloc(required);
    if (summary == NULL) return 1;
    if (mdkr_save_summary_json(payload, sizeof(payload), summary, required,
                               NULL, NULL) != MDKR_SAVE_TOOL_OK) {
        free(summary);
        return 1;
    }
    puts(summary);
    free(summary);
    return 0;
}

static void current_timestamp(char output[64]) {
    time_t now = time(NULL);
    struct tm value;
    memset(output, 0, 64);
    if (mdkr_cli_gmtime(&now, &value) != NULL) {
        (void) strftime(output, 64, "%Y-%m-%dT%H:%M:%SZ", &value);
    }
}

static int command_export(const char *input_path, const char *output_path,
                          int raw) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    if (!decode_input(input_path, payload)) return 1;
    if (!raw && !report_and_validate(payload, 0, NULL)) return 1;
    if (raw) {
        return write_atomic(output_path, payload, sizeof(payload), 0) ? 0 : 1;
    } else {
        MdkrSaveContainerMetadata metadata;
        size_t required;
        size_t size;
        char *container;
        memset(&metadata, 0, sizeof(metadata));
        current_timestamp(metadata.created_at);
        snprintf(metadata.app_version, sizeof(metadata.app_version), "%s",
                 MDKR_APP_VERSION_STR);
        strcpy(metadata.source, "native");
        if (mdkr_save_container_encode(payload, &metadata, NULL, 0, &size,
                                       &required) != MDKR_SAVE_CONTAINER_OK) {
            return 1;
        }
        container = (char *) malloc(required);
        if (container == NULL) return 1;
        if (mdkr_save_container_encode(payload, &metadata, container, required,
                                       &size, NULL) !=
            MDKR_SAVE_CONTAINER_OK) {
            free(container);
            return 1;
        }
        if (!write_atomic(output_path, (const uint8_t *) container, size, 0)) {
            free(container);
            return 1;
        }
        free(container);
        return 0;
    }
}

static int command_import(const char *input_path, const char *output_path,
                          int recover) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t repaired[MDKR_SAVE_IMAGE_SIZE];
    unsigned corrupt_mask = 0;
    if (!decode_input(input_path, payload) ||
        !report_and_validate(payload, recover, &corrupt_mask)) {
        return 1;
    }
    if (recover && corrupt_mask != 0) {
        if (mdkr_save_recover_blocks(payload, corrupt_mask, repaired) !=
            MDKR_SAVE_TOOL_OK) {
            fprintf(stderr, "mdkr-save: recovery failed\n");
            return 1;
        }
        memcpy(payload, repaired, sizeof(payload));
    }
    return write_atomic(output_path, payload, sizeof(payload), 1) ? 0 : 1;
}

/* Shared tail for every edit-* command: validate the freshly-patched image
   decodes cleanly (apply_patch() inside save_tools_core already guarantees
   this, but a corrupt INPUT can still slip an unrelated block through
   unchanged) and install it atomically. Editing in place (OUTPUT == INPUT) is
   supported the same way import/recover already support it. */
static int install_edit(const uint8_t payload[MDKR_SAVE_IMAGE_SIZE],
                        const char *output_path) {
    return write_atomic(output_path, payload, MDKR_SAVE_IMAGE_SIZE, 1) ? 0 : 1;
}

static int parse_unsigned(const char *text, unsigned long *out) {
    char *end = NULL;
    unsigned long value;
    if (text == NULL || text[0] == '\0') return 0;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == NULL || *end != '\0') return 0;
    *out = value;
    return 1;
}

static int command_edit_slot_state(const char *input_path,
                                   const char *output_path,
                                   const char *slot_text, const char *mode) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot;
    int create = strcmp(mode, "create") == 0;
    int erase = strcmp(mode, "erase") == 0;
    MdkrSaveToolResult result;
    if (!create && !erase) {
        fprintf(stderr, "mdkr-save: edit-slot-state mode must be create or erase\n");
        return 1;
    }
    if (!parse_unsigned(slot_text, &slot)) {
        fprintf(stderr, "mdkr-save: invalid SLOT '%s'\n", slot_text);
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_slot_state(payload, (unsigned) slot, create, erase,
                                       edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-slot-state failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int command_edit_slot_name(const char *input_path,
                                  const char *output_path,
                                  const char *slot_text, const char *name) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot)) {
        fprintf(stderr, "mdkr-save: invalid SLOT '%s'\n", slot_text);
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_slot_name(payload, (unsigned) slot, name, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-slot-name failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int slot_field_from_name(const char *name, MdkrSaveSlotField *out) {
    static const struct { const char *name; MdkrSaveSlotField field; } table[] = {
        { "taj-flags", MDKR_SAVE_SLOT_FIELD_TAJ_FLAGS },
        { "trophies", MDKR_SAVE_SLOT_FIELD_TROPHIES },
        { "bosses", MDKR_SAVE_SLOT_FIELD_BOSSES },
        { "tt-amulet", MDKR_SAVE_SLOT_FIELD_TT_AMULET },
        { "wizpig-amulet", MDKR_SAVE_SLOT_FIELD_WIZPIG_AMULET },
        { "keys", MDKR_SAVE_SLOT_FIELD_KEYS },
        { "cutscenes", MDKR_SAVE_SLOT_FIELD_CUTSCENES },
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].field;
            return 1;
        }
    }
    return 0;
}

static int command_edit_slot_field(const char *input_path,
                                   const char *output_path,
                                   const char *slot_text,
                                   const char *field_text,
                                   const char *value_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot;
    unsigned long value;
    MdkrSaveSlotField field;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) || !parse_unsigned(value_text, &value)) {
        fprintf(stderr, "mdkr-save: invalid SLOT or VALUE\n");
        return 1;
    }
    if (!slot_field_from_name(field_text, &field)) {
        fprintf(stderr, "mdkr-save: unknown slot field '%s'\n", field_text);
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_slot_field(payload, (unsigned) slot, field,
                                       (uint32_t) value, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-slot-field failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int command_edit_course(const char *input_path, const char *output_path,
                               const char *slot_text, const char *course_text,
                               const char *status_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, course, status;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(course_text, &course) ||
        !parse_unsigned(status_text, &status)) {
        fprintf(stderr, "mdkr-save: invalid SLOT, COURSE or STATUS\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_course(payload, (unsigned) slot, (unsigned) course,
                                   (unsigned) status, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-course failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

/* Sets every course in SLOT to STATUS in one pass. The course count comes
   from MDKR_SAVE_COURSE_COUNT (platform/save_codec.h), the codec's own
   compile-time constant derived from the ROM's level table -- never a
   caller-supplied guess, which is the whole point: a hardcoded course count
   here is exactly the bug class this tool exists to remove. */
static int command_edit_course_all(const char *input_path,
                                   const char *output_path,
                                   const char *slot_text,
                                   const char *status_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, status;
    unsigned course;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(status_text, &status)) {
        fprintf(stderr, "mdkr-save: invalid SLOT or STATUS\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    for (course = 0; course < MDKR_SAVE_COURSE_COUNT; course++) {
        result = mdkr_save_edit_course(payload, (unsigned) slot, course,
                                       (unsigned) status, edited);
        if (result != MDKR_SAVE_TOOL_OK) {
            fprintf(stderr, "mdkr-save: edit-course-all failed at course %u: %s\n",
                    course, mdkr_save_tool_result_string(result));
            return 1;
        }
        memcpy(payload, edited, MDKR_SAVE_IMAGE_SIZE);
    }
    return install_edit(payload, output_path);
}

static int command_edit_balloon(const char *input_path, const char *output_path,
                                const char *slot_text, const char *world_text,
                                const char *count_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, world, count;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(world_text, &world) ||
        !parse_unsigned(count_text, &count)) {
        fprintf(stderr, "mdkr-save: invalid SLOT, WORLD or COUNT\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_balloon(payload, (unsigned) slot, (unsigned) world,
                                    (unsigned) count, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-balloon failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

/* Same shape as edit-course-all: MDKR_SAVE_WORLD_COUNT is the codec's own
   compile-time constant, so a save built with this can never diverge from
   whatever gNumberOfWorlds the engine derives from the ROM at runtime. */
static int command_edit_balloon_all(const char *input_path,
                                    const char *output_path,
                                    const char *slot_text,
                                    const char *count_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, count;
    unsigned world;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(count_text, &count)) {
        fprintf(stderr, "mdkr-save: invalid SLOT or COUNT\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    for (world = 0; world < MDKR_SAVE_WORLD_COUNT; world++) {
        result = mdkr_save_edit_balloon(payload, (unsigned) slot, world,
                                        (unsigned) count, edited);
        if (result != MDKR_SAVE_TOOL_OK) {
            fprintf(stderr, "mdkr-save: edit-balloon-all failed at world %u: %s\n",
                    world, mdkr_save_tool_result_string(result));
            return 1;
        }
        memcpy(payload, edited, MDKR_SAVE_IMAGE_SIZE);
    }
    return install_edit(payload, output_path);
}

static int command_edit_world_flags(const char *input_path,
                                    const char *output_path,
                                    const char *slot_text,
                                    const char *world_text,
                                    const char *flags_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, world, flags;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(world_text, &world) ||
        !parse_unsigned(flags_text, &flags)) {
        fprintf(stderr, "mdkr-save: invalid SLOT, WORLD or FLAGS\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_world_flags(payload, (unsigned) slot,
                                        (unsigned) world, (unsigned) flags,
                                        edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-world-flags failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int command_edit_world_flags_all(const char *input_path,
                                        const char *output_path,
                                        const char *slot_text,
                                        const char *flags_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long slot, flags;
    unsigned world;
    MdkrSaveToolResult result;
    if (!parse_unsigned(slot_text, &slot) ||
        !parse_unsigned(flags_text, &flags)) {
        fprintf(stderr, "mdkr-save: invalid SLOT or FLAGS\n");
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    for (world = 0; world < MDKR_SAVE_WORLD_COUNT; world++) {
        result = mdkr_save_edit_world_flags(payload, (unsigned) slot, world,
                                            (unsigned) flags, edited);
        if (result != MDKR_SAVE_TOOL_OK) {
            fprintf(stderr,
                    "mdkr-save: edit-world-flags-all failed at world %u: %s\n",
                    world, mdkr_save_tool_result_string(result));
            return 1;
        }
        memcpy(payload, edited, MDKR_SAVE_IMAGE_SIZE);
    }
    return install_edit(payload, output_path);
}

static int config_field_from_name(const char *name, MdkrSaveConfigField *out) {
    static const struct { const char *name; MdkrSaveConfigField field; } table[] = {
        { "adventure-two", MDKR_SAVE_CONFIG_FIELD_ADVENTURE_TWO },
        { "drumstick", MDKR_SAVE_CONFIG_FIELD_DRUMSTICK },
        { "language", MDKR_SAVE_CONFIG_FIELD_LANGUAGE },
        { "tt-courses", MDKR_SAVE_CONFIG_FIELD_TT_COURSES },
        { "default", MDKR_SAVE_CONFIG_FIELD_DEFAULT },
        { "subtitles", MDKR_SAVE_CONFIG_FIELD_SUBTITLES },
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].field;
            return 1;
        }
    }
    return 0;
}

static int command_edit_config(const char *input_path, const char *output_path,
                                const char *field_text,
                                const char *value_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long value;
    MdkrSaveConfigField field;
    MdkrSaveToolResult result;
    if (!parse_unsigned(value_text, &value)) {
        fprintf(stderr, "mdkr-save: invalid VALUE '%s'\n", value_text);
        return 1;
    }
    if (!config_field_from_name(field_text, &field)) {
        fprintf(stderr, "mdkr-save: unknown config field '%s'\n", field_text);
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_edit_config(payload, field, (uint32_t) value, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: edit-config failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int command_reset_records(const char *input_path,
                                  const char *output_path,
                                  const char *mask_text) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t edited[MDKR_SAVE_IMAGE_SIZE];
    unsigned long mask;
    MdkrSaveToolResult result;
    if (!parse_unsigned(mask_text, &mask)) {
        fprintf(stderr, "mdkr-save: invalid MASK '%s'\n", mask_text);
        return 1;
    }
    if (!decode_input(input_path, payload)) return 1;
    result = mdkr_save_reset_records(payload, (unsigned) mask, edited);
    if (result != MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: reset-records failed: %s\n",
                mdkr_save_tool_result_string(result));
        return 1;
    }
    return install_edit(edited, output_path);
}

static int command_recover(const char *input_path, const char *output_path) {
    uint8_t payload[MDKR_SAVE_IMAGE_SIZE];
    uint8_t repaired[MDKR_SAVE_IMAGE_SIZE];
    unsigned corrupt_mask = 0;
    if (!decode_input(input_path, payload) ||
        !report_and_validate(payload, 1, &corrupt_mask)) {
        return 1;
    }
    if (corrupt_mask == 0) {
        memcpy(repaired, payload, sizeof(repaired));
    } else if (mdkr_save_recover_blocks(payload, corrupt_mask, repaired) !=
               MDKR_SAVE_TOOL_OK) {
        fprintf(stderr, "mdkr-save: recovery failed\n");
        return 1;
    }
    return write_atomic(output_path, repaired, sizeof(repaired), 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        return command_inspect(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "export") == 0) {
        return command_export(argv[2], argv[3], 0);
    }
    if (argc == 4 && strcmp(argv[1], "export-raw") == 0) {
        return command_export(argv[2], argv[3], 1);
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "import") == 0) {
        int recover = argc == 5 && strcmp(argv[4], "--recover-corrupt") == 0;
        if (argc == 5 && !recover) {
            usage(argv[0]);
            return 2;
        }
        return command_import(argv[2], argv[3], recover);
    }
    if (argc == 4 && strcmp(argv[1], "recover") == 0) {
        return command_recover(argv[2], argv[3]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-slot-state") == 0) {
        return command_edit_slot_state(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-slot-name") == 0) {
        return command_edit_slot_name(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 7 && strcmp(argv[1], "edit-slot-field") == 0) {
        return command_edit_slot_field(argv[2], argv[3], argv[4], argv[5],
                                       argv[6]);
    }
    if (argc == 7 && strcmp(argv[1], "edit-course") == 0) {
        return command_edit_course(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-course-all") == 0) {
        return command_edit_course_all(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 7 && strcmp(argv[1], "edit-balloon") == 0) {
        return command_edit_balloon(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-balloon-all") == 0) {
        return command_edit_balloon_all(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 7 && strcmp(argv[1], "edit-world-flags") == 0) {
        return command_edit_world_flags(argv[2], argv[3], argv[4], argv[5],
                                        argv[6]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-world-flags-all") == 0) {
        return command_edit_world_flags_all(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 6 && strcmp(argv[1], "edit-config") == 0) {
        return command_edit_config(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 5 && strcmp(argv[1], "reset-records") == 0) {
        return command_reset_records(argv[2], argv[3], argv[4]);
    }
    usage(argv[0]);
    return 2;
}
